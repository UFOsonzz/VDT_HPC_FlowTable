#ifndef FT_CONFIG_H
#define FT_CONFIG_H

#include "ft_common.h"

typedef enum {
    FT_DIRECTION_MATCH_INGRESS_PORT = 0,
    FT_DIRECTION_MATCH_VLAN,
    FT_DIRECTION_MATCH_SRC_PREFIX,
    FT_DIRECTION_MATCH_DST_PREFIX
} ft_direction_match_t;

typedef struct {
    ft_direction_match_t match;
    uint16_t value;
    uint32_t network;
    uint32_t mask;
    uint16_t tenant_id;
    ft_direction_t direction;
} ft_direction_rule_t;

typedef struct {
    ft_direction_rule_t rules[FT_MAX_DIRECTION_RULES];
    uint16_t count;
    uint16_t vlan_rule_index[4096];
    uint16_t ingress_rule_indices[FT_MAX_DIRECTION_RULES];
    uint16_t src_prefix_rule_indices[FT_MAX_DIRECTION_RULES];
    uint16_t dst_prefix_rule_indices[FT_MAX_DIRECTION_RULES];
    uint16_t ingress_rule_count;
    uint16_t src_prefix_rule_count;
    uint16_t dst_prefix_rule_count;
} ft_direction_config_t;

int ft_direction_config_load(ft_direction_config_t *config, const char *path);
bool ft_direction_resolve(const ft_direction_config_t *config,
                          uint16_t ingress_port,
                          uint16_t vlan_id,
                          uint32_t src_ip,
                          uint32_t dst_ip,
                          uint16_t tenant_hint,
                          ft_direction_t direction_hint,
                          uint16_t *tenant_id,
                          ft_direction_t *direction);

#endif
