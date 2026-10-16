# PS5 payload (etaHEN or onionHEN), built with the ps5-payload-dev SDK. Included by
# the top-level Makefile; platform/ps5/docker/Dockerfile is the SDK image (with the
# pacbrew static libraries).

PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
PS5_HOST ?= ps5
PS5_PORT ?= 9021

PS5_CC      := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
PS5_PKGCONF := $(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config
PS5_STRIP   := $(firstword $(wildcard $(PS5_PAYLOAD_SDK)/bin/prospero-llvm-strip) \
	$(wildcard $(PS5_PAYLOAD_SDK)/bin/prospero-strip))
PS5_ELF     := build/ps5/ps5-adblock.elf
# The SDK's Mozilla CA bundle, built into the payload so HTTPS works out of the box.
PS5_CA      := $(PS5_PAYLOAD_SDK)/target/user/homebrew/etc/ca-bundle.crt

ps5: $(PS5_ELF)

build/ps5/gen/ca_bundle.h: $(PS5_CA) tools/embed.py
	@mkdir -p $(@D)
	python3 tools/embed.py $< $@ CA_BUNDLE

$(PS5_ELF): $(SRCS) src/main.c platform/ps5/platform.c $(HDRS) build/ps5/gen/ca_bundle.h
	@test -x $(PS5_CC) || { echo "PS5_PAYLOAD_SDK not found: $(PS5_PAYLOAD_SDK) (try tools/ps5-build.sh)"; exit 1; }
	@mkdir -p $(@D)
	$(PS5_CC) $(CFLAGS_COMMON) -O2 -Ibuild/ps5/gen -DADBLOCK_CA_BUNDLE $$($(PS5_PKGCONF) --cflags libcurl) \
		-o $@ $(SRCS) src/main.c platform/ps5/platform.c \
		$$($(PS5_PKGCONF) --static --libs libcurl) -lSceNet -lSceNetCtl -lpthread
	$(PS5_STRIP) --strip-all $@

deploy: $(PS5_ELF)
	nc -w 1 $(PS5_HOST) $(PS5_PORT) < $(PS5_ELF)

# Copy the payload into the HEN's autostart folder over FTP, then run it. From
# then on it starts with the HEN and keeps the other HEN's folder in sync.
# etaHEN's FTP server is on 2121; onionHEN's FTP plugin on 1337 with
# PS5_HEN_DIR=/data/OnionHEN.
PS5_FTP ?= ftp://$(PS5_HOST):2121
PS5_HEN_DIR ?= /data/etaHEN
install: $(PS5_ELF)
	curl -sS --ftp-create-dirs -T $(PS5_ELF) $(PS5_FTP)$(PS5_HEN_DIR)/payloads/ps5-adblock.elf
	printf '' | curl -sS -T - $(PS5_FTP)$(PS5_HEN_DIR)/payloads/ps5-adblock.elf.auto_start
	nc -w 1 $(PS5_HOST) $(PS5_PORT) < $(PS5_ELF)
