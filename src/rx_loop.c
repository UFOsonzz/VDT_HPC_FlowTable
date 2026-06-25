#include "rx_loop.h"

#include <inttypes.h>
#include <stdio.h>

#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_pause.h>

#define MAX_BURST_SIZE 64

int app_rx_loop(const app_port_t *port, const app_config_t *config) {
    struct rte_mbuf *mbufs[MAX_BURST_SIZE];
    uint64_t received_total = 0;

    if (config->burst_size == 0 || config->burst_size > MAX_BURST_SIZE) {
        fprintf(stderr, "Invalid burst size=%u\n", config->burst_size);
        return -1;
    }

    printf("start rx loop: port=%u queue=%u max_packets=%" PRIu64 "\n",
            port->port_id,
            port->rx_queue_id,
            config->max_packets);
    
    while (config->max_packets == 0 || received_total < config->max_packets) {
        uint64_t nb_rx; // so goi tin thuc te nhan duoc
        nb_rx = rte_eth_rx_burst(
            port->port_id,
            port->rx_queue_id,
            mbufs,
            config->burst_size
        );

        if (nb_rx == 0) {
            rte_pause(); // neu ko co goi tin nao den thi pause cpu 
            continue;
        }

        for (uint16_t i = 0; i < nb_rx; i++) {
            struct rte_mbuf *mbuf = mbufs[i];
            uint32_t packet_len = rte_pktmbuf_pkt_len(mbuf);

            received_total++;

            printf("packet=%" PRIu64 " len=%u port=%u\n",
                    received_total,
                    packet_len,
                    port->port_id);
            
            rte_pktmbuf_free(mbuf); // xu ly xong free tranh mem leak
            if (config->max_packets != 0 && received_total >= config->max_packets)
                break;
        }
    }

    printf("RX finished: received=%", PRIu64 "\n", received_total);
    return 0;
}