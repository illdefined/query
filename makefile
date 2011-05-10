CPPFLAGS ?= -D_FORTIFY_SOURCE=2
CFLAGS   ?= -combine -pipe -Wall \
	-fstack-protector -fwhole-program -O2 \
	-march=native -fno-common -fPIE
LDFLAGS  ?= -Wl,-O1 -Wl,--as-needed -Wl,--gc-sections

CPPFLAGS += -D_XOPEN_SOURCE=500 $(:!pkg-config --cflags libowfat!) $(:!pkg-config --cflags sqlite3!)
CFLAGS   += -std=c99
LDFLAGS  += $(:!pkg-config --libs libowfat!) $(:!pkg-config --libs sqlite3!)

PREFIX   ?= /usr

query    := query.c

query: .depend
	sparse $(CPPFLAGS) $(query:M*.c)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(query) $(LDFLAGS)

.depend: $(query)
	$(CPP) $(CPPFLAGS) -M -MT query $(query) >$@

clean:
	rm -f .depend query

install: query
	mkdir -p $(DESTDIR)$(PREFIX)/bin/
	cp $> $(DESTDIR)$(PREFIX)/bin/

.PHONY: clean install
