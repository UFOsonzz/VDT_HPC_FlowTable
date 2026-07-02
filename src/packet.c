#include "ft_packet.h"

#include <string.h>
#include <netinet/in.h>

#include <rte_byteorder.h> // doi byte network sang dang host byte order
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_tcp.h>
#include <rte_udp.h>

int ft_packet_parse_mbuf(struct rte_mbuf *mbuf, ft_packet_t *packet) {
    /*
        Vị trí bộ nhớ (Offset tăng dần từ trái sang phải)
+---------------------------------------------------------------------------------------+
|  Ethernet Header  |  VLAN Tag (Optional)  |    IPv4 Header     |  TCP / UDP Header   | Data / Payload |
|    (14 bytes)     |       (4 bytes)       | (ihl: min 20 bytes)| (TCP:20B / UDP:8B)  | (Còn lại)      |
+---------------------------------------------------------------------------------------+
^                   ^                       ^                    ^
|                   |                       |                    |
offset = 0          offset = 14             offset = 18          offset = 18 + ihl
(Bắt đầu)           (Sau Bước 2)            (Sau Bước 3)         (Sau Bước 6)
    */
    const struct rte_ether_hdr *ether;
    const struct rte_vlan_hdr *vlan;
    const struct rte_ipv4_hdr *ipv4; // de const de goi tin goc ko bi thay doi
    uint16_t ether_type; // giao thuc tang ke tiep
    uint32_t offset = 0; // offset so voi dau goi tin
    uint8_t ihl; // kich thuoc thuc te phan ipv4 header

    if (mbuf == NULL || packet == NULL) 
        return -1;

    // mbuf la dau vao, packet la dau ra
    memset(packet, 0, sizeof(*packet));
    packet->ingress_port = FT_INGRESS_PORT_UNKNOWN;

    // kiem tra packet co ethernet header ko
    if (rte_pktmbuf_pkt_len(mbuf) < sizeof(*ether))
        return -1;

    // lay ethernet header 
    ether = rte_pktmbuf_mtod_offset( // ether se tro thang vao ethernet header
        mbuf,
        const struct rte_ether_hdr *,
        offset // nhay toi offset trong goi tin, ep kien thanh rte_ether_hdr *
    );

    offset += sizeof(*ether);
    ether_type = rte_be_to_cpu_16(ether->ether_type); // doc truong ether_type trong ether theo host byte order
    /*
        parse vlan neu co:
        2 loai: 
            0x8100: vlan 802.1Q
            0x88A8: QinQ outer tag
    */

    if (ether_type == RTE_ETHER_TYPE_VLAN || ether_type == RTE_ETHER_TYPE_QINQ) {
        if (rte_pktmbuf_pkt_len(mbuf) < offset + sizeof(*vlan))
            return -1;
        vlan = rte_pktmbuf_mtod_offset(
            mbuf, 
            const struct rte_vlan_hdr *,
            offset
        );
        packet->vlan_id = rte_be_to_cpu_16(vlan->vlan_tci) & 0x0fff;
        ether_type = rte_be_to_cpu_16(vlan->eth_proto);
        offset += sizeof(*vlan);
    }

    // xu ly ipv4
    if (ether_type != RTE_ETHER_TYPE_IPV4)
        return -1;
    if (rte_pktmbuf_pkt_len(mbuf) < offset + sizeof(*ipv4))
        return -1;

    ipv4 = rte_pktmbuf_mtod_offset(
        mbuf,
        const struct rte_ipv4_hdr *,
        offset
    );

    /*
        ihl ko co dinh nhung co the xac dinh qua version_ihl (8 bit)
        4 bit cao = 0100
        4 bit thap = ihl (don vi: so word)
    */
    ihl = (ipv4->version_ihl & 0x0f) * 4;
    if (ihl < sizeof(*ipv4))
        return -1;
    if (rte_pktmbuf_pkt_len(mbuf) < offset + ihl)
        return -1;

    // lay ip va protocol
    packet->src_ip = rte_be_to_cpu_32(ipv4->src_addr);
    packet->dst_ip = rte_be_to_cpu_32(ipv4->dst_addr);
    packet->protocol = ipv4->next_proto_id;
    
    offset += ihl;

    // lay tcp ports (neu tcp)
    if (packet->protocol == IPPROTO_TCP) {
        const struct rte_tcp_hdr *tcp;
        if (rte_pktmbuf_pkt_len(mbuf) < offset + sizeof(*tcp))
            return -1;
        tcp = rte_pktmbuf_mtod_offset(
            mbuf,
            const struct rte_tcp_hdr *,
            offset
        );
        packet->src_port = rte_be_to_cpu_16(tcp->src_port);
        packet->dst_port = rte_be_to_cpu_16(tcp->dst_port);
    }

    // lay up ports (neu udp)

    if (packet->protocol == IPPROTO_UDP) {
        const struct rte_udp_hdr *udp;
        if (rte_pktmbuf_pkt_len(mbuf) < offset + sizeof(*udp))
            return -1;
        udp = rte_pktmbuf_mtod_offset(
            mbuf,
            const struct rte_udp_hdr *,
            offset
        );
        packet->src_port = rte_be_to_cpu_16(udp->src_port);
        packet->dst_port = rte_be_to_cpu_16(udp->dst_port);
    }

    packet->packet_len = (uint16_t)RTE_MIN(rte_pktmbuf_pkt_len(mbuf), UINT16_MAX); // phong khi card mang gop goi tin -> phai lay min tranh tran
    packet->mbuf = mbuf;
    return 0;
}

static int endpoint_compare(uint32_t ip_a, uint16_t port_a, uint32_t ip_b, uint16_t port_b){
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

void ft_packet_normalize(const ft_packet_t *packet, const ft_direction_config_t *directions, ft_normalized_flow_t *normalized) {
    uint16_t tenant_id = 0;
    ft_direction_t direction = FT_DIR_UNKNOWN;
    bool mapped; // da duoc direction resolve chua

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
        // uplink
        normalized->key.client_ip = packet->src_ip;
        normalized->key.server_ip = packet->dst_ip;
        normalized->key.client_port = packet->src_port;
        normalized->key.server_port = packet->dst_port;
    } else if (direction == FT_DIR_DOWNLINK) {
        // downlink
        normalized->key.client_ip = packet->dst_ip;
        normalized->key.server_ip = packet->src_ip;
        normalized->key.client_port = packet->dst_port;
        normalized->key.server_port = packet->src_port;
    } else if (endpoint_compare(packet->src_ip,
                                packet->src_port,
                                packet->dst_ip,
                                packet->dst_port) <= 0) {
        /*
            khong biet huong la uplink hay downlink thi sort endpoint
            -> packet di/ve co cung key
            nhung ko dam bao biet dau la client dau la server that
        */
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