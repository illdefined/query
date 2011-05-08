CPPFLAGS ?= -D_FORTIFY_SOURCE=2
CFLAGS   ?= -combine -pipe -Wall \
	-fstack-protector -fwhole-program -O2 \
	-march=native -fno-common -fPIE
LDFLAGS  ?= -Wl,-O1 -Wl,--as-needed -Wl,--gc-sections

CPPFLAGS += -D_XOPEN_SOURCE=500
CFLAGS   += -std=c99
LDFLAGS  += -lowfat -lsqlite3

query    := query.c

query: .depend
	sparse $(CPPFLAGS) $(query:M*.c)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(query) $(LDFLAGS)

.depend: $(query)
	$(CPP) $(CPPFLAGS) -M -MT query $(query) >$@

clean:
	rm -f .depend query

.PHONY: clean
