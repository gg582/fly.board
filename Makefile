CC ?= gcc

CWIST_PREFIX ?= /usr/local

MD4C_DIR := third_party/md4c
MD4C_LIB := $(MD4C_DIR)/build/libmd4c_example.a
MD4C_OBJS := $(MD4C_DIR)/build/md4c.o $(MD4C_DIR)/build/md4c-html.o $(MD4C_DIR)/build/entity.o

MULTIPART_DIR := third_party/multipart-parser-c

SRCS := src/main.c \
        src/db/db.c src/db/user.c src/db/board.c src/db/post.c src/db/file.c src/db/comment.c src/db/vote.c src/db/tag.c src/db/sql_escape.c \
        src/auth/auth.c \
        src/crypto/fly_crypto.c \
        src/render/theme/theme.c src/render/theme/rules.c src/render/theme/json.c src/render/theme/css.c \
        src/render/render_common.c src/render/render_page.c src/render/render_md.c src/render/render_auth.c src/render/render_profile.c src/render/render_post.c src/render/render_board.c src/render/render_admin.c src/render/render_file.c \
        src/handlers/handlers.c src/handlers/home.c src/handlers/auth.c src/handlers/board.c src/handlers/post.c src/handlers/comment.c src/handlers/file.c src/handlers/admin.c src/handlers/api.c \
        src/utils/utils.c \
        src/utils/image_contrast.c \
        src/utils/image_size.c \
        src/nats/fly_nats.c \
        src/core/log.c \
        src/config/config.c \
        $(MULTIPART_DIR)/multipart_parser.c
OBJS := $(SRCS:.c=.o)

CFLAGS := -Wall -Wextra -Ofast \
          -I$(CWIST_PREFIX)/include \
          -I$(MD4C_DIR)/src \
          -I$(MULTIPART_DIR) \
          -Isrc \
          -Iinclude \
          -Ithird_party/stb \
          -Ithird_party/file/src

ifeq ($(DEBUG),1)
CFLAGS += -DDEBUG=1
endif

LDFLAGS := -L$(CWIST_PREFIX)/lib \
           -Wl,-rpath,$(CWIST_PREFIX)/lib

CWIST_ROOT ?= /home/yjlee/cwist

CWIST_LIB := $(CWIST_PREFIX)/lib/libcwist.a
ifeq ($(wildcard $(CWIST_LIB)),)
  CWIST_LIB := $(CWIST_PREFIX)/libcwist.a
endif

LIBS := -lcwist -lssl -lcrypto -lpthread -ldl -lstdc++ -lz -lm /usr/lib/x86_64-linux-gnu/libmagic.so.1
HAS_NGHTTP2 := $(shell pkg-config --exists libnghttp2 2>/dev/null && echo 1 || echo 0)
HAS_NGTCP2 := $(shell pkg-config --exists libngtcp2 2>/dev/null && echo 1 || echo 0)
HAS_NGHTTP3 := $(shell pkg-config --exists libnghttp3 2>/dev/null && echo 1 || echo 0)

ifeq ($(HAS_NGHTTP2),1)
LIBS += -lnghttp2
endif
ifeq ($(HAS_NGTCP2),1)
HAS_NGTCP2_CRYPTO_OSSL := $(shell pkg-config --exists libngtcp2_crypto_ossl 2>/dev/null && echo 1 || echo 0)
LIBS += -lngtcp2
ifeq ($(HAS_NGTCP2_CRYPTO_OSSL),1)
LIBS += -lngtcp2_crypto_ossl
else
LIBS += -lngtcp2_crypto_quictls
endif
endif
ifeq ($(HAS_NGHTTP3),1)
LIBS += -lnghttp3
endif

TARGET := fly_board

.PHONY: all clean distclean deps

all: deps $(TARGET)

deps: $(MD4C_LIB)

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
	$(CC) $(CFLAGS) -D__has_include\(x\)=0 -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS) $(MD4C_LIB)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(MD4C_LIB) $(LDFLAGS) $(CWIST_LIB) $(LIBS)

clean:
	rm -f $(OBJS) $(TARGET)

distclean: clean
	rm -rf third_party
