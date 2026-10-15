# Build image for the PS5 payload: the ps5-payload-dev SDK on Ubuntu with LLVM 18.
# libclang-rt-18-dev adds libFuzzer and the sanitizers for tools/fuzz.sh.
# Runs on arm64 and amd64.
FROM ubuntu:24.04

ARG SDK_VERSION=v0.43

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        bash ca-certificates wget unzip make python3 netcat-openbsd \
        clang-18 lld-18 llvm-18 libclang-rt-18-dev \
    && ln -s /usr/bin/clang-18 /usr/local/bin/clang \
    && rm -rf /var/lib/apt/lists/*

RUN wget -q -O /tmp/sdk.zip \
        https://github.com/ps5-payload-dev/sdk/releases/download/${SDK_VERSION}/ps5-payload-sdk.zip \
    && unzip -q /tmp/sdk.zip -d /opt \
    && rm /tmp/sdk.zip \
    && chmod +x /opt/ps5-payload-sdk/bin/*

ENV PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
ENV PATH=/opt/ps5-payload-sdk/bin:${PATH}
# prospero-llvm-config honours $LLVM_CONFIG first
ENV LLVM_CONFIG=/usr/bin/llvm-config-18
