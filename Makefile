# Compiler & pkg-config
CC ?= gcc
PKG_CONFIG ?= pkg-config

# pkg-config lay cac duong dan cua thu vien libdpdk roi luu vao 2 bien DPDK_CFLAGS & DPDK_LIBS
DPDK_CFLAGS := $(shell $(PKG_CONFIG) --cflags libdpdk)
DPDK_LIBS := $(shell $(PKG_CONFIG) --libs libdpdk)

# compile flag
CFLAGS := -O2 -g -std=gnu11 -Wall -Wextra -Wpedantic
# cac flag W la cac canh bao nghiem ngat de kiem tra code loi

# CPPFLAGS : trong buoc preprocess can include cac thu vien cua DPDK nho file header (lay tu bien DPDK_CFLAGS)
CPPFLAGS := $(DPDK_CFLAGS)

# LDLIBS : tuong tu nhu CPPFLAGS
LDLIBS := $(DPDK_LIBS)

# thu muc dau ra va file thuc thi cuoi cung
BUILD_DIR = build
TARGET = $(BUILD_DIR)/eal_demo

.PHONY: all clean

# khi go lenh make, he thong chay $(TARGET) dau tien va kich hoat qua trinh build file chay
all: $(TARGET)

# tao BUILD_DIR
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)
# flag -p de tranh loi neu da co san folder build

# pipeline bien dich chuong trinh chinh
$(TARGET): src/main.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDLIBS) -o $@

# xoa build output
clean: 
	rm -rf $(BUILD_DIR)



