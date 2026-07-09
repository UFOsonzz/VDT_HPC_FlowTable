#include "pipeline_internal.h"

#include <stdio.h>
#include <string.h>

#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>

/* Configure RX/TX queues, mempool, RSS, and start the selected ethdev port. */
int ft_configure_ethdev(uint16_t port_id,
                        uint16_t rx_queue_count,
                        uint16_t worker_count,
                        uint32_t requested_mbuf_count,
                        bool tx_enabled,
                        struct rte_mempool **mbuf_pool) {
    struct rte_eth_dev_info info;
    struct rte_eth_conf port_conf;
    uint16_t rx_desc = 1024;
    uint16_t tx_desc = 1024;
    uint16_t tx_queues = tx_enabled ? worker_count : 1;
    int socket_id = rte_eth_dev_socket_id(port_id);
    uint32_t minimum_mbuf_count;
    uint32_t mbuf_count;
    int result;

    if (!rte_eth_dev_is_valid_port(port_id))
        return -1;
    if (socket_id < 0)
        socket_id = rte_socket_id();
    memset(&port_conf, 0, sizeof(port_conf));
    result = rte_eth_dev_info_get(port_id, &info);
    if (result != 0) {
        fprintf(stderr, "rte_eth_dev_info_get: %s (%d)\n",
                rte_strerror(-result), result);
        return result;
    }
    if (rx_queue_count > info.max_rx_queues) {
        fprintf(stderr, "Requested %u RX queues, but port %u supports %u\n",
                rx_queue_count, port_id, info.max_rx_queues);
        return -1;
    }
    if (tx_queues > info.max_tx_queues) {
        fprintf(stderr, "Requested %u TX queues, but port %u supports %u\n",
                tx_queues, port_id, info.max_tx_queues);
        return -1;
    }
    if (rx_queue_count > 1) {
        uint64_t rss_hf = RTE_ETH_RSS_IP | RTE_ETH_RSS_TCP | RTE_ETH_RSS_UDP;

        rss_hf &= info.flow_type_rss_offloads;
        if (rss_hf != 0) {
            port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
            port_conf.rx_adv_conf.rss_conf.rss_hf = rss_hf;
        } else {
            fprintf(stderr, "Port %u does not advertise RSS offloads;"
                    " RX queue distribution depends on the PMD\n",
                    port_id);
        }
    }

    /* Configure queues before allocating workers so port errors fail early. */
    result = rte_eth_dev_configure(port_id, rx_queue_count, tx_queues,
                                   &port_conf);
    if (result != 0) {
        fprintf(stderr, "rte_eth_dev_configure: %s (%d)\n",
                rte_strerror(-result), result);
        return result;
    }
    rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &rx_desc, &tx_desc);
    minimum_mbuf_count = 8192U * rx_queue_count;
    mbuf_count = requested_mbuf_count == 0 ? minimum_mbuf_count :
        requested_mbuf_count;
    if (mbuf_count < minimum_mbuf_count)
        mbuf_count = minimum_mbuf_count;
    *mbuf_pool = rte_pktmbuf_pool_create("ft_rx_mbuf_pool",
                                         mbuf_count, 256, 0,
                                         RTE_MBUF_DEFAULT_BUF_SIZE,
                                         socket_id);
    if (*mbuf_pool == NULL) {
        fprintf(stderr, "rte_pktmbuf_pool_create: %s\n",
                rte_strerror(rte_errno));
        return -1;
    }
    for (uint16_t i = 0; i < rx_queue_count; i++) {
        result = rte_eth_rx_queue_setup(port_id, i, rx_desc, socket_id,
                                        &info.default_rxconf, *mbuf_pool);
        if (result != 0) {
            fprintf(stderr, "rte_eth_rx_queue_setup queue=%u: %s (%d)\n",
                    i, rte_strerror(-result), result);
            return result;
        }
    }
    for (uint16_t i = 0; i < tx_queues; i++) {
        result = rte_eth_tx_queue_setup(port_id, i, tx_desc, socket_id,
                                        &info.default_txconf);
        if (result != 0) {
            fprintf(stderr, "rte_eth_tx_queue_setup: %s (%d)\n",
                    rte_strerror(-result), result);
            return result;
        }
    }
    result = rte_eth_dev_start(port_id);
    if (result != 0) {
        fprintf(stderr, "rte_eth_dev_start: %s (%d)\n",
                rte_strerror(-result), result);
        return result;
    }
    rte_eth_promiscuous_enable(port_id);
    return 0;
}
