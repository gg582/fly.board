FROM archlinux:base-devel

RUN pacman -Syu --noconfirm \
    && pacman -S --noconfirm \
        git cmake ninja gcc make pkgconf \
        openssl libnghttp3 libngtcp2 openssl-quic \
        sqlite cjson uriparser \
        curl wget \
    && pacman -Scc --noconfirm

# Build and install cwist
COPY cwist_http2.patch /tmp/cwist_http2.patch
RUN git clone --depth 1 --recursive https://github.com/religiya-serdtsa/cwist.git /tmp/cwist \
    && cd /tmp/cwist \
    && patch -p0 < /tmp/cwist_http2.patch \
    && make -j$(nproc) \
    && make install \
    && rm -rf /tmp/cwist

WORKDIR /app
COPY . .

# Build md4c and fly_board
RUN make deps \
    && make

EXPOSE 8443/tcp
EXPOSE 8443/udp

CMD ["./fly_board"]
