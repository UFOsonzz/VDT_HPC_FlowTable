#include "ft_pipeline.h"

#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_errno.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_lcore.h>
#include <rte_mempool.h>
#include <rte_pause.h>
#include <rte_ring.h>

#define FT_PUBLISH_INTERVAL 4096U
#define FT_RULE_RETIRED_MAX 64U
#define FT_CONTROL_INTERVAL 4096U

static volatile sig_atomic_t force_quit;
static volatile sig_atomic_t reload_signal;
static volatile sig_atomic_t scale_up_signal;
static volatile sig_atomic_t scale_down_signal;

typedef struct {
    struct rte_hash *hash;
    char name[FT_NAME_LEN];
    uint32_t capacity;
} ft_owner_table_t;

typedef struct {
    _Atomic(ft_rule_set_t *) current;
    ft_rule_set_t *retired[FT_RULE_RETIRED_MAX];
    uint16_t retired_count;
    uint64_t version;
} ft_rule_store_t;

typedef struct {
    ft_worker_t *workers;
    uint16_t worker_count;
    _Atomic uint16_t *active_worker_count;
    ft_rule_store_t *rule_store;
    const char *rule_path;
    _Atomic bool *reload_requested;
    _Atomic int *scale_delta;
    _Atomic bool *print_requested;
    _Atomic bool *stop;
} ft_cli_context_t;

static void handle_signal(int signal_number) {
    (void)signal_number;
    force_quit = 1;
}

static void handle_reload_signal(int signal_number) {
    (void)signal_number;
    reload_signal = 1;
}

static void handle_scale_up_signal(int signal_number) {
    (void)signal_number;
    scale_up_signal = 1;
}

static void handle_scale_down_signal(int signal_number) {
    (void)signal_number;
    scale_down_signal = 1;
}

static ft_rule_set_t *load_rule_snapshot(const char *path) {
    ft_rule_set_t *rules = calloc(1, sizeof(*rules));

    if (rules == NULL)
        return NULL;
    if (ft_rule_set_load(rules, path) != 0) {
        free(rules);
        return NULL;
    }
    return rules;
}

static int rule_store_init(ft_rule_store_t *store, const char *path) {
    ft_rule_set_t *rules;

    memset(store, 0, sizeof(*store));
    rules = load_rule_snapshot(path);
    if (rules == NULL)
        return -1;
    atomic_init(&store->current, rules);
    store->version = 1;
    return 0;
}

static ft_rule_set_t *rule_store_current(ft_rule_store_t *store) {
    return atomic_load_explicit(&store->current, memory_order_acquire);
}

static int rule_store_reload(ft_rule_store_t *store, const char *path) {
    ft_rule_set_t *new_rules;
    ft_rule_set_t *old_rules;

    if (store->retired_count == FT_RULE_RETIRED_MAX)
        return -1;
    new_rules = load_rule_snapshot(path);
    if (new_rules == NULL)
        return -1;
    old_rules = atomic_exchange_explicit(&store->current, new_rules,
                                         memory_order_acq_rel);
    if (old_rules != NULL)
        store->retired[store->retired_count++] = old_rules;
    store->version++;
    printf("rules reloaded: path=%s version=%" PRIu64 " count=%u\n",
           path, store->version, new_rules->count);
    return 0;
}

static void rule_store_destroy(ft_rule_store_t *store) {
    ft_rule_set_t *current = rule_store_current(store);

    free(current);
    for (uint16_t i = 0; i < store->retired_count; i++)
        free(store->retired[i]);
    memset(store, 0, sizeof(*store));
}

static int owner_table_create(ft_owner_table_t *table,
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

static void owner_table_destroy(ft_owner_table_t *table) {
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
    selected = (uint16_t)(ft_flow_hash(key) % active_worker_count);
    data = (void *)(uintptr_t)(selected + 1U);
    if (rte_hash_add_key_data(table->hash, key, data) < 0)
        return -1;
    if (created != NULL)
        *created = true;
    *worker_id = selected;
    return 0;
}

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

static const char *traffic_name(ft_traffic_class_t traffic_class) {
    switch (traffic_class) {
    case FT_TRAFFIC_HTTP:
        return "HTTP";
    case FT_TRAFFIC_HTTPS:
        return "HTTPS";
    case FT_TRAFFIC_DNS:
        return "DNS";
    case FT_TRAFFIC_TCP:
        return "TCP";
    case FT_TRAFFIC_UDP:
        return "UDP";
    case FT_TRAFFIC_OTHER:
    default:
        return "OTHER";
    }
}

static void print_live_stats(const ft_worker_t *workers,
                             uint16_t worker_count,
                             uint16_t active_worker_count,
                             uint64_t dispatched,
                             const ft_rule_set_t *rules) {
    uint64_t packets = 0;
    uint64_t bytes = 0;
    uint64_t forwarded = 0;
    uint64_t dropped = 0;
    uint64_t active_flows = 0;
    uint64_t created_flows = 0;
    uint64_t deleted_flows = 0;
    uint64_t timed_out_flows = 0;
    uint64_t rule_hits = 0;

    for (uint16_t i = 0; i < worker_count; i++) {
        packets += atomic_load_explicit(&workers[i].published.packets,
                                        memory_order_relaxed);
        bytes += atomic_load_explicit(&workers[i].published.bytes,
                                      memory_order_relaxed);
        forwarded += atomic_load_explicit(&workers[i].published.forwarded,
                                          memory_order_relaxed);
        dropped += atomic_load_explicit(&workers[i].published.dropped,
                                        memory_order_relaxed);
        active_flows += atomic_load_explicit(&workers[i].published.active_flows,
                                             memory_order_relaxed);
        created_flows += atomic_load_explicit(&workers[i].published.created_flows,
                                              memory_order_relaxed);
        deleted_flows += atomic_load_explicit(&workers[i].published.deleted_flows,
                                              memory_order_relaxed);
        timed_out_flows += atomic_load_explicit(
            &workers[i].published.timed_out_flows, memory_order_relaxed);
        for (uint16_t rule_id = 0; rule_id < FT_MAX_RULES; rule_id++)
            rule_hits += workers[i].local.rule_hits[rule_id];
    }
    printf("live active_workers=%u launched_workers=%u dispatched=%" PRIu64
           " processed=%" PRIu64 " bytes=%" PRIu64
           " forwarded=%" PRIu64 " dropped=%" PRIu64
           " active_flows=%" PRIu64 " created=%" PRIu64
           " deleted=%" PRIu64 " timed_out=%" PRIu64
           " rule_hits=%" PRIu64 " rules=%u\n",
           active_worker_count, worker_count, dispatched, packets, bytes,
           forwarded, dropped, active_flows, created_flows, deleted_flows,
           timed_out_flows, rule_hits, rules == NULL ? 0 : rules->count);
}

static void request_scale(_Atomic int *scale_delta, int delta) {
    atomic_fetch_add_explicit(scale_delta, delta, memory_order_release);
}

static void *cli_loop(void *argument) {
    ft_cli_context_t *context = argument;
    char line[128];

    printf("flowtable cli ready: help, show, rules, reload, scale up, scale down, quit\n");
    while (!atomic_load_explicit(context->stop, memory_order_acquire) &&
           fgets(line, sizeof(line), stdin) != NULL) {
        if (strncmp(line, "help", 4) == 0) {
            printf("commands: show | rules | reload | scale up | scale down | quit\n");
        } else if (strncmp(line, "show", 4) == 0) {
            atomic_store_explicit(context->print_requested, true,
                                  memory_order_release);
        } else if (strncmp(line, "rules", 5) == 0) {
            ft_rule_set_t *rules = rule_store_current(context->rule_store);
            printf("rules version=%" PRIu64 " count=%u path=%s\n",
                   context->rule_store->version,
                   rules == NULL ? 0 : rules->count,
                   context->rule_path);
        } else if (strncmp(line, "reload", 6) == 0) {
            atomic_store_explicit(context->reload_requested, true,
                                  memory_order_release);
        } else if (strncmp(line, "scale up", 8) == 0) {
            request_scale(context->scale_delta, 1);
        } else if (strncmp(line, "scale down", 10) == 0) {
            request_scale(context->scale_delta, -1);
        } else if (strncmp(line, "quit", 4) == 0) {
            force_quit = 1;
            atomic_store_explicit(context->stop, true, memory_order_release);
            break;
        } else {
            printf("unknown command: %s", line);
        }
    }
    return NULL;
}

static void apply_control_events(const ft_app_config_t *config,
                                 ft_worker_t *workers,
                                 uint16_t worker_count,
                                 _Atomic uint16_t *active_worker_count,
                                 ft_rule_store_t *rule_store,
                                 _Atomic bool *reload_requested,
                                 _Atomic int *scale_delta,
                                 _Atomic bool *print_requested,
                                 uint64_t dispatched) {
    int delta;
    uint16_t active;

    if (reload_signal) {
        reload_signal = 0;
        atomic_store_explicit(reload_requested, true, memory_order_release);
    }
    if (scale_up_signal) {
        scale_up_signal = 0;
        request_scale(scale_delta, 1);
    }
    if (scale_down_signal) {
        scale_down_signal = 0;
        request_scale(scale_delta, -1);
    }
    if (atomic_exchange_explicit(reload_requested, false,
                                 memory_order_acq_rel)) {
        if (rule_store_reload(rule_store, config->rule_path) != 0)
            fprintf(stderr, "rule reload failed: %s\n", config->rule_path);
    }
    delta = atomic_exchange_explicit(scale_delta, 0, memory_order_acq_rel);
    if (delta != 0) {
        uint16_t previous;

        active = atomic_load_explicit(active_worker_count, memory_order_acquire);
        previous = active;
        while (delta > 0 && active < worker_count) {
            active++;
            delta--;
        }
        while (delta < 0 && active > 1) {
            active--;
            delta++;
        }
        atomic_store_explicit(active_worker_count, active,
                              memory_order_release);
        if (active != previous)
            printf("active worker count now %u/%u\n", active, worker_count);
    }
    if (atomic_exchange_explicit(print_requested, false,
                                 memory_order_acq_rel)) {
        active = atomic_load_explicit(active_worker_count, memory_order_acquire);
        print_live_stats(workers, worker_count, active, dispatched,
                         rule_store_current(rule_store));
    }
}

static void finish_packet(ft_worker_t *worker, ft_work_item_t *item, ft_action_t action) {
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
    rte_mempool_put(worker->work_pool, item);
}

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
        rte_mempool_put(worker->work_pool, item);
        return;
    }
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

static int worker_loop(void *argument) {
    ft_worker_t *worker = argument;
    void *objects[64];
    uint64_t next_age = rte_get_tsc_cycles() + rte_get_tsc_hz();

    while (!atomic_load_explicit(worker->stop, memory_order_acquire) ||
           !rte_ring_empty(worker->input)) {
        unsigned int count =
            rte_ring_sc_dequeue_burst(worker->input, objects, RTE_DIM(objects),
                                      NULL);
        if (count == 0) {
            rte_pause();
        } else {
            for (unsigned int i = 0; i < count; i++)
                process_item(worker, objects[i]);
            if ((worker->local.packets & (FT_PUBLISH_INTERVAL - 1)) == 0)
                publish_stats(worker);
        }
        if (rte_get_tsc_cycles() >= next_age) {
            ft_flow_table_age(&worker->flow_table, rte_get_tsc_cycles(),
                              worker->timeout_cycles, worker->aging_budget);
            publish_stats(worker);
            next_age = rte_get_tsc_cycles() + rte_get_tsc_hz();
        }
    }
    publish_stats(worker);
    return 0;
}

static void generate_synthetic_packet(uint64_t packet_index,
                          uint32_t flow_count,
                          ft_packet_t *packet) {
    uint32_t flow_id = (uint32_t)((packet_index / 2) % flow_count);
    uint32_t client_ip = 0x0a000001U + flow_id;
    uint32_t server_ip;
    uint16_t server_port;
    uint8_t protocol;
    bool downlink = (packet_index & 1U) != 0;

    switch (flow_id % 6U) {
    case 0:
        server_ip = 0x8efa0001U + (flow_id & 0xffU); /* 142.250.0.0/15 */
        server_port = 443;
        protocol = IPPROTO_TCP;
        break;
    case 1:
        server_ip = 0x1f0d4001U + (flow_id & 0xffU); /* 31.13.64.0/18 */
        server_port = 443;
        protocol = IPPROTO_TCP;
        break;
    case 2:
        server_ip = 0xcb007101U + (flow_id & 0xffU); /* 203.0.113.0/24 */
        server_port = 80;
        protocol = IPPROTO_TCP;
        break;
    case 3:
        server_ip = 0x08080808U;
        server_port = 53;
        protocol = IPPROTO_UDP;
        break;
    case 4:
        server_ip = 0xc6336401U + (flow_id & 0xffU); /* 198.51.100.0/24 */
        server_port = 22;
        protocol = IPPROTO_TCP;
        break;
    default:
        server_ip = 0xc0000201U + (flow_id & 0xffU); /* 192.0.2.0/24 */
        server_port = 123;
        protocol = IPPROTO_UDP;
        break;
    }
    memset(packet, 0, sizeof(*packet));
    packet->ingress_port = FT_INGRESS_PORT_UNKNOWN;
    packet->src_ip = downlink ? server_ip : client_ip;
    packet->dst_ip = downlink ? client_ip : server_ip;
    packet->src_port = downlink ? server_port : (uint16_t)(1024 + flow_id % 50000);
    packet->dst_port = downlink ? (uint16_t)(1024 + flow_id % 50000) : server_port;
    packet->vlan_id = 0;
    packet->protocol = protocol;
    packet->packet_len = (uint16_t)(64 + (flow_id % 1400));
    packet->timestamp = rte_get_tsc_cycles();
}

static int create_worker(ft_worker_t *worker,
              uint16_t worker_id,
              unsigned int lcore_id,
              const ft_app_config_t *config,
              _Atomic(ft_rule_set_t *) *rules_ref,
              _Atomic bool *stop) {
    char name[FT_NAME_LEN];
    uint32_t pool_size = config->ring_size * 2;

    memset(worker, 0, sizeof(*worker));
    worker->worker_id = worker_id;
    worker->lcore_id = lcore_id;
    worker->socket_id = rte_lcore_to_socket_id(lcore_id);
    worker->rules_ref = rules_ref;
    worker->stop = stop;
    worker->timeout_cycles = config->timeout_seconds * rte_get_tsc_hz();
    worker->aging_budget = config->aging_budget;

    snprintf(name, sizeof(name), "ft_ring_%u", worker_id);
    worker->input = rte_ring_create(name, config->ring_size,
                                    worker->socket_id,
                                    RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (worker->input == NULL)
        return -1;
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

static void destroy_worker(ft_worker_t *worker) {
    ft_flow_table_destroy(&worker->flow_table);
    if (worker->input != NULL)
        rte_ring_free(worker->input);
    if (worker->work_pool != NULL)
        rte_mempool_free(worker->work_pool);
}

static void print_summary(const ft_worker_t *workers,
              uint16_t worker_count,
              uint16_t active_worker_count,
              uint64_t dispatched,
              uint64_t elapsed_cycles,
              const ft_rule_set_t *rules) {
    uint64_t packets = 0;
    uint64_t forwarded = 0;
    uint64_t dropped = 0;
    uint64_t active = 0;
    uint64_t created = 0;
    uint64_t traffic[FT_TRAFFIC_CLASS_COUNT] = {0};
    uint64_t rule_hits[FT_MAX_RULES] = {0};
    double seconds = (double)elapsed_cycles / (double)rte_get_tsc_hz();

    for (uint16_t i = 0; i < worker_count; i++) {
        packets += workers[i].local.packets;
        forwarded += workers[i].local.forwarded;
        dropped += workers[i].local.dropped;
        active += workers[i].flow_table.active;
        created += workers[i].flow_table.created;
        for (uint16_t traffic_id = 0; traffic_id < FT_TRAFFIC_CLASS_COUNT;
             traffic_id++)
            traffic[traffic_id] += workers[i].local.traffic[traffic_id];
        for (uint16_t rule_id = 0; rule_id < FT_MAX_RULES; rule_id++)
            rule_hits[rule_id] += workers[i].local.rule_hits[rule_id];
        printf("worker=%u lcore=%u socket=%d packets=%" PRIu64
               " active_flows=%u created=%" PRIu64 "\n",
               workers[i].worker_id, workers[i].lcore_id, workers[i].socket_id,
               workers[i].local.packets, workers[i].flow_table.active,
               workers[i].flow_table.created);
    }
    printf("summary active_workers=%u launched_workers=%u"
           " dispatched=%" PRIu64 " processed=%" PRIu64
           " forwarded=%" PRIu64 " dropped=%" PRIu64
           " active_flows=%" PRIu64 " created_flows=%" PRIu64
           " seconds=%.6f pps=%.0f\n",
           active_worker_count, worker_count, dispatched, packets, forwarded,
           dropped, active, created, seconds,
           seconds > 0.0 ? (double)packets / seconds : 0.0);
    for (uint16_t traffic_id = 0; traffic_id < FT_TRAFFIC_CLASS_COUNT;
         traffic_id++) {
        if (traffic[traffic_id] != 0)
            printf("traffic %s=%" PRIu64 "\n",
                   traffic_name((ft_traffic_class_t)traffic_id),
                   traffic[traffic_id]);
    }
    if (rules != NULL) {
        for (uint16_t rule_id = 0; rule_id < rules->count; rule_id++) {
            if (rule_hits[rule_id] != 0)
                printf("rule_hit id=%u name=%s action=%s hits=%" PRIu64 "\n",
                       rule_id, rules->rules[rule_id].name,
                       ft_action_name(rules->rules[rule_id].action),
                       rule_hits[rule_id]);
        }
    }
}

int ft_pipeline_run_synthetic(const ft_app_config_t *config) {
    ft_direction_config_t directions;
    ft_rule_store_t rule_store;
    ft_owner_table_t owners;
    ft_worker_t workers[FT_MAX_WORKERS];
    unsigned int lcore_ids[FT_MAX_WORKERS];
    unsigned int lcore_id;
    _Atomic bool stop = false;
    _Atomic bool reload_requested = false;
    _Atomic bool print_requested = false;
    _Atomic int scale_delta = 0;
    _Atomic uint16_t active_worker_count;
    uint16_t available = 0;
    uint16_t launched_workers = config->max_worker_count;
    uint32_t owner_capacity;
    uint64_t start = rte_get_tsc_cycles();
    uint64_t end = start;
    uint64_t next_stats = 0;
    uint64_t dispatched = 0;
    bool launch_ok = true;
    int result = -1;

    memset(&owners, 0, sizeof(owners));
    atomic_init(&active_worker_count, config->worker_count);
    if (ft_direction_config_load(&directions, config->direction_path) != 0 ||
        rule_store_init(&rule_store, config->rule_path) != 0) {
        fprintf(stderr, "Cannot load direction or SPI rule configuration\n");
        return -1;
    }
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (available < FT_MAX_WORKERS)
            lcore_ids[available++] = lcore_id;
    }
    if (launched_workers == 0)
        launched_workers = config->worker_count;
    if (launched_workers > available) {
        fprintf(stderr, "Need %u worker lcores, but EAL provided %u\n",
                launched_workers, available);
        goto cleanup_rules;
    }
    owner_capacity = config->flow_capacity_per_worker * launched_workers * 2U;
    if (owner_capacity < config->synthetic_flow_count * 2U)
        owner_capacity = config->synthetic_flow_count * 2U;
    if (owner_table_create(&owners, "ft_owner_synthetic", owner_capacity,
                           rte_socket_id()) != 0) {
        fprintf(stderr, "Cannot create dispatcher owner table\n");
        goto cleanup_rules;
    }
    memset(workers, 0, sizeof(workers));
    for (uint16_t i = 0; i < launched_workers; i++) {
        if (create_worker(&workers[i], i, lcore_ids[i], config,
                          &rule_store.current, &stop) != 0) {
            fprintf(stderr, "Cannot create worker %u\n", i);
            goto cleanup;
        }
    }
    for (uint16_t i = 0; i < launched_workers; i++) {
        if (rte_eal_remote_launch(worker_loop, &workers[i],
                                  workers[i].lcore_id) != 0) {
            fprintf(stderr, "Cannot launch worker %u\n", i);
            launch_ok = false;
            atomic_store(&stop, true);
            goto wait;
        }
    }

    start = rte_get_tsc_cycles();
    if (config->stats_interval_seconds != 0)
        next_stats = start + config->stats_interval_seconds * rte_get_tsc_hz();
    for (uint64_t i = 0; i < config->packet_count; i++) {
        ft_packet_t packet;
        ft_normalized_flow_t normalized;
        ft_work_item_t *item;
        uint16_t worker_id;

        generate_synthetic_packet(i, config->synthetic_flow_count, &packet);
        ft_packet_normalize(&packet, &directions, &normalized);
        if (owner_table_get_or_create(&owners, &normalized.key,
                                      atomic_load_explicit(
                                          &active_worker_count,
                                          memory_order_acquire),
                                      &worker_id, NULL) != 0)
            continue;
        while (rte_mempool_get(workers[worker_id].work_pool,
                               (void **)&item) != 0)
            rte_pause();
        item->packet = packet;
        item->normalized = normalized;
        while (rte_ring_sp_enqueue(workers[worker_id].input, item) != 0)
            rte_pause();
        dispatched++;
        if (config->scale_interval_packets != 0 &&
            dispatched % config->scale_interval_packets == 0)
            request_scale(&scale_delta, 1);
        if ((dispatched & (FT_CONTROL_INTERVAL - 1U)) == 0)
            apply_control_events(config, workers, launched_workers,
                                 &active_worker_count, &rule_store,
                                 &reload_requested, &scale_delta,
                                 &print_requested, dispatched);
        if (next_stats != 0 && rte_get_tsc_cycles() >= next_stats) {
            print_live_stats(workers, launched_workers,
                             atomic_load_explicit(&active_worker_count,
                                                  memory_order_acquire),
                             dispatched, rule_store_current(&rule_store));
            next_stats = rte_get_tsc_cycles() +
                         config->stats_interval_seconds * rte_get_tsc_hz();
        }
    }
    apply_control_events(config, workers, launched_workers,
                         &active_worker_count, &rule_store,
                         &reload_requested, &scale_delta,
                         &print_requested, dispatched);
    atomic_store_explicit(&stop, true, memory_order_release);

wait:
    for (uint16_t i = 0; i < launched_workers; i++) {
        if (rte_eal_get_lcore_state(workers[i].lcore_id) != WAIT)
            rte_eal_wait_lcore(workers[i].lcore_id);
    }
    end = rte_get_tsc_cycles();
    print_summary(workers, launched_workers,
                  atomic_load_explicit(&active_worker_count,
                                       memory_order_acquire),
                  dispatched, end - start, rule_store_current(&rule_store));
    result = launch_ok ? 0 : -1;

cleanup:
    for (uint16_t i = 0; i < launched_workers; i++)
        destroy_worker(&workers[i]);
    owner_table_destroy(&owners);
cleanup_rules:
    rule_store_destroy(&rule_store);
    return result;
}

static int configure_ethdev(uint16_t port_id,
                 uint16_t worker_count,
                 bool tx_enabled,
                 struct rte_mempool **mbuf_pool) {
    struct rte_eth_dev_info info;
    struct rte_eth_conf port_conf;
    uint16_t rx_desc = 1024;
    uint16_t tx_desc = 1024;
    uint16_t tx_queues = tx_enabled ? worker_count : 1;
    int socket_id = rte_eth_dev_socket_id(port_id);
    int result;

    if (!rte_eth_dev_is_valid_port(port_id))
        return -1;
    if (socket_id < 0)
        socket_id = rte_socket_id();
    memset(&port_conf, 0, sizeof(port_conf));
    result = rte_eth_dev_info_get(port_id, &info);
    if (result != 0) {
        fprintf(stderr, "rte_eth_dev_info_get: %s (%d)\n",
                rte_strerror(-result), result);
        return result;
    }
    result = rte_eth_dev_configure(port_id, 1, tx_queues, &port_conf);
    if (result != 0) {
        fprintf(stderr, "rte_eth_dev_configure: %s (%d)\n",
                rte_strerror(-result), result);
        return result;
    }
    rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &rx_desc, &tx_desc);
    *mbuf_pool = rte_pktmbuf_pool_create("ft_rx_mbuf_pool",
                                         8192, 256, 0,
                                         RTE_MBUF_DEFAULT_BUF_SIZE,
                                         socket_id);
    if (*mbuf_pool == NULL) {
        fprintf(stderr, "rte_pktmbuf_pool_create: %s\n",
                rte_strerror(rte_errno));
        return -1;
    }
    result = rte_eth_rx_queue_setup(port_id, 0, rx_desc, socket_id,
                                    &info.default_rxconf, *mbuf_pool);
    if (result != 0) {
        fprintf(stderr, "rte_eth_rx_queue_setup: %s (%d)\n",
                rte_strerror(-result), result);
        return result;
    }
    for (uint16_t i = 0; i < tx_queues; i++) {
        result = rte_eth_tx_queue_setup(port_id, i, tx_desc, socket_id,
                                        &info.default_txconf);
        if (result != 0) {
            fprintf(stderr, "rte_eth_tx_queue_setup: %s (%d)\n",
                    rte_strerror(-result), result);
            return result;
        }
    }
    result = rte_eth_dev_start(port_id);
    if (result != 0) {
        fprintf(stderr, "rte_eth_dev_start: %s (%d)\n",
                rte_strerror(-result), result);
        return result;
    }
    rte_eth_promiscuous_enable(port_id);
    return 0;
}

int ft_pipeline_run_ethdev(const ft_app_config_t *config) {
    ft_direction_config_t directions;
    ft_rule_store_t rule_store;
    ft_owner_table_t owners;
    pthread_t cli_thread;
    ft_cli_context_t cli_context;
    ft_worker_t workers[FT_MAX_WORKERS];
    unsigned int lcore_ids[FT_MAX_WORKERS];
    unsigned int lcore_id;
    struct rte_mempool *mbuf_pool = NULL;
    struct rte_mbuf *mbufs[64];
    _Atomic bool stop = false;
    _Atomic bool reload_requested = false;
    _Atomic bool print_requested = false;
    _Atomic int scale_delta = 0;
    _Atomic uint16_t active_worker_count;
    uint16_t available = 0;
    uint16_t launched_workers = config->max_worker_count;
    uint32_t owner_capacity;
    uint64_t start = rte_get_tsc_cycles();
    uint64_t end = start;
    uint64_t next_stats = 0;
    uint64_t dispatched = 0;
    bool launch_ok = true;
    int result = -1;

    memset(&owners, 0, sizeof(owners));
    memset(&cli_context, 0, sizeof(cli_context));
    atomic_init(&active_worker_count, config->worker_count);
    if (ft_direction_config_load(&directions, config->direction_path) != 0 ||
        rule_store_init(&rule_store, config->rule_path) != 0)
        return -1;
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (available < FT_MAX_WORKERS)
            lcore_ids[available++] = lcore_id;
    }
    if (launched_workers == 0)
        launched_workers = config->worker_count;
    if (launched_workers > available)
        goto cleanup_rules;
    memset(workers, 0, sizeof(workers));
    if (configure_ethdev(config->port_id, launched_workers,
                         config->tx_enabled, &mbuf_pool) != 0) {
        fprintf(stderr, "Cannot configure ethdev port %u\n", config->port_id);
        goto cleanup;
    }
    owner_capacity = config->flow_capacity_per_worker * launched_workers * 2U;
    if (owner_table_create(&owners, "ft_owner_ethdev", owner_capacity,
                           rte_socket_id()) != 0) {
        fprintf(stderr, "Cannot create dispatcher owner table\n");
        goto cleanup;
    }
    for (uint16_t i = 0; i < launched_workers; i++) {
        if (create_worker(&workers[i], i, lcore_ids[i], config,
                          &rule_store.current, &stop) != 0)
            goto cleanup;
        workers[i].tx_enabled = config->tx_enabled;
        workers[i].tx_port = config->port_id;
        workers[i].tx_queue = i;
    }
    for (uint16_t i = 0; i < launched_workers; i++) {
        if (rte_eal_remote_launch(worker_loop, &workers[i],
                                  workers[i].lcore_id) != 0) {
            launch_ok = false;
            goto stop_workers;
        }
    }
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_reload_signal);
    signal(SIGUSR1, handle_scale_up_signal);
    signal(SIGUSR2, handle_scale_down_signal);
    force_quit = 0;
    reload_signal = 0;
    scale_up_signal = 0;
    scale_down_signal = 0;
    if (config->cli_enabled) {
        cli_context.workers = workers;
        cli_context.worker_count = launched_workers;
        cli_context.active_worker_count = &active_worker_count;
        cli_context.rule_store = &rule_store;
        cli_context.rule_path = config->rule_path;
        cli_context.reload_requested = &reload_requested;
        cli_context.scale_delta = &scale_delta;
        cli_context.print_requested = &print_requested;
        cli_context.stop = &stop;
        if (pthread_create(&cli_thread, NULL, cli_loop, &cli_context) == 0)
            pthread_detach(cli_thread);
    }
    start = rte_get_tsc_cycles();
    if (config->stats_interval_seconds != 0)
        next_stats = start + config->stats_interval_seconds * rte_get_tsc_hz();
    while (!force_quit &&
           (config->packet_count == 0 || dispatched < config->packet_count)) {
        uint16_t count = rte_eth_rx_burst(config->port_id, 0, mbufs,
                                          RTE_DIM(mbufs));
        if (count == 0) {
            apply_control_events(config, workers, launched_workers,
                                 &active_worker_count, &rule_store,
                                 &reload_requested, &scale_delta,
                                 &print_requested, dispatched);
            rte_pause();
            continue;
        }
        for (uint16_t i = 0; i < count; i++) {
            ft_packet_t packet;
            ft_normalized_flow_t normalized;
            ft_work_item_t *item;
            uint16_t worker_id;

            if (config->packet_count != 0 &&
                dispatched >= config->packet_count) {
                rte_pktmbuf_free(mbufs[i]);
                continue;
            }
            if (ft_packet_parse_mbuf(mbufs[i], &packet) != 0) {
                rte_pktmbuf_free(mbufs[i]);
                continue;
            }
            packet.ingress_port = config->port_id;
            packet.timestamp = rte_get_tsc_cycles();
            ft_packet_normalize(&packet, &directions, &normalized);
            if (owner_table_get_or_create(&owners, &normalized.key,
                                          atomic_load_explicit(
                                              &active_worker_count,
                                              memory_order_acquire),
                                          &worker_id, NULL) != 0) {
                rte_pktmbuf_free(mbufs[i]);
                continue;
            }
            while (rte_mempool_get(workers[worker_id].work_pool,
                                   (void **)&item) != 0)
                rte_pause();
            item->packet = packet;
            item->normalized = normalized;
            while (rte_ring_sp_enqueue(workers[worker_id].input, item) != 0)
                rte_pause();
            dispatched++;
            if ((dispatched & (FT_CONTROL_INTERVAL - 1U)) == 0)
                apply_control_events(config, workers, launched_workers,
                                     &active_worker_count, &rule_store,
                                     &reload_requested, &scale_delta,
                                     &print_requested, dispatched);
            if (next_stats != 0 && rte_get_tsc_cycles() >= next_stats) {
                print_live_stats(workers, launched_workers,
                                 atomic_load_explicit(&active_worker_count,
                                                      memory_order_acquire),
                                 dispatched, rule_store_current(&rule_store));
                next_stats = rte_get_tsc_cycles() +
                             config->stats_interval_seconds * rte_get_tsc_hz();
            }
        }
    }

stop_workers:
    atomic_store_explicit(&stop, true, memory_order_release);
    for (uint16_t i = 0; i < launched_workers; i++) {
        if (rte_eal_get_lcore_state(workers[i].lcore_id) != WAIT)
            rte_eal_wait_lcore(workers[i].lcore_id);
    }
    end = rte_get_tsc_cycles();
    print_summary(workers, launched_workers,
                  atomic_load_explicit(&active_worker_count,
                                       memory_order_acquire),
                  dispatched, end - start, rule_store_current(&rule_store));
    result = launch_ok ? 0 : -1;

cleanup:
    for (uint16_t i = 0; i < launched_workers; i++)
        destroy_worker(&workers[i]);
    owner_table_destroy(&owners);
    if (rte_eth_dev_is_valid_port(config->port_id)) {
        rte_eth_dev_stop(config->port_id);
        rte_eth_dev_close(config->port_id);
    }
    if (mbuf_pool != NULL)
        rte_mempool_free(mbuf_pool);
cleanup_rules:
    rule_store_destroy(&rule_store);
    return result;
}
