#include "ft_stats.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <rte_cycles.h>
#include <rte_ring.h>

#define FT_ANSI_CLEAR "\033[2J\033[H"
#define FT_ANSI_RESET "\033[0m"
#define FT_ANSI_BOLD "\033[1m"
#define FT_ANSI_DIM "\033[2m"
#define FT_ANSI_GREEN "\033[32m"
#define FT_ANSI_RED "\033[31m"
#define FT_ANSI_CYAN "\033[36m"

/* Convert traffic-class counters into stable CLI labels. */
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

/* Convert direction counters into stable CLI labels. */
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

/* Highlight drop counters only when packets were actually dropped. */
static const char *drop_color(uint64_t dropped) {
    return dropped == 0 ? FT_ANSI_GREEN : FT_ANSI_RED;
}

/* Show active, draining, or standby based on scale state and remaining work. */
static const char *worker_state(const ft_worker_t *worker,
                                uint16_t worker_index,
                                uint16_t active_worker_count,
                                unsigned int queued) {
    if (worker_index < active_worker_count)
        return "active";
    if (worker->flow_table.active != 0 || queued != 0)
        return "draining";
    return "standby";
}

/* Convert TSC deltas to seconds using the runtime DPDK clock rate. */
static double cycles_to_seconds(uint64_t cycles) {
    return (double)cycles / (double)rte_get_tsc_hz();
}

/* Append one sample to the fixed-size dashboard history ring. */
static void dashboard_history_append(ft_dashboard_state_t *state,
                                     double active_flows,
                                     double pps,
                                     double drops) {
    uint16_t index = state->history_next;

    state->active_flow_history[index] = active_flows;
    state->throughput_history[index] = pps;
    state->drop_history[index] = drops;
    state->history_next =
        (uint16_t)((state->history_next + 1U) % FT_DASHBOARD_HISTORY);
    if (state->history_count < FT_DASHBOARD_HISTORY)
        state->history_count++;
}

/* Translate logical graph position into circular history storage index. */
static uint16_t dashboard_history_index(const ft_dashboard_state_t *state,
                                        uint16_t position) {
    if (state->history_count < FT_DASHBOARD_HISTORY)
        return position;
    return (uint16_t)((state->history_next + position) %
                      FT_DASHBOARD_HISTORY);
}

/* Find a graph scale ceiling for the selected history series. */
static double dashboard_history_max(const ft_dashboard_state_t *state,
                                    const double *history) {
    double maximum = 0.0;

    for (uint16_t i = 0; i < state->history_count; i++) {
        uint16_t index = dashboard_history_index(state, i);

        if (history[index] > maximum)
            maximum = history[index];
    }
    return maximum;
}

/* Render one compact ASCII graph line for the realtime dashboard. */
static void print_graph_line(const char *label,
                             const ft_dashboard_state_t *state,
                             const double *history,
                             double latest,
                             const char *unit) {
    double maximum = dashboard_history_max(state, history);

    printf("%-12s latest=%10.0f %-8s max=%10.0f |",
           label, latest, unit, maximum);
    for (uint16_t i = 0; i < state->history_count; i++) {
        uint16_t index = dashboard_history_index(state, i);
        double ratio = maximum > 0.0 ? history[index] / maximum : 0.0;
        char marker = '.';

        if (ratio >= 0.66)
            marker = '#';
        else if (ratio >= 0.33)
            marker = '+';
        else if (ratio > 0.0)
            marker = '-';
        putchar(marker);
    }
    printf("|\n");
}

/* Update interval-rate state and append one dashboard history sample. */
static void update_runtime_window(const ft_stats_snapshot_t *snapshot,
                                  ft_dashboard_state_t *state,
                                  uint64_t now,
                                  double *elapsed,
                                  double *interval_seconds,
                                  double *pps,
                                  double *mbps,
                                  double *flow_create_rate,
                                  uint64_t *drop_delta) {
    uint64_t created_delta = 0;

    if (state->start_cycles == 0)
        state->start_cycles = now;
    *elapsed = now > state->start_cycles ?
        cycles_to_seconds(now - state->start_cycles) : 0.0;
    *interval_seconds = 0.0;
    *pps = 0.0;
    *mbps = 0.0;
    *flow_create_rate = 0.0;
    *drop_delta = 0;
    if (state->last_cycles != 0 && now > state->last_cycles) {
        *interval_seconds = cycles_to_seconds(now - state->last_cycles);
        *pps = *interval_seconds > 0.0 ?
            (double)(snapshot->packets - state->last_packets) /
            *interval_seconds : 0.0;
        *mbps = *interval_seconds > 0.0 ?
            ((double)(snapshot->bytes - state->last_bytes) * 8.0) /
            *interval_seconds / 1000000.0 : 0.0;
        created_delta = snapshot->created_flows - state->last_created_flows;
        *flow_create_rate = *interval_seconds > 0.0 ?
            (double)created_delta / *interval_seconds : 0.0;
        *drop_delta = snapshot->dropped - state->last_dropped;
    }
    state->last_cycles = now;
    state->last_packets = snapshot->packets;
    state->last_bytes = snapshot->bytes;
    state->last_dropped = snapshot->dropped;
    state->last_created_flows = snapshot->created_flows;
    dashboard_history_append(state, (double)snapshot->active_flows, *pps,
                             (double)*drop_delta);
}

/* Print a consistent colored heading for CLI views. */
void ft_stats_print_title(const char *title) {
    printf(FT_ANSI_BOLD FT_ANSI_CYAN "%s" FT_ANSI_RESET "\n", title);
}

/* Print non-zero rule hit counters, optionally limited to top rows. */
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

/* Sum all input rings feeding one worker across dispatcher producers. */
static unsigned int worker_queue_count(const ft_worker_t *worker) {
    unsigned int queued = 0;

    for (uint16_t i = 0; i < worker->input_count; i++) {
        if (worker->inputs[i] != NULL)
            queued += rte_ring_count(worker->inputs[i]);
    }
    return queued;
}

/* Aggregate per-worker counters into one snapshot for display. */
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

/* Print global packet, byte, flow, and rule counters for the CLI. */
void ft_stats_print_statistics(const ft_worker_t *workers,
                               uint16_t worker_count,
                               uint16_t active_worker_count,
                               uint64_t dispatched,
                               const ft_rule_set_t *rules) {
    ft_stats_snapshot_t snapshot;

    collect_stats(workers, worker_count, &snapshot);
    ft_stats_print_title("show statistics");
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

/* Print per-worker flow lifecycle counters and totals. */
void ft_stats_print_flow(const ft_worker_t *workers,
                         uint16_t worker_count,
                         uint16_t active_worker_count,
                         uint64_t dispatched) {
    uint64_t active = 0;
    uint64_t created = 0;
    uint64_t deleted = 0;
    uint64_t timed_out = 0;

    ft_stats_print_title("show flow");
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

/* Print a one-row-per-worker view for queue and packet balance. */
void ft_stats_print_worker(const ft_worker_t *workers,
                           uint16_t worker_count,
                           uint16_t active_worker_count) {
    ft_stats_print_title("show worker");
    printf("active_workers=%u launched_workers=%u\n\n",
           active_worker_count, worker_count);
    printf("+--------+----------+-------+--------+--------+--------------+--------------+--------------+--------------+\n");
    printf("| worker | state    | lcore | socket | queue  | packets      | bytes        | dropped      | active flow  |\n");
    printf("+--------+----------+-------+--------+--------+--------------+--------------+--------------+--------------+\n");
    for (uint16_t i = 0; i < worker_count; i++) {
        uint64_t packets = workers[i].local.packets;
        uint64_t bytes = workers[i].local.bytes;
        uint64_t dropped = workers[i].local.dropped;
        uint64_t active = workers[i].flow_table.active;
        unsigned int queued = worker_queue_count(&workers[i]);

        printf("| %6u | %-8s | %5u | %6d | %6u | %12" PRIu64
               " | %12" PRIu64 " | %s%12" PRIu64 FT_ANSI_RESET
               " | %12" PRIu64 " |\n",
               workers[i].worker_id,
               worker_state(&workers[i], i, active_worker_count, queued),
               workers[i].lcore_id, workers[i].socket_id, queued,
               packets, bytes, drop_color(dropped), dropped, active);
    }
    printf("+--------+----------+-------+--------+--------+--------------+--------------+--------------+--------------+\n");
}

/* Print detailed counters for one selected worker core. */
void ft_stats_print_worker_detail(const ft_worker_t *workers,
                                  uint16_t worker_count,
                                  uint16_t active_worker_count,
                                  uint16_t worker_id) {
    const ft_worker_t *worker;

    ft_stats_print_title("show worker detail");
    if (worker_id >= worker_count) {
        printf("worker_id=%u is out of range; launched_workers=%u\n",
               worker_id, worker_count);
        return;
    }
    worker = &workers[worker_id];
    unsigned int queued = worker_queue_count(worker);

    printf("+--------+----------+-------+--------+--------+--------------+--------------+--------------+--------------+\n");
    printf("| worker | state    | lcore | socket | queue  | packets      | bytes        | forwarded    | dropped      |\n");
    printf("+--------+----------+-------+--------+--------+--------------+--------------+--------------+--------------+\n");
    printf("| %6u | %-8s | %5u | %6d | %6u | %12" PRIu64 " | %12" PRIu64
           " | %12" PRIu64 " | %s%12" PRIu64 FT_ANSI_RESET " |\n",
           worker->worker_id,
           worker_state(worker, worker_id, active_worker_count, queued),
           worker->lcore_id, worker->socket_id, queued,
           worker->local.packets, worker->local.bytes,
           worker->local.forwarded, drop_color(worker->local.dropped),
           worker->local.dropped);
    printf("+--------+----------+-------+--------+--------+--------------+--------------+--------------+--------------+\n\n");
    printf("+--------------+--------------+--------------+--------------+--------------+\n");
    printf("| active flows | capacity     | created      | deleted      | timed out    |\n");
    printf("+--------------+--------------+--------------+--------------+--------------+\n");
    printf("| %12u | %12u | %12" PRIu64 " | %12" PRIu64 " | %12" PRIu64
           " |\n",
           worker->flow_table.active, worker->flow_table.capacity,
           worker->flow_table.created, worker->flow_table.deleted,
           worker->flow_table.timed_out);
    printf("+--------------+--------------+--------------+--------------+--------------+\n\n");
    printf("+------------+--------------+\n");
    printf("| class      | packets      |\n");
    printf("+------------+--------------+\n");
    for (uint16_t traffic_id = 0; traffic_id < FT_TRAFFIC_CLASS_COUNT;
         traffic_id++)
        printf("| %-10s | %12" PRIu64 " |\n",
               traffic_name((ft_traffic_class_t)traffic_id),
               worker->local.traffic[traffic_id]);
    printf("+------------+--------------+\n\n");
    printf("+------------+--------------+\n");
    printf("| direction  | packets      |\n");
    printf("+------------+--------------+\n");
    for (unsigned int direction = 0; direction < RTE_DIM(worker->local.direction);
         direction++)
        printf("| %-10s | %12" PRIu64 " |\n",
               direction_name(direction), worker->local.direction[direction]);
    printf("+------------+--------------+\n");
}

/* Keep traffic view naming separate from the generic rule table helper. */
static void print_rule_hits(const ft_stats_snapshot_t *snapshot,
                            const ft_rule_set_t *rules,
                            unsigned int limit) {
    print_rule_table(snapshot, rules, limit);
}

/* Print aggregate direction, traffic class, and SPI rule-hit tables. */
void ft_stats_print_traffic(const ft_worker_t *workers,
                            uint16_t worker_count,
                            const ft_rule_set_t *rules) {
    ft_stats_snapshot_t snapshot;

    collect_stats(workers, worker_count, &snapshot);
    ft_stats_print_title("show traffic");
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

/* Render the realtime ANSI dashboard used by CLI and dashboard mode. */
void ft_stats_print_dashboard(const ft_worker_t *workers,
                              uint16_t worker_count,
                              uint16_t active_worker_count,
                              uint64_t dispatched,
                              const ft_rule_set_t *rules,
                              ft_dashboard_state_t *state,
                              bool clear_screen) {
    ft_stats_snapshot_t snapshot;
    uint64_t now = rte_get_tsc_cycles();
    double elapsed = 0.0;
    double interval_seconds = 0.0;
    double pps = 0.0;
    double mbps = 0.0;
    double flow_create_rate = 0.0;
    double avg_pps = 0.0;
    double avg_flow_create_rate = 0.0;
    uint64_t drop_delta = 0;

    collect_stats(workers, worker_count, &snapshot);
    update_runtime_window(&snapshot, state, now, &elapsed,
                          &interval_seconds, &pps, &mbps,
                          &flow_create_rate, &drop_delta);
    avg_pps = elapsed > 0.0 ? (double)snapshot.packets / elapsed : 0.0;
    avg_flow_create_rate =
        elapsed > 0.0 ? (double)snapshot.created_flows / elapsed : 0.0;

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
    printf(FT_ANSI_BOLD "runtime rates" FT_ANSI_RESET "\n");
    printf("+--------------+--------------+--------------+--------------+--------------+\n");
    printf("| elapsed sec  | avg pps      | flow create/s| interval sec | rules        |\n");
    printf("+--------------+--------------+--------------+--------------+--------------+\n");
    printf("| %12.3f | %12.0f | %12.0f | %12.3f | %12u |\n",
           elapsed, avg_pps, avg_flow_create_rate, interval_seconds,
           rules == NULL ? 0 : rules->count);
    printf("+--------------+--------------+--------------+--------------+--------------+\n\n");
    printf(FT_ANSI_BOLD "graphs" FT_ANSI_RESET "\n");
    print_graph_line("Active Flow", state, state->active_flow_history,
                     (double)snapshot.active_flows, "flows");
    print_graph_line("Throughput", state, state->throughput_history,
                     pps, "pps");
    print_graph_line("Packet Drop", state, state->drop_history,
                     (double)drop_delta, "drops");
    printf("\n");
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
               worker_state(&workers[i], i, active_worker_count, queued),
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
           "commands: show statistics | show flow | show worker"
           " | show worker N | show traffic | show dashboard"
           " | rules | reload | scale up | scale down | quit"
           FT_ANSI_RESET "\n");
    fflush(stdout);
}

/* Print a single-line live snapshot for script-friendly monitoring. */
void ft_stats_print_live(const ft_worker_t *workers,
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

/* Print final summary lines consumed by benchmark and report scripts. */
void ft_stats_print_summary(const ft_worker_t *workers,
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
    uint64_t deleted = 0;
    uint64_t timed_out = 0;
    uint64_t traffic[FT_TRAFFIC_CLASS_COUNT] = {0};
    uint64_t rule_hits[FT_MAX_RULES] = {0};
    double seconds = (double)elapsed_cycles / (double)rte_get_tsc_hz();

    for (uint16_t i = 0; i < worker_count; i++) {
        packets += workers[i].local.packets;
        forwarded += workers[i].local.forwarded;
        dropped += workers[i].local.dropped;
        active += workers[i].flow_table.active;
        created += workers[i].flow_table.created;
        deleted += workers[i].flow_table.deleted;
        timed_out += workers[i].flow_table.timed_out;
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
           " deleted_flows=%" PRIu64 " timed_out_flows=%" PRIu64
           " seconds=%.6f pps=%.0f flow_create_rate=%.0f\n",
           active_worker_count, worker_count, dispatched, packets, forwarded,
           dropped, active, created, deleted, timed_out, seconds,
           seconds > 0.0 ? (double)packets / seconds : 0.0,
           seconds > 0.0 ? (double)created / seconds : 0.0);
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
