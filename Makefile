# ps5-adblock
#
#   make host        development build for macOS/Linux in build/host/
#   make test        build and run every tests/test_*.c with ASan + UBSan
#   make fuzz        libFuzzer on the DNS parser (needs clang with libFuzzer, see tools/fuzz.sh)
#   make ps5         PS5 payload, needs PS5_PAYLOAD_SDK (or tools/ps5-build.sh for Docker)
#   make deploy PS5_HOST=<ip>   send the payload to the ELF loader on port 9021
#   make install PS5_HOST=<ip>  copy it into the HEN autostart folder over FTP, then run it

VERSION := 0.1.0

# src/<module>/*.c, one folder per responsibility (see README "Source layout").
SRCS := $(wildcard src/*/*.c)
HDRS := $(wildcard src/*/*.h)
CFLAGS_COMMON := -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Isrc \
	-DADBLOCK_VERSION=\"$(VERSION)\"

HOST_CC ?= cc
HOST_LIBS := -lpthread -lcurl
SAN := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -g
HOST_PLAT := platform/host/platform.c

# tests/<module>/test_*.c mirror src/; vpath lets one rule build them all.
TESTS := $(patsubst %.c,build/host/%,$(notdir $(wildcard tests/*/test_*.c)))
vpath %.c $(sort $(dir $(wildcard tests/*/)))

FUZZ_CC ?= clang
FUZZ_SECONDS ?= 600

.PHONY: all host test fuzz fixtures bench ps5 deploy install clean

all: host

host: build/host/ps5-adblock

build/host/ps5-adblock: $(SRCS) src/main.c $(HOST_PLAT) $(HDRS)
	@mkdir -p $(@D)
	$(HOST_CC) $(CFLAGS_COMMON) -O1 $(SAN) -o $@ $(SRCS) src/main.c $(HOST_PLAT) $(HOST_LIBS)

build/host/test_%: test_%.c $(SRCS) $(HOST_PLAT) $(HDRS)
	@mkdir -p $(@D)
	$(HOST_CC) $(CFLAGS_COMMON) -Itests -O1 $(SAN) -o $@ $< $(SRCS) $(HOST_PLAT) $(HOST_LIBS)

test: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; ./$$t; done; echo "all tests passed"

FUZZ ?= dns   # dns or listparse

build/fuzz/fuzz_%: fuzz_%.c $(SRCS) $(HOST_PLAT) $(HDRS)
	@mkdir -p $(@D)
	$(FUZZ_CC) $(CFLAGS_COMMON) -g -O1 -fsanitize=fuzzer,address,undefined -o $@ $< $(SRCS) $(HOST_PLAT) $(HOST_LIBS)

fuzz: build/fuzz/fuzz_$(strip $(FUZZ))
	@mkdir -p build/fuzz/corpus-$(strip $(FUZZ))
	./build/fuzz/fuzz_$(strip $(FUZZ)) -max_total_time=$(FUZZ_SECONDS) -max_len=1024 build/fuzz/corpus-$(strip $(FUZZ))

# Real blocklists for the parser tests and the benchmark (git-ignored).
fixtures:
	@mkdir -p tests/fixtures
	curl -fsSL -o tests/fixtures/stevenblack.txt https://raw.githubusercontent.com/StevenBlack/hosts/master/hosts
	curl -fsSL -o tests/fixtures/hagezi-pro.txt https://raw.githubusercontent.com/hagezi/dns-blocklists/main/wildcard/pro-onlydomains.txt
	curl -fsSL -o tests/fixtures/hagezi-popupads.txt https://raw.githubusercontent.com/hagezi/dns-blocklists/main/wildcard/popupads-onlydomains.txt

build/host/bench_blocklist: bench_blocklist.c $(SRCS) $(HOST_PLAT) $(HDRS)
	@mkdir -p $(@D)
	$(HOST_CC) $(CFLAGS_COMMON) -O2 -o $@ $< $(SRCS) $(HOST_PLAT) $(HOST_LIBS)

bench: build/host/bench_blocklist
	./build/host/bench_blocklist

include platform/ps5/ps5.mk

clean:
	rm -rf build
