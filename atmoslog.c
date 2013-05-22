/*
 * Atmoslog - Atmospheric data logger
 *
 * Version: 0.5
 *
 * Copyright (c) 2013, Joey Loman, <joey@binbash.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the author nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
*/

#include <sys/stat.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libusb-1.0/libusb.h>

#define VERSION "0.5"

#define VENDOR  0x0c45
#define PRODUCT 0x7402

unsigned static char uTemperature[] = { 0x01, 0x80, 0x33, 0x01, 0x00, 0x00, 0x00, 0x00 };
unsigned static char uIni1[] = { 0x01, 0x82, 0x77, 0x01, 0x00, 0x00, 0x00, 0x00 };
unsigned static char uIni2[] = { 0x01, 0x86, 0xff, 0x01, 0x00, 0x00, 0x00, 0x00 };

const static int reqIntLen = 8;
const static int nTimeout = 5000; /* timeout in ms */
const static int nEndpoint = 0x82;

int debug;
int attached;

float tempc;
float humi;

extern char *__progname;

void
usage()
{
    printf("usage: %s <options>\n\n", __progname);
    printf("options:\n\n");
    printf("  -d              -> daemonize (send program to the background)\n");
    printf("  -h              -> help/show options\n");
    printf("  -i <interval>   -> log interval (default: 30 sec)\n");
    printf("  -l <logfile>    -> log to file\n");
    printf("  -o <offset>     -> offset (calibration adjustment)\n");
    printf("  -v              -> enable verbose/debug logging\n");
    printf("\nversion: %s\n",VERSION);
    printf("\n");

    exit(1);
}

void
dup_proc()
{
    pid_t pid;

    if((pid = fork()) < 0) {
        exit(2);
    } else if(pid != 0) {
        exit(0);
    }

    setsid();

    if((pid = fork()) < 0) {
        exit(2);
    } else if(pid != 0) {
        exit(0);
    }

    umask(022);
}

void
cleanup_and_exit(libusb_context *ctx, libusb_device **list) {
    // free the usb device list
    libusb_free_device_list(list, 1);

    // deinitialize libusb
    libusb_exit(ctx);

    exit(1);
}

void
int_handler(int sig, libusb_context *ctx, libusb_device **list, libusb_device_handle *handle)
{
    // TODO: this doesn't work in ssh sessions
    //signal(sig, SIG_IGN);

    if (debug)
        printf("closing usb.\n");

    // if we detached kernel driver, reattach.
    if (attached == 1) {
        libusb_attach_kernel_driver(handle,0);
        libusb_attach_kernel_driver(handle,1);
    }

    // close device handle
    libusb_close(handle);

    // cleanup and exit
    cleanup_and_exit(ctx, list);
}

int
is_usbdevblock(libusb_device *dev)
{
    struct libusb_device_descriptor desc;

    libusb_get_device_descriptor(dev, &desc);
    if (desc.idVendor == VENDOR && desc.idProduct == PRODUCT) {
        return 1;
    }

    return 0;
}

static int
init_control_transfer(libusb_device_handle *handle)
{
    unsigned char question[] = { 0x01,0x01 };

    int ret;
    int i;

    ret = libusb_control_transfer(handle, 0x21, 0x09, 0x0201, 0x00, question, 2, nTimeout);
    if (ret < 0) {
        printf("error: failed to init control transfer..");

        switch (ret) {
            case LIBUSB_ERROR_TIMEOUT:
                printf("timeout!\n");
                break;
            case LIBUSB_ERROR_PIPE:
                printf("request not supported!\n");
                break;
            case LIBUSB_ERROR_NO_DEVICE:
                printf("device disconnected!\n");
                break;
            default:
                printf("other!\n");
                break;
        }
    }

    if (debug) {
        for (i = 0; i < reqIntLen; i++)
            printf("%02x ",question[i] & 0xFF);

        printf("\n");
    }

    return(ret);
}

static int
control_transfer(libusb_device_handle *handle, unsigned char *pquestion)
{
    unsigned char question[reqIntLen];

    int ret;
    int i;

    memcpy(question, pquestion, sizeof question);

    ret = libusb_control_transfer(handle, 0x21, 0x09, 0x0200, 0x01, question, reqIntLen, nTimeout);
    if (ret < 0) {
        printf("error: failed to control transfer..");

        switch (ret) {
            case LIBUSB_ERROR_TIMEOUT:
                printf("timeout!\n");
                break;
            case LIBUSB_ERROR_PIPE:
                printf("request not supported!\n");
                break;
            case LIBUSB_ERROR_NO_DEVICE:
                printf("device disconnected!\n");
                break;
            default:
                printf("other!\n");
                break;
        }
    }

    if (debug) {
        for (i = 0; i < reqIntLen; i++)
            printf("%02x ",question[i] & 0xFF);

        printf("\n");
    }

    return(ret);
}

static int
read_interrupt_transfer(libusb_device_handle *handle)
{
    unsigned char answer[reqIntLen];

    int BytesWritten = 0;

    int ret;
    int i;
    
    bzero(answer, reqIntLen);

    ret = libusb_interrupt_transfer(handle, nEndpoint, answer, reqIntLen, &BytesWritten, nTimeout);
    if (ret < 0) {
        printf("error: failed to read interrupt transfer..");

        switch (ret) {
            case LIBUSB_ERROR_TIMEOUT:
                printf("timeout!\n");
                break;
            case LIBUSB_ERROR_PIPE:
                printf("endpoint halted!\n");
                break;
            case LIBUSB_ERROR_OVERFLOW:
                printf("device offered more data!\n");
                break;
            case LIBUSB_ERROR_NO_DEVICE:
                printf("device disconnected!\n");
                break;
            default:
                printf("other!\n");
                break;
        }
    }

    if (debug) {
        printf("read %d bytes from endpoint address 0x%X\n", BytesWritten, nEndpoint);

        for (i = 0; i < reqIntLen; i++)
            printf("%02x ",answer[i] & 0xFF);

        printf("\n");
    }

    return(ret);
}
static int
read_interrupt_transfer_temperature(libusb_device_handle *handle)
{
    /* To get the temperature and humidity, query with 01 80 33 01 00 00 00 00.
     * A typical response is 80 04 19 24 04 ca 9c dc, where the temperature is 
     * in the third and fourth byte, and the humidity in the fifth and sixth byte.
     * To calculate the temperature and relative humidity, use the SHT1x methods; 
     * the temperature high and low bytes are in offset 2 and 3 respectively, 
     * while the relative humidity high and low bytes are in offset 4 and 5 respectively.
     */

    unsigned char answer[reqIntLen];

    int BytesWritten = 0;

    int ret;
    int i;
    int temp;
    int rh;

    float tempC2;
    float relhum;
    
    bzero(answer, reqIntLen);

    ret = libusb_interrupt_transfer(handle, nEndpoint, answer, reqIntLen, &BytesWritten, nTimeout);
    if (ret < 0) {
        printf("error: failed to read interrupt transfer..");

        switch (ret) {
            case LIBUSB_ERROR_TIMEOUT:
                printf("timeout!\n");
                break;
            case LIBUSB_ERROR_PIPE:
                printf("endpoint halted!\n");
                break;
            case LIBUSB_ERROR_OVERFLOW:
                printf("device offered more data!\n");
                break;
            case LIBUSB_ERROR_NO_DEVICE:
                printf("device disconnected!\n");
                break;
            default:
                printf("other!\n");
                break;
        }
    }

    if (debug) {
        printf("read %d bytes from endpoint address 0x%X\n", BytesWritten, nEndpoint);

        for (i = 0; i < reqIntLen; i++)
            printf("%02x ",answer[i] & 0xFF);

        printf("\n");
    }

    /*
     * SHT1x method: https://github.com/edorfaus/TEMPered/wiki/SHT1x
     */
    temp = ((signed char)answer[2] << 8) + ((unsigned char)answer[3] && 0xFF);
    tempC2 = -39.7 + 0.01 * temp;

    rh = ((answer[4] & 0xFF) << 8) + (answer[5] & 0xFF);
    relhum = -2.0468 + 0.0367 * rh - 1.5955e-6 * rh * rh;
    relhum = (tempC2 - 25) * (0.01 + 0.00008 * rh) + relhum;

    if (relhum <= 0)
        relhum = 0;

    if (relhum > 99)
        relhum = 100;

    if (debug) {
        printf("fetched tempature data = %f\n", tempC2);
        printf("fetched humidity data = %f\n", relhum);
    }

    tempc = tempC2;
    humi = relhum;

    return(ret);
}

libusb_device_handle*
setup_device(libusb_context *ctx, libusb_device **list, libusb_device *device)
{
    int err;

    libusb_device_handle *handle;

    // open usb device
    err = libusb_open(device, &handle);
    if (err) {
        printf("error: unable to open usb device\n");

        // cleanup and exit
        cleanup_and_exit(ctx, list);
    }

    if (libusb_kernel_driver_active(handle,0)) { 
        if (debug)
            printf("Device busy...detaching...\n"); 

        // detach usb device when active
        libusb_detach_kernel_driver(handle,0); 
        libusb_detach_kernel_driver(handle,1); 
        attached = 1;
    } else {
        if (debug)
            printf("Device free from kernel\n");
    }

    // set the active configuration for the device
    err = libusb_set_configuration(handle,0x01);
    if (err) {
        printf("error: failed to set the configuration.");

        switch (err) {
            case LIBUSB_ERROR_NOT_FOUND:
                printf("not found\n");
                break;
            case LIBUSB_ERROR_BUSY:
                printf("busy\n");
                break;
            case LIBUSB_ERROR_NO_DEVICE:
                printf("no device\n");
                break;
            default:
                printf("other\n");
                break;
        }

        // cleanup and exit
        cleanup_and_exit(ctx, list);
    }

    // claim usb device 0
    err = libusb_claim_interface(handle,0);
    if (err) {
        printf("error: failed to claim interface.");

        switch (err) {
            case LIBUSB_ERROR_NOT_FOUND:
                printf("not found\n");
                break;
            case LIBUSB_ERROR_BUSY:
                printf("busy\n");
                break;
            case LIBUSB_ERROR_NO_DEVICE:
                printf("no device\n");
                break;
            default:
                printf("other\n");
                break;
        }

        // cleanup and exit
        cleanup_and_exit(ctx, list);
    }

    // claim usb device 1 
    err = libusb_claim_interface(handle,1);
    if (err) {
        printf("error: failed to claim interface.");

        switch (err) {
            case LIBUSB_ERROR_NOT_FOUND:
                printf("not found\n");
                break;
            case LIBUSB_ERROR_BUSY:
                printf("busy\n");
                break;
            case LIBUSB_ERROR_NO_DEVICE:
                printf("no device\n");
                break;
            default:
                printf("other\n");
                break;
        }

        // cleanup and exit
        cleanup_and_exit(ctx, list);
    }

    if (init_control_transfer(handle) < 0)
        cleanup_and_exit(ctx, list);

    if (control_transfer(handle, uTemperature) < 0)
        cleanup_and_exit(ctx, list);

    if (read_interrupt_transfer(handle) < 0)
        cleanup_and_exit(ctx, list);

    if (control_transfer(handle, uIni1) < 0)
        cleanup_and_exit(ctx, list);

    if (read_interrupt_transfer(handle) < 0)
        cleanup_and_exit(ctx, list);

    if (control_transfer(handle, uIni2) < 0)
        cleanup_and_exit(ctx, list);

    if (read_interrupt_transfer(handle) < 0)
        cleanup_and_exit(ctx, list);

    if (read_interrupt_transfer(handle) < 0)
        cleanup_and_exit(ctx, list);

    return(handle);
}

int
main(int argc, char *argv[])
{
    char filename[256];

    // default log interval 30 secs
    int log_interval = 30;

    // default disable logfile logging
    int log2file = 0;

    // default daemonize is set to no
    int daemonize = 0;

    int i;

    ssize_t cnt;

    float scale;
    float offset;

    FILE *log_fp;

    // device list
    libusb_device **list;

    // device found
    libusb_device *found = NULL;

    // device context
    libusb_context *ctx = NULL;

    // create device handle
    libusb_device_handle *handle;   

    // set default values
    scale = 1.0000;
    offset = 0.0000;
    tempc = 0.0000;
    humi = 0.0000;
    debug = 0;
    attached = 0;

    // get arguments
    for(i = 0; i < argc; i++) {
        if(i == 0) {
            /* do nothing */
        } else if(!strcmp(argv[i],"-d")) {
            daemonize = 1;
        } else if(!strcmp(argv[i],"-h")) {
            usage();
        } else if(!strcmp(argv[i],"-i")) {
            if (argv[i+1]) {
                log_interval = atoi(argv[i+1]);

                i++;
            } else {
                printf("error: no interval value given!\n\n");
                usage();
            }
        } else if(!strcmp(argv[i],"-l")) {
            if (argv[i+1]) {
                snprintf(filename,sizeof(filename),"%s",argv[i+1]);
                log2file = 1;
                i++;
            } else {
                printf("error: no filename given!\n\n");
                usage();
            }
        } else if(!strcmp(argv[i],"-o")) {
            if (argv[i+1]) {
                offset = atof(argv[i+1]);

                i++;
            } else {
                printf("error: no offset value given!\n\n");
                usage();
            }
        } else if(!strcmp(argv[i],"-v")) {
            debug = 1;
        } else {
            printf("error: unknown option [%s]\n\n", argv[i]);
            usage();
        }
    }

    if (daemonize) {
        if (debug) {
            printf("error: you cannot enable verbose logging if you want to daemonize the program.\n\n");
            usage();
        }
    }

    // initialize libusb
    libusb_init(&ctx);

    // set debug to maximal (Level 3: informational messages are printed to stdout, warning and error messages are printed to stderr)
    if (debug)
        libusb_set_debug(ctx,3);
    else
        libusb_set_debug(ctx,0);

    // get usb device list
    cnt = libusb_get_device_list(ctx, &list);
    if (cnt < 0) {
        printf("error: no usb devices found\n");

        // cleanup and exit
        cleanup_and_exit(ctx, list);
    }

    // find our device
    for (i = 0; i < cnt; i++) {
        libusb_device *device = list[i];
        if (is_usbdevblock(device)) {
            found = device;
            break;
        }
    }

    if (!found) {
        printf("error: usb device not found!\n");

        cleanup_and_exit(ctx, list);
    }

    if (debug)
        printf("Found usb-dev-block!\n");

    handle = setup_device(ctx, list, found);

    // TODO: this doesn't work in ssh sessions
    // handle <ctrl> + c 
    //signal(SIGINT, int_handler, ctx, list, handle);

    if (daemonize) {
        if (log2file) {
            dup_proc();
        } else {
            printf("error: you also need to use the -l option if you want to daemonize the program.\n\n");
            usage();
        }
    }

    if (debug) {
        printf("scale value: %f\n", scale);
        printf("offset value: %f\n", offset);
    }

    while (1) {
        if (control_transfer(handle, uTemperature) < 0)
            cleanup_and_exit(ctx, list);

        if (read_interrupt_transfer_temperature(handle) < 0)
            cleanup_and_exit(ctx, list);

        tempc = (tempc * scale) + offset;

        // log output: "[temperature value] [himidity value]"
        if (log2file) {
            log_fp = fopen(filename,"w");
            fprintf(log_fp,"%f %f\n",tempc, humi);
            fclose(log_fp);
        } else {
            printf("%f %f\n", tempc, humi);
        }

        fflush(stdout);

        sleep(log_interval);
    }
}
