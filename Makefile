# Compiler and Flags
CC ?= gcc

# Detect OS
UNAME_S := $(shell uname -s)

# CWIST Paths (Source root or Installed prefix)
CWIST_ROOT ?= /home/yjlee/cwist
CWIST_PREFIX ?= /usr/local

ifeq ($(wildcard $(CWIST_ROOT)/libcwist.a),)
    CWIST_LIB = $(CWIST_PREFIX)/lib/libcwist.a
    CWIST_DEPS_DIR = $(CWIST_PREFIX)/lib/cwist
    CWIST_DEPS = $(CWIST_DEPS_DIR)/libnats_static.a \
                 $(CWIST_DEPS_DIR)/libttak.a \
                 $(CWIST_DEPS_DIR)/libcjson.a \
                 $(CWIST_DEPS_DIR)/liburiparser.a \
                 $(CWIST_DEPS_DIR)/liblsquic.a \
                 $(CWIST_DEPS_DIR)/libssl.a \
                 $(CWIST_DEPS_DIR)/libcrypto.a
    CWIST_INCLUDES = -I$(CWIST_PREFIX)/include \
                     -I$(CWIST_PREFIX)/include/cwist \
                     -I$(CWIST_PREFIX)/include/cwist/vendor \
                     -I$(CWIST_PREFIX)/include/cwist/vendor/cjson \
                     -I$(CWIST_PREFIX)/include/cwist/vendor/lsquic
else
    CWIST_LIB = $(CWIST_ROOT)/libcwist.a
    CWIST_DEPS = $(CWIST_ROOT)/lib/cnats/build/lib/libnats_static.a \
                 $(CWIST_ROOT)/lib/libttak/lib/libttak.a \
                 $(CWIST_ROOT)/lib/cjson/libcjson.a \
                 $(CWIST_ROOT)/lib/uriparser/build/liburiparser.a \
                 $(CWIST_ROOT)/lib/lsquic/build/src/liblsquic/liblsquic.a \
                 $(CWIST_ROOT)/lib/boringssl/build/libssl.a \
                 $(CWIST_ROOT)/lib/boringssl/build/libcrypto.a
    CWIST_INCLUDES = -I$(CWIST_ROOT)/include \
                     -I$(CWIST_ROOT)/lib \
                     -I$(CWIST_ROOT)/lib/boringssl/include \
                     -I$(CWIST_ROOT)/lib/cjson \
                     -I$(CWIST_ROOT)/lib/libttak/include \
                     -I$(CWIST_ROOT)/lib/uriparser/include \
                     -I$(CWIST_ROOT)/lib/cnats/src \
                     -I$(CWIST_ROOT)/lib/lsquic/include \
                     -I$(CWIST_ROOT)/lib/sqlite3 \
                     -I$(CWIST_ROOT)/lib/multipart-parser-c
endif

# Third Party Dependencies
MD4C_DIR := third_party/md4c
MD4C_LIB := $(MD4C_DIR)/build/libmd4c_example.a
MD4C_OBJS := $(MD4C_DIR)/build/md4c.o $(MD4C_DIR)/build/md4c-html.o $(MD4C_DIR)/build/entity.o

MULTIPART_DIR := third_party/multipart-parser-c
LIBMAGIC_DIR := third_party/file
LIBMAGIC_A := $(LIBMAGIC_DIR)/src/.libs/libmagic.a

# Pkg-config flags and libraries
CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
NGHTTP2_CFLAGS := $(shell pkg-config --cflags libnghttp2 2>/dev/null)
BROTLI_CFLAGS := $(shell pkg-config --cflags libbrotlienc libbrotlicommon libbrotlidec 2>/dev/null)
WEBP_CFLAGS := $(shell pkg-config --cflags libwebp libwebpmux 2>/dev/null)

CURL_LIBS := $(shell pkg-config --libs libcurl 2>/dev/null || echo -lcurl)
NGHTTP2_LIBS := $(shell pkg-config --libs libnghttp2 2>/dev/null)
BROTLI_LIBS := $(shell pkg-config --libs libbrotlienc libbrotlicommon libbrotlidec 2>/dev/null || echo -lbrotlienc -lbrotlicommon -lbrotlidec)
WEBP_LIBS := $(shell pkg-config --libs libwebp libwebpmux 2>/dev/null)
ifneq ($(strip $(WEBP_LIBS)),)
WEBP_CFLAGS += -DHAVE_WEBP
endif
ZSTD_LIBS := $(shell pkg-config --libs libzstd 2>/dev/null)
ifeq ($(strip $(ZSTD_LIBS)),)
ZSTD_LIBS = -lzstd
endif

# Common flags & defines matching cwist buildchain
COMMON_DEFINES = -D_GNU_SOURCE -D_XOPEN_SOURCE=700 -D_REENTRANT -DSQLITE_ENABLE_DESERIALIZE
COMMON_WARNINGS = -Wall -Wextra -pthread -fPIC
# Match cwist's GCC_STACK_FLAGS (-Ofast -g).  The FP audit found no NaN/Inf
# or signed-zero dependencies (media_preview guards src_h > 0, image_contrast
# uses only finite pow()/threshold heuristics), so -ffast-math is safe here.
OPT_FLAGS = -Ofast -g -march=native -mtune=native -fno-plt -fomit-frame-pointer

CFLAGS := $(OPT_FLAGS) $(COMMON_WARNINGS) $(COMMON_DEFINES) \
          -Iinclude \
          -Isrc \
          $(CWIST_INCLUDES) \
          -I$(MD4C_DIR)/src \
          -I$(MULTIPART_DIR) \
          -Ithird_party/stb \
          -Ithird_party/file/src \
          $(CURL_CFLAGS) $(NGHTTP2_CFLAGS) $(BROTLI_CFLAGS) $(WEBP_CFLAGS)

ifeq ($(UNAME_S),Linux)
    CFLAGS += -DCWIST_OS_LINUX
endif
ifeq ($(UNAME_S),Darwin)
    CFLAGS += -DCWIST_OS_BSD -D_DARWIN_C_SOURCE
endif
ifeq ($(UNAME_S),FreeBSD)
    CFLAGS += -DCWIST_OS_BSD -D_DEFAULT_SOURCE
endif

EXTRA_CFLAGS ?= -D_GNU_SOURCE
CFLAGS += $(EXTRA_CFLAGS)

ifeq ($(DEBUG),1)
CFLAGS += -DDEBUG=1 -g
endif

LDFLAGS := -L$(CWIST_PREFIX)/lib \
           -Wl,-rpath,$(CWIST_PREFIX)/lib \
           -Wl,--wrap=cwist_http_send_response \
           -Wl,--wrap=cwist_https_send_response

LIBS := $(CWIST_LIB) \
        $(CWIST_DEPS) \
        $(MD4C_LIB) \
        $(LIBMAGIC_A) \
        $(CURL_LIBS) \
        $(NGHTTP2_LIBS) \
        $(BROTLI_LIBS) \
        $(WEBP_LIBS) \
        $(ZSTD_LIBS) \
        -pthread -ldl -lm -lstdc++ -lz

SRCS := src/main.c \
        src/db/db.c src/db/user.c src/db/board.c src/db/board_tree.c src/db/post.c src/db/file.c src/db/comment.c src/db/notification.c src/db/vote.c src/db/tag.c src/db/sql_escape.c src/db/orm.c \
        src/auth/auth.c \
        src/crypto/fly_crypto.c \
        src/render/theme/theme.c src/render/theme/rules.c src/render/theme/json.c src/render/theme/css.c \
        src/render/render_common.c src/render/render_page.c src/render/render_md.c src/render/render_auth.c src/render/render_profile.c src/render/render_post.c src/render/render_board.c src/render/render_admin.c src/render/render_file.c src/render/render_notifications.c \
        src/handlers/handlers.c src/handlers/home.c src/handlers/auth.c src/handlers/board.c src/handlers/post.c src/handlers/comment.c src/handlers/notifications.c src/handlers/file.c src/handlers/tasfa/common.c src/handlers/tasfa/crypto.c src/handlers/tasfa/queue.c src/handlers/tasfa/cache.c src/handlers/tasfa/session.c src/handlers/tasfa/scheduler.c src/handlers/tasfa/htp.c src/handlers/tasfa/upload.c src/handlers/tasfa/download.c src/handlers/tasfa/asset.c src/handlers/admin.c src/handlers/api.c \
        src/utils/utils.c \
        src/utils/cache.c \
        src/utils/reqshare.c \
        src/utils/tcp_cork_wrap.c \
        src/utils/legal.c \
        src/utils/stb_image_impl.c \
        src/utils/image_contrast.c \
        src/utils/image_size.c \
        src/utils/image_inline.c \
        src/utils/image_invert.c \
        src/utils/media_preview.c \
        src/utils/cert_renewal.c \
        src/utils/email.c \
        src/utils/s3_client.c \
        src/nats/fly_nats.c \
        src/core/log.c \
        src/config/config.c \
        src/engine/pool.c \
        src/engine/nats.c \
        src/engine/db.c \
        src/engine/settings.c \
        src/engine/routes.c \
        src/engine/warmup.c

OBJS := $(SRCS:.c=.o)

TARGET := fly_board

.PHONY: all clean distclean deps prepare_assets check-render

all: deps $(TARGET)

deps: $(MD4C_LIB) $(LIBMAGIC_A) prepare_assets

prepare_assets: tools/prepare_assets.py
	python3 tools/prepare_assets.py

$(LIBMAGIC_A):
	cd $(LIBMAGIC_DIR) && if [ ! -f configure ]; then autoreconf -fi; fi
	cd $(LIBMAGIC_DIR) && ./configure --disable-shared --enable-static --disable-zstdlib --disable-xzlib --disable-bzlib --quiet
	$(MAKE) -C $(LIBMAGIC_DIR) clean
	$(MAKE) -C $(LIBMAGIC_DIR) -j$(shell nproc 2>/dev/null || echo 1)

$(MD4C_DIR):
	git clone --depth 1 https://github.com/mity/md4c.git $(MD4C_DIR)

$(MD4C_DIR)/build:
	mkdir -p $(MD4C_DIR)/build

$(MD4C_DIR)/build/md4c.o: $(MD4C_DIR) $(MD4C_DIR)/build
	$(CC) $(CFLAGS) -c $(MD4C_DIR)/src/md4c.c -o $@

$(MD4C_DIR)/build/md4c-html.o: $(MD4C_DIR) $(MD4C_DIR)/build
	$(CC) $(CFLAGS) -c $(MD4C_DIR)/src/md4c-html.c -o $@

$(MD4C_DIR)/build/entity.o: $(MD4C_DIR) $(MD4C_DIR)/build
	$(CC) $(CFLAGS) -c $(MD4C_DIR)/src/entity.c -o $@

$(MD4C_LIB): $(MD4C_OBJS)
	ar rcs $@ $(MD4C_OBJS)

src/crypto/fly_crypto.o: src/crypto/fly_crypto.c
	$(CC) $(CFLAGS) -DFLY_NO_PQC -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Auto-generated header dependencies: struct layout changes (e.g. config.h)
# must rebuild every consumer, not just the files edited in the same commit.
-include $(OBJS:.o=.d)

$(TARGET): $(OBJS) $(MD4C_LIB) $(LIBMAGIC_A)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LIBS)

setup:
	mkdir -p data

# Markdown renderer regression tests.
RENDER_TEST_SRCS := src/render/render_md.c src/utils/image_size.c src/utils/stb_image_impl.c
RENDER_TESTS := tests/test_render_md_video tests/test_render_md_blocks tests/test_render_md_iframe tests/test_render_md_tikz

tests/test_render_%: tests/test_render_%.c $(RENDER_TEST_SRCS) $(MD4C_OBJS)
	$(CC) $(CFLAGS) -Ithird_party/stb -o $@ $^ $(LDFLAGS) $(LIBS)

tests/render_file: tests/render_file.c $(RENDER_TEST_SRCS) $(MD4C_OBJS)
	$(CC) $(CFLAGS) -Ithird_party/stb -o $@ $^ $(LDFLAGS) $(LIBS)

check-render: $(RENDER_TESTS)
	@for t in $(RENDER_TESTS); do $$t || exit 1; done

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(TARGET) $(RENDER_TESTS) tests/render_file

distclean: clean
	-$(MAKE) -C $(LIBMAGIC_DIR) distclean 2>/dev/null || true
	rm -rf third_party/md4c/build $(MD4C_LIB)
