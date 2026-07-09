CC ?= gcc
PKG_CONFIG ?= pkg-config

DPDK_CFLAGS := $(shell $(PKG_CONFIG) --cflags libdpdk)
DPDK_LIBS := $(shell $(PKG_CONFIG) --libs libdpdk)

CPPFLAGS += -Iinclude $(DPDK_CFLAGS)
CFLAGS ?= -O3 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Wpedantic -pthread
LDFLAGS += -pthread
LDLIBS += $(DPDK_LIBS)

BUILD_DIR := build
TARGET := $(BUILD_DIR)/flowtable

CORE_SOURCES := src/config.c src/flow.c src/packet.c src/rule.c
PIPELINE_SOURCES := src/pipeline.c

.PHONY: all clean

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(CORE_SOURCES) $(PIPELINE_SOURCES) src/main.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDFLAGS) $(LDLIBS) -o $@

clean:
	rm -rf $(BUILD_DIR)
