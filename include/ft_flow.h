#ifndef FT_FLOW_H
#define FT_FLOW_H

#include "ft_common.h"
#include "ft_packet.h"

struct rte_hash; // nhiem vu: nhan ve 1 key -> tinh toan -> tra ve index

/*
    struct flow entry
    key: cannonical bidirectional flow key

    created_at: tdiem tao flow
    last_seen: tdiem cuoi cung packet thuoc flow xuat hien
    packets/bytes: counter
    in_use: slot nay dang duoc dung hay free
*/

typedef struct {
    ft_flow_key_t key;
    uint64_t created_at;
    uint64_t last_seen;
    uint64_t packets;
    uint64_t bytes;
    uint8_t in_use;
} ft_flow_entry_t;

/*
    struct flow table
    hash: dung de lookup key -> entry pointer
    entries: mang flow entry
    free_stack: stack chua cac slot con trong
    active: bnh luong dang hoat dong
    created/deleted/timed_out: da tao, da xoa, bi xoa do qua tgian phan hoi
*/

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

    uint32_t age_cursor; // con tro don dep cac luong bi timed out
    int socket_id; // numa awareness
} ft_flow_table_t;

int ft_flow_table_create(ft_flow_table_t *table, const char *name, uint32_t capacity, int socket_id);
void ft_flow_table_destroy(ft_flow_table_t *table);
ft_flow_entry_t *ft_flow_table_lookup(ft_flow_table_t *table, const ft_flow_key_t *key);
ft_flow_entry_t *ft_flow_table_get_or_create(ft_flow_table_t *table, const ft_flow_key_t *key, uint64_t now, bool *created);
int ft_flow_table_delete(ft_flow_table_t *table, const ft_flow_key_t *key, bool timeout);
uint32_t ft_flow_table_age(ft_flow_table_t *table, uint64_t now, uint64_t timeout_cycles, uint32_t budget); // ham don rac (luong don dep gioi han boi budget)

#endif