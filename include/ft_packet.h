#ifndef FT_PACKET_H
#define FT_PACKET_H

#include "ft_common.h"

struct rte_mbuf; // khai bao de compiler biet co struct nay ma ko can #include <rte_mbuf.h> rat nang
/* 
    struct luu metadata sau khi parse packet
    *host byte order: little endian / big endian theo phan cung may tinh 
    ip va port luu theo host byte order
*/

typedef struct {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t vlan_id; // == 0 neu packet ko co vlan
    uint16_t ingress_port; // cong vat ly ma packet vua di vao NIC cua ban
    uint8_t protocol;
    uint16_t packet_len;
    struct rte_mbuf *mbuf;
} ft_packet_t;

// return 0 neu thanh cong, -1 neu ko hop le
int ft_packet_parse_mbuf(struct rte_mbuf *mbuf, ft_packet_t *packet);

#endif