FROM archlinux:base-devel

# Install build/runtime dependencies
RUN pacman -Syu --noconfirm \
    && pacman -S --needed --noconfirm \
        git cmake ninja gcc make pkgconf \
        openssl libnghttp3 libngtcp2 zlib \
        sqlite cjson uriparser \
        libwebp zstd brotli \
        curl wget coreutils \
    && pacman -Scc --noconfirm

# Build and install cwist (with retry in case of transient network issues).
# The seds are the patch set .github/workflows/memory-checks.yml applies:
# usrsctp ships -Werror, which newer GCCs break, and fly.board needs SQLite
# FTS5 (posts_fts), which cwist's bundled sqlite3 is built without.
RUN set -eux; \
    for i in 1 2 3; do \
        git clone --depth 1 --recursive --branch v4.1 https://github.com/religiya-serdtsa/cwist.git /tmp/cwist && break; \
        echo "cwist clone attempt $i failed, retrying..."; \
        rm -rf /tmp/cwist; \
        sleep 5; \
    done; \
    cd /tmp/cwist; \
    sed -i 's|FIND_PATH(ZLIB_INCLUDE_DIR NAMES zlib.h)|SET(ZLIB_INCLUDE_DIR /usr/include)|' lib/lsquic/CMakeLists.txt; \
    sed -i 's|FIND_LIBRARY(ZLIB_LIB libz\${LIB_SUFFIX})|SET(ZLIB_LIB /usr/lib/libz.so)|' lib/lsquic/CMakeLists.txt; \
    sed -i 's|option(sctp_werror "Treat warning as error" 1)|option(sctp_werror "Treat warning as error" 0)|' lib/usrsctp/CMakeLists.txt; \
    sed -i 's|-DSQLITE_ENABLE_DESERIALIZE|-DSQLITE_ENABLE_DESERIALIZE -DSQLITE_ENABLE_FTS5|' Makefile; \
    grep -q 'SQLITE_ENABLE_FTS5' Makefile; \
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
