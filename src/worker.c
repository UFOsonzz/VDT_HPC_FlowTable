#include "pipeline_internal.h"

#include <stdio.h>
#include <string.h>

#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mempool.h>
#include <rte_pause.h>
#include <rte_ring.h>

/* Copy local hot-path counters into atomics readable by control/CLI code. */
static void publish_stats(ft_worker_t *worker) {
    atomic_store_explicit(&worker->published.packets,
                          worker->local.packets, memory_order_relaxed);
    atomic_store_explicit(&worker->published.bytes,
                          worker->local.bytes, memory_order_relaxed);
    atomic_store_explicit(&worker->published.forwarded,
                          worker->local.forwarded, memory_order_relaxed);
    atomic_store_explicit(&worker->published.dropped,
                          worker->local.dropped, memory_order_relaxed);
    atomic_store_explicit(&worker->published.active_flows,
                          worker->flow_table.active, memory_order_relaxed);
    atomic_store_explicit(&worker->published.created_flows,
                          worker->flow_table.created, memory_order_relaxed);
    atomic_store_explicit(&worker->published.deleted_flows,
                          worker->flow_table.deleted, memory_order_relaxed);
    atomic_store_explicit(&worker->published.timed_out_flows,
                          worker->flow_table.timed_out, memory_order_relaxed);
}

/* Check whether every dispatcher ring feeding this worker has drained. */
static bool worker_inputs_empty(const ft_worker_t *worker) {
    for (uint16_t i = 0; i < worker->input_count; i++) {
        if (worker->inputs[i] != NULL && !rte_ring_empty(worker->inputs[i]))
            return false;
    }
    return true;
}

/* Cooperatively pause after queues drain so flow migration can be safe. */
static void maybe_pause_worker(ft_worker_t *worker) {
    if (!atomic_load_explicit(&worker->pause_requested, memory_order_acquire))
        return;
    if (!worker_inputs_empty(worker))
        return;

    publish_stats(worker);
    atomic_store_explicit(&worker->paused, true, memory_order_release);
    while (atomic_load_explicit(&worker->pause_requested,
                                memory_order_acquire) &&
           !atomic_load_explicit(worker->stop, memory_order_acquire))
        rte_pause();
    atomic_store_explicit(&worker->paused, false, memory_order_release);
}

/* Apply the cached action and either free or transmit the mbuf. */
static void finish_packet(ft_worker_t *worker,
                          ft_work_item_t *item,
                          ft_action_t action) {
    struct rte_mbuf *mbuf = item->packet.mbuf;

    if (action == FT_ACTION_DROP) {
        worker->local.dropped++;
        if (mbuf != NULL)
            rte_pktmbuf_free(mbuf);
    } else {
        worker->local.forwarded++;
        if (mbuf != NULL && worker->tx_enabled) {
            if (rte_eth_tx_burst(worker->tx_port, worker->tx_queue,
                                 &mbuf, 1) == 0)
                rte_pktmbuf_free(mbuf);
        } else if (mbuf != NULL) {
            rte_pktmbuf_free(mbuf);
        }
    }
}

/* Update flow state, run SPI on first packet, and account traffic counters. */
static void process_item(ft_worker_t *worker, ft_work_item_t *item) {
    const ft_rule_t *rule;
    ft_flow_entry_t *entry;
    ft_action_t action;
    ft_traffic_class_t traffic_class;
    bool created;
    unsigned int direction_index;

    entry = ft_flow_table_get_or_create(&worker->flow_table,
                                        &item->normalized.key,
                                        item->packet.timestamp,
                                        &created);
    if (entry == NULL) {
        worker->local.dropped++;
        if (item->packet.mbuf != NULL)
            rte_pktmbuf_free(item->packet.mbuf);
        return;
    }

    /* SPI is expensive, so match once on flow creation and cache the action. */
    if (created) {
        ft_rule_set_t *rules = atomic_load_explicit(worker->rules_ref,
                                                    memory_order_acquire);

        rule = ft_rule_match(rules, &item->normalized.key);
        entry->rule_id = rule == NULL ? UINT16_MAX : rule->id;
        entry->action = rule == NULL ? FT_ACTION_FORWARD : rule->action;
    }

    entry->last_seen = item->packet.timestamp;
    direction_index =
        item->normalized.direction == FT_DIR_DOWNLINK ? 1U : 0U;
    entry->packets[direction_index]++;
    entry->bytes[direction_index] += item->packet.packet_len;

    action = (ft_action_t)entry->action;
    traffic_class = ft_packet_classify(&item->normalized);
    worker->local.packets++;
    worker->local.bytes += item->packet.packet_len;
    worker->local.direction[item->normalized.direction]++;
    worker->local.traffic[traffic_class]++;
    if (entry->rule_id < FT_MAX_RULES)
        worker->local.rule_hits[entry->rule_id]++;
    finish_packet(worker, item, action);
}

/* Worker lcore loop: drain rings, process bursts, and age stale flows. */
int ft_worker_loop(void *argument) {
    ft_worker_t *worker = argument;
    void *objects[64];
    uint64_t next_age = rte_get_tsc_cycles() + rte_get_tsc_hz();
    uint16_t next_input = 0;

    while (!atomic_load_explicit(worker->stop, memory_order_acquire) ||
           !worker_inputs_empty(worker)) {
        unsigned int count = 0;

        if (worker->pause_enabled)
            maybe_pause_worker(worker);

        /* Multi-dispatcher workers round-robin rings to avoid starving a queue. */
        for (uint16_t scanned = 0; scanned < worker->input_count; scanned++) {
            uint16_t index = (uint16_t)((next_input + scanned) %
                                        worker->input_count);

            count = rte_ring_sc_dequeue_burst(worker->inputs[index], objects,
                                              RTE_DIM(objects), NULL);
            if (count != 0) {
                next_input = (uint16_t)((index + 1) % worker->input_count);
                break;
            }
        }
        if (count == 0) {
            rte_pause();
        } else {
            for (unsigned int i = 0; i < count; i++)
                process_item(worker, objects[i]);
            rte_mempool_put_bulk(worker->work_pool, objects, count);
            if ((worker->local.packets & (FT_PUBLISH_INTERVAL - 1)) == 0)
                publish_stats(worker);
        }

        /* Aging is budgeted so timeout work cannot monopolize the worker loop. */
        uint64_t now = rte_get_tsc_cycles();
        if (now >= next_age) {
            ft_flow_table_age(&worker->flow_table, now, worker->timeout_cycles,
                              worker->aging_budget);
            publish_stats(worker);
            next_age = now + rte_get_tsc_hz();
        }
    }
    publish_stats(worker);
    return 0;
}

/* Allocate worker rings, work-item pool, and the private flow-table shard. */
int ft_worker_create(ft_worker_t *worker,
                     uint16_t worker_id,
                     unsigned int lcore_id,
                     const ft_app_config_t *config,
                     _Atomic(ft_rule_set_t *) *rules_ref,
                     _Atomic bool *stop,
                     uint16_t input_count) {
    char name[FT_NAME_LEN];
    uint32_t pool_size = config->ring_size * 2;

    memset(worker, 0, sizeof(*worker));
    if (input_count == 0 || input_count > FT_MAX_DISPATCHERS)
        return -1;
    worker->worker_id = worker_id;
    worker->lcore_id = lcore_id;
    worker->socket_id = rte_lcore_to_socket_id(lcore_id);
    worker->pause_enabled = !config->fixed_workers;
    atomic_init(&worker->pause_requested, false);
    atomic_init(&worker->paused, false);
    worker->rules_ref = rules_ref;
    worker->stop = stop;
    worker->timeout_cycles = config->timeout_seconds * rte_get_tsc_hz();
    worker->aging_budget = config->aging_budget;
    worker->input_count = input_count;

    for (uint16_t i = 0; i < input_count; i++) {
        snprintf(name, sizeof(name), "ft_ring_%u_%u", worker_id, i);
        worker->inputs[i] = rte_ring_create(name, config->ring_size,
                                            worker->socket_id,
                                            RING_F_SP_ENQ | RING_F_SC_DEQ);
        if (worker->inputs[i] == NULL)
            return -1;
    }
    worker->input = worker->inputs[0];
    snprintf(name, sizeof(name), "ft_work_pool_%u", worker_id);
    worker->work_pool = rte_mempool_create(name, pool_size,
                                           sizeof(ft_work_item_t),
                                           0, 0, NULL, NULL, NULL, NULL,
                                           worker->socket_id, 0);
    if (worker->work_pool == NULL)
        return -1;
    snprintf(name, sizeof(name), "ft_flow_%u", worker_id);
    return ft_flow_table_create(&worker->flow_table, name,
                                config->flow_capacity_per_worker,
                                worker->socket_id);
}

/* Release all per-worker resources after its lcore has stopped. */
void ft_worker_destroy(ft_worker_t *worker) {
    ft_flow_table_destroy(&worker->flow_table);
    for (uint16_t i = 0; i < worker->input_count; i++) {
        if (worker->inputs[i] != NULL)
            rte_ring_free(worker->inputs[i]);
        worker->inputs[i] = NULL;
    }
    worker->input = NULL;
    worker->input_count = 0;
    if (worker->work_pool != NULL)
        rte_mempool_free(worker->work_pool);
}
