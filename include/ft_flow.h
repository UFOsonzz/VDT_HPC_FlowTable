#ifndef FT_FLOW_H
#define FT_FLOW_H

#include "ft_common.h"
#include "ft_packet.h"

struct rte_hash;

typedef struct {
    ft_flow_key_t key;
    uint64_t created_at;
    uint64_t last_seen;
    uint64_t packets[2];
    uint64_t bytes[2];
    uint16_t rule_id;
    uint8_t action;
    uint8_t in_use;
} ft_flow_entry_t;

typedef struct {
    struct rte_hash *hash;
    ft_flow_entry_t *entries;
    uint32_t *free_stack;
    uint32_t capacity;
    uint32_t free_top;
    uint32_t active;
    uint64_t created;
    uint64_t deleted;
    uint64_t timed_out;
    uint32_t age_cursor;
    int socket_id;
    char name[FT_NAME_LEN];
} ft_flow_table_t;

int ft_flow_table_create(ft_flow_table_t *table,
                         const char *name,
                         uint32_t capacity,
                         int socket_id);
void ft_flow_table_destroy(ft_flow_table_t *table);
ft_flow_entry_t *ft_flow_table_lookup(ft_flow_table_t *table,
                                      const ft_flow_key_t *key);
ft_flow_entry_t *ft_flow_table_get_or_create(ft_flow_table_t *table,
                                             const ft_flow_key_t *key,
                                             uint64_t now,
                                             bool *created);
int ft_flow_table_delete(ft_flow_table_t *table,
                         const ft_flow_key_t *key,
                         bool timeout);
uint32_t ft_flow_table_age(ft_flow_table_t *table,
                           uint64_t now,
                           uint64_t timeout_cycles,
                           uint32_t budget);

#endif
