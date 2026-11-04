# windf - df for Windows
#
# Requires a MinGW-w64 cross toolchain:
#   Debian/Ubuntu: apt-get install gcc-mingw-w64-x86-64 gcc-mingw-w64-i686
#   MSYS2:         pacman -S mingw-w64-x86_64-gcc mingw-w64-i686-gcc

CC64  ?= x86_64-w64-mingw32-gcc
CC32  ?= i686-w64-mingw32-gcc
CFLAGS  = -O2 -Wall -Wextra -static -s
LDFLAGS = -lmpr

DIST  = dist
SRC   = src/df.c

BIN64 = $(DIST)/df-x64.exe
BIN32 = $(DIST)/df-x86.exe

.PHONY: all clean test

all: $(BIN64) $(BIN32)

$(DIST):
	mkdir -p $(DIST)

$(BIN64): $(SRC) | $(DIST)
	$(CC64) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BIN32): $(SRC) | $(DIST)
	$(CC32) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Formatting and argument-parsing self-tests. These do not need Windows:
# the helpers are pure string/math functions, so a host build is enough.
test: $(DIST)/df-selftest
	$(DIST)/df-selftest

$(DIST)/df-selftest: src/selftest.c src/df.c | $(DIST)
	$(CC) -O2 -Wall -Wextra -DWINDF_SELFTEST -o $@ src/selftest.c

clean:
	rm -f $(BIN64) $(BIN32) $(DIST)/df-selftest
