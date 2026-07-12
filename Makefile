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
CORE_SOURCES := src/config.c src/flow.c src/packet.c src/rule.c
PIPELINE_SOURCES := src/control.c src/dispatcher.c src/pipeline.c src/port.c \
	src/stats.c src/worker.c

.PHONY: all clean test benchmark-e2e workbook

all: $(BUILD_DIR)/flowtable $(BUILD_DIR)/test_flowtable

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/flowtable: $(CORE_SOURCES) $(PIPELINE_SOURCES) src/main.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/test_flowtable: $(CORE_SOURCES) tests/test_flowtable.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(LDFLAGS) $(LDLIBS) -o $@

test: $(BUILD_DIR)/test_flowtable
	XDG_RUNTIME_DIR=/tmp ./$(BUILD_DIR)/test_flowtable \
		-l 0 --no-huge --in-memory --no-pci --no-telemetry

benchmark-e2e: $(BUILD_DIR)/flowtable
	./scripts/run_e2e_benchmark.sh

workbook:
	python3 scripts/generate_test_workbook.py

clean:
	rm -rf $(BUILD_DIR)
