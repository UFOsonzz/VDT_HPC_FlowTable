#ifndef FT_STATS_H
#define FT_STATS_H

#include <stdbool.h>
#include <stdint.h>

#include "ft_pipeline.h"

#define FT_DASHBOARD_HISTORY 32U

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
    uint64_t start_cycles;
    uint64_t last_cycles;
    uint64_t last_packets;
    uint64_t last_bytes;
    uint64_t last_dropped;
    uint64_t last_created_flows;
    double active_flow_history[FT_DASHBOARD_HISTORY];
    double throughput_history[FT_DASHBOARD_HISTORY];
    double drop_history[FT_DASHBOARD_HISTORY];
    uint16_t history_count;
    uint16_t history_next;
} ft_dashboard_state_t;

void ft_stats_print_title(const char *title);
void ft_stats_print_statistics(const ft_worker_t *workers,
                               uint16_t worker_count,
                               uint16_t active_worker_count,
                               uint64_t dispatched,
                               const ft_rule_set_t *rules);
void ft_stats_print_flow(const ft_worker_t *workers,
                         uint16_t worker_count,
                         uint16_t active_worker_count,
                         uint64_t dispatched);
void ft_stats_print_worker(const ft_worker_t *workers,
                           uint16_t worker_count,
                           uint16_t active_worker_count);
void ft_stats_print_worker_detail(const ft_worker_t *workers,
                                  uint16_t worker_count,
                                  uint16_t active_worker_count,
                                  uint16_t worker_id);
void ft_stats_print_traffic(const ft_worker_t *workers,
                            uint16_t worker_count,
                            const ft_rule_set_t *rules);
void ft_stats_print_dashboard(const ft_worker_t *workers,
                              uint16_t worker_count,
                              uint16_t active_worker_count,
                              uint64_t dispatched,
                              const ft_rule_set_t *rules,
                              ft_dashboard_state_t *state,
                              bool clear_screen);
void ft_stats_print_benchmark(const ft_worker_t *workers,
                              uint16_t worker_count,
                              uint16_t active_worker_count,
                              uint64_t dispatched,
                              const ft_rule_set_t *rules,
                              ft_dashboard_state_t *state);
void ft_stats_print_live(const ft_worker_t *workers,
                         uint16_t worker_count,
                         uint16_t active_worker_count,
                         uint64_t dispatched,
                         const ft_rule_set_t *rules);
void ft_stats_print_summary(const ft_worker_t *workers,
                            uint16_t worker_count,
                            uint16_t active_worker_count,
                            uint64_t dispatched,
                            uint64_t elapsed_cycles,
                            const ft_rule_set_t *rules);

#endif
