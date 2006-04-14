ifndef NAVISERVER
    NAVISERVER  = /usr/local/ns
endif

#
# Module name
#
MOD      =  nstftpd.so

#
# Objects to build.
#
OBJS     = nstftpd.o

PROCS   = tftpd_procs.tcl

INSTALL += install-procs

install-procs: $(PROCS)
	for f in $(PROCS); do $(INSTALL_SH) $$f $(INSTTCL)/; done

include  $(NAVISERVER)/include/Makefile.module

