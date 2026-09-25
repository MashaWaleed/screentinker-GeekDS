CC ?= gcc
PKGS = libavformat libavcodec libavutil libswscale libswresample libdrm libwebsockets libcurl libcjson alsa
CFLAGS ?= -O3 -Wall -Wextra -Iinclude -DWITH_ALSA $(shell pkg-config --cflags $(PKGS))
LDFLAGS ?= $(shell pkg-config --libs $(PKGS)) -lpthread -lm

SRC = $(wildcard src/*.c)
OBJ = $(SRC:src/%.c=build/%.o)
BIN = stlinux-player

all: $(BIN)

build/%.o: src/%.c include/*.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

clean:
	rm -rf build $(BIN)

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)/usr/bin/$(BIN)
	install -Dm644 systemd/stlinux-player.service $(DESTDIR)/etc/systemd/system/stlinux-player.service
	mkdir -p $(DESTDIR)/etc/stlinux-player $(DESTDIR)/var/cache/stlinux-player

.PHONY: all clean install
