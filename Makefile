
OPTIONS=-DUSE_SIGNAL -DUSE_SYSLOG 

VERSION=1.0

# Note that you must have -O to fully use -Wall
CC=gcc -Wall -O

# If you change this, you also have to edit startdemon 
# to set the PATH variable properly, AND pass in -s
BINDIR=/opt/freeha/bin

#### Set one of these as appropriate to your OS
#### Or, override on command-line with "make LDFLAGS=xxxx"
# Solaris flags
LDFLAGS=-lnsl -lsocket -lresolv
# Linux flags
#LDFLAGS=-lnsl -lresolv
# OSF flags
#LDFLAGS=-lresolv
# BSD flags
#LDFLAGS=


#########################################################################
# You shouldnt have to change anything below here
#########################################################################


CFLAGS=-g $(OPTIONS) -DBINDIR=\"$(BINDIR)\"

FILES=freehad.c freehad.h Makefile startdemon \
           README INSTALL RUNNING HEARTBEATS *hasrv service_scripts

all:	freehad

clean:
	rm -f freehad tags

#freehad:
#	$(CC) $(CFLAGS) -o $@ freehad.c $(LIBS)


# DESTDIR is a tweak to allow for ease of package creation.
# just leave it blank, normally

install:	all
	if [ ! -d $(DESTDIR)$(BINDIR) ] ; then mkdir -p $(DESTDIR)$(BINDIR) ; fi
	cp freehad startdemon *hasrv $(DESTDIR)$(BINDIR)
	cp -r service_scripts $(DESTDIR)$(BINDIR)
	@echo You now need to make your own boot-time script
	@echo See the startdemon script as an example

tar:
	sccs check
	mkdir /tmp/freeha-$(VERSION)
	cp -r $(FILES) /tmp/freeha-$(VERSION)
	(cd /tmp ; tar cvf freeha-$(VERSION).tar freeha-$(VERSION))
	rm -fr /tmp/freeha-$(VERSION)
	@echo archive is /tmp/freeha-$(VERSION).tar
