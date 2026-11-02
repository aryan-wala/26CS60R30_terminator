CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?= -lX11

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
X11_PREFIX ?= /opt/X11
CPPFLAGS += -I$(X11_PREFIX)/include
LDFLAGS += -L$(X11_PREFIX)/lib
endif
LDLIBS += -pthread

.PHONY: all clean

all: terminator

SOURCES := src/main.c src/worker.c src/shell.c src/history.c

terminator: $(SOURCES) src/terminator.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) -o $@ $(LDFLAGS) $(LDLIBS)

clean:
	rm -f terminator
