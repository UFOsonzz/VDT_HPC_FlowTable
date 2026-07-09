#ifndef FT_PACKET_H
#define FT_PACKET_H

#include "ft_common.h"
#include "ft_config.h"

struct rte_mbuf;

typedef struct {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t vlan_id;
    uint16_t ingress_port;
    uint16_t tenant_hint;
    uint8_t protocol;
    ft_direction_t direction_hint;
    uint16_t packet_len;
    uint64_t timestamp;
    struct rte_mbuf *mbuf;
} ft_packet_t;

typedef struct {
    uint16_t tenant_id;
    uint8_t protocol;
    uint8_t reserved;
    uint32_t client_ip;
    uint32_t server_ip;
    uint16_t client_port;
    uint16_t server_port;
} __attribute__((packed)) ft_flow_key_t;

typedef struct {
    ft_flow_key_t key;
    ft_direction_t direction;
} ft_normalized_flow_t;

int ft_packet_parse_mbuf(struct rte_mbuf *mbuf, ft_packet_t *packet);
void ft_packet_normalize(const ft_packet_t *packet,
                         const ft_direction_config_t *directions,
                         ft_normalized_flow_t *normalized);
uint32_t ft_flow_hash(const ft_flow_key_t *key);
ft_traffic_class_t ft_packet_classify(const ft_normalized_flow_t *flow);

#endif
