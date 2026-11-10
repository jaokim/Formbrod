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
#
# Translations:
#   make catalogs    builds Catalogs/<language>/formbrod.catalog from locale/*.ct
#                    with FlexCat (CATTOOL=catgen uses tools/catgen.py instead)
#   make compare-catalogs
#                    builds them with both tools and checks they agree
#   make strings     regenerates src/locale/strings.h after editing
#                    locale/formbrod.cd (needs Python 3)

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
      src/locale/catalog.c \
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

# ---- Locale -------------------------------------------------------------
# locale/*.cd and *.ct are UTF-8. FlexCat copies bytes as they are, so they
# are converted to ISO-8859-1 (what AmigaOS expects) with iconv first.
# catgen does that conversion itself.
PYTHON    ?= python3
FLEXCAT   ?= flexcat
ICONV     ?= iconv
CATTOOL   ?= flexcat
CATGEN     = $(PYTHON) tools/catgen.py
CD         = locale/formbrod.cd
CT         = $(wildcard locale/*.ct)
LANGUAGES  = $(CT:locale/%.ct=%)
CATALOGS   = $(LANGUAGES:%=Catalogs/%/formbrod.catalog)
LOCBUILD   = build/locale

strings: $(CD) tools/catgen.py
	$(CATGEN) header $(CD) -o src/locale/strings.h

catalogs: $(CATALOGS)

ifeq ($(CATTOOL),catgen)
Catalogs/%/formbrod.catalog: locale/%.ct $(CD) tools/catgen.py
	@mkdir -p $(dir $@)
	$(CATGEN) catalog $(CD) $< -o $@
else
Catalogs/%/formbrod.catalog: locale/%.ct $(CD)
	@mkdir -p $(dir $@) $(LOCBUILD)/latin1
	$(ICONV) -f UTF-8 -t ISO-8859-1 $(CD) > $(LOCBUILD)/latin1/formbrod.cd
	$(ICONV) -f UTF-8 -t ISO-8859-1 $< > $(LOCBUILD)/latin1/$*.ct
	$(FLEXCAT) $(LOCBUILD)/latin1/formbrod.cd $(LOCBUILD)/latin1/$*.ct \
	    CATALOG=$@
endif

# Builds every catalog with catgen too and checks both tools agree.
compare-catalogs: $(CATALOGS)
	@for l in $(LANGUAGES); do \
	    mkdir -p $(LOCBUILD)/catgen/$$l && \
	    $(CATGEN) catalog $(CD) locale/$$l.ct \
	        -o $(LOCBUILD)/catgen/$$l/formbrod.catalog && \
	    $(CATGEN) compare Catalogs/$$l/formbrod.catalog \
	        $(LOCBUILD)/catgen/$$l/formbrod.catalog || exit 1; \
	done

clean:
	rm -rf $(BUILD) $(TARGET) Catalogs

test:
	$(MAKE) -f Makefile.host test

.PHONY: clean test strings catalogs compare-catalogs
