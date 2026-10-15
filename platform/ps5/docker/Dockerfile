# Build image for the PS5 payload: the ps5-payload-dev SDK on Ubuntu with LLVM 18,
# plus the pacbrew static libraries (curl, OpenSSL, zlib) for list downloads.
# libclang-rt-18-dev adds libFuzzer and the sanitizers; libcurl4-openssl-dev
# lets the host build and its tests run inside the image too.
# Runs on arm64 and amd64.
FROM ubuntu:24.04

ARG SDK_VERSION=v0.43
ARG PACBREW_VERSION=v0.40.3

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        bash ca-certificates wget unzip make pkg-config python3 netcat-openbsd \
        clang-18 lld-18 llvm-18 libclang-rt-18-dev libcurl4-openssl-dev openssl \
    && ln -s /usr/bin/clang-18 /usr/local/bin/clang \
    && rm -rf /var/lib/apt/lists/*

RUN wget -q -O /tmp/sdk.zip \
        https://github.com/ps5-payload-dev/sdk/releases/download/${SDK_VERSION}/ps5-payload-sdk.zip \
    && unzip -q /tmp/sdk.zip -d /opt \
    && rm /tmp/sdk.zip \
    && chmod +x /opt/ps5-payload-sdk/bin/*

# The pacbrew bundle carries its own SDK copy; only take the libraries and the
# *-config scripts so the SDK stays at ${SDK_VERSION}.
RUN wget -q -O /tmp/pb.tar.gz \
        https://github.com/ps5-payload-dev/pacbrew-repo/releases/download/${PACBREW_VERSION}/ps5-payload-dev.tar.gz \
    && tar -xzf /tmp/pb.tar.gz -C / opt/ps5-payload-sdk/target/user \
    && tar -xzf /tmp/pb.tar.gz -C /tmp opt/ps5-payload-sdk/bin \
    && for f in /tmp/opt/ps5-payload-sdk/bin/prospero-*-config; do \
         [ -e "/opt/ps5-payload-sdk/bin/$(basename "$f")" ] || cp -a "$f" /opt/ps5-payload-sdk/bin/; \
       done \
    && rm -rf /tmp/pb.tar.gz /tmp/opt

ENV PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
ENV PATH=/opt/ps5-payload-sdk/bin:${PATH}
# prospero-llvm-config honours $LLVM_CONFIG first
ENV LLVM_CONFIG=/usr/bin/llvm-config-18

# Fail early if static libcurl does not resolve.
RUN prospero-pkg-config --static --libs libcurl && prospero-pkg-config --cflags libcurl \
    && test -f /opt/ps5-payload-sdk/target/user/homebrew/etc/ca-bundle.crt
