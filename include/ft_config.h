// direction resolver
#ifndef FT_CONFIG_H
#define FT_CONFIG_H

#include "ft_common.h"

typedef enum {
    FT_DIRECTION_MATCH_INGRESS_PORT = 0,
    FT_DIRECTION_MATCH_VLAN,
    FT_DIRECTION_MATCH_SRC_PREFIX,
    FT_DIRECTION_MATCH_DST_PREFIX
} ft_direction_match_t; // cac tieu chi phan luong

typedef struct {
    ft_direction_match_t match; // chon tieu chi nao
    uint16_t value; // gia tri cua INGRESS_PORT hoac VLAN
    uint32_t network; // luu ip neu tieu chi la IP
    uint32_t mask;
    uint16_t tenant_id; // sau khi xu ly thi tenant_id la gi
    ft_direction_t direction; // sau khi xu ly chon huong nao
} ft_direction_rule_t; // luat cau hinh

#define FT_MAX_DIRECTION_RULES 128

typedef struct {
    ft_direction_rule_t rules[FT_MAX_DIRECTION_RULES];
    uint16_t count;
} ft_direction_config_t; // danh sach cac luat

int ft_direction_config_load(ft_direction_config_t *config, const char *path); // doc file cau hinh tu path (con the la direction_rules.csv)

// neu truyen thang packet -> phai #include <ft_packet.h> -> loi include thanh vong lap A-B-C-A
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
