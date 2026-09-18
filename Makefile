CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
DESTDIR ?=

CPPFLAGS += -D_GNU_SOURCE -Isrc
CFLAGS ?= -O2 -pipe
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic
GTK_CFLAGS := $(shell $(PKG_CONFIG) --cflags gtk4 2>/dev/null)
GTK_LIBS := $(shell $(PKG_CONFIG) --libs gtk4 2>/dev/null)

BUILD_DIR := build
GUI := $(BUILD_DIR)/cpu-switch-control
DAEMON := $(BUILD_DIR)/cpu-clock-switch-daemon
TEST := $(BUILD_DIR)/test-config

.PHONY: all clean test install uninstall enable

all: check-gtk $(GUI) $(DAEMON)

check-gtk:
	@$(PKG_CONFIG) --exists gtk4 || { echo "Erro: GTK4 de desenvolvimento não encontrado (pkg-config gtk4)." >&2; exit 1; }

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(GUI): src/main.c src/config.c src/config.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GTK_CFLAGS) src/main.c src/config.c -o $@ $(GTK_LIBS)

$(DAEMON): src/daemon.c src/config.c src/config.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) src/daemon.c src/config.c -o $@

$(TEST): tests/test_config.c src/config.c src/config.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_config.c src/config.c -o $@

test: $(TEST)
	./$(TEST)

install: all
	install -Dm755 $(GUI) $(DESTDIR)$(PREFIX)/bin/cpu-switch-control
	install -Dm755 $(DAEMON) $(DESTDIR)$(PREFIX)/bin/cpu-clock-switch-daemon
	install -Dm644 service/cpu-clock-switch.service $(DESTDIR)/etc/systemd/system/cpu-clock-switch.service
	install -Dm644 assets/cpu-switch-control.svg $(DESTDIR)/usr/share/icons/hicolor/scalable/apps/cpu-switch-control.svg
	install -Dm644 packaging/cpu-switch-control.desktop $(DESTDIR)/usr/share/applications/cpu-switch-control.desktop
	@if [ ! -e "$(DESTDIR)/etc/cpu-clock-switch.json" ]; then \
		install -Dm644 config/cpu-clock-switch.json $(DESTDIR)/etc/cpu-clock-switch.json; \
	fi

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/cpu-switch-control
	rm -f $(DESTDIR)$(PREFIX)/bin/cpu-clock-switch-daemon
	rm -f $(DESTDIR)/etc/systemd/system/cpu-clock-switch.service
	rm -f $(DESTDIR)/usr/share/icons/hicolor/scalable/apps/cpu-switch-control.svg
	rm -f $(DESTDIR)/usr/share/applications/cpu-switch-control.desktop

enable:
	systemctl daemon-reload
	systemctl enable --now cpu-clock-switch.service

clean:
	rm -rf $(BUILD_DIR)
