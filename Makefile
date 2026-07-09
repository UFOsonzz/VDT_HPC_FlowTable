CC ?= gcc
PKG_CONFIG ?= pkg-config

DPDK_CFLAGS := $(shell $(PKG_CONFIG) --cflags libdpdk)
DPDK_LIBS := $(shell $(PKG_CONFIG) --libs libdpdk)

CPPFLAGS += -Iinclude $(DPDK_CFLAGS)
CFLAGS += -O2 -g -std=gnu11 -Wall -Wextra -Wpedantic
LDLIBS += $(DPDK_LIBS)

BUILD_DIR := build
TARGET := $(BUILD_DIR)/rx_demo

SOURCES := \
	src/main.c \
	src/port.c \
	src/rx_loop.c \
	src/packet.c \
	src/config.c \
	src/flow.c \
	src/rule.c

.PHONY: all clean

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(SOURCES) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDLIBS) -o $@

clean:
	rm -rf $(BUILD_DIR)
