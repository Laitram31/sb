PREFIX  ?= /usr/local
CFLAGS  += -std=c99 -pedantic -D_DEFAULT_SOURCE -O3
LDFLAGS += -s -lX11 -lconfig -pthread

all: sb

sb: sb.c
	${CC} $< ${CFLAGS} ${LDFLAGS} -o $@

clean:
	rm -f sb

install: all
	mkdir -p ${DESTDIR}${PREFIX}/bin
	cp -f sb ${DESTDIR}${PREFIX}/bin
	chmod 755 ${DESTDIR}${PREFIX}/bin/sb

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/sb

.PHONY: all clean install uninstall
