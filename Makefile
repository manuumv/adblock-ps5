# ps5-adblock
#
#   make host        development build for macOS/Linux in build/host/
#   make test        build and run every tests/test_*.c with ASan + UBSan
#   make fuzz        libFuzzer on the DNS parser (needs clang with libFuzzer, see tools/fuzz.sh)
#   make ps5         PS5 payload, needs PS5_PAYLOAD_SDK (or tools/ps5-build.sh for Docker)
#   make deploy PS5_HOST=<ip>   send the payload to the ELF loader on port 9021

VERSION := 0.1.0

SRCS := src/dns.c src/rules.c src/config.c src/forward.c src/server.c src/log.c
HDRS := $(wildcard src/*.h)
CFLAGS_COMMON := -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Isrc \
	-DADBLOCK_VERSION=\"$(VERSION)\"

HOST_CC ?= cc
SAN := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -g
HOST_PLAT := platform/host/platform.c

TESTS := $(patsubst tests/%.c,build/host/%,$(wildcard tests/test_*.c))

FUZZ_CC ?= clang
FUZZ_SECONDS ?= 600

.PHONY: all host test fuzz ps5 deploy clean

all: host

host: build/host/ps5-adblock

build/host/ps5-adblock: $(SRCS) src/main.c $(HOST_PLAT) $(HDRS)
	@mkdir -p $(@D)
	$(HOST_CC) $(CFLAGS_COMMON) -O1 $(SAN) -o $@ $(SRCS) src/main.c $(HOST_PLAT) -lpthread

build/host/test_%: tests/test_%.c $(SRCS) $(HOST_PLAT) $(HDRS)
	@mkdir -p $(@D)
	$(HOST_CC) $(CFLAGS_COMMON) -O1 $(SAN) -o $@ $< $(SRCS) $(HOST_PLAT) -lpthread

test: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; ./$$t; done; echo "all tests passed"

build/fuzz/fuzz_dns: tests/fuzz_dns.c src/dns.c $(HDRS)
	@mkdir -p $(@D)
	$(FUZZ_CC) $(CFLAGS_COMMON) -g -O1 -fsanitize=fuzzer,address,undefined -o $@ tests/fuzz_dns.c src/dns.c

fuzz: build/fuzz/fuzz_dns
	@mkdir -p build/fuzz/corpus
	./build/fuzz/fuzz_dns -max_total_time=$(FUZZ_SECONDS) -max_len=1024 build/fuzz/corpus

include platform/ps5/ps5.mk

clean:
	rm -rf build
