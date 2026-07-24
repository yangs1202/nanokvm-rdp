FROM debian:bookworm AS builder

ARG FREERDP_VERSION=3.14.0

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        git \
        libavcodec-dev \
        libavutil-dev \
        libssl-dev \
        pkg-config \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

RUN git clone --branch "${FREERDP_VERSION}" --depth 1 https://github.com/FreeRDP/FreeRDP.git freerdp

RUN cmake -S freerdp -B build/freerdp \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/freerdp \
        -DBUILD_SHARED_LIBS=OFF \
        -DWITH_SERVER=ON \
        -DWITH_CLIENT=OFF \
        -DWITH_SAMPLE=OFF \
        -DWITH_SHADOW=OFF \
        -DWITH_PROXY=OFF \
        -DWITH_PLATFORM_SERVER=OFF \
        -DWITH_X11=OFF \
        -DWITH_WAYLAND=OFF \
        -DWITH_CAIRO=OFF \
        -DWITH_FFMPEG=OFF \
        -DWITH_JPEG=OFF \
        -DWITH_PNG=OFF \
        -DWITH_ZLIB=OFF \
        -DWITH_CUPS=OFF \
        -DWITH_PCSC=OFF \
        -DWITH_LIBUSB=OFF \
        -DCHANNEL_URBDRC=OFF \
        -DWITH_MANPAGES=OFF \
        -DWITH_SMARTCARD_EMULATE=OFF \
        -DWITH_KRB5=OFF \
        -DWITH_ALSA=OFF \
        -DWITH_PULSE=OFF \
        -DWITH_UNICODE_BUILTIN=ON \
        -DWITH_JSON_DISABLED=ON \
        -DWITH_URIPARSER=OFF \
        -DWITH_FUSE=OFF \
        -DWITH_OPUS=OFF \
        -DWITH_AAD=OFF \
        -DUSE_UNWIND=OFF \
        -DWITH_DSP_FFMPEG=OFF \
        -DWITH_VIDEO_FFMPEG=OFF \
        -DWITH_SWSCALE=OFF \
        -DWITH_GFX_H264=ON \
        -DWITH_OPENSSL=ON \
    && cmake --build build/freerdp --parallel \
    && cmake --install build/freerdp

WORKDIR /src/nanokvm-rdp

COPY . .

RUN cmake -S . -B build/gateway \
        -DCMAKE_PREFIX_PATH=/opt/freerdp \
        -DNANOKVM_RDP_BUILD_AGENT=OFF \
        -DNANOKVM_RDP_BUILD_TESTS=OFF \
        -DNANOKVM_RDP_USE_INSTALLED_FREERDP=ON \
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
