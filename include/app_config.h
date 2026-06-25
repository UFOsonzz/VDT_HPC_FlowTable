// dinh nghia struct luu tham so nguoi dung truyen vao tu dong lenh khi khoi chay (sau dau --)
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t port_id;
    uint16_t rx_queue_id;
    uint16_t tx_queue_id;
    uint16_t burst_size;
    uint64_t max_packets; // nhan bao nhieu packet roi dung
    bool promiscuous; // (promiscuous mode) true la nhan het moi goi, false la chi nhan cac goi tin dich danh cho minh
} app_config_t;

#endif