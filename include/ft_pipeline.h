#ifndef FT_PIPELINE_H
#define FT_PIPELINE_H

#include <stdatomic.h>
#include <stdio.h>

#include "ft_common.h"
#include "ft_config.h"
#include "ft_flow.h"
#include "ft_packet.h"
#include "ft_rule.h"

struct rte_mempool;
struct rte_ring;

typedef struct {
    uint64_t packets;
    uint64_t bytes;
    uint64_t forwarded;
    uint64_t dropped;
    uint64_t ring_dropped;
    uint64_t direction[3];
    uint64_t traffic[FT_TRAFFIC_CLASS_COUNT];
    uint64_t rule_hits[FT_MAX_RULES];
} ft_worker_local_stats_t;

typedef struct {
    _Atomic uint64_t packets;
    _Atomic uint64_t bytes;
    _Atomic uint64_t forwarded;
    _Atomic uint64_t dropped;
    _Atomic uint64_t active_flows;
    _Atomic uint64_t created_flows;
    _Atomic uint64_t deleted_flows;
    _Atomic uint64_t timed_out_flows;
} __rte_cache_aligned ft_worker_published_stats_t;

typedef struct {
    uint16_t worker_id;
    unsigned int lcore_id;
    int socket_id;
    struct rte_ring *input;
    struct rte_mempool *work_pool;
    ft_flow_table_t flow_table;
    _Atomic(ft_rule_set_t *) *rules_ref;
    ft_worker_local_stats_t local;
    ft_worker_published_stats_t published;
    uint64_t timeout_cycles;
    uint32_t aging_budget;
    uint16_t tx_port;
    uint16_t tx_queue;
    bool tx_enabled;
    _Atomic bool *stop;
} ft_worker_t;

typedef struct {
    ft_packet_t packet;
    ft_normalized_flow_t normalized;
} ft_work_item_t;

typedef struct {
    uint16_t worker_count;
    uint16_t max_worker_count;
    uint32_t flow_capacity_per_worker;
    uint32_t ring_size;
    uint32_t burst_size;
    uint32_t aging_budget;
    uint32_t stats_interval_seconds;
    uint32_t timeout_seconds;
    uint64_t packet_count;
    uint64_t scale_interval_packets;
    uint32_t synthetic_flow_count;
    const char *rule_path;
    const char *direction_path;
    uint16_t port_id;
    bool tx_enabled;
    bool cli_enabled;
    bool dashboard_enabled;
    bool fixed_workers;
} ft_app_config_t;

int ft_pipeline_run_synthetic(const ft_app_config_t *config);
int ft_pipeline_run_ethdev(const ft_app_config_t *config);

#endif
