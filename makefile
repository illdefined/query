CPPFLAGS ?= -D_FORTIFY_SOURCE=2
CFLAGS   ?= -pipe -Wall -fstack-protector -O2 -march=native -fno-common -fPIE
LDFLAGS  ?= -Wl,-O1 -Wl,--as-needed -Wl,--gc-sections -Wl,-pie

CPPFLAGS += -D_XOPEN_SOURCE=500
CFLAGS   += -std=c99
LDFLAGS  += -lowfat -lsqlite3

query: query.c

%: %.c
	echo "  CC  $*"; \
	$(CC) $(CPPFLAGS) $(CFLAGS) -o "$@" "$<"

.SILENT:
