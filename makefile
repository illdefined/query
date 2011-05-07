CFLAGS   ?= -pipe -O2 -march=native

CPPFLAGS += -D_XOPEN_SOURCE=500
CFLAGS   += -std=c99
LDFLAGS  += -lowfat -lsqlite3

query: query.c

%: %.c
	echo "  CC  $*"; \
	$(CC) $(CPPFLAGS) $(CFLAGS) -o "$@" "$<"

.SILENT:
