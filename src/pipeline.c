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
#define FT_DISPATCH_BURST 64U
#define FT_ANSI_CLEAR "\033[2J\033[H"
#define FT_ANSI_RESET "\033[0m"
#define FT_ANSI_BOLD "\033[1m"
#define FT_ANSI_DIM "\033[2m"
#define FT_ANSI_GREEN "\033[32m"
#define FT_ANSI_RED "\033[31m"
#define FT_ANSI_YELLOW "\033[33m"
#define FT_ANSI_CYAN "\033[36m"

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

typedef enum {
    FT_SHOW_NONE = 0,
    FT_SHOW_STATISTICS,
    FT_SHOW_FLOW,
    FT_SHOW_WORKER,
    FT_SHOW_TRAFFIC,
    FT_SHOW_DASHBOARD
} ft_show_request_t;

typedef struct {
    uint64_t packets;
    uint64_t bytes;
    uint64_t forwarded;
    uint64_t dropped;
    uint64_t active_flows;
    uint64_t created_flows;
    uint64_t deleted_flows;
    uint64_t timed_out_flows;
    uint64_t direction[3];
    uint64_t traffic[FT_TRAFFIC_CLASS_COUNT];
    uint64_t rule_hits[FT_MAX_RULES];
    uint64_t total_rule_hits;
} ft_stats_snapshot_t;

typedef struct {
    uint64_t last_cycles;
    uint64_t last_packets;
    uint64_t last_bytes;
    uint64_t last_dropped;
} ft_dashboard_state_t;

typedef struct {
    ft_worker_t *workers;
    uint16_t worker_count;
    _Atomic uint16_t *active_worker_count;
    ft_rule_store_t *rule_store;
    const char *rule_path;
    _Atomic bool *reload_requested;
    _Atomic int *scale_delta;
    _Atomic int *show_request;
    _Atomic bool *stop;
} ft_cli_context_t;

typedef struct {
    void *pending[FT_DISPATCH_BURST];
    void *free_items[FT_DISPATCH_BURST];
    uint16_t pending_count;
    uint16_t free_count;
} ft_dispatch_queue_t;

typedef struct {
    uint16_t dispatcher_id;
    uint16_t rx_queue_id;
    const ft_app_config_t *config;
    ft_direction_config_t *directions;
    ft_owner_table_t *owners;
    ft_worker_t *workers;
    uint16_t worker_count;
    _Atomic uint16_t *active_worker_count;
    _Atomic uint64_t *dispatched;
    _Atomic bool *stop;
    uint16_t dispatch_burst;
    uint16_t producer_id;
    bool use_owner_map;
    ft_dispatch_queue_t queues[FT_MAX_WORKERS];
} ft_dispatcher_t;

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

static int select_worker(ft_owner_table_t *table,
                         const ft_flow_key_t *key,
                         uint16_t active_worker_count,
                         bool use_owner_map,
                         uint16_t *worker_id) {
    if (key == NULL || active_worker_count == 0 || worker_id == NULL)
        return -1;
    if (use_owner_map)
        return owner_table_get_or_create(table, key, active_worker_count,
                                         worker_id, NULL);
    *worker_id = (uint16_t)(ft_flow_hash(key) % active_worker_count);
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

static const char *direction_name(unsigned int direction) {
    switch (direction) {
    case FT_DIR_UPLINK:
        return "UPLINK";
    case FT_DIR_DOWNLINK:
        return "DOWNLINK";
    case FT_DIR_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

static const char *drop_color(uint64_t dropped) {
    return dropped == 0 ? FT_ANSI_GREEN : FT_ANSI_RED;
}

static void print_title(const char *title) {
    printf(FT_ANSI_BOLD FT_ANSI_CYAN "%s" FT_ANSI_RESET "\n", title);
}

static void print_rule_table(const ft_stats_snapshot_t *snapshot,
                             const ft_rule_set_t *rules,
                             unsigned int limit) {
    unsigned int printed = 0;

    if (rules == NULL)
        return;
    printf("+------+------------------------------+----------+------------+\n");
    printf("| id   | rule                         | action   | hits       |\n");
    printf("+------+------------------------------+----------+------------+\n");
    for (uint16_t rule_id = 0; rule_id < rules->count; rule_id++) {
        if (snapshot->rule_hits[rule_id] == 0)
            continue;
        printf("| %-4u | %-28.28s | %-8s | %10" PRIu64 " |\n",
               rule_id, rules->rules[rule_id].name,
               ft_action_name(rules->rules[rule_id].action),
               snapshot->rule_hits[rule_id]);
        printed++;
        if (limit != 0 && printed == limit)
            break;
    }
    if (printed == 0)
        printf("| %-4s | %-28s | %-8s | %10u |\n", "-", "none", "-", 0U);
    printf("+------+------------------------------+----------+------------+\n");
}

static unsigned int worker_queue_count(const ft_worker_t *worker) {
    unsigned int queued = 0;

    for (uint16_t i = 0; i < worker->input_count; i++) {
        if (worker->inputs[i] != NULL)
            queued += rte_ring_count(worker->inputs[i]);
    }
    return queued;
}

static bool worker_inputs_empty(const ft_worker_t *worker) {
    for (uint16_t i = 0; i < worker->input_count; i++) {
        if (worker->inputs[i] != NULL && !rte_ring_empty(worker->inputs[i]))
            return false;
    }
    return true;
}

static void collect_stats(const ft_worker_t *workers,
                          uint16_t worker_count,
                          ft_stats_snapshot_t *snapshot) {
    memset(snapshot, 0, sizeof(*snapshot));
    for (uint16_t i = 0; i < worker_count; i++) {
        snapshot->packets += workers[i].local.packets;
        snapshot->bytes += workers[i].local.bytes;
        snapshot->forwarded += workers[i].local.forwarded;
        snapshot->dropped += workers[i].local.dropped;
        snapshot->active_flows += workers[i].flow_table.active;
        snapshot->created_flows += workers[i].flow_table.created;
        snapshot->deleted_flows += workers[i].flow_table.deleted;
        snapshot->timed_out_flows += workers[i].flow_table.timed_out;
        for (unsigned int direction = 0; direction < RTE_DIM(snapshot->direction);
             direction++)
            snapshot->direction[direction] += workers[i].local.direction[direction];
        for (uint16_t traffic_id = 0; traffic_id < FT_TRAFFIC_CLASS_COUNT;
             traffic_id++)
            snapshot->traffic[traffic_id] += workers[i].local.traffic[traffic_id];
        for (uint16_t rule_id = 0; rule_id < FT_MAX_RULES; rule_id++) {
            snapshot->rule_hits[rule_id] += workers[i].local.rule_hits[rule_id];
            snapshot->total_rule_hits += workers[i].local.rule_hits[rule_id];
        }
    }
}

static void print_statistics_view(const ft_worker_t *workers,
                                  uint16_t worker_count,
                                  uint16_t active_worker_count,
                                  uint64_t dispatched,
                                  const ft_rule_set_t *rules) {
    ft_stats_snapshot_t snapshot;

    collect_stats(workers, worker_count, &snapshot);
    print_title("show statistics");
    printf("+----------------+----------------+----------------+\n");
    printf("| active workers | launched       | dispatched     |\n");
    printf("+----------------+----------------+----------------+\n");
    printf("| %14u | %14u | %14" PRIu64 " |\n",
           active_worker_count, worker_count, dispatched);
    printf("+----------------+----------------+----------------+\n\n");
    printf("+--------------+--------------+--------------+--------------+\n");
    printf("| processed    | bytes        | forwarded    | dropped      |\n");
    printf("+--------------+--------------+--------------+--------------+\n");
    printf("| %12" PRIu64 " | %12" PRIu64 " | %12" PRIu64 " | %s%12" PRIu64
           FT_ANSI_RESET " |\n",
           snapshot.packets, snapshot.bytes, snapshot.forwarded,
           drop_color(snapshot.dropped), snapshot.dropped);
    printf("+--------------+--------------+--------------+--------------+\n\n");
    printf("+--------------+--------------+--------------+--------------+\n");
    printf("| active flows | created      | deleted      | timed out    |\n");
    printf("+--------------+--------------+--------------+--------------+\n");
    printf("| %12" PRIu64 " | %12" PRIu64 " | %12" PRIu64 " | %12" PRIu64
           " |\n",
           snapshot.active_flows, snapshot.created_flows,
           snapshot.deleted_flows, snapshot.timed_out_flows);
    printf("+--------------+--------------+--------------+--------------+\n");
    printf("rule_hits=%" PRIu64 " rules=%u\n",
           snapshot.total_rule_hits, rules == NULL ? 0 : rules->count);
}

static void print_flow_view(const ft_worker_t *workers,
                            uint16_t worker_count,
                            uint16_t active_worker_count,
                            uint64_t dispatched) {
    uint64_t active = 0;
    uint64_t created = 0;
    uint64_t deleted = 0;
    uint64_t timed_out = 0;

    print_title("show flow");
    printf("active_workers=%u launched_workers=%u dispatched=%" PRIu64 "\n\n",
           active_worker_count, worker_count, dispatched);
    printf("+--------+--------------+--------------+--------------+--------------+--------------+\n");
    printf("| worker | active       | capacity     | created      | deleted      | timed out    |\n");
    printf("+--------+--------------+--------------+--------------+--------------+--------------+\n");
    for (uint16_t i = 0; i < worker_count; i++) {
        uint64_t worker_active = workers[i].flow_table.active;
        uint64_t worker_created = workers[i].flow_table.created;
        uint64_t worker_deleted = workers[i].flow_table.deleted;
        uint64_t worker_timed_out = workers[i].flow_table.timed_out;

        active += worker_active;
        created += worker_created;
        deleted += worker_deleted;
        timed_out += worker_timed_out;
        printf("| %6u | %12" PRIu64 " | %12u | %12" PRIu64 " | %12" PRIu64
               " | %12" PRIu64 " |\n",
               workers[i].worker_id, worker_active,
               workers[i].flow_table.capacity, worker_created,
               worker_deleted, worker_timed_out);
    }
    printf("+--------+--------------+--------------+--------------+--------------+--------------+\n");
    printf("| %-6s | %12" PRIu64 " | %-12s | %12" PRIu64 " | %12" PRIu64
           " | %12" PRIu64 " |\n",
           "total", active, "-", created, deleted, timed_out);
    printf("+--------+--------------+--------------+--------------+--------------+--------------+\n");
    printf("total active=%" PRIu64 " created=%" PRIu64
           " deleted=%" PRIu64 " timed_out=%" PRIu64 "\n",
           active, created, deleted, timed_out);
}

static void print_worker_view(const ft_worker_t *workers,
                              uint16_t worker_count,
                              uint16_t active_worker_count) {
    print_title("show worker");
    printf("active_workers=%u launched_workers=%u\n\n",
           active_worker_count, worker_count);
    printf("+--------+----------+-------+--------+--------+--------------+--------------+--------------+--------------+\n");
    printf("| worker | state    | lcore | socket | queue  | packets      | bytes        | dropped      | active flow  |\n");
    printf("+--------+----------+-------+--------+--------+--------------+--------------+--------------+--------------+\n");
    for (uint16_t i = 0; i < worker_count; i++) {
        uint64_t packets = workers[i].local.packets;
        uint64_t bytes = workers[i].local.bytes;
        uint64_t forwarded = workers[i].local.forwarded;
        uint64_t dropped = workers[i].local.dropped;
        uint64_t active = workers[i].flow_table.active;
        unsigned int queued = worker_queue_count(&workers[i]);

        printf("| %6u | %-8s | %5u | %6d | %6u | %12" PRIu64
               " | %12" PRIu64 " | %s%12" PRIu64 FT_ANSI_RESET
               " | %12" PRIu64 " |\n",
               workers[i].worker_id,
               i < active_worker_count ? "active" : "standby",
               workers[i].lcore_id, workers[i].socket_id, queued,
               packets, bytes, drop_color(dropped), dropped, active);
        (void)forwarded;
    }
    printf("+--------+----------+-------+--------+--------+--------------+--------------+--------------+--------------+\n");
}

static void print_rule_hits(const ft_stats_snapshot_t *snapshot,
                            const ft_rule_set_t *rules,
                            unsigned int limit) {
    print_rule_table(snapshot, rules, limit);
}

static void print_traffic_view(const ft_worker_t *workers,
                               uint16_t worker_count,
                               const ft_rule_set_t *rules) {
    ft_stats_snapshot_t snapshot;

    collect_stats(workers, worker_count, &snapshot);
    print_title("show traffic");
    printf("+------------+--------------+\n");
    printf("| direction  | packets      |\n");
    printf("+------------+--------------+\n");
    for (unsigned int direction = 0; direction < RTE_DIM(snapshot.direction);
         direction++)
        printf("| %-10s | %12" PRIu64 " |\n",
               direction_name(direction), snapshot.direction[direction]);
    printf("+------------+--------------+\n\n");
    printf("+------------+--------------+\n");
    printf("| class      | packets      |\n");
    printf("+------------+--------------+\n");
    for (uint16_t traffic_id = 0; traffic_id < FT_TRAFFIC_CLASS_COUNT;
         traffic_id++)
        printf("| %-10s | %12" PRIu64 " |\n",
               traffic_name((ft_traffic_class_t)traffic_id),
               snapshot.traffic[traffic_id]);
    printf("+------------+--------------+\n\n");
    print_rule_hits(&snapshot, rules, 0);
}

static void print_dashboard_view(const ft_worker_t *workers,
                                 uint16_t worker_count,
                                 uint16_t active_worker_count,
                                 uint64_t dispatched,
                                 const ft_rule_set_t *rules,
                                 ft_dashboard_state_t *state,
                                 bool clear_screen) {
    ft_stats_snapshot_t snapshot;
    uint64_t now = rte_get_tsc_cycles();
    double seconds = 0.0;
    double pps = 0.0;
    double mbps = 0.0;
    uint64_t drop_delta = 0;

    collect_stats(workers, worker_count, &snapshot);
    if (state->last_cycles != 0 && now > state->last_cycles) {
        seconds = (double)(now - state->last_cycles) /
                  (double)rte_get_tsc_hz();
        pps = seconds > 0.0 ?
            (double)(snapshot.packets - state->last_packets) / seconds : 0.0;
        mbps = seconds > 0.0 ?
            ((double)(snapshot.bytes - state->last_bytes) * 8.0) /
            seconds / 1000000.0 : 0.0;
        drop_delta = snapshot.dropped - state->last_dropped;
    }
    state->last_cycles = now;
    state->last_packets = snapshot.packets;
    state->last_bytes = snapshot.bytes;
    state->last_dropped = snapshot.dropped;

    if (clear_screen)
        printf(FT_ANSI_CLEAR);
    printf(FT_ANSI_BOLD FT_ANSI_CYAN "FlowTable realtime dashboard"
           FT_ANSI_RESET "\n");
    printf("+----------------+----------------+----------------+----------------+\n");
    printf("| active workers | dispatched     | processed      | active flows   |\n");
    printf("+----------------+----------------+----------------+----------------+\n");
    printf("| %7u/%-6u | %14" PRIu64 " | %14" PRIu64 " | %14" PRIu64
           " |\n",
           active_worker_count, worker_count, dispatched, snapshot.packets,
           snapshot.active_flows);
    printf("+----------------+----------------+----------------+----------------+\n\n");
    printf("+--------------+--------------+--------------+--------------+--------------+\n");
    printf("| pps          | mbps         | int drops    | total drops  | forwarded    |\n");
    printf("+--------------+--------------+--------------+--------------+--------------+\n");
    printf("| %12.0f | %12.2f | %s%12" PRIu64 FT_ANSI_RESET
           " | %s%12" PRIu64 FT_ANSI_RESET " | %12" PRIu64 " |\n",
           pps, mbps, drop_color(drop_delta), drop_delta,
           drop_color(snapshot.dropped), snapshot.dropped,
           snapshot.forwarded);
    printf("+--------------+--------------+--------------+--------------+--------------+\n\n");
    printf(FT_ANSI_BOLD "workers" FT_ANSI_RESET "\n");
    printf("+----+----------+-------+-------+--------+--------------+--------------+--------------+\n");
    printf("| id | state    | lcore | queue | packet | active flow  | dropped      | bytes        |\n");
    printf("+----+----------+-------+-------+--------+--------------+--------------+--------------+\n");
    for (uint16_t i = 0; i < worker_count; i++) {
        uint64_t packets = workers[i].local.packets;
        uint64_t active = workers[i].flow_table.active;
        uint64_t dropped = workers[i].local.dropped;
        uint64_t bytes = workers[i].local.bytes;
        unsigned int queued = worker_queue_count(&workers[i]);

        printf("| %2u | %-8s | %5u | %5u | %6" PRIu64 " | %12" PRIu64
               " | %s%12" PRIu64 FT_ANSI_RESET " | %12" PRIu64 " |\n",
               workers[i].worker_id,
               i < active_worker_count ? "active" : "standby",
               workers[i].lcore_id, queued, packets, active,
               drop_color(dropped), dropped, bytes);
    }
    printf("+----+----------+-------+-------+--------+--------------+--------------+--------------+\n\n");
    printf(FT_ANSI_BOLD "traffic" FT_ANSI_RESET "\n");
    printf("+------------+--------------+\n");
    printf("| class      | packets      |\n");
    printf("+------------+--------------+\n");
    for (uint16_t traffic_id = 0; traffic_id < FT_TRAFFIC_CLASS_COUNT;
         traffic_id++)
        printf("| %-10s | %12" PRIu64 " |\n",
               traffic_name((ft_traffic_class_t)traffic_id),
               snapshot.traffic[traffic_id]);
    printf("+------------+--------------+\n");
    printf("direction UNKNOWN=%" PRIu64 " UPLINK=%" PRIu64
           " DOWNLINK=%" PRIu64 " rules=%u\n",
           snapshot.direction[FT_DIR_UNKNOWN], snapshot.direction[FT_DIR_UPLINK],
           snapshot.direction[FT_DIR_DOWNLINK],
           rules == NULL ? 0 : rules->count);
    printf("\n" FT_ANSI_BOLD "top rule hits" FT_ANSI_RESET "\n");
    print_rule_table(&snapshot, rules, 8);
    printf("\n" FT_ANSI_DIM
           "commands: show statistics | show flow | show worker | show traffic"
           " | show dashboard | rules | reload | scale up | scale down | quit"
           FT_ANSI_RESET "\n");
    fflush(stdout);
}

static void print_live_stats(const ft_worker_t *workers,
                             uint16_t worker_count,
                             uint16_t active_worker_count,
                             uint64_t dispatched,
                             const ft_rule_set_t *rules) {
    ft_stats_snapshot_t snapshot;

    collect_stats(workers, worker_count, &snapshot);
    printf("live active_workers=%u launched_workers=%u dispatched=%" PRIu64
           " processed=%" PRIu64 " bytes=%" PRIu64
           " forwarded=%" PRIu64 " dropped=%" PRIu64
           " active_flows=%" PRIu64 " created=%" PRIu64
           " deleted=%" PRIu64 " timed_out=%" PRIu64
           " rule_hits=%" PRIu64 " rules=%u\n",
           active_worker_count, worker_count, dispatched, snapshot.packets,
           snapshot.bytes, snapshot.forwarded, snapshot.dropped,
           snapshot.active_flows, snapshot.created_flows,
           snapshot.deleted_flows, snapshot.timed_out_flows,
           snapshot.total_rule_hits, rules == NULL ? 0 : rules->count);
}

static void request_scale(_Atomic int *scale_delta, int delta) {
    atomic_fetch_add_explicit(scale_delta, delta, memory_order_release);
}

static void request_show(_Atomic int *show_request,
                         _Atomic bool *stop,
                         ft_show_request_t request) {
    atomic_store_explicit(show_request, (int)request, memory_order_release);
    while (!force_quit &&
           !atomic_load_explicit(stop, memory_order_acquire) &&
           atomic_load_explicit(show_request, memory_order_acquire) !=
               FT_SHOW_NONE)
        rte_pause();
}

static void *cli_loop(void *argument) {
    ft_cli_context_t *context = argument;
    char line[128];

    printf(FT_ANSI_CLEAR);
    printf(FT_ANSI_BOLD FT_ANSI_CYAN "FlowTable CLI" FT_ANSI_RESET "\n");
    printf("commands: help | show statistics | show flow | show worker |"
           " show traffic | show dashboard | rules | reload | scale up |"
           " scale down | quit\n\n");
    printf("flowtable> ");
    fflush(stdout);
    while (!atomic_load_explicit(context->stop, memory_order_acquire) &&
           fgets(line, sizeof(line), stdin) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strcmp(line, "help") == 0) {
            print_title("help");
            printf("+-----------------+-----------------------------------------+\n");
            printf("| command         | description                             |\n");
            printf("+-----------------+-----------------------------------------+\n");
            printf("| show statistics | total packet, byte, flow and rule stats  |\n");
            printf("| show flow       | per-worker flow lifecycle counters       |\n");
            printf("| show worker     | per-worker queue and packet counters     |\n");
            printf("| show traffic    | direction, class and rule-hit tables      |\n");
            printf("| show dashboard  | one-shot dashboard view                   |\n");
            printf("| rules           | rule version and loaded rule count        |\n");
            printf("| reload          | reload SPI rules for new flows            |\n");
            printf("| scale up/down   | adjust active workers in dynamic mode     |\n");
            printf("| quit            | stop the pipeline                         |\n");
            printf("+-----------------+-----------------------------------------+\n");
        } else if (strcmp(line, "show") == 0 ||
                   strcmp(line, "show statistics") == 0) {
            request_show(context->show_request, context->stop,
                         FT_SHOW_STATISTICS);
        } else if (strcmp(line, "show flow") == 0) {
            request_show(context->show_request, context->stop, FT_SHOW_FLOW);
        } else if (strcmp(line, "show worker") == 0) {
            request_show(context->show_request, context->stop, FT_SHOW_WORKER);
        } else if (strcmp(line, "show traffic") == 0) {
            request_show(context->show_request, context->stop, FT_SHOW_TRAFFIC);
        } else if (strcmp(line, "show dashboard") == 0) {
            request_show(context->show_request, context->stop,
                         FT_SHOW_DASHBOARD);
        } else if (strcmp(line, "rules") == 0) {
            ft_rule_set_t *rules = rule_store_current(context->rule_store);
            printf("rules version=%" PRIu64 " count=%u path=%s\n",
                   context->rule_store->version,
                   rules == NULL ? 0 : rules->count,
                   context->rule_path);
        } else if (strcmp(line, "reload") == 0) {
            atomic_store_explicit(context->reload_requested, true,
                                  memory_order_release);
        } else if (strcmp(line, "scale up") == 0) {
            request_scale(context->scale_delta, 1);
        } else if (strcmp(line, "scale down") == 0) {
            request_scale(context->scale_delta, -1);
        } else if (strcmp(line, "quit") == 0) {
            force_quit = 1;
            atomic_store_explicit(context->stop, true, memory_order_release);
            break;
        } else {
            printf("unknown command: %s\n", line);
        }
        printf("flowtable> ");
        fflush(stdout);
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
                                 _Atomic int *show_request,
                                 ft_dashboard_state_t *dashboard_state,
                                 uint64_t dispatched) {
    int delta;
    int requested_show;
    uint16_t active;

    if (reload_signal) {
        reload_signal = 0;
        atomic_store_explicit(reload_requested, true, memory_order_release);
    }
    if (scale_up_signal) {
        scale_up_signal = 0;
        if (!config->fixed_workers)
            request_scale(scale_delta, 1);
    }
    if (scale_down_signal) {
        scale_down_signal = 0;
        if (!config->fixed_workers)
            request_scale(scale_delta, -1);
    }
    if (atomic_exchange_explicit(reload_requested, false,
                                 memory_order_acq_rel)) {
        if (rule_store_reload(rule_store, config->rule_path) != 0)
            fprintf(stderr, "rule reload failed: %s\n", config->rule_path);
    }
    if (config->fixed_workers)
        atomic_store_explicit(scale_delta, 0, memory_order_release);
    delta = config->fixed_workers ?
        0 : atomic_exchange_explicit(scale_delta, 0, memory_order_acq_rel);
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
    requested_show = atomic_exchange_explicit(show_request, FT_SHOW_NONE,
                                              memory_order_acq_rel);
    if (requested_show != FT_SHOW_NONE) {
        active = atomic_load_explicit(active_worker_count, memory_order_acquire);
        switch ((ft_show_request_t)requested_show) {
        case FT_SHOW_FLOW:
            print_flow_view(workers, worker_count, active, dispatched);
            break;
        case FT_SHOW_WORKER:
            print_worker_view(workers, worker_count, active);
            break;
        case FT_SHOW_TRAFFIC:
            print_traffic_view(workers, worker_count,
                               rule_store_current(rule_store));
            break;
        case FT_SHOW_DASHBOARD:
            print_dashboard_view(workers, worker_count, active, dispatched,
                                 rule_store_current(rule_store),
                                 dashboard_state, false);
            break;
        case FT_SHOW_STATISTICS:
        default:
            print_statistics_view(workers, worker_count, active, dispatched,
                                  rule_store_current(rule_store));
            break;
        }
    }
}

static uint16_t dispatch_burst_size(const ft_app_config_t *config) {
    uint32_t burst_size = config->burst_size;

    if (burst_size == 0 || burst_size > FT_DISPATCH_BURST)
        burst_size = FT_DISPATCH_BURST;
    if (config->ring_size != 0 && burst_size > config->ring_size)
        burst_size = config->ring_size;
    return (uint16_t)(burst_size == 0 ? 1U : burst_size);
}

static ft_work_item_t *dispatch_get_item(ft_worker_t *worker,
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

static void dispatch_enqueue_item(ft_worker_t *worker,
                                  ft_dispatch_queue_t *queue,
                                  ft_work_item_t *item,
                                  uint16_t burst_size,
                                  uint16_t producer_id) {
    queue->pending[queue->pending_count++] = item;
    if (queue->pending_count == burst_size)
        flush_dispatch_queue(worker, queue, producer_id);
}

static void flush_dispatch_queues(ft_worker_t *workers,
                                  ft_dispatch_queue_t *queues,
                                  uint16_t worker_count,
                                  uint16_t producer_id) {
    for (uint16_t i = 0; i < worker_count; i++) {
        if (queues[i].pending_count != 0)
            flush_dispatch_queue(&workers[i], &queues[i], producer_id);
    }
}

static void return_dispatch_cached_items(ft_worker_t *workers,
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

static int dispatcher_loop(void *argument) {
    ft_dispatcher_t *dispatcher = argument;
    const ft_app_config_t *config = dispatcher->config;
    struct rte_mbuf *mbufs[64];

    while (!force_quit &&
           !atomic_load_explicit(dispatcher->stop, memory_order_acquire) &&
           (config->packet_count == 0 ||
            atomic_load_explicit(dispatcher->dispatched,
                                 memory_order_acquire) < config->packet_count)) {
        uint16_t count = rte_eth_rx_burst(config->port_id,
                                          dispatcher->rx_queue_id,
                                          mbufs, RTE_DIM(mbufs));
        uint64_t rx_timestamp;

        if (count == 0) {
            flush_dispatch_queues(dispatcher->workers, dispatcher->queues,
                                  dispatcher->worker_count,
                                  dispatcher->producer_id);
            rte_pause();
            continue;
        }
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
            if (ft_packet_parse_mbuf(mbufs[i], &packet) != 0) {
                rte_pktmbuf_free(mbufs[i]);
                continue;
            }
            packet.ingress_port = config->port_id;
            packet.timestamp = rx_timestamp;
            ft_packet_normalize(&packet, dispatcher->directions, &normalized);
            if (select_worker(dispatcher->owners, &normalized.key,
                              atomic_load_explicit(
                                  dispatcher->active_worker_count,
                                  memory_order_acquire),
                              dispatcher->use_owner_map, &worker_id) != 0) {
                rte_pktmbuf_free(mbufs[i]);
                continue;
            }
            if (!reserve_dispatched_slot(dispatcher->dispatched,
                                         config->packet_count)) {
                rte_pktmbuf_free(mbufs[i]);
                continue;
            }
            item = dispatch_get_item(&dispatcher->workers[worker_id],
                                     &dispatcher->queues[worker_id],
                                     dispatcher->dispatch_burst);
            item->packet = packet;
            item->normalized = normalized;
            dispatch_enqueue_item(&dispatcher->workers[worker_id],
                                  &dispatcher->queues[worker_id], item,
                                  dispatcher->dispatch_burst,
                                  dispatcher->producer_id);
        }
    }
    flush_dispatch_queues(dispatcher->workers, dispatcher->queues,
                          dispatcher->worker_count,
                          dispatcher->producer_id);
    return_dispatch_cached_items(dispatcher->workers, dispatcher->queues,
                                 dispatcher->worker_count);
    return 0;
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
    uint16_t next_input = 0;

    while (!atomic_load_explicit(worker->stop, memory_order_acquire) ||
           !worker_inputs_empty(worker)) {
        unsigned int count = 0;

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

static void generate_synthetic_packet(uint64_t packet_index,
                          uint32_t flow_count,
                          uint64_t timestamp,
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
    packet->ingress_port = FT_INGRESS_PORT_UNKNOWN;
    packet->tenant_hint = 0;
    packet->direction_hint = FT_DIR_UNKNOWN;
    packet->src_ip = downlink ? server_ip : client_ip;
    packet->dst_ip = downlink ? client_ip : server_ip;
    packet->src_port = downlink ? server_port : (uint16_t)(1024 + flow_id % 50000);
    packet->dst_port = downlink ? (uint16_t)(1024 + flow_id % 50000) : server_port;
    packet->vlan_id = 0;
    packet->protocol = protocol;
    packet->packet_len = (uint16_t)(64 + (flow_id % 1400));
    packet->timestamp = timestamp;
    packet->mbuf = NULL;
}

static int create_worker(ft_worker_t *worker,
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

static void destroy_worker(ft_worker_t *worker) {
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
    ft_dispatch_queue_t dispatch_queues[FT_MAX_WORKERS];
    unsigned int lcore_ids[FT_MAX_WORKERS];
    unsigned int lcore_id;
    _Atomic bool stop = false;
    _Atomic bool reload_requested = false;
    _Atomic int show_request = FT_SHOW_NONE;
    _Atomic int scale_delta = 0;
    _Atomic uint16_t active_worker_count;
    ft_dashboard_state_t dashboard_state = {0};
    uint16_t available = 0;
    uint16_t launched_workers = config->max_worker_count;
    uint16_t dispatch_burst;
    uint32_t owner_capacity;
    uint64_t start = rte_get_tsc_cycles();
    uint64_t end = start;
    uint64_t next_stats = 0;
    uint64_t packet_timestamp;
    uint64_t dispatched = 0;
    bool use_owner_map;
    bool launch_ok = true;
    int result = -1;

    memset(&owners, 0, sizeof(owners));
    memset(dispatch_queues, 0, sizeof(dispatch_queues));
    atomic_init(&active_worker_count, config->worker_count);
    dispatch_burst = dispatch_burst_size(config);
    use_owner_map = !config->fixed_workers &&
                    config->scale_interval_packets != 0;
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
    if (use_owner_map) {
        owner_capacity = config->flow_capacity_per_worker * launched_workers * 2U;
        if (owner_capacity < config->synthetic_flow_count * 2U)
            owner_capacity = config->synthetic_flow_count * 2U;
        if (owner_table_create(&owners, "ft_owner_synthetic", owner_capacity,
                               rte_socket_id()) != 0) {
            fprintf(stderr, "Cannot create dispatcher owner table\n");
            goto cleanup_rules;
        }
    }
    memset(workers, 0, sizeof(workers));
    for (uint16_t i = 0; i < launched_workers; i++) {
        if (create_worker(&workers[i], i, lcore_ids[i], config,
                          &rule_store.current, &stop, 1) != 0) {
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
    dashboard_state.last_cycles = start;
    packet_timestamp = start;
    if (config->stats_interval_seconds != 0)
        next_stats = start + config->stats_interval_seconds * rte_get_tsc_hz();
    for (uint64_t i = 0; i < config->packet_count; i++) {
        ft_packet_t packet;
        ft_normalized_flow_t normalized;
        ft_work_item_t *item;
        uint16_t worker_id;

        if ((i & (FT_CONTROL_INTERVAL - 1U)) == 0)
            packet_timestamp = rte_get_tsc_cycles();
        generate_synthetic_packet(i, config->synthetic_flow_count,
                                  packet_timestamp, &packet);
        ft_packet_normalize(&packet, &directions, &normalized);
        if (select_worker(&owners, &normalized.key,
                          atomic_load_explicit(&active_worker_count,
                                               memory_order_acquire),
                          use_owner_map, &worker_id) != 0)
            continue;
        item = dispatch_get_item(&workers[worker_id],
                                 &dispatch_queues[worker_id],
                                 dispatch_burst);
        item->packet = packet;
        item->normalized = normalized;
        dispatch_enqueue_item(&workers[worker_id],
                              &dispatch_queues[worker_id], item,
                              dispatch_burst, 0);
        dispatched++;
        if (config->scale_interval_packets != 0 &&
            dispatched % config->scale_interval_packets == 0)
            request_scale(&scale_delta, 1);
        if ((dispatched & (FT_CONTROL_INTERVAL - 1U)) == 0)
            apply_control_events(config, workers, launched_workers,
                                 &active_worker_count, &rule_store,
                                 &reload_requested, &scale_delta,
                                 &show_request, &dashboard_state, dispatched);
        if (next_stats != 0 && rte_get_tsc_cycles() >= next_stats) {
            uint16_t active = atomic_load_explicit(&active_worker_count,
                                                   memory_order_acquire);

            if (config->dashboard_enabled)
                print_dashboard_view(workers, launched_workers, active,
                                     dispatched,
                                     rule_store_current(&rule_store),
                                     &dashboard_state, true);
            else
                print_live_stats(workers, launched_workers, active, dispatched,
                                 rule_store_current(&rule_store));
            next_stats = rte_get_tsc_cycles() +
                         config->stats_interval_seconds * rte_get_tsc_hz();
        }
    }
    flush_dispatch_queues(workers, dispatch_queues, launched_workers, 0);
    return_dispatch_cached_items(workers, dispatch_queues, launched_workers);
    apply_control_events(config, workers, launched_workers,
                         &active_worker_count, &rule_store,
                         &reload_requested, &scale_delta,
                         &show_request, &dashboard_state, dispatched);
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
                 uint16_t rx_queue_count,
                 uint16_t worker_count,
                 bool tx_enabled,
                 struct rte_mempool **mbuf_pool) {
    struct rte_eth_dev_info info;
    struct rte_eth_conf port_conf;
    uint16_t rx_desc = 1024;
    uint16_t tx_desc = 1024;
    uint16_t tx_queues = tx_enabled ? worker_count : 1;
    int socket_id = rte_eth_dev_socket_id(port_id);
    uint32_t mbuf_count;
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
    if (rx_queue_count > info.max_rx_queues) {
        fprintf(stderr, "Requested %u RX queues, but port %u supports %u\n",
                rx_queue_count, port_id, info.max_rx_queues);
        return -1;
    }
    if (tx_queues > info.max_tx_queues) {
        fprintf(stderr, "Requested %u TX queues, but port %u supports %u\n",
                tx_queues, port_id, info.max_tx_queues);
        return -1;
    }
    if (rx_queue_count > 1) {
        uint64_t rss_hf = RTE_ETH_RSS_IP | RTE_ETH_RSS_TCP | RTE_ETH_RSS_UDP;

        rss_hf &= info.flow_type_rss_offloads;
        if (rss_hf != 0) {
            port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
            port_conf.rx_adv_conf.rss_conf.rss_hf = rss_hf;
        } else {
            fprintf(stderr, "Port %u does not advertise RSS offloads;"
                    " RX queue distribution depends on the PMD\n",
                    port_id);
        }
    }
    result = rte_eth_dev_configure(port_id, rx_queue_count, tx_queues,
                                   &port_conf);
    if (result != 0) {
        fprintf(stderr, "rte_eth_dev_configure: %s (%d)\n",
                rte_strerror(-result), result);
        return result;
    }
    rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &rx_desc, &tx_desc);
    mbuf_count = 8192U * rx_queue_count;
    *mbuf_pool = rte_pktmbuf_pool_create("ft_rx_mbuf_pool",
                                         mbuf_count, 256, 0,
                                         RTE_MBUF_DEFAULT_BUF_SIZE,
                                         socket_id);
    if (*mbuf_pool == NULL) {
        fprintf(stderr, "rte_pktmbuf_pool_create: %s\n",
                rte_strerror(rte_errno));
        return -1;
    }
    for (uint16_t i = 0; i < rx_queue_count; i++) {
        result = rte_eth_rx_queue_setup(port_id, i, rx_desc, socket_id,
                                        &info.default_rxconf, *mbuf_pool);
        if (result != 0) {
            fprintf(stderr, "rte_eth_rx_queue_setup queue=%u: %s (%d)\n",
                    i, rte_strerror(-result), result);
            return result;
        }
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
    ft_dispatcher_t dispatchers[FT_MAX_DISPATCHERS];
    ft_dispatch_queue_t dispatch_queues[FT_MAX_WORKERS];
    unsigned int lcore_ids[FT_MAX_WORKERS + FT_MAX_DISPATCHERS];
    unsigned int lcore_id;
    struct rte_mempool *mbuf_pool = NULL;
    struct rte_mbuf *mbufs[64];
    _Atomic bool stop = false;
    _Atomic bool dispatcher_stop = false;
    _Atomic bool reload_requested = false;
    _Atomic int show_request = FT_SHOW_NONE;
    _Atomic int scale_delta = 0;
    _Atomic uint16_t active_worker_count;
    _Atomic uint64_t dispatched_atomic = 0;
    ft_dashboard_state_t dashboard_state = {0};
    uint16_t available = 0;
    uint16_t launched_workers = config->max_worker_count;
    uint16_t launched_dispatchers = 0;
    uint16_t dispatcher_lcores = config->dispatcher_count > 1 ?
        config->dispatcher_count : 0;
    uint16_t dispatch_burst;
    uint32_t owner_capacity;
    uint64_t start = rte_get_tsc_cycles();
    uint64_t end = start;
    uint64_t next_stats = 0;
    uint64_t dispatched = 0;
    bool use_owner_map;
    bool launch_ok = true;
    int result = -1;

    memset(&owners, 0, sizeof(owners));
    memset(&cli_context, 0, sizeof(cli_context));
    memset(dispatchers, 0, sizeof(dispatchers));
    memset(dispatch_queues, 0, sizeof(dispatch_queues));
    atomic_init(&active_worker_count, config->worker_count);
    dispatch_burst = dispatch_burst_size(config);
    use_owner_map = !config->fixed_workers;
    if (ft_direction_config_load(&directions, config->direction_path) != 0 ||
        rule_store_init(&rule_store, config->rule_path) != 0)
        return -1;
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (available < RTE_DIM(lcore_ids))
            lcore_ids[available++] = lcore_id;
    }
    if (launched_workers == 0)
        launched_workers = config->worker_count;
    if (launched_workers + dispatcher_lcores > available) {
        fprintf(stderr, "Need %u worker lcores and %u dispatcher lcores,"
                " but EAL provided %u\n",
                launched_workers, dispatcher_lcores, available);
        goto cleanup_rules;
    }
    memset(workers, 0, sizeof(workers));
    if (configure_ethdev(config->port_id, config->rx_queue_count,
                         launched_workers,
                         config->tx_enabled, &mbuf_pool) != 0) {
        fprintf(stderr, "Cannot configure ethdev port %u\n", config->port_id);
        goto cleanup;
    }
    if (use_owner_map) {
        owner_capacity = config->flow_capacity_per_worker * launched_workers * 2U;
        if (owner_table_create(&owners, "ft_owner_ethdev", owner_capacity,
                               rte_socket_id()) != 0) {
            fprintf(stderr, "Cannot create dispatcher owner table\n");
            goto cleanup;
        }
    }
    for (uint16_t i = 0; i < launched_workers; i++) {
        uint16_t lcore_index = config->dispatcher_count > 1 ?
            (uint16_t)(config->dispatcher_count + i) : i;

        if (create_worker(&workers[i], i, lcore_ids[lcore_index], config,
                          &rule_store.current, &stop,
                          config->dispatcher_count) != 0)
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
        cli_context.show_request = &show_request;
        cli_context.stop = config->dispatcher_count > 1 ?
            &dispatcher_stop : &stop;
        if (pthread_create(&cli_thread, NULL, cli_loop, &cli_context) == 0)
            pthread_detach(cli_thread);
    }
    start = rte_get_tsc_cycles();
    dashboard_state.last_cycles = start;
    if (config->stats_interval_seconds != 0)
        next_stats = start + config->stats_interval_seconds * rte_get_tsc_hz();
    if (config->dispatcher_count > 1) {
        for (uint16_t i = 0; i < config->dispatcher_count; i++) {
            dispatchers[i].dispatcher_id = i;
            dispatchers[i].rx_queue_id = i;
            dispatchers[i].config = config;
            dispatchers[i].directions = &directions;
            dispatchers[i].owners = &owners;
            dispatchers[i].workers = workers;
            dispatchers[i].worker_count = launched_workers;
            dispatchers[i].active_worker_count = &active_worker_count;
            dispatchers[i].dispatched = &dispatched_atomic;
            dispatchers[i].stop = &dispatcher_stop;
            dispatchers[i].dispatch_burst = dispatch_burst;
            dispatchers[i].producer_id = i;
            dispatchers[i].use_owner_map = use_owner_map;
        }
        for (uint16_t i = 0; i < config->dispatcher_count; i++) {
            if (rte_eal_remote_launch(dispatcher_loop, &dispatchers[i],
                                      lcore_ids[i]) != 0) {
                launch_ok = false;
                fprintf(stderr, "Cannot launch dispatcher %u\n", i);
                goto wait_dispatchers;
            }
            launched_dispatchers++;
        }
        while (!force_quit &&
               (config->packet_count == 0 ||
                atomic_load_explicit(&dispatched_atomic,
                                     memory_order_acquire) <
                    config->packet_count)) {
            uint64_t current =
                atomic_load_explicit(&dispatched_atomic, memory_order_acquire);

            apply_control_events(config, workers, launched_workers,
                                 &active_worker_count, &rule_store,
                                 &reload_requested, &scale_delta,
                                 &show_request, &dashboard_state, current);
            if (next_stats != 0 && rte_get_tsc_cycles() >= next_stats) {
                uint16_t active = atomic_load_explicit(&active_worker_count,
                                                       memory_order_acquire);

                current = atomic_load_explicit(&dispatched_atomic,
                                               memory_order_acquire);
                if (config->dashboard_enabled)
                    print_dashboard_view(workers, launched_workers, active,
                                         current,
                                         rule_store_current(&rule_store),
                                         &dashboard_state, true);
                else
                    print_live_stats(workers, launched_workers, active,
                                     current,
                                     rule_store_current(&rule_store));
                next_stats = rte_get_tsc_cycles() +
                             config->stats_interval_seconds * rte_get_tsc_hz();
            }
            for (uint16_t spin = 0; spin < FT_CONTROL_INTERVAL; spin++) {
                if (force_quit ||
                    (config->packet_count != 0 &&
                     atomic_load_explicit(&dispatched_atomic,
                                          memory_order_acquire) >=
                         config->packet_count))
                    break;
                rte_pause();
            }
        }
        dispatched = atomic_load_explicit(&dispatched_atomic,
                                          memory_order_acquire);
        goto wait_dispatchers;
    }
    while (!force_quit &&
           (config->packet_count == 0 || dispatched < config->packet_count)) {
        uint16_t count = rte_eth_rx_burst(config->port_id, 0, mbufs,
                                          RTE_DIM(mbufs));
        uint64_t rx_timestamp;

        if (count == 0) {
            flush_dispatch_queues(workers, dispatch_queues, launched_workers,
                                  0);
            apply_control_events(config, workers, launched_workers,
                                 &active_worker_count, &rule_store,
                                 &reload_requested, &scale_delta,
                                 &show_request, &dashboard_state, dispatched);
            rte_pause();
            continue;
        }
        rx_timestamp = rte_get_tsc_cycles();
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
            packet.timestamp = rx_timestamp;
            ft_packet_normalize(&packet, &directions, &normalized);
            if (select_worker(&owners, &normalized.key,
                              atomic_load_explicit(&active_worker_count,
                                                   memory_order_acquire),
                              use_owner_map, &worker_id) != 0) {
                rte_pktmbuf_free(mbufs[i]);
                continue;
            }
            item = dispatch_get_item(&workers[worker_id],
                                     &dispatch_queues[worker_id],
                                     dispatch_burst);
            item->packet = packet;
            item->normalized = normalized;
            dispatch_enqueue_item(&workers[worker_id],
                                  &dispatch_queues[worker_id], item,
                                  dispatch_burst, 0);
            dispatched++;
            if ((dispatched & (FT_CONTROL_INTERVAL - 1U)) == 0)
                apply_control_events(config, workers, launched_workers,
                                     &active_worker_count, &rule_store,
                                     &reload_requested, &scale_delta,
                                     &show_request, &dashboard_state,
                                     dispatched);
            if (next_stats != 0 && rte_get_tsc_cycles() >= next_stats) {
                uint16_t active = atomic_load_explicit(&active_worker_count,
                                                       memory_order_acquire);

                if (config->dashboard_enabled)
                    print_dashboard_view(workers, launched_workers, active,
                                         dispatched,
                                         rule_store_current(&rule_store),
                                         &dashboard_state, true);
                else
                    print_live_stats(workers, launched_workers, active,
                                     dispatched,
                                     rule_store_current(&rule_store));
                next_stats = rte_get_tsc_cycles() +
                             config->stats_interval_seconds * rte_get_tsc_hz();
            }
        }
    }
    flush_dispatch_queues(workers, dispatch_queues, launched_workers, 0);
    return_dispatch_cached_items(workers, dispatch_queues, launched_workers);

wait_dispatchers:
    if (config->dispatcher_count > 1) {
        atomic_store_explicit(&dispatcher_stop, true, memory_order_release);
        for (uint16_t i = 0; i < launched_dispatchers; i++) {
            if (rte_eal_get_lcore_state(lcore_ids[i]) != WAIT)
                rte_eal_wait_lcore(lcore_ids[i]);
        }
        dispatched = atomic_load_explicit(&dispatched_atomic,
                                          memory_order_acquire);
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
