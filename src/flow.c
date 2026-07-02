#include "ft_flow.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_malloc.h> // cho phep cap phat tren hugepage va chi dinh cap phat tren ram cua socket X

int ft_flow_table_create(ft_flow_table_t *table, const char *name, uint32_t capacity, int socket_id) {
    struct rte_hash_parameters params;
    uint32_t i;

    if (table == NULL || name == NULL || capacity == 0) 
        return -EINVAL;
    
    memset(table, 0, sizeof(*table));

    table->capacity = capacity;
    table->socket_id = socket_id;

    // cap phat mang entry CO DINH
    table->entries = rte_zmalloc_socket("flow_entries", sizeof(*table->entries) * capacity,
                                        RTE_CACHE_LINE_SIZE, socket_id); // ham nay malloc + memset toan bo vung duoc malloc ve 0

    table->free_stack = rte_malloc_socket("flow_free_stack", sizeof(*table->free_stack) * capacity,
                                            RTE_CACHE_LINE_SIZE, socket_id); // ko can zmalloc vi ghi de ngay sau day
    if (table->entries == NULL || table->free_stack == NULL)
        goto fail;

    for (i = 0; i < capacity; i++)
        table->free_stack[i] = capacity - i - 1;

    table->free_top = capacity;
    
    memset(&params, 0, sizeof(params));
    params.name = name;
    params.entries = capacity;
    params.key_len = sizeof(ft_flow_key_t);
    params.hash_func = rte_jhash;
    params.hash_func_init_val = 0x9e3779b9U; // phan thap phan cua ti le vang (dam bao tinh ngau nhien)
    params.socket_id = socket_id;
    params.extra_flag = RTE_HASH_EXTRA_FLAGS_EXT_TABLE; // flag nay cho phep khi dung cuckoo hashing ma bang bi day hoac collide nhieu -> kich hoat 1 bang mo rong

    table->hash = rte_hash_create(&params);
    if (table->hash == NULL)
        goto fail;
    return 0;


fail: 
    ft_flow_table_destroy(table);
    return -ENOMEM;
}

void ft_flow_table_destroy(ft_flow_table_t *table) {
    if (table == NULL)
        return;
    if (table->hash != NULL)
        rte_hash_free(table->hash);

    rte_free(table->entries);
    rte_free(table->free_stack);

    memset(table, 0, sizeof(*table));
}

ft_flow_entry_t* ft_flow_table_lookup(ft_flow_table_t *table, const ft_flow_key_t *key) {
    void *data = NULL; 
    if (table == NULL || key == NULL)
        return NULL;

    if (rte_hash_lookup_data(table->hash, key, &data) < 0)
        return NULL;

    return data;
}

ft_flow_entry_t* ft_flow_table_get_or_create(ft_flow_table_t *table, const ft_flow_key_t *key, uint64_t now, bool *created) {
    ft_flow_entry_t *entry;
    uint32_t index;

    if (created != NULL)
        *created = false;
    
    // lookup truoc
    entry = ft_flow_table_lookup(table, key);
    if (entry != NULL)
        return entry;

    if (table == NULL || table->free_top == 0)
        return NULL;

    // con slot thi lay 1 slot free
    index = table->free_stack[--table->free_top];
    entry = &table->entries[index];

    memset(entry, 0, sizeof(*entry));

    entry->key = *key;
    entry->created_at = now;
    entry->last_seen = now;
    entry->in_use = 1;

    // add key entry pointer vao rte_hash
    if (rte_hash_add_key_data(table->hash, &entry->key, entry) < 0) {
        entry->in_use = 0;
        table->free_stack[table->free_top++] = index;
        return NULL;
    }

    table->active++;
    table->created++;
    if (created != NULL)
        *created = true;
    return entry;
}

int ft_flow_table_delete(ft_flow_table_t *table, const ft_flow_key_t *key, bool timeout) {
    ft_flow_entry_t *entry;
    uint32_t index;
    
    entry = ft_flow_table_lookup(table, key);
    if (entry == NULL)
        return -ENOENT;

    if (rte_hash_del_key(table->hash, key) < 0)
        return -EIO;
    
    index = (uint32_t)(entry - table->entries); // tru con tro ra index
    entry->in_use = 0;
    table->free_stack[table->free_top++] = index;

    table->active--;
    table->deleted++;

    if (timeout)
        table->timed_out++;
    return 0;
}

// FLOW TIMEOUT MAC DINH LA 5 GIAY
// if now - last_seen > timeout_cycles -> delete flow
uint32_t ft_flow_table_age(ft_flow_table_t *table, uint64_t now, uint64_t timeout_cycles, uint32_t budget) {
    uint32_t visited = 0;
    uint32_t deleted = 0;

    if (table == NULL || table->capacity == 0)
        return 0;

    while (visited < budget) {
        ft_flow_entry_t *entry = &table->entries[table->age_cursor];
        table->age_cursor++;
        if (table->age_cursor == table->capacity)
            table->age_cursor = 0;
        visited++;
        if (entry->in_use && now - entry->last_seen > timeout_cycles) {
            ft_flow_key_t key = entry->key;
            if (ft_flow_table_delete(table, &key, true) == 0)
                deleted++;
        }
    }

    return deleted;
}

