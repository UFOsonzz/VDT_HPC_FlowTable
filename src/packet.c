#include "ft_packet.h"

#include <string.h>

#include <rte_ether.h>
#include <rte_hash_crc.h>
#include <rte_ip.h>
#include <rte_jhash.h>
#include <rte_mbuf.h>
#include <rte_tcp.h>
#include <rte_udp.h>

static int endpoint_compare(uint32_t ip_a, uint16_t port_a, uint32_t ip_b, uint16_t port_b) {
    if (ip_a < ip_b)
        return -1;
    if (ip_a > ip_b)
        return 1;
    if (port_a < port_b)
        return -1;
    if (port_a > port_b)
        return 1;
    return 0;
}

int ft_packet_parse_mbuf(struct rte_mbuf *mbuf, ft_packet_t *packet) {
    const struct rte_ether_hdr *ether;
    const struct rte_vlan_hdr *vlan;
    const struct rte_ipv4_hdr *ipv4;
    uint16_t ether_type;
    uint32_t offset = 0;
    uint8_t ihl;

    if (mbuf == NULL || packet == NULL)
        return -1;
    memset(packet, 0, sizeof(*packet));
    packet->ingress_port = FT_INGRESS_PORT_UNKNOWN;
    if (rte_pktmbuf_pkt_len(mbuf) < sizeof(*ether))
        return -1;

    ether = rte_pktmbuf_mtod_offset(mbuf, const struct rte_ether_hdr *, offset);
    offset += sizeof(*ether);
    ether_type = rte_be_to_cpu_16(ether->ether_type);
    if (ether_type == RTE_ETHER_TYPE_VLAN ||
        ether_type == RTE_ETHER_TYPE_QINQ) {
        if (rte_pktmbuf_pkt_len(mbuf) < offset + sizeof(*vlan))
            return -1;
        vlan = rte_pktmbuf_mtod_offset(mbuf, const struct rte_vlan_hdr *, offset);
        packet->vlan_id = rte_be_to_cpu_16(vlan->vlan_tci) & 0x0fff;
        ether_type = rte_be_to_cpu_16(vlan->eth_proto);
        offset += sizeof(*vlan);
    }
    if (ether_type != RTE_ETHER_TYPE_IPV4 ||
        rte_pktmbuf_pkt_len(mbuf) < offset + sizeof(*ipv4))
        return -1;

    ipv4 = rte_pktmbuf_mtod_offset(mbuf, const struct rte_ipv4_hdr *, offset);
    ihl = (ipv4->version_ihl & 0x0f) * 4;
    if (ihl < sizeof(*ipv4) || rte_pktmbuf_pkt_len(mbuf) < offset + ihl)
        return -1;
    packet->src_ip = rte_be_to_cpu_32(ipv4->src_addr);
    packet->dst_ip = rte_be_to_cpu_32(ipv4->dst_addr);
    packet->protocol = ipv4->next_proto_id;
    offset += ihl;

    if (packet->protocol == IPPROTO_TCP) {
        const struct rte_tcp_hdr *tcp;
        if (rte_pktmbuf_pkt_len(mbuf) < offset + sizeof(*tcp))
            return -1;
        tcp = rte_pktmbuf_mtod_offset(mbuf, const struct rte_tcp_hdr *, offset);
        packet->src_port = rte_be_to_cpu_16(tcp->src_port);
        packet->dst_port = rte_be_to_cpu_16(tcp->dst_port);
    } else if (packet->protocol == IPPROTO_UDP) {
        const struct rte_udp_hdr *udp;
        if (rte_pktmbuf_pkt_len(mbuf) < offset + sizeof(*udp))
            return -1;
        udp = rte_pktmbuf_mtod_offset(mbuf, const struct rte_udp_hdr *, offset);
        packet->src_port = rte_be_to_cpu_16(udp->src_port);
        packet->dst_port = rte_be_to_cpu_16(udp->dst_port);
    }
    packet->packet_len = (uint16_t)RTE_MIN(rte_pktmbuf_pkt_len(mbuf), UINT16_MAX);
    packet->mbuf = mbuf;
    return 0;
}

void ft_packet_normalize(const ft_packet_t *packet,
                    const ft_direction_config_t *directions,
                    ft_normalized_flow_t *normalized) {
    uint16_t tenant_id = 0;
    ft_direction_t direction = FT_DIR_UNKNOWN;
    bool mapped;

    memset(normalized, 0, sizeof(*normalized));
    mapped = ft_direction_resolve(directions,
                                  packet->ingress_port,
                                  packet->vlan_id,
                                  packet->src_ip,
                                  packet->dst_ip,
                                  packet->tenant_hint,
                                  packet->direction_hint,
                                  &tenant_id,
                                  &direction);
    normalized->key.tenant_id = tenant_id;
    normalized->key.protocol = packet->protocol;
    normalized->direction = mapped ? direction : FT_DIR_UNKNOWN;

    if (direction == FT_DIR_UPLINK) {
        normalized->key.client_ip = packet->src_ip;
        normalized->key.server_ip = packet->dst_ip;
        normalized->key.client_port = packet->src_port;
        normalized->key.server_port = packet->dst_port;
    } else if (direction == FT_DIR_DOWNLINK) {
        normalized->key.client_ip = packet->dst_ip;
        normalized->key.server_ip = packet->src_ip;
        normalized->key.client_port = packet->dst_port;
        normalized->key.server_port = packet->src_port;
    } else if (endpoint_compare(packet->src_ip, packet->src_port,
                                packet->dst_ip, packet->dst_port) <= 0) {
        normalized->key.client_ip = packet->src_ip;
        normalized->key.server_ip = packet->dst_ip;
        normalized->key.client_port = packet->src_port;
        normalized->key.server_port = packet->dst_port;
    } else {
        normalized->key.client_ip = packet->dst_ip;
        normalized->key.server_ip = packet->src_ip;
        normalized->key.client_port = packet->dst_port;
        normalized->key.server_port = packet->src_port;
    }
}

uint32_t ft_flow_hash(const ft_flow_key_t *key) {
    return rte_jhash(key, sizeof(*key), 0x9e3779b9U);
}

ft_traffic_class_t ft_packet_classify(const ft_normalized_flow_t *flow) {
    if (flow->key.protocol == IPPROTO_TCP) {
        if (flow->key.server_port == 80)
            return FT_TRAFFIC_HTTP;
        if (flow->key.server_port == 443)
            return FT_TRAFFIC_HTTPS;
        return FT_TRAFFIC_TCP;
    }
    if (flow->key.protocol == IPPROTO_UDP) {
        if (flow->key.server_port == 53)
            return FT_TRAFFIC_DNS;
        return FT_TRAFFIC_UDP;
    }
    return FT_TRAFFIC_OTHER;
}
