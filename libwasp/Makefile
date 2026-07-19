# libwasp — the wasp coordinator as a C library (POSIX build).
# `make` builds libwasp.so; `make check` runs the fake-node test.
CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -std=c11 -fPIC -pthread \
	-Iinclude -I../app/src -D_GNU_SOURCE
LDFLAGS += -pthread

SRCS := src/core.c src/host.c src/discover.c
OBJS := $(SRCS:.c=.o)

all: libwasp.so test/mock_test

libwasp.so: $(OBJS)
	$(CC) -shared $(LDFLAGS) -o $@ $(OBJS)

$(OBJS): include/wasp/libwasp.h src/internal.h

test/mock_test: test/mock_test.c libwasp.so
	$(CC) $(CFLAGS) -o $@ $< -L. -lwasp -Wl,-rpath,'$$ORIGIN/..'

check: test/mock_test
	./test/mock_test

clean:
	rm -f $(OBJS) libwasp.so test/mock_test

.PHONY: all check clean
