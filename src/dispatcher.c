#include "pipeline_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_mempool.h>
#include <rte_pause.h>
#include <rte_ring.h>

int ft_owner_table_create(ft_owner_table_t *table,
                          const char *name,
                          uint32_t capacity,
                          int socket_id) {
    struct rte_hash_parameters params;

    if (table == NULL || name == NULL || capacity == 0)
        return -1;
    memset(table, 0, sizeof(*table));
    snprintf(table->name, sizeof(table->name), "%s", name);
    table->capacity = capacity;
    memset(&params, 0, sizeof(params));
    params.name = table->name;
    params.entries = capacity;
    params.key_len = sizeof(ft_flow_key_t);
    params.hash_func = rte_jhash;
    params.hash_func_init_val = 0x9e3779b9U;
    params.socket_id = socket_id;
    params.extra_flag = RTE_HASH_EXTRA_FLAGS_EXT_TABLE;
    table->hash = rte_hash_create(&params);
    return table->hash == NULL ? -1 : 0;
}

void ft_owner_table_destroy(ft_owner_table_t *table) {
    if (table == NULL)
        return;
    if (table->hash != NULL)
        rte_hash_free(table->hash);
    memset(table, 0, sizeof(*table));
}

static int owner_table_get_or_create(ft_owner_table_t *table,
                                     const ft_flow_key_t *key,
                                     uint16_t active_worker_count,
                                     uint16_t *worker_id,
                                     bool *created) {
    void *data = NULL;
    uint16_t selected;

    if (created != NULL)
        *created = false;
    if (table == NULL || table->hash == NULL || key == NULL ||
        active_worker_count == 0 || worker_id == NULL)
        return -1;
    if (rte_hash_lookup_data(table->hash, key, &data) >= 0) {
        *worker_id = (uint16_t)((uintptr_t)data - 1U);
        return 0;
    }

    /* Dynamic mode pins a flow to its first selected worker. */
    selected = (uint16_t)(ft_flow_hash(key) % active_worker_count);
    data = (void *)(uintptr_t)(selected + 1U);
    if (rte_hash_add_key_data(table->hash, key, data) < 0)
        return -1;
    if (created != NULL)
        *created = true;
    *worker_id = selected;
    return 0;
}

int ft_select_worker(ft_owner_table_t *table,
                     const ft_flow_key_t *key,
                     uint16_t active_worker_count,
                     bool use_owner_map,
                     uint16_t *worker_id) {
    if (key == NULL || active_worker_count == 0 || worker_id == NULL)
        return -1;
    if (use_owner_map)
        return owner_table_get_or_create(table, key, active_worker_count,
                                         worker_id, NULL);

    /* Fixed-worker mode avoids owner-map overhead on the hot path. */
    *worker_id = (uint16_t)(ft_flow_hash(key) % active_worker_count);
    return 0;
}

uint16_t ft_dispatch_burst_size(const ft_app_config_t *config) {
    uint32_t burst_size = config->burst_size;

    if (burst_size == 0 || burst_size > FT_DISPATCH_BURST)
        burst_size = FT_DISPATCH_BURST;
    if (config->ring_size != 0 && burst_size > config->ring_size)
        burst_size = config->ring_size;
    return (uint16_t)(burst_size == 0 ? 1U : burst_size);
}

ft_work_item_t *ft_dispatch_get_item(ft_worker_t *worker,
                                     ft_dispatch_queue_t *queue,
                                     uint16_t burst_size) {
    void *object;

    if (queue->free_count != 0)
        return queue->free_items[--queue->free_count];
    if (burst_size > 1 &&
        rte_mempool_get_bulk(worker->work_pool, queue->free_items,
                             burst_size) == 0) {
        queue->free_count = burst_size;
        return queue->free_items[--queue->free_count];
    }
    while (rte_mempool_get(worker->work_pool, &object) != 0)
        rte_pause();
    return object;
}

static void flush_dispatch_queue(ft_worker_t *worker,
                                 ft_dispatch_queue_t *queue,
                                 uint16_t producer_id) {
    uint16_t offset = 0;

    if (producer_id >= worker->input_count || worker->inputs[producer_id] == NULL)
        return;
    while (offset < queue->pending_count) {
        unsigned int enqueued =
            rte_ring_sp_enqueue_burst(worker->inputs[producer_id],
                                      &queue->pending[offset],
                                      queue->pending_count - offset, NULL);

        if (enqueued == 0)
            rte_pause();
        else
            offset = (uint16_t)(offset + enqueued);
    }
    queue->pending_count = 0;
}

void ft_dispatch_enqueue_item(ft_worker_t *worker,
                              ft_dispatch_queue_t *queue,
                              ft_work_item_t *item,
                              uint16_t burst_size,
                              uint16_t producer_id) {
    queue->pending[queue->pending_count++] = item;
    if (queue->pending_count == burst_size)
        flush_dispatch_queue(worker, queue, producer_id);
}

void ft_flush_dispatch_queues(ft_worker_t *workers,
                              ft_dispatch_queue_t *queues,
                              uint16_t worker_count,
                              uint16_t producer_id) {
    for (uint16_t i = 0; i < worker_count; i++) {
        if (queues[i].pending_count != 0)
            flush_dispatch_queue(&workers[i], &queues[i], producer_id);
    }
}

void ft_return_dispatch_cached_items(ft_worker_t *workers,
                                     ft_dispatch_queue_t *queues,
                                     uint16_t worker_count) {
    for (uint16_t i = 0; i < worker_count; i++) {
        if (queues[i].free_count == 0)
            continue;
        rte_mempool_put_bulk(workers[i].work_pool, queues[i].free_items,
                             queues[i].free_count);
        queues[i].free_count = 0;
    }
}

static bool reserve_dispatched_slot(_Atomic uint64_t *dispatched,
                                    uint64_t packet_limit) {
    uint64_t current;

    if (packet_limit == 0) {
        atomic_fetch_add_explicit(dispatched, 1, memory_order_relaxed);
        return true;
    }
    current = atomic_load_explicit(dispatched, memory_order_acquire);
    while (current < packet_limit) {
        if (atomic_compare_exchange_weak_explicit(dispatched, &current,
                                                  current + 1,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire))
            return true;
    }
    return false;
}

int ft_dispatcher_loop(void *argument) {
    ft_dispatcher_t *dispatcher = argument;
    const ft_app_config_t *config = dispatcher->config;
    struct rte_mbuf *mbufs[64];
    uint64_t local_dispatched = 0;

    while (!force_quit &&
           !atomic_load_explicit(dispatcher->stop, memory_order_acquire) &&
           (config->packet_count == 0 ||
            atomic_load_explicit(dispatcher->dispatched,
                                 memory_order_acquire) < config->packet_count) &&
           (dispatcher->packet_limit == 0 ||
            local_dispatched < dispatcher->packet_limit)) {
        uint16_t count = rte_eth_rx_burst(config->port_id,
                                          dispatcher->rx_queue_id,
                                          mbufs, RTE_DIM(mbufs));
        uint64_t rx_timestamp;

        if (count == 0) {
            ft_flush_dispatch_queues(dispatcher->workers, dispatcher->queues,
                                     dispatcher->worker_count,
                                     dispatcher->producer_id);
            rte_pause();
            continue;
        }

        /* One timestamp per RX burst keeps the hot path cheaper and stable. */
        rx_timestamp = rte_get_tsc_cycles();
        for (uint16_t i = 0; i < count; i++) {
            ft_packet_t packet;
            ft_normalized_flow_t normalized;
            ft_work_item_t *item;
            uint16_t worker_id;

            if (config->packet_count != 0 &&
                atomic_load_explicit(dispatcher->dispatched,
                                     memory_order_acquire) >=
                    config->packet_count) {
                rte_pktmbuf_free(mbufs[i]);
                continue;
            }
            if (dispatcher->packet_limit != 0 &&
                local_dispatched >= dispatcher->packet_limit) {
                rte_pktmbuf_free(mbufs[i]);
                continue;
            }
            if (ft_packet_parse_mbuf(mbufs[i], &packet) != 0) {
                rte_pktmbuf_free(mbufs[i]);
                continue;
            }

            /* Normalize before hashing so both packet directions share a worker. */
            packet.ingress_port = config->port_id;
            packet.timestamp = rx_timestamp;
            ft_packet_normalize(&packet, dispatcher->directions, &normalized);
            if (ft_select_worker(dispatcher->owners, &normalized.key,
                                 atomic_load_explicit(
                                     dispatcher->active_worker_count,
                                     memory_order_acquire),
                                 dispatcher->use_owner_map,
                                 &worker_id) != 0) {
                rte_pktmbuf_free(mbufs[i]);
                continue;
            }
            if (!reserve_dispatched_slot(dispatcher->dispatched,
                                         config->packet_count)) {
                rte_pktmbuf_free(mbufs[i]);
                continue;
            }
            local_dispatched++;
            item = ft_dispatch_get_item(&dispatcher->workers[worker_id],
                                        &dispatcher->queues[worker_id],
                                        dispatcher->dispatch_burst);
            item->packet = packet;
            item->normalized = normalized;
            ft_dispatch_enqueue_item(&dispatcher->workers[worker_id],
                                     &dispatcher->queues[worker_id], item,
                                     dispatcher->dispatch_burst,
                                     dispatcher->producer_id);
        }
    }
    ft_flush_dispatch_queues(dispatcher->workers, dispatcher->queues,
                             dispatcher->worker_count,
                             dispatcher->producer_id);
    ft_return_dispatch_cached_items(dispatcher->workers, dispatcher->queues,
                                    dispatcher->worker_count);
    return 0;
}
