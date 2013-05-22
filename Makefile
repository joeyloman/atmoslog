ATMOSLOG 		= atmoslog.c
CC     			= gcc
CFLAGS 			= -O2 -Wall -lusb-1.0

atmoslog:		$(ATMOSLOG)
				$(CC) $(ATMOSLOG) $(CFLAGS)
				strip a.out
				mv a.out atmoslog

clean:			$(ATMOSLOG)
				rm -f atmoslog *.core

install:		$(ATMOSLOG)
				chmod 755 atmoslog
				cp atmoslog /usr/bin

uninstall:		$(ATMOSLOG)
				rm -f /usr/bin/atmoslog
