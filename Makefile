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
TEST_CONFIG := $(BUILD_DIR)/test-config
TEST_UNDERVOLT := $(BUILD_DIR)/test-undervolt

GUI_SOURCES := src/main.c src/config.c src/cpu_linux.c src/command.c src/undervolt.c
DAEMON_SOURCES := src/daemon.c src/config.c src/cpu_linux.c

.PHONY: all clean test install uninstall enable check-gtk

all: check-gtk $(GUI) $(DAEMON)

check-gtk:
	@$(PKG_CONFIG) --exists gtk4 || { 		echo "Erro: headers do GTK4 não encontrados (pkg-config gtk4)." >&2; 		exit 1; 	}

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(GUI): $(GUI_SOURCES) src/config.h src/cpu_linux.h src/command.h src/undervolt.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GTK_CFLAGS) $(GUI_SOURCES) -o $@ $(GTK_LIBS) -lm

$(DAEMON): $(DAEMON_SOURCES) src/config.h src/cpu_linux.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DAEMON_SOURCES) -o $@

$(TEST_CONFIG): tests/test_config.c src/config.c src/config.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_config.c src/config.c -o $@

$(TEST_UNDERVOLT): tests/test_undervolt.c src/undervolt.c src/undervolt.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_undervolt.c src/undervolt.c -o $@ -lm

test: $(TEST_CONFIG) $(TEST_UNDERVOLT)
	./$(TEST_CONFIG)
	./$(TEST_UNDERVOLT)

install: all
	install -Dm755 $(GUI) $(DESTDIR)$(PREFIX)/bin/cpu-switch-control
	install -Dm755 $(DAEMON) $(DESTDIR)$(PREFIX)/bin/cpu-clock-switch-daemon
	install -Dm644 service/cpu-clock-switch.service $(DESTDIR)/etc/systemd/system/cpu-clock-switch.service
	install -Dm644 assets/cpu-switch-control.svg $(DESTDIR)/usr/share/icons/hicolor/scalable/apps/cpu-switch-control.svg
	install -Dm644 packaging/cpu-switch-control.desktop $(DESTDIR)/usr/share/applications/cpu-switch-control.desktop
	@if [ ! -e "$(DESTDIR)/etc/cpu-clock-switch.json" ]; then 		install -Dm644 config/cpu-clock-switch.json $(DESTDIR)/etc/cpu-clock-switch.json; 	fi
	@if command -v update-desktop-database >/dev/null 2>&1; then 		update-desktop-database $(DESTDIR)/usr/share/applications || true; 	fi

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
