#ifndef FT_PACKET_H
#define FT_PACKET_H

#include "ft_common.h"
#include "ft_config.h"

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
    uint16_t tenant_hint; // tenant giong nhu ten khach hang, 1 tenant co the co nhieu vlan
    uint8_t protocol;
    ft_direction_t direction_hint;
    uint16_t packet_len;
    struct rte_mbuf *mbuf;
} ft_packet_t;

/*
    flow key ~ flow identity
*/
typedef struct {
    uint16_t tenant_id; // 2
    uint8_t protocol; // 1
    uint8_t reserved; // 1 : bien nay ko lam j ca chi de tu chon vi tri padding them 1 bit, neu padding cho khac thi block dau tien se chi co 3 bit -> lang phi
    uint32_t client_ip;
    uint32_t server_ip;
    uint16_t client_port;
    uint16_t server_port;
} __attribute__((packed)) ft_flow_key_t; // vi se dung cho hash nen ko duoc de tu dong padding (mac du tong chia het cho 4 nhung khi compile, compiler van co the tu dong chen byte rac vao mien la chia het cho 4)

typedef struct {
    ft_flow_key_t key;
    ft_direction_t direction;
} ft_normalized_flow_t;

// return 0 neu thanh cong, -1 neu ko hop le
int ft_packet_parse_mbuf(struct rte_mbuf *mbuf, ft_packet_t *packet);

void ft_packet_normalize(const ft_packet_t *packet, const ft_direction_config_t *directions, ft_normalized_flow_t *normalized);

#endif