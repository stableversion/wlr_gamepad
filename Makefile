WAYLAND_SCANNER ?= wayland-scanner
WAYLAND_PROTOCOLS_DATADIR := $(shell pkg-config --variable=pkgdatadir wayland-protocols)

# wlr-layer-shell protocol
XML = protocol/wlr-layer-shell-unstable-v1.xml
PROTO_H = protocol/wlr-layer-shell-unstable-v1-client-protocol.h
PROTO_C = protocol/wlr-layer-shell-unstable-v1-client-protocol.c

# stable xdg-shell protocol provided by wayland-protocols
XDG_XML = $(WAYLAND_PROTOCOLS_DATADIR)/stable/xdg-shell/xdg-shell.xml
XDG_PROTO_H = protocol/xdg-shell-client-protocol.h
XDG_PROTO_C = protocol/xdg-shell-client-protocol.c

CC = gcc
CFLAGS +=  $(shell pkg-config --cflags wayland-client wayland-egl egl glesv2)
LDFLAGS += $(shell pkg-config --libs wayland-client wayland-egl egl glesv2)
BINARY = wlr_gamepad

all: $(BINARY)

$(PROTO_H): $(XML)
	$(WAYLAND_SCANNER) client-header $< $@

$(PROTO_C): $(XML)
	$(WAYLAND_SCANNER) private-code $< $@

$(XDG_PROTO_H): $(XDG_XML)
	$(WAYLAND_SCANNER) client-header $< $@

$(XDG_PROTO_C): $(XDG_XML)
	@rm -f $@
	$(WAYLAND_SCANNER) public-code  $< $@

# Build demo
$(BINARY): main.c $(PROTO_C) $(PROTO_H) $(XDG_PROTO_C) $(XDG_PROTO_H)
	$(CC) -o $@ main.c $(PROTO_C) $(XDG_PROTO_C) $(CFLAGS) $(LDFLAGS) -lGL -lm

.PHONY: all clean

clean:
	rm -f $(BINARY) $(PROTO_C) $(PROTO_H) $(XDG_PROTO_C) $(XDG_PROTO_H)
