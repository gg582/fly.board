FROM archlinux:base-devel

# Install build/runtime dependencies
RUN pacman -Syu --noconfirm \
    && pacman -S --needed --noconfirm \
        git cmake ninja gcc make pkgconf \
        openssl libnghttp3 libngtcp2 zlib \
        sqlite cjson uriparser \
        curl wget coreutils \
    && pacman -Scc --noconfirm

# Build and install cwist (with retry in case of transient network issues)
RUN set -eux; \
    for i in 1 2 3; do \
        git clone --depth 1 --recursive https://github.com/religiya-serdtsa/cwist.git /tmp/cwist && break; \
        echo "cwist clone attempt $i failed, retrying..."; \
        rm -rf /tmp/cwist; \
        sleep 5; \
    done; \
    cd /tmp/cwist; \
    sed -i 's|FIND_PATH(ZLIB_INCLUDE_DIR NAMES zlib.h)|SET(ZLIB_INCLUDE_DIR /usr/include)|' lib/lsquic/CMakeLists.txt; \
    sed -i 's|FIND_LIBRARY(ZLIB_LIB libz\${LIB_SUFFIX})|SET(ZLIB_LIB /usr/lib/libz.so)|' lib/lsquic/CMakeLists.txt; \
    make -j$(nproc); \
    make install; \
    rm -rf /tmp/cwist

WORKDIR /app
COPY . .

ENV EXTRA_CFLAGS="-I/app/third_party/libttak/include"

# Rebuild libttak inside the container so its LTO bytecode matches the image's toolchain
RUN if [ -f /app/third_party/libttak/Makefile ]; then make -C /app/third_party/libttak clean || true; fi \
    && rm -f /app/third_party/libttak/lib/libttak.a

# Build md4c and fly_board
RUN make deps \
    && make

RUN chmod +x /app/entrypoint.sh

EXPOSE 8888/tcp
EXPOSE 8888/udp

CMD ["/app/entrypoint.sh"]
