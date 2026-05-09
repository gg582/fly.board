CC ?= gcc

CWIST_PREFIX ?= /usr/local

MD4C_DIR := third_party/md4c
MD4C_LIB := $(MD4C_DIR)/build/libmd4c_example.a
MD4C_OBJS := $(MD4C_DIR)/build/md4c.o $(MD4C_DIR)/build/md4c-html.o $(MD4C_DIR)/build/entity.o

MULTIPART_DIR := third_party/multipart-parser-c

SRCS := src/main.c src/db/db.c src/auth/auth.c src/crypto/fly_crypto.c src/render/theme_json.c src/render/render_common.c src/render/render_page.c src/render/render_md.c src/render/render_auth.c src/render/render_profile.c src/render/render_post.c src/render/render_board.c src/render/render_admin.c src/render/render_file.c src/handlers/handlers.c src/utils/utils.c src/nats/fly_nats.c src/core/log.c src/config/config.c $(MULTIPART_DIR)/multipart_parser.c
OBJS := $(SRCS:.c=.o)

CFLAGS := -Wall -Wextra -O2 \
          -I$(CWIST_PREFIX)/include \
          -I$(MD4C_DIR)/src \
          -I$(MULTIPART_DIR) \
          -Isrc \
          -Iinclude

ifeq ($(DEBUG),1)
CFLAGS += -DDEBUG=1
endif

LDFLAGS := -L$(CWIST_PREFIX)/lib \
           -Wl,-rpath,$(CWIST_PREFIX)/lib

CWIST_LIB := $(CWIST_PREFIX)/lib/libcwist.a
ifeq ($(wildcard $(CWIST_LIB)),)
  CWIST_LIB := $(CWIST_PREFIX)/libcwist.a
endif

# Only system libs remain; libttak, cjson, uriparser, sqlite3, cnats are all embedded in libcwist.a
LIBS := -lssl -lcrypto -lpthread -ldl
HAS_NGTCP2 := $(shell pkg-config --exists ngtcp2 2>/dev/null && echo 1 || echo 0)
HAS_NGHTTP3 := $(shell pkg-config --exists nghttp3 2>/dev/null && echo 1 || echo 0)
ifeq ($(HAS_NGTCP2),1)
LIBS += -lngtcp2 -lngtcp2_crypto_ossl
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

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS) $(MD4C_LIB)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(MD4C_LIB) $(CWIST_LIB) $(LDFLAGS) $(LIBS)

clean:
	rm -f $(OBJS) $(TARGET)

distclean: clean
	rm -rf third_party
