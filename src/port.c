#include "port.h"

#include <stdio.h>
#include <string.h>

#include <rte_ethdev.h>
#include <rte_errno.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

// kich thuoc hang doi rx, tx
#define RX_DESC_COUNT 1024
#define TX_DESC_COUNT 1024

// kich thuoc pool tren ram, kich thuoc moi core xin cho cache
// pool_size > rx + tx + so core * cache + phan danh cho poll mode
#define MBUF_POOL_SIZE 8192
#define MBUF_CACHE_SIZE 256

int app_port_init(app_port_t *port, const app_config_t *config) {
    struct rte_eth_conf eth_conf; // struct config card mang
    struct rte_eth_dev_info dev_info; // struct chua thong tin cau hinh card
    int socket_id, ret; // id chip cpu, ma tra ve

    memset(port, 0, sizeof(*port)); // xoa vung nho port de ghi app_config ghi vao
    memset(&eth_conf, 0, sizeof(eth_conf)); // eth_conf ko phai con tro ma la chinh noi dung -> phai bien thanh con tro de memset

    port->port_id = config->port_id;
    port->rx_queue_id = config->rx_queue_id;
    port->tx_queue_id = config->tx_queue_id;

    if (!rte_eth_dev_is_valid_port(port->port_id)) {
        uint16_t nb_ports = rte_eth_dev_count_avail();

        fprintf(stderr,
                "invalid dpdk port_id=%u (available DPDK ports: %u)\n",
                port->port_id,
                nb_ports);
        if (nb_ports == 0) {
            fprintf(stderr,
                    "No DPDK ethdev is available. For a real NIC, bind or allowlist a DPDK-compatible device; "
                    "for pcap input, use the net_pcap vdev script.\n");
        }
        return -1;
    }

    socket_id = rte_eth_dev_socket_id(port->port_id); // dep nhat la dung duoc cpu tren socket co card mang noi toi
    if (socket_id < 0)
        socket_id = rte_socket_id(); // neu ko duoc thi lay cpu dang thuc hien dong lenh nay

    ret = rte_eth_dev_info_get(port->port_id, &dev_info); // doc thong tin cong mang nap vao dev_info
    if (ret != 0) {
        fprintf(stderr, "rte_eth_dev_info_get failed: %s\n", rte_strerror(-ret));
        return ret;
    }

    ret = rte_eth_dev_configure(port->port_id, 1, 1, &eth_conf); // 1 rx, 1 tx
    if (ret != 0) {
        fprintf(stderr, "rte_eth_dev_configure failed: %s\n", rte_strerror(-ret));
        return ret;
    }

    // khoi tao rx mbuf pool
    port->mbuf_pool = rte_pktmbuf_pool_create(
        "rx_mbuf_pool", // ten pool
        MBUF_POOL_SIZE,
        MBUF_CACHE_SIZE,
        0, // kich thuoc vung nho private (ko dung)
        RTE_MBUF_DEFAULT_BUF_SIZE,
        socket_id // tao pool ram o cpu socket da tim duoc
    );

    if (port->mbuf_pool == NULL) {
        fprintf(stderr, "rte_pktmbuf_pool_create failed: %s\n", rte_strerror(rte_errno));
        return -1;
    }

    ret = rte_eth_rx_queue_setup(
        port->port_id,
        port->rx_queue_id,
        RX_DESC_COUNT,
        socket_id, 
        &dev_info.default_rxconf, // cau hinh mac dinh cua driver card mang
        port->mbuf_pool // cho phep queue dung mempool nay
    );

    if (ret != 0) {
        fprintf(stderr, "rte_eth_dev_rx_queue_setup failed: %s\n", rte_strerror(-ret));
        return ret;
    }

    ret = rte_eth_tx_queue_setup(
        port->port_id,
        port->tx_queue_id,
        TX_DESC_COUNT,
        socket_id,
        &dev_info.default_txconf
    );

    if (ret != 0) {
        fprintf(stderr, "rte_eth_tx_queue_setup failed: %s\n", rte_strerror(-ret));
        return ret;
    }

    ret = rte_eth_dev_start(port->port_id); // start 
    if (ret != 0) {
        fprintf(stderr, "rte_eth_dev_start failed: %s\n", rte_strerror(-ret));
        return ret;
    }

    // promiscuous mode
    if (config->promiscuous)
        rte_eth_promiscuous_enable(port->port_id);

    printf("Port %u started on socket %d\n", port->port_id, socket_id);
    return 0;
}

void app_port_close(app_port_t *port) {
    if (rte_eth_dev_is_valid_port(port->port_id)) {
        rte_eth_dev_stop(port->port_id);
        rte_eth_dev_close(port->port_id);
    }

    if (port->mbuf_pool != NULL) {
        rte_mempool_free(port->mbuf_pool);
        port->mbuf_pool = NULL; // chong mem leak
    }
}
