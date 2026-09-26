FROM debian:bookworm AS builder

ARG FREERDP_VERSION=3.14.0

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        curl \
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
        -DBUILD_SHARED_LIBS=ON \
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

ARG GO_VERSION=1.26.4
RUN curl -fsSL https://go.dev/dl/go${GO_VERSION}.linux-amd64.tar.gz | tar -C /usr/local -xz
ENV PATH=/usr/local/go/bin:${PATH}
ENV PKG_CONFIG_PATH=/opt/freerdp/lib/pkgconfig
ENV LD_LIBRARY_PATH=/opt/freerdp/lib
RUN CGO_ENABLED=1 go build -o /usr/local/bin/nanokvm-rdp-gateway ./go/cmd/nanokvm-rdp-gateway

FROM debian:bookworm-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        ffmpeg \
        libssl3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /usr/local/bin/nanokvm-rdp-gateway /usr/local/bin/nanokvm-rdp-gateway
COPY --from=builder /opt/freerdp/lib/libfreerdp*.so* /opt/freerdp/lib/libwinpr*.so* /usr/local/lib/
ENV LD_LIBRARY_PATH=/usr/local/lib

EXPOSE 3389/tcp 3390/tcp 5004/udp

ENTRYPOINT ["/usr/local/bin/nanokvm-rdp-gateway"]
CMD ["-listen", "0.0.0.0:3389", "-cert", "/run/tls/tls.crt", "-key", "/run/tls/tls.key"]
