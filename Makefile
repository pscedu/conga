# $Id: Makefile,v 1.8 2013/02/14 13:47:46 akadams Exp $

PREFIX = /usr/local

#PKG_CONFIG_PATH = /usr/local/lib/pkgconfig
#export PKG_CONFIG_PATH

DOC_DIR = ../doc
MAN_DIR = doc
BIN_DIR = bin
CONFIG_DIR = etc
TMP_DIR = tmp
RC_DIR = init.d

CXX = g++

all: server

server: libip-utils.a
	cd server ; $(MAKE) conga

libip-utils.a:
	cd ip-utils; ${MAKE} libip-utils.a

install: server
	cp -f server/conga ${PREFIX}/${BIN_DIR} ;
	/bin/chmod 755 ${PREFIX}/${BIN_DIR}/conga ;

install-devel: server
	cp -f server/conga ${PREFIX}/${BIN_DIR}/devel-conga ;
	/bin/chmod 755 ${PREFIX}/${BIN_DIR}/devel-conga ;

clean:	
	cd ip-utils ; ${MAKE} clean
	cd server ; ${MAKE} clean
