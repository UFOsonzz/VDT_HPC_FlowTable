#include "rx_loop.h"
#include "ft_config.h"
#include "ft_packet.h"
#include "ft_flow.h"

#include <inttypes.h>
#include <stdio.h>

#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_pause.h>
#include <rte_cycles.h>

#define MAX_BURST_SIZE 64

int app_rx_loop(const app_port_t *port, const app_config_t *config) {
    struct rte_mbuf *mbufs[MAX_BURST_SIZE];
    uint64_t received_total = 0;

    if (config->burst_size == 0 || config->burst_size > MAX_BURST_SIZE) {
        fprintf(stderr, "Invalid burst size=%u\n", config->burst_size);
        return -1;
    }

    ft_direction_config_t directions;
    if (ft_direction_config_load(&directions, "config/direction_rules.csv") != 0) {
        fprintf(stderr, "Cannot load direction_rules.csv\n");
        return -1;
    }

    ft_flow_table_t flow_table;
    uint64_t hz = rte_get_tsc_hz(); // so chu ky xung nhip cpu tren 1 giay
    uint64_t timeout_cycles = 5 * hz; // 5 giay
    uint64_t next_age = rte_get_tsc_cycles() + hz; // tsc_cycles() tinh so chu ky cpu tu khi bat may
    // next_age = moc thoi gian cho lan don dep tiep theo
    if (ft_flow_table_create(&flow_table, "flow_table", 65536, rte_socket_id()) != 0) { // 65536 = 2^16 toi uu cho viec tinh toan bitwise noi bo cua bang bam
        fprintf(stderr, "Cannot create flow table\n");
        return -1;
    }

    printf("start rx loop: port=%u queue=%u max_packets=%" PRIu64 "\n",
            port->port_id,
            port->rx_queue_id,
            config->max_packets);
    
    while (config->max_packets == 0 || received_total < config->max_packets) {
        // scan dinh ky
        uint64_t now = rte_get_tsc_cycles();
        if (now >= next_age) {
            uint32_t deleted;
            deleted = ft_flow_table_age(&flow_table, now, timeout_cycles, 1024);
            printf("aging: deleted=%u active=%u timed_out=%lu\n",
                    deleted,
                    flow_table.active,
                    flow_table.timed_out);
            next_age = now + hz;
        }

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
            ft_packet_t packet;
            
            received_total++;
            if (ft_packet_parse_mbuf(mbuf, &packet) != 0) {
                printf("packet=%" PRIu64 " parse=FAILED len=%u\n",
                        received_total,
                        rte_pktmbuf_pkt_len(mbuf));
                rte_pktmbuf_free(mbuf);
                continue;
            }

            printf("packet=%" PRIu64
                " len=%u vlan=%u proto=%u "
                "src=%u.%u.%u.%u:%u "
                "dst=%u.%u.%u.%u:%u\n",
                received_total,
                packet.packet_len,
                packet.vlan_id,
                packet.protocol,

                (packet.src_ip >> 24) & 0xff,
                (packet.src_ip >> 16) & 0xff,
                (packet.src_ip >> 8) & 0xff,
                packet.src_ip & 0xff,
                packet.src_port,

                (packet.dst_ip >> 24) & 0xff,
                (packet.dst_ip >> 16) & 0xff,
                (packet.dst_ip >> 8) & 0xff,
                packet.dst_ip & 0xff,
                packet.dst_port);
            
            rte_pktmbuf_free(mbuf); // xu ly xong free tranh mem leak
            if (config->max_packets != 0 && received_total >= config->max_packets)
                break;
            
            ft_normalized_flow_t flow;
            packet.ingress_port = port->port_id;
            ft_packet_normalize(&packet, &directions, &flow);
            
            printf("raw five-tuple: "
                "src=%u.%u.%u.%u:%u "
                "dst=%u.%u.%u.%u:%u "
                "proto=%u\n",
                (packet.src_ip >> 24) & 0xff,
                (packet.src_ip >> 16) & 0xff,
                (packet.src_ip >> 8) & 0xff,
                packet.src_ip & 0xff,
                packet.src_port,
                (packet.dst_ip >> 24) & 0xff,
                (packet.dst_ip >> 16) & 0xff,
                (packet.dst_ip >> 8) & 0xff,
                packet.dst_ip & 0xff,
                packet.dst_port,
                packet.protocol);

            printf("canonical flow-key: "
                "tenant=%u direction=%u "
                "client=%u.%u.%u.%u:%u "
                "server=%u.%u.%u.%u:%u "
                "proto=%u\n",
                flow.key.tenant_id,
                flow.direction,
                (flow.key.client_ip >> 24) & 0xff,
                (flow.key.client_ip >> 16) & 0xff,
                (flow.key.client_ip >> 8) & 0xff,
                flow.key.client_ip & 0xff,
                flow.key.client_port,
                (flow.key.server_ip >> 24) & 0xff,
                (flow.key.server_ip >> 16) & 0xff,
                (flow.key.server_ip >> 8) & 0xff,
                flow.key.server_ip & 0xff,
                flow.key.server_port,
                flow.key.protocol);

            bool created;
            uint64_t now = rte_get_tsc_cycles();

            ft_flow_entry_t *entry = ft_flow_table_get_or_create(&flow_table, &flow.key, now, &created);
            if (entry == NULL) {
                printf("flow table full, dropping packet\n");
                rte_pktmbuf_free(mbuf);
                continue;
            }

            entry->last_seen = now;
            entry->packets++;
            entry->bytes += packet.packet_len;

            printf("flow %s active=%u created_total=%lu packets=%lu\n",
                    created ? "CREATED" : "HIT",
                    flow_table.active,
                    flow_table.created,
                    entry->packets);
        }
    }

    ft_flow_table_destroy(&flow_table);
    printf("RX finished: received=%" PRIu64 "\n", received_total);
    return 0;
}
