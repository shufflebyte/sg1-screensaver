# Ägyptische Tänzer – Linux
#
#   make                 -> build/egypt-dancers
#   make install         -> Programm nach ~/.local/bin, Eintrag im App-Menü
#   make uninstall
#   make snapshot        -> build/snapshot.ppm (2560x1440) und Zeitmessung
#   make test-core       -> Kern ohne GPU prüfen (build/core.ppm)

PREFIX  ?= $(HOME)/.local
BINDIR  ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
APPDIR  := $(DATADIR)/applications
ICONDIR := $(DATADIR)/icons/hicolor/256x256/apps
CORE    := ../core
BUILD   := build

CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Wall -Wextra -I$(CORE)
LIBS    := $(shell pkg-config --libs x11 gl) -lm

all: $(BUILD)/egypt-dancers

$(BUILD)/egypt-dancers: egypt_dancers.c $(CORE)/egypt_core.c $(CORE)/egypt_core.h | $(BUILD)
	$(CC) $(CFLAGS) egypt_dancers.c $(CORE)/egypt_core.c -o $@ $(LIBS)

$(BUILD)/egypt-dancers.desktop: egypt-dancers.desktop.in | $(BUILD)
	sed 's|@BINDIR@|$(BINDIR)|g' $< > $@

$(BUILD):
	mkdir -p $@

install: $(BUILD)/egypt-dancers $(BUILD)/egypt-dancers.desktop
	install -Dm755 $(BUILD)/egypt-dancers $(BINDIR)/egypt-dancers
	install -Dm644 egypt-dancers.png $(ICONDIR)/egypt-dancers.png
	install -Dm644 $(BUILD)/egypt-dancers.desktop $(APPDIR)/egypt-dancers.desktop
	-gtk-update-icon-cache -q -t $(DATADIR)/icons/hicolor 2>/dev/null
	-update-desktop-database -q $(APPDIR) 2>/dev/null
	@echo "Installiert: „Ägyptische Tänzer“ im App-Menü"

uninstall:
	rm -f $(BINDIR)/egypt-dancers $(ICONDIR)/egypt-dancers.png $(APPDIR)/egypt-dancers.desktop
	-update-desktop-database -q $(APPDIR) 2>/dev/null

snapshot: $(BUILD)/egypt-dancers
	$(BUILD)/egypt-dancers --snapshot $(BUILD)/snapshot.ppm --size 2560x1440 --time 3

test-core: | $(BUILD)
	$(CC) $(CFLAGS) $(CORE)/egypt_core.c $(CORE)/test_render.c -lm -o $(BUILD)/test_render
	$(BUILD)/test_render $(BUILD)/core.ppm 1600 1000 3

clean:
	rm -rf $(BUILD)

.PHONY: all install uninstall snapshot test-core clean
