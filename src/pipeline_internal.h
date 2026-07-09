#ifndef FT_PIPELINE_INTERNAL_H
#define FT_PIPELINE_INTERNAL_H

#include "ft_pipeline.h"
#include "ft_stats.h"

#include <signal.h>
#include <stdatomic.h>

#include <rte_common.h>

#define FT_PUBLISH_INTERVAL 4096U
#define FT_RULE_RETIRED_MAX 64U
#define FT_CONTROL_INTERVAL 4096U
#define FT_DISPATCH_BURST 64U

extern volatile sig_atomic_t force_quit;

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
    FT_SHOW_DASHBOARD,
    FT_SHOW_WORKER_DETAIL_BASE = 1000
} ft_show_request_t;

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
    uint64_t packet_limit;
    uint16_t dispatch_burst;
    uint16_t producer_id;
    bool use_owner_map;
    ft_dispatch_queue_t queues[FT_MAX_WORKERS];
} ft_dispatcher_t;

int ft_rule_store_init(ft_rule_store_t *store, const char *path);
ft_rule_set_t *ft_rule_store_current(ft_rule_store_t *store);
int ft_rule_store_reload(ft_rule_store_t *store, const char *path);
void ft_rule_store_destroy(ft_rule_store_t *store);

void ft_control_reset_signals(void);
void ft_control_install_signal_handlers(void);
void *ft_cli_loop(void *argument);
void ft_apply_control_events(const ft_app_config_t *config,
                             ft_worker_t *workers,
                             uint16_t worker_count,
                             _Atomic uint16_t *active_worker_count,
                             ft_rule_store_t *rule_store,
                             _Atomic bool *reload_requested,
                             _Atomic int *scale_delta,
                             _Atomic int *show_request,
                             ft_dashboard_state_t *dashboard_state,
                             uint64_t dispatched);

int ft_owner_table_create(ft_owner_table_t *table,
                          const char *name,
                          uint32_t capacity,
                          int socket_id);
void ft_owner_table_destroy(ft_owner_table_t *table);
int ft_select_worker(ft_owner_table_t *table,
                     const ft_flow_key_t *key,
                     uint16_t active_worker_count,
                     bool use_owner_map,
                     uint16_t *worker_id);
uint16_t ft_dispatch_burst_size(const ft_app_config_t *config);
ft_work_item_t *ft_dispatch_get_item(ft_worker_t *worker,
                                     ft_dispatch_queue_t *queue,
                                     uint16_t burst_size);
void ft_dispatch_enqueue_item(ft_worker_t *worker,
                              ft_dispatch_queue_t *queue,
                              ft_work_item_t *item,
                              uint16_t burst_size,
                              uint16_t producer_id);
void ft_flush_dispatch_queues(ft_worker_t *workers,
                              ft_dispatch_queue_t *queues,
                              uint16_t worker_count,
                              uint16_t producer_id);
void ft_return_dispatch_cached_items(ft_worker_t *workers,
                                     ft_dispatch_queue_t *queues,
                                     uint16_t worker_count);
int ft_dispatcher_loop(void *argument);

int ft_worker_create(ft_worker_t *worker,
                     uint16_t worker_id,
                     unsigned int lcore_id,
                     const ft_app_config_t *config,
                     _Atomic(ft_rule_set_t *) *rules_ref,
                     _Atomic bool *stop,
                     uint16_t input_count);
void ft_worker_destroy(ft_worker_t *worker);
int ft_worker_loop(void *argument);

int ft_configure_ethdev(uint16_t port_id,
                        uint16_t rx_queue_count,
                        uint16_t worker_count,
                        uint32_t requested_mbuf_count,
                        bool tx_enabled,
                        struct rte_mempool **mbuf_pool);

#endif
