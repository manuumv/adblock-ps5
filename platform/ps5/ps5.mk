# PS5 payload (etaHEN or onionHEN), built with the ps5-payload-dev SDK. Included by
# the top-level Makefile; platform/ps5/docker/Dockerfile is the SDK image.

PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
PS5_HOST ?= ps5
PS5_PORT ?= 9021

PS5_CC    := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
PS5_STRIP := $(firstword $(wildcard $(PS5_PAYLOAD_SDK)/bin/prospero-llvm-strip) \
	$(wildcard $(PS5_PAYLOAD_SDK)/bin/prospero-strip))
PS5_ELF   := build/ps5/ps5-adblock.elf

ps5: $(PS5_ELF)

$(PS5_ELF): $(SRCS) src/main.c platform/ps5/platform.c $(HDRS)
	@test -x $(PS5_CC) || { echo "PS5_PAYLOAD_SDK not found: $(PS5_PAYLOAD_SDK) (try tools/ps5-build.sh)"; exit 1; }
	@mkdir -p $(@D)
	$(PS5_CC) $(CFLAGS_COMMON) -O2 -o $@ $(SRCS) src/main.c platform/ps5/platform.c -lSceNet -lpthread
	$(PS5_STRIP) --strip-all $@

deploy: $(PS5_ELF)
	nc -w 1 $(PS5_HOST) $(PS5_PORT) < $(PS5_ELF)
