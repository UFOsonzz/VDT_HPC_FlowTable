#ifndef FT_RULE_H
#define FT_RULE_H

#include "ft_common.h"
#include "ft_packet.h"

typedef struct {
    uint32_t network;
    uint32_t mask;
    bool any;
} ft_ipv4_match_t;

typedef struct {
    uint16_t port;
    bool any;
} ft_port_match_t;

typedef struct {
    uint16_t id;
    uint16_t precedence;
    char name[FT_NAME_LEN];
    char group[FT_GROUP_LEN];
    ft_ipv4_match_t src_ip;
    ft_ipv4_match_t dst_ip;
    ft_port_match_t src_port;
    ft_port_match_t dst_port;
    uint8_t protocol;
    bool protocol_any;
    ft_action_t action;
} ft_rule_t;

typedef struct {
    ft_rule_t rules[FT_MAX_RULES];
    uint16_t count;
    uint16_t default_rule_id;
} ft_rule_set_t;

int ft_rule_set_load(ft_rule_set_t *set, const char *path);
const ft_rule_t *ft_rule_match(const ft_rule_set_t *set,
                               const ft_flow_key_t *key);
const char *ft_action_name(ft_action_t action);

#endif
