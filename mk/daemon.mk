# $Id$

include ../mk/config.mk
include ../mk/common.mk

COMSRCS  = errors.c ipc.c misc.c
SRVSRCS  = daemon.c server.c torrents.c
CLISRCS  = client.c remote.c

COMOBJS  = $(COMSRCS:%.c=%.o)
SRVOBJS  = $(SRVSRCS:%.c=%.o)
CLIOBJS  = $(CLISRCS:%.c=%.o)
SRCS     = $(COMSRCS) $(SRVSRCS) $(CLISRCS)

CFLAGS  += $(CFLAGS_EVENT) -I../libtransmission
LDLIBS  += ../libtransmission/libtransmission.a
LDFLAGS += $(LDFLAGS_EVENT)

all: transmission-daemon transmission-remote

transmission-daemon: OBJS    = $(SRVOBJS) $(COMOBJS)
transmission-daemon: $(LDLIBS) $(SRVOBJS) $(COMOBJS)
	$(LINK_RULE)

transmission-remote: OBJS    = $(CLIOBJS) $(COMOBJS)
transmission-remote: $(LDLIBS) $(CLIOBJS) $(COMOBJS)
	$(LINK_RULE)

%.o: %.c ../mk/config.mk ../mk/common.mk ../mk/daemon.mk
	$(CC_RULE)

clean:
	@echo "Clean transmission-daemon"
	@echo "Clean transmission-remote"
	@echo "Clean $(COMOBJS) $(SRVOBJS) $(CLIOBJS)"
	@$(RM) transmission-daemon transmission-remote
	@$(RM) $(COMOBJS) $(SRVOBJS) $(CLIOBJS)

.depend: $(SRCS) ../mk/config.mk ../mk/common.mk ../mk/daemon.mk
	$(DEP_RULE)

install: install.srv install.srv.man install.cli install.cli.man

install.srv: transmission-daemon
	$(INSTALL_BIN_RULE)

install.srv.man: transmission-daemon.1
	$(INSTALL_MAN_RULE)

install.cli: transmission-remote
	$(INSTALL_BIN_RULE)

install.cli.man: transmission-remote.1
	$(INSTALL_MAN_RULE)

-include .depend
