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

include  $(NAVISERVER)/include/Makefile.module

