#include "pipeline_internal.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mempool.h>
#include <rte_pause.h>

typedef struct {
    const ft_app_config_t *config;
    ft_direction_config_t directions;
    ft_rule_store_t rule_store;
    ft_owner_table_t owners;
    ft_cli_context_t cli_context;
    ft_worker_t workers[FT_MAX_WORKERS];
    ft_dispatcher_t dispatchers[FT_MAX_DISPATCHERS];
    ft_dispatch_queue_t dispatch_queues[FT_MAX_WORKERS];
    unsigned int lcore_ids[FT_MAX_WORKERS + FT_MAX_DISPATCHERS];
    struct rte_mempool *mbuf_pool;
    _Atomic bool stop;
    _Atomic bool dispatcher_stop;
    _Atomic bool reload_requested;
    _Atomic int show_request;
    _Atomic int scale_delta;
    _Atomic uint16_t active_worker_count;
    _Atomic uint64_t dispatched_atomic;
    ft_dashboard_state_t dashboard_state;
    uint16_t available_lcores;
    uint16_t launched_workers;
    uint16_t launched_dispatchers;
    uint16_t dispatcher_lcores;
    uint16_t dispatch_burst;
    uint64_t start_cycles;
    uint64_t end_cycles;
    uint64_t next_stats_cycles;
    uint64_t dispatched;
    bool use_owner_map;
    bool ethdev_ready;
    bool launch_ok;
} ft_ethdev_runtime_t;

/* Initialize runtime state derived from immutable application config. */
static void runtime_init(ft_ethdev_runtime_t *runtime,
                         const ft_app_config_t *config) {
    memset(runtime, 0, sizeof(*runtime));
    runtime->config = config;
    runtime->launched_workers = config->max_worker_count == 0 ?
        config->worker_count : config->max_worker_count;
    runtime->dispatcher_lcores = config->dispatcher_count > 1 ?
        config->dispatcher_count : 0;
    runtime->dispatch_burst = ft_dispatch_burst_size(config);
    runtime->use_owner_map = !config->fixed_workers;
    runtime->launch_ok = true;
    runtime->start_cycles = rte_get_tsc_cycles();
    runtime->end_cycles = runtime->start_cycles;
    atomic_init(&runtime->stop, false);
    atomic_init(&runtime->dispatcher_stop, false);
    atomic_init(&runtime->reload_requested, false);
    atomic_init(&runtime->show_request, FT_SHOW_NONE);
    atomic_init(&runtime->scale_delta, 0);
    atomic_init(&runtime->active_worker_count, config->worker_count);
    atomic_init(&runtime->dispatched_atomic, 0);
}

/* Load direction and SPI rule files before allocating DPDK resources. */
static int load_runtime_config(ft_ethdev_runtime_t *runtime) {
    const ft_app_config_t *config = runtime->config;

    if (ft_direction_config_load(&runtime->directions,
                                 config->direction_path) != 0 ||
        ft_rule_store_init(&runtime->rule_store, config->rule_path) != 0) {
        fprintf(stderr, "Cannot load direction or SPI rule configuration\n");
        return -1;
    }
    return 0;
}

/* Collect EAL worker lcores in launch order for dispatcher/worker placement. */
static void collect_worker_lcores(ft_ethdev_runtime_t *runtime) {
    unsigned int lcore_id;

    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (runtime->available_lcores < RTE_DIM(runtime->lcore_ids))
            runtime->lcore_ids[runtime->available_lcores++] = lcore_id;
    }
}

/* Ensure EAL was started with enough lcores for the selected topology. */
static bool validate_lcore_budget(const ft_ethdev_runtime_t *runtime) {
    if (runtime->launched_workers + runtime->dispatcher_lcores <=
        runtime->available_lcores)
        return true;
    fprintf(stderr, "Need %u worker lcores and %u dispatcher lcores,"
            " but EAL provided %u\n",
            runtime->launched_workers, runtime->dispatcher_lcores,
            runtime->available_lcores);
    return false;
}

/* Configure ethdev and create the owner-map needed by dynamic mode. */
static int configure_port_and_owner_map(ft_ethdev_runtime_t *runtime) {
    const ft_app_config_t *config = runtime->config;
    uint32_t owner_capacity;

    if (ft_configure_ethdev(config->port_id, config->rx_queue_count,
                            runtime->launched_workers,
                            config->rx_mbuf_count,
                            config->tx_enabled,
                            &runtime->mbuf_pool) != 0) {
        fprintf(stderr, "Cannot configure ethdev port %u\n", config->port_id);
        return -1;
    }
    runtime->ethdev_ready = true;
    if (!runtime->use_owner_map)
        return 0;

    owner_capacity =
        config->flow_capacity_per_worker * runtime->launched_workers * 2U;
    if (ft_owner_table_create(&runtime->owners, "ft_owner_ethdev",
                              owner_capacity, rte_socket_id()) != 0) {
        fprintf(stderr, "Cannot create dispatcher owner table\n");
        return -1;
    }
    return 0;
}

/* Create all launched workers, including standby workers for scale-up. */
static int create_workers(ft_ethdev_runtime_t *runtime) {
    const ft_app_config_t *config = runtime->config;

    for (uint16_t i = 0; i < runtime->launched_workers; i++) {
        uint16_t lcore_index = config->dispatcher_count > 1 ?
            (uint16_t)(config->dispatcher_count + i) : i;

        if (ft_worker_create(&runtime->workers[i], i,
                             runtime->lcore_ids[lcore_index], config,
                             &runtime->rule_store.current, &runtime->stop,
                             config->dispatcher_count) != 0) {
            fprintf(stderr, "Cannot create worker %u\n", i);
            return -1;
        }
        runtime->workers[i].tx_enabled = config->tx_enabled;
        runtime->workers[i].tx_port = config->port_id;
        runtime->workers[i].tx_queue = i;
    }
    return 0;
}

/* Launch every worker loop on its assigned EAL lcore. */
static int launch_workers(ft_ethdev_runtime_t *runtime) {
    for (uint16_t i = 0; i < runtime->launched_workers; i++) {
        if (rte_eal_remote_launch(ft_worker_loop, &runtime->workers[i],
                                  runtime->workers[i].lcore_id) != 0) {
            fprintf(stderr, "Cannot launch worker %u\n", i);
            runtime->launch_ok = false;
            return -1;
        }
    }
    return 0;
}

/* Start the optional terminal CLI after worker/control state is ready. */
static void start_cli_if_enabled(ft_ethdev_runtime_t *runtime) {
    const ft_app_config_t *config = runtime->config;
    pthread_t cli_thread;

    if (!config->cli_enabled)
        return;
    runtime->cli_context.workers = runtime->workers;
    runtime->cli_context.worker_count = runtime->launched_workers;
    runtime->cli_context.active_worker_count = &runtime->active_worker_count;
    runtime->cli_context.rule_store = &runtime->rule_store;
    runtime->cli_context.rule_path = config->rule_path;
    runtime->cli_context.reload_requested = &runtime->reload_requested;
    runtime->cli_context.scale_delta = &runtime->scale_delta;
    runtime->cli_context.show_request = &runtime->show_request;
    runtime->cli_context.stop = config->dispatcher_count > 1 ?
        &runtime->dispatcher_stop : &runtime->stop;
    if (pthread_create(&cli_thread, NULL, ft_cli_loop,
                       &runtime->cli_context) == 0)
        pthread_detach(cli_thread);
}

/* Mark the beginning of runtime measurement after setup work is complete. */
static void begin_measurement_window(ft_ethdev_runtime_t *runtime) {
    const ft_app_config_t *config = runtime->config;

    runtime->start_cycles = rte_get_tsc_cycles();
    runtime->dashboard_state.start_cycles = runtime->start_cycles;
    runtime->dashboard_state.last_cycles = runtime->start_cycles;
    if (config->cli_enabled)
        return;
    if (config->stats_interval_seconds != 0) {
        runtime->next_stats_cycles =
            runtime->start_cycles +
            config->stats_interval_seconds * rte_get_tsc_hz();
    }
}

/* Print either the compact live line or the full dashboard snapshot. */
static void print_interval_stats(ft_ethdev_runtime_t *runtime,
                                 uint64_t dispatched) {
    const ft_app_config_t *config = runtime->config;
    uint16_t active = atomic_load_explicit(&runtime->active_worker_count,
                                           memory_order_acquire);

    if (config->dashboard_enabled) {
        ft_stats_print_dashboard(runtime->workers, runtime->launched_workers,
                                 active, dispatched,
                                 ft_rule_store_current(&runtime->rule_store),
                                 &runtime->dashboard_state, true);
    } else {
        ft_stats_print_live(runtime->workers, runtime->launched_workers,
                            active, dispatched,
                            ft_rule_store_current(&runtime->rule_store));
    }
}

/* Emit periodic stats only when the configured interval has elapsed. */
static void maybe_print_interval_stats(ft_ethdev_runtime_t *runtime,
                                       uint64_t dispatched) {
    const ft_app_config_t *config = runtime->config;

    if (runtime->next_stats_cycles == 0 ||
        rte_get_tsc_cycles() < runtime->next_stats_cycles)
        return;
    print_interval_stats(runtime, dispatched);
    runtime->next_stats_cycles =
        rte_get_tsc_cycles() + config->stats_interval_seconds *
        rte_get_tsc_hz();
}

/* Process control events and report whether active worker count changed. */
static bool handle_control_events(ft_ethdev_runtime_t *runtime,
                                  uint64_t dispatched,
                                  uint16_t *previous_active,
                                  uint16_t *current_active) {
    return ft_apply_control_events(runtime->config, runtime->workers,
                                   runtime->launched_workers,
                                   &runtime->active_worker_count,
                                   &runtime->rule_store,
                                   &runtime->reload_requested,
                                   &runtime->scale_delta,
                                   &runtime->show_request,
                                   &runtime->dashboard_state,
                                   dispatched, previous_active,
                                   current_active);
}

/* Ask dynamic workers to pause once their rings are empty. */
static void pause_dynamic_workers(ft_ethdev_runtime_t *runtime) {
    for (uint16_t i = 0; i < runtime->launched_workers; i++) {
        atomic_store_explicit(&runtime->workers[i].pause_requested, true,
                              memory_order_release);
    }
    while (!force_quit) {
        bool all_paused = true;

        for (uint16_t i = 0; i < runtime->launched_workers; i++) {
            if (!atomic_load_explicit(&runtime->workers[i].paused,
                                      memory_order_acquire)) {
                all_paused = false;
                break;
            }
        }
        if (all_paused)
            break;
        rte_pause();
    }
}

/* Release workers after flow migration or an interrupted rebalance. */
static void resume_dynamic_workers(ft_ethdev_runtime_t *runtime) {
    for (uint16_t i = 0; i < runtime->launched_workers; i++) {
        atomic_store_explicit(&runtime->workers[i].pause_requested, false,
                              memory_order_release);
    }
}

/* Move active flow entries to their hash owner under the new worker count. */
static uint64_t migrate_dynamic_flow_entries(ft_ethdev_runtime_t *runtime,
                                             uint16_t active_worker_count,
                                             uint64_t *failed) {
    uint64_t migrated = 0;

    *failed = 0;
    for (uint16_t source_id = 0; source_id < runtime->launched_workers;
         source_id++) {
        ft_flow_table_t *source = &runtime->workers[source_id].flow_table;

        for (uint32_t index = 0; index < source->capacity; index++) {
            ft_flow_entry_t copy;
            ft_flow_entry_t *entry = &source->entries[index];
            uint16_t target_id;
            int result;

            if (!entry->in_use)
                continue;
            target_id = (uint16_t)(ft_flow_hash(&entry->key) %
                                   active_worker_count);
            if (target_id == source_id)
                continue;
            copy = *entry;
            result = ft_flow_table_insert_existing(
                &runtime->workers[target_id].flow_table, &copy);
            if (result < 0) {
                (*failed)++;
                continue;
            }
            if (ft_flow_table_delete_migrated(source, &copy.key) != 0) {
                (*failed)++;
                continue;
            }
            migrated++;
        }
    }
    return migrated;
}

/* Rebuild flow-key ownership from the actual per-worker flow tables. */
static uint64_t rebuild_dynamic_owner_map(ft_ethdev_runtime_t *runtime,
                                          uint64_t *failed) {
    uint64_t owners = 0;

    *failed = 0;
    ft_owner_table_reset(&runtime->owners);
    for (uint16_t worker_id = 0; worker_id < runtime->launched_workers;
         worker_id++) {
        ft_flow_table_t *table = &runtime->workers[worker_id].flow_table;

        for (uint32_t index = 0; index < table->capacity; index++) {
            ft_flow_entry_t *entry = &table->entries[index];

            if (!entry->in_use)
                continue;
            if (ft_owner_table_set(&runtime->owners, &entry->key,
                                   worker_id) != 0) {
                (*failed)++;
                continue;
            }
            owners++;
        }
    }
    return owners;
}

/* Pause, migrate, rebuild ownership, and resume after dynamic scaling. */
static void rebalance_dynamic_flows(ft_ethdev_runtime_t *runtime,
                                    uint16_t previous_active,
                                    uint16_t current_active) {
    uint64_t migrated;
    uint64_t migration_failed;
    uint64_t owners;
    uint64_t owner_failed;

    if (!runtime->use_owner_map || previous_active == current_active ||
        current_active == 0)
        return;

    /* Stop dispatch-side buffering before waiting for worker rings to drain. */
    ft_flush_dispatch_queues(runtime->workers, runtime->dispatch_queues,
                             runtime->launched_workers, 0);
    pause_dynamic_workers(runtime);
    if (!force_quit) {
        /* Workers own flow tables, so migrate only while they are paused. */
        migrated = migrate_dynamic_flow_entries(runtime, current_active,
                                                &migration_failed);
        /* Owner-map must reflect the post-migration placement exactly. */
        owners = rebuild_dynamic_owner_map(runtime, &owner_failed);
        printf("dynamic rebalance active %u->%u moved_flows=%llu"
               " owners=%llu failed=%llu\n",
               previous_active, current_active,
               (unsigned long long)migrated,
               (unsigned long long)owners,
               (unsigned long long)(migration_failed + owner_failed));
    }
    resume_dynamic_workers(runtime);
}

/* Fill dispatcher descriptors for multi-RX fixed-worker execution. */
static void prepare_dispatchers(ft_ethdev_runtime_t *runtime) {
    const ft_app_config_t *config = runtime->config;
    uint64_t base_limit = 0;
    uint64_t limit_remainder = 0;

    if (config->per_dispatcher_limit && config->packet_count != 0) {
        base_limit = config->packet_count / config->dispatcher_count;
        limit_remainder = config->packet_count % config->dispatcher_count;
    }
    for (uint16_t i = 0; i < config->dispatcher_count; i++) {
        runtime->dispatchers[i].dispatcher_id = i;
        runtime->dispatchers[i].rx_queue_id = i;
        runtime->dispatchers[i].config = config;
        runtime->dispatchers[i].directions = &runtime->directions;
        runtime->dispatchers[i].owners = &runtime->owners;
        runtime->dispatchers[i].workers = runtime->workers;
        runtime->dispatchers[i].worker_count = runtime->launched_workers;
        runtime->dispatchers[i].active_worker_count =
            &runtime->active_worker_count;
        runtime->dispatchers[i].dispatched = &runtime->dispatched_atomic;
        runtime->dispatchers[i].stop = &runtime->dispatcher_stop;
        runtime->dispatchers[i].packet_limit = base_limit;
        if (i == config->dispatcher_count - 1)
            runtime->dispatchers[i].packet_limit += limit_remainder;
        runtime->dispatchers[i].dispatch_burst = runtime->dispatch_burst;
        runtime->dispatchers[i].producer_id = i;
        runtime->dispatchers[i].use_owner_map = runtime->use_owner_map;
    }
}

/* Launch RX dispatcher lcores for multi-queue fixed-worker mode. */
static int launch_dispatchers(ft_ethdev_runtime_t *runtime) {
    const ft_app_config_t *config = runtime->config;

    prepare_dispatchers(runtime);
    for (uint16_t i = 0; i < config->dispatcher_count; i++) {
        if (rte_eal_remote_launch(ft_dispatcher_loop,
                                  &runtime->dispatchers[i],
                                  runtime->lcore_ids[i]) != 0) {
            runtime->launch_ok = false;
            fprintf(stderr, "Cannot launch dispatcher %u\n", i);
            return -1;
        }
        runtime->launched_dispatchers++;
    }
    return 0;
}

/* Run control/stat polling while independent dispatcher lcores process RX. */
static void run_multi_dispatcher_control_loop(ft_ethdev_runtime_t *runtime) {
    const ft_app_config_t *config = runtime->config;

    while (!force_quit &&
           (config->packet_count == 0 ||
            atomic_load_explicit(&runtime->dispatched_atomic,
                                 memory_order_acquire) <
                config->packet_count)) {
        uint64_t current =
            atomic_load_explicit(&runtime->dispatched_atomic,
                                 memory_order_acquire);

        handle_control_events(runtime, current, NULL, NULL);
        maybe_print_interval_stats(runtime, current);
        for (uint16_t spin = 0; spin < FT_CONTROL_INTERVAL; spin++) {
            if (force_quit ||
                (config->packet_count != 0 &&
                 atomic_load_explicit(&runtime->dispatched_atomic,
                                      memory_order_acquire) >=
                     config->packet_count))
                break;
            rte_pause();
        }
    }
    runtime->dispatched = atomic_load_explicit(&runtime->dispatched_atomic,
                                               memory_order_acquire);
}

/* Run RX, parse, worker selection, and dispatch on the main pipeline lcore. */
static void run_single_dispatcher_loop(ft_ethdev_runtime_t *runtime) {
    const ft_app_config_t *config = runtime->config;
    struct rte_mbuf *mbufs[64];

    while (!force_quit &&
           (config->packet_count == 0 ||
            runtime->dispatched < config->packet_count)) {
        uint16_t count = rte_eth_rx_burst(config->port_id, 0, mbufs,
                                          RTE_DIM(mbufs));
        uint64_t rx_timestamp;

        if (count == 0) {
            uint16_t previous_active;
            uint16_t current_active;

            ft_flush_dispatch_queues(runtime->workers,
                                     runtime->dispatch_queues,
                                     runtime->launched_workers, 0);
            if (handle_control_events(runtime, runtime->dispatched,
                                      &previous_active, &current_active))
                rebalance_dynamic_flows(runtime, previous_active,
                                        current_active);
            rte_pause();
            continue;
        }

        /* Parse and dispatch a whole RX burst before checking slow controls. */
        rx_timestamp = rte_get_tsc_cycles();
        for (uint16_t i = 0; i < count; i++) {
            ft_packet_t packet;
            ft_normalized_flow_t normalized;
            ft_work_item_t *item;
            uint16_t worker_id;

            if (config->packet_count != 0 &&
                runtime->dispatched >= config->packet_count) {
                rte_pktmbuf_free(mbufs[i]);
                continue;
            }
            if (ft_packet_parse_mbuf(mbufs[i], &packet) != 0) {
                rte_pktmbuf_free(mbufs[i]);
                continue;
            }

            /* Canonical key is the flow-affinity contract between directions. */
            packet.ingress_port = config->port_id;
            packet.timestamp = rx_timestamp;
            ft_packet_normalize(&packet, &runtime->directions, &normalized);
            if (ft_select_worker(&runtime->owners, &normalized.key,
                                 atomic_load_explicit(
                                     &runtime->active_worker_count,
                                     memory_order_acquire),
                                 runtime->use_owner_map,
                                 &worker_id) != 0) {
                rte_pktmbuf_free(mbufs[i]);
                continue;
            }

            item = ft_dispatch_get_item(&runtime->workers[worker_id],
                                        &runtime->dispatch_queues[worker_id],
                                        runtime->dispatch_burst);
            item->packet = packet;
            item->normalized = normalized;
            ft_dispatch_enqueue_item(&runtime->workers[worker_id],
                                     &runtime->dispatch_queues[worker_id],
                                     item, runtime->dispatch_burst, 0);
            runtime->dispatched++;
            if ((runtime->dispatched & (FT_CONTROL_INTERVAL - 1U)) == 0) {
                uint16_t previous_active;
                uint16_t current_active;

                if (handle_control_events(runtime, runtime->dispatched,
                                          &previous_active, &current_active))
                    rebalance_dynamic_flows(runtime, previous_active,
                                            current_active);
            }
            maybe_print_interval_stats(runtime, runtime->dispatched);
        }
    }
    ft_flush_dispatch_queues(runtime->workers, runtime->dispatch_queues,
                             runtime->launched_workers, 0);
    ft_return_dispatch_cached_items(runtime->workers,
                                    runtime->dispatch_queues,
                                    runtime->launched_workers);
}

/* Signal dispatcher lcores and wait for them before stopping workers. */
static void stop_dispatchers(ft_ethdev_runtime_t *runtime) {
    const ft_app_config_t *config = runtime->config;

    if (config->dispatcher_count <= 1)
        return;
    atomic_store_explicit(&runtime->dispatcher_stop, true,
                          memory_order_release);
    for (uint16_t i = 0; i < runtime->launched_dispatchers; i++) {
        if (rte_eal_get_lcore_state(runtime->lcore_ids[i]) != WAIT)
            rte_eal_wait_lcore(runtime->lcore_ids[i]);
    }
    runtime->dispatched = atomic_load_explicit(&runtime->dispatched_atomic,
                                               memory_order_acquire);
}

/* Stop worker lcores after dispatchers have stopped feeding their rings. */
static void stop_workers(ft_ethdev_runtime_t *runtime) {
    atomic_store_explicit(&runtime->stop, true, memory_order_release);
    for (uint16_t i = 0; i < runtime->launched_workers; i++) {
        if (rte_eal_get_lcore_state(runtime->workers[i].lcore_id) != WAIT)
            rte_eal_wait_lcore(runtime->workers[i].lcore_id);
    }
}

/* Print final counters and benchmark metrics for scripts to parse. */
static void print_final_summary(ft_ethdev_runtime_t *runtime) {
    runtime->end_cycles = rte_get_tsc_cycles();
    ft_stats_print_summary(runtime->workers, runtime->launched_workers,
                           atomic_load_explicit(
                               &runtime->active_worker_count,
                               memory_order_acquire),
                           runtime->dispatched,
                           runtime->end_cycles - runtime->start_cycles,
                           ft_rule_store_current(&runtime->rule_store));
}

/* Release DPDK objects and config snapshots in reverse setup order. */
static void cleanup_runtime(ft_ethdev_runtime_t *runtime) {
    const ft_app_config_t *config = runtime->config;

    for (uint16_t i = 0; i < runtime->launched_workers; i++)
        ft_worker_destroy(&runtime->workers[i]);
    ft_owner_table_destroy(&runtime->owners);
    if (runtime->ethdev_ready && rte_eth_dev_is_valid_port(config->port_id)) {
        rte_eth_dev_stop(config->port_id);
        rte_eth_dev_close(config->port_id);
    }
    if (runtime->mbuf_pool != NULL)
        rte_mempool_free(runtime->mbuf_pool);
    ft_rule_store_destroy(&runtime->rule_store);
}

/* Top-level ethdev/PCAP PMD pipeline orchestration entry point. */
int ft_pipeline_run_ethdev(const ft_app_config_t *config) {
    ft_ethdev_runtime_t runtime;
    int result = -1;

    runtime_init(&runtime, config);
    if (load_runtime_config(&runtime) != 0)
        return -1;
    collect_worker_lcores(&runtime);
    if (!validate_lcore_budget(&runtime))
        goto cleanup;
    if (configure_port_and_owner_map(&runtime) != 0)
        goto cleanup;
    if (create_workers(&runtime) != 0)
        goto cleanup;
    if (launch_workers(&runtime) != 0)
        goto stop_workers_only;

    ft_control_reset_signals();
    ft_control_install_signal_handlers();
    start_cli_if_enabled(&runtime);
    begin_measurement_window(&runtime);

    if (config->dispatcher_count > 1) {
        if (launch_dispatchers(&runtime) == 0)
            run_multi_dispatcher_control_loop(&runtime);
    } else {
        run_single_dispatcher_loop(&runtime);
    }

    stop_dispatchers(&runtime);

stop_workers_only:
    stop_workers(&runtime);
    print_final_summary(&runtime);
    result = runtime.launch_ok ? 0 : -1;

cleanup:
    cleanup_runtime(&runtime);
    return result;
}
