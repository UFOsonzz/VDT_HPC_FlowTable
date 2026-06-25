#ifndef PORT_H
#define PORT_H

#include "app_config.h"

#include <rte_ethdev.h>
#include <rte_mempool.h>

/*
[ Người dùng gõ tham số ]
             │
             ▼
┌──────────────────────────┐
│  config.port_id = 0      │ (Số thô trong app_config.h)
│  config.burst_size = 32  │
└────────────┬─────────────┘
             │ (Truyền vào app_port_init)
             ▼
┌────────────────────────────────────────────────────────┐
│                   MÔ-ĐUN PORT.H / PORT.C               │
│                                                        │
│ 1. Kiểm tra phần cứng: Port 0 có tồn tại trên máy ko?  │
│ 2. Cấu hình phần cứng: Cài đặt Port 0 dùng 1 RX Queue  │
│ 3. Cấp phát Mempool: Tạo kho mbuf chứa gói tin         │
│ 4. Bật công tắc: Kích hoạt Port 0 bắt đầu nhận sóng    │
└────────────────────────────────────────────────────────┘
*/ 

typedef struct {
    uint16_t port_id;
    uint16_t rx_queue_id;
    uint16_t tx_queue_id;
    struct rte_mempool *mbuf_pool;
} app_port_t;

int app_port_init(app_port_t *port, const app_config_t *config);
void app_port_close(app_port_t *port);

#endif