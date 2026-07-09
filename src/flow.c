#include "ft_flow.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_malloc.h>

/* Create one worker-owned flow table with external storage for entries. */
int ft_flow_table_create(ft_flow_table_t *table,
                     const char *name,
                     uint32_t capacity,
                     int socket_id) {
    struct rte_hash_parameters params;
    uint32_t i;

    if (table == NULL || name == NULL || capacity == 0)
        return -EINVAL;
    memset(table, 0, sizeof(*table));
    snprintf(table->name, sizeof(table->name), "%s", name);
    table->capacity = capacity;
    table->socket_id = socket_id;
    table->entries = rte_zmalloc_socket("flow_entries",
                                        sizeof(*table->entries) * capacity,
                                        RTE_CACHE_LINE_SIZE,
                                        socket_id);
    table->free_stack = rte_malloc_socket("flow_free_stack",
                                          sizeof(*table->free_stack) * capacity,
                                          RTE_CACHE_LINE_SIZE,
                                          socket_id);
    if (table->entries == NULL || table->free_stack == NULL)
        goto fail;
    for (i = 0; i < capacity; i++)
        table->free_stack[i] = capacity - i - 1;
    table->free_top = capacity;

    memset(&params, 0, sizeof(params));
    params.name = table->name;
    params.entries = capacity;
    params.key_len = sizeof(ft_flow_key_t);
    params.hash_func = rte_jhash;
    params.hash_func_init_val = 0x9e3779b9U;
    params.socket_id = socket_id;
    params.extra_flag = RTE_HASH_EXTRA_FLAGS_EXT_TABLE;
    table->hash = rte_hash_create(&params);
    if (table->hash == NULL)
        goto fail;
    return 0;

fail:
    ft_flow_table_destroy(table);
    return -ENOMEM;
}

/* Release hash/index storage and reset the table to an inert state. */
void ft_flow_table_destroy(ft_flow_table_t *table) {
    if (table == NULL)
        return;
    if (table->hash != NULL)
        rte_hash_free(table->hash);
    rte_free(table->entries);
    rte_free(table->free_stack);
    memset(table, 0, sizeof(*table));
}

/* Look up an active flow entry by canonical key without creating it. */
ft_flow_entry_t *ft_flow_table_lookup(ft_flow_table_t *table, const ft_flow_key_t *key) {
    void *data = NULL;

    if (table == NULL || key == NULL)
        return NULL;
    if (rte_hash_lookup_data(table->hash, key, &data) < 0)
        return NULL;
    return data;
}

/* Return an existing entry or allocate a new one and update create counters. */
ft_flow_entry_t *ft_flow_table_get_or_create(ft_flow_table_t *table,
                            const ft_flow_key_t *key,
                            uint64_t now,
                            bool *created) {
    ft_flow_entry_t *entry;
    uint32_t index;

    if (created != NULL)
        *created = false;
    entry = ft_flow_table_lookup(table, key);
    if (entry != NULL)
        return entry;
    if (table == NULL || table->free_top == 0)
        return NULL;

    index = table->free_stack[--table->free_top];
    entry = &table->entries[index];
    memset(entry, 0, sizeof(*entry));
    entry->key = *key;
    entry->created_at = now;
    entry->last_seen = now;
    entry->in_use = 1;
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

/* Delete a flow for timeout or normal aging and update lifecycle counters. */
int ft_flow_table_delete(ft_flow_table_t *table,
                     const ft_flow_key_t *key,
                     bool timeout) {
    ft_flow_entry_t *entry;
    uint32_t index;

    entry = ft_flow_table_lookup(table, key);
    if (entry == NULL)
        return -ENOENT;
    if (rte_hash_del_key(table->hash, key) < 0)
        return -EIO;
    index = (uint32_t)(entry - table->entries);
    entry->in_use = 0;
    table->free_stack[table->free_top++] = index;
    table->active--;
    table->deleted++;
    if (timeout)
        table->timed_out++;
    return 0;
}

/* Merge migrated duplicate state without creating an extra active flow. */
static void merge_existing_flow(ft_flow_entry_t *entry,
                                const ft_flow_entry_t *source) {
    if (source->created_at < entry->created_at)
        entry->created_at = source->created_at;
    if (source->last_seen > entry->last_seen) {
        entry->last_seen = source->last_seen;
        entry->rule_id = source->rule_id;
        entry->action = source->action;
    }
    for (unsigned int i = 0;
         i < sizeof(entry->packets) / sizeof(entry->packets[0]); i++) {
        entry->packets[i] += source->packets[i];
        entry->bytes[i] += source->bytes[i];
    }
}

/* Insert migrated flow state without increasing the created-flow counter. */
int ft_flow_table_insert_existing(ft_flow_table_t *table,
                                  const ft_flow_entry_t *source) {
    ft_flow_entry_t *entry;
    uint32_t index;

    if (table == NULL || source == NULL || !source->in_use)
        return -EINVAL;
    entry = ft_flow_table_lookup(table, &source->key);
    if (entry != NULL) {
        merge_existing_flow(entry, source);
        return 1;
    }
    if (table->free_top == 0)
        return -ENOSPC;

    index = table->free_stack[--table->free_top];
    entry = &table->entries[index];
    *entry = *source;
    entry->in_use = 1;
    if (rte_hash_add_key_data(table->hash, &entry->key, entry) < 0) {
        memset(entry, 0, sizeof(*entry));
        table->free_stack[table->free_top++] = index;
        return -EIO;
    }
    table->active++;
    return 0;
}

/* Remove a migrated source entry without counting it as a real delete. */
int ft_flow_table_delete_migrated(ft_flow_table_t *table,
                                  const ft_flow_key_t *key) {
    ft_flow_entry_t *entry;
    uint32_t index;

    entry = ft_flow_table_lookup(table, key);
    if (entry == NULL)
        return -ENOENT;
    if (rte_hash_del_key(table->hash, key) < 0)
        return -EIO;
    index = (uint32_t)(entry - table->entries);
    memset(entry, 0, sizeof(*entry));
    table->free_stack[table->free_top++] = index;
    table->active--;
    return 0;
}

/* Scan a bounded slice of the table and expire stale flow entries. */
uint32_t ft_flow_table_age(ft_flow_table_t *table,
                  uint64_t now,
                  uint64_t timeout_cycles,
                  uint32_t budget) {
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
        if (entry->in_use &&
            ft_elapsed_cycles(now, entry->last_seen) > timeout_cycles) {
            ft_flow_key_t key = entry->key;
            if (ft_flow_table_delete(table, &key, true) == 0)
                deleted++;
        }
    }
    return deleted;
}
