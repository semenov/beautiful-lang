# A Linux (Alpine, musl) environment for testing the compiler and static builds:
#   docker build -t lang-linux tools/linux
#   docker run --rm -v "$PWD":/src -w /src lang-linux sh -c 'cd compiler && cargo build --release && tests/run.sh'
FROM rust:alpine
RUN apk add --no-cache build-base pkgconf bash python3 \
    curl-dev curl-static openssl-dev openssl-libs-static \
    sqlite-dev sqlite-static zlib-dev zlib-static tzdata \
    nghttp2-static brotli-static zstd-static libidn2-static libpsl-static c-ares-dev libunistring-static
