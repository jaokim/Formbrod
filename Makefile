# Formbrod - AmigaOS 4 build.
#
# Cross-compile (e.g. with the walkero/amigagccondocker image):
#   make
# Native on AmigaOS 4 with the SDK installed:
#   make CC=gcc
#
# libav* (ffmpeg) is linked in when LIBAV=1 and LIBAV_PREFIX points to a
# PPC build of ffmpeg's libraries:
#   make LIBAV=1 LIBAV_PREFIX=/opt/ffmpeg-ppc
#
# The portable core can be tested on any host: make -f Makefile.host test

CC        ?= ppc-amigaos-gcc
LIBAV     ?= 0
LIBAV_PREFIX ?= /usr/local/amiga/ffmpeg

TARGET    = Formbrod
BUILD     = build/amiga

CFLAGS    = -std=gnu99 -O2 -Wall -Wextra -Wno-unused-parameter \
            -Iinclude -Isrc
LDFLAGS   =
LIBS      = -lauto

ifeq ($(LIBAV),1)
CFLAGS   += -DHAVE_LIBAV -I$(LIBAV_PREFIX)/include
LDFLAGS  += -L$(LIBAV_PREFIX)/lib
LIBS     := -lavfilter -lavformat -lavcodec -lswscale -lswresample \
            -lavutil -lz -lpthread -lm $(LIBS)
endif

SRC = src/main.c \
      src/app.c \
      src/gui/mainwindow.c \
      src/arexx/arexx.c \
      src/timeline/timeline.c \
      src/job/job.c \
      src/job/module.c \
      src/backend/local_backend.c \
      src/backend/media.c \
      modules/fade/fade.c

OBJ = $(SRC:%.c=$(BUILD)/%.o)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD) $(TARGET)

test:
	$(MAKE) -f Makefile.host test

.PHONY: clean test
