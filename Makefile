CC ?= cc
PKGS = ncursesw json-c libqrencode
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS))
PREFIX ?= $(HOME)/.local
APP_DIR = $(PREFIX)/lib/whatsapp-tui
BIN_DIR = $(PREFIX)/bin

.PHONY: all deps install install-app install-plugin clean
all: build/whatsapp-tui
build/whatsapp-tui: src/main.c
	mkdir -p build
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

deps:
	cd bridge && npm ci --no-audit --no-fund

install: install-app

install-app: all
	command -v omarchy >/dev/null || { echo 'Install Omarchy to create its terminal launcher.' >&2; exit 1; }
	command -v node >/dev/null || { echo 'Node.js 20 or newer is required.' >&2; exit 1; }
	command -v magick >/dev/null || { echo 'ImageMagick is required for image features.' >&2; exit 1; }
	command -v wl-paste >/dev/null || { echo 'wl-clipboard is required for image pasting.' >&2; exit 1; }
	install -d $(APP_DIR)/build $(APP_DIR)/bridge $(BIN_DIR)
	install -m 0755 build/whatsapp-tui $(APP_DIR)/build/whatsapp-tui
	cp bridge/*.mjs bridge/package.json bridge/package-lock.json $(APP_DIR)/bridge/
	cd $(APP_DIR)/bridge && npm ci --omit=dev --no-audit --no-fund --ignore-scripts
	install -m 0755 scripts/whatsapp-tui-launch $(BIN_DIR)/whatsapp-tui-launch
	ln -sfn $(APP_DIR)/build/whatsapp-tui $(BIN_DIR)/whatsapp-tui
	omarchy tui install 'WhatsApp TUI' '$(BIN_DIR)/whatsapp-tui-launch' float whatsapp

install-plugin:
	command -v omarchy >/dev/null || { echo 'Omarchy is required to install the bar button.' >&2; exit 1; }
	REMOTE="$$(git config --get remote.origin.url)"; test -n "$$REMOTE" || { echo 'Set the published Git remote first.' >&2; exit 1; }; omarchy plugin add "$$REMOTE" --enable

clean:
	$(RM) build/whatsapp-tui
