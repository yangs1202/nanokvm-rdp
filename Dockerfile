FROM debian:bookworm AS builder

ARG FREERDP_VERSION=3.14.0

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        git \
        libssl-dev \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

RUN git clone --branch "${FREERDP_VERSION}" --depth 1 https://github.com/FreeRDP/FreeRDP.git freerdp

COPY . .

RUN cmake -S . -B build/gateway \
        -DNANOKVM_RDP_FREERDP_DIR=/src/freerdp \
        -DNANOKVM_RDP_BUILD_AGENT=OFF \
        -DNANOKVM_RDP_BUILD_TESTS=OFF \
    && cmake --build build/gateway --target nanokvm-rdp-gateway --parallel

FROM debian:bookworm-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        ffmpeg \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/gateway/nanokvm-rdp-gateway /usr/local/bin/nanokvm-rdp-gateway

EXPOSE 3389/tcp 3390/tcp 5004/udp

ENTRYPOINT ["/usr/local/bin/nanokvm-rdp-gateway"]
CMD ["-listen", "0.0.0.0:3389", "-cert", "/run/tls/tls.crt", "-key", "/run/tls/tls.key"]
