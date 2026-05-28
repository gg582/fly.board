FROM archlinux:base-devel

RUN pacman -Syu --noconfirm \
    && pacman -S --noconfirm \
        git cmake ninja gcc make pkgconf \
        openssl libnghttp3 libngtcp2 zlib \
        sqlite cjson uriparser \
        curl wget \
    && pacman -Scc --noconfirm

# Build and install cwist
COPY cwist_http2.patch /tmp/cwist_http2.patch
RUN git clone --depth 1 --recursive https://github.com/religiya-serdtsa/cwist.git /tmp/cwist \
    && cd /tmp/cwist \
    && if [ -s /tmp/cwist_http2.patch ]; then patch -p0 < /tmp/cwist_http2.patch; fi \
    && sed -i 's|FIND_PATH(ZLIB_INCLUDE_DIR NAMES zlib.h)|SET(ZLIB_INCLUDE_DIR /usr/include)|' lib/lsquic/CMakeLists.txt \
    && sed -i 's|FIND_LIBRARY(ZLIB_LIB libz\${LIB_SUFFIX})|SET(ZLIB_LIB /usr/lib/libz.so)|' lib/lsquic/CMakeLists.txt \
    && make -j$(nproc) \
    && make install \
    && rm -rf /tmp/cwist

WORKDIR /app
COPY . .

# Build md4c and fly_board
RUN make deps \
    && make

RUN chmod +x /app/entrypoint.sh

EXPOSE 8443/tcp
EXPOSE 8443/udp

CMD ["/app/entrypoint.sh"]
