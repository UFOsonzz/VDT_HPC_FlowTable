#include "ft_pipeline.h"

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_errno.h>
#include <rte_lcore.h>
#include <rte_mempool.h>
#include <rte_pause.h>
#include <rte_ring.h>

#define FT_PUBLISH_INTERVAL 4096U

static volatile sig_atomic_t force_quit;

static void handle_signal(int signal_number) {
    (void)signal_number;
    force_quit = 1;
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
        rule = ft_rule_match(worker->rules, &item->normalized.key);
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
              const ft_rule_set_t *rules,
              _Atomic bool *stop) {
    char name[FT_NAME_LEN];
    uint32_t pool_size = config->ring_size * 2;

    memset(worker, 0, sizeof(*worker));
    worker->worker_id = worker_id;
    worker->lcore_id = lcore_id;
    worker->socket_id = rte_lcore_to_socket_id(lcore_id);
    worker->rules = rules;
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
              uint64_t dispatched,
              uint64_t elapsed_cycles) {
    uint64_t packets = 0;
    uint64_t forwarded = 0;
    uint64_t dropped = 0;
    uint64_t active = 0;
    uint64_t created = 0;
    double seconds = (double)elapsed_cycles / (double)rte_get_tsc_hz();

    for (uint16_t i = 0; i < worker_count; i++) {
        packets += workers[i].local.packets;
        forwarded += workers[i].local.forwarded;
        dropped += workers[i].local.dropped;
        active += workers[i].flow_table.active;
        created += workers[i].flow_table.created;
        printf("worker=%u lcore=%u socket=%d packets=%" PRIu64
               " active_flows=%u created=%" PRIu64 "\n",
               workers[i].worker_id, workers[i].lcore_id, workers[i].socket_id,
               workers[i].local.packets, workers[i].flow_table.active,
               workers[i].flow_table.created);
    }
    printf("summary dispatched=%" PRIu64 " processed=%" PRIu64
           " forwarded=%" PRIu64 " dropped=%" PRIu64
           " active_flows=%" PRIu64 " created_flows=%" PRIu64
           " seconds=%.6f pps=%.0f\n",
           dispatched, packets, forwarded, dropped, active, created, seconds,
           seconds > 0.0 ? (double)packets / seconds : 0.0);
}

int ft_pipeline_run_synthetic(const ft_app_config_t *config) {
    ft_direction_config_t directions;
    ft_rule_set_t rules;
    ft_worker_t workers[FT_MAX_WORKERS];
    unsigned int lcore_ids[FT_MAX_WORKERS];
    unsigned int lcore_id;
    _Atomic bool stop = false;
    uint16_t available = 0;
    uint64_t start = rte_get_tsc_cycles();
    uint64_t end = start;
    uint64_t dispatched = 0;
    bool launch_ok = true;
    int result = -1;

    if (ft_direction_config_load(&directions, config->direction_path) != 0 ||
        ft_rule_set_load(&rules, config->rule_path) != 0) {
        fprintf(stderr, "Cannot load direction or SPI rule configuration\n");
        return -1;
    }
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (available < FT_MAX_WORKERS)
            lcore_ids[available++] = lcore_id;
    }
    if (config->worker_count == 0 || config->worker_count > available) {
        fprintf(stderr, "Need %u worker lcores, but EAL provided %u\n",
                config->worker_count, available);
        return -1;
    }
    memset(workers, 0, sizeof(workers));
    for (uint16_t i = 0; i < config->worker_count; i++) {
        if (create_worker(&workers[i], i, lcore_ids[i], config,
                          &rules, &stop) != 0) {
            fprintf(stderr, "Cannot create worker %u\n", i);
            goto cleanup;
        }
    }
    for (uint16_t i = 0; i < config->worker_count; i++) {
        if (rte_eal_remote_launch(worker_loop, &workers[i],
                                  workers[i].lcore_id) != 0) {
            fprintf(stderr, "Cannot launch worker %u\n", i);
            launch_ok = false;
            atomic_store(&stop, true);
            goto wait;
        }
    }

    start = rte_get_tsc_cycles();
    for (uint64_t i = 0; i < config->packet_count; i++) {
        ft_packet_t packet;
        ft_normalized_flow_t normalized;
        ft_work_item_t *item;
        uint16_t worker_id;

        generate_synthetic_packet(i, config->synthetic_flow_count, &packet);
        ft_packet_normalize(&packet, &directions, &normalized);
        worker_id = (uint16_t)(ft_flow_hash(&normalized.key) %
                               config->worker_count);
        while (rte_mempool_get(workers[worker_id].work_pool,
                               (void **)&item) != 0)
            rte_pause();
        item->packet = packet;
        item->normalized = normalized;
        while (rte_ring_sp_enqueue(workers[worker_id].input, item) != 0)
            rte_pause();
        dispatched++;
    }
    atomic_store_explicit(&stop, true, memory_order_release);

wait:
    for (uint16_t i = 0; i < config->worker_count; i++) {
        if (rte_eal_get_lcore_state(workers[i].lcore_id) != WAIT)
            rte_eal_wait_lcore(workers[i].lcore_id);
    }
    end = rte_get_tsc_cycles();
    print_summary(workers, config->worker_count, dispatched, end - start);
    result = launch_ok ? 0 : -1;

cleanup:
    for (uint16_t i = 0; i < config->worker_count; i++)
        destroy_worker(&workers[i]);
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
    ft_rule_set_t rules;
    ft_worker_t workers[FT_MAX_WORKERS];
    unsigned int lcore_ids[FT_MAX_WORKERS];
    unsigned int lcore_id;
    struct rte_mempool *mbuf_pool = NULL;
    struct rte_mbuf *mbufs[64];
    _Atomic bool stop = false;
    uint16_t available = 0;
    uint64_t start = rte_get_tsc_cycles();
    uint64_t end = start;
    uint64_t dispatched = 0;
    bool launch_ok = true;
    int result = -1;

    if (ft_direction_config_load(&directions, config->direction_path) != 0 ||
        ft_rule_set_load(&rules, config->rule_path) != 0)
        return -1;
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (available < FT_MAX_WORKERS)
            lcore_ids[available++] = lcore_id;
    }
    if (config->worker_count == 0 || config->worker_count > available)
        return -1;
    memset(workers, 0, sizeof(workers));
    if (configure_ethdev(config->port_id, config->worker_count,
                         config->tx_enabled, &mbuf_pool) != 0) {
        fprintf(stderr, "Cannot configure ethdev port %u\n", config->port_id);
        goto cleanup;
    }
    for (uint16_t i = 0; i < config->worker_count; i++) {
        if (create_worker(&workers[i], i, lcore_ids[i], config,
                          &rules, &stop) != 0)
            goto cleanup;
        workers[i].tx_enabled = config->tx_enabled;
        workers[i].tx_port = config->port_id;
        workers[i].tx_queue = i;
    }
    for (uint16_t i = 0; i < config->worker_count; i++) {
        if (rte_eal_remote_launch(worker_loop, &workers[i],
                                  workers[i].lcore_id) != 0) {
            launch_ok = false;
            goto stop_workers;
        }
    }
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    force_quit = 0;
    start = rte_get_tsc_cycles();
    while (!force_quit &&
           (config->packet_count == 0 || dispatched < config->packet_count)) {
        uint16_t count = rte_eth_rx_burst(config->port_id, 0, mbufs,
                                          RTE_DIM(mbufs));
        if (count == 0) {
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
            worker_id = (uint16_t)(ft_flow_hash(&normalized.key) %
                                   config->worker_count);
            while (rte_mempool_get(workers[worker_id].work_pool,
                                   (void **)&item) != 0)
                rte_pause();
            item->packet = packet;
            item->normalized = normalized;
            while (rte_ring_sp_enqueue(workers[worker_id].input, item) != 0)
                rte_pause();
            dispatched++;
        }
    }

stop_workers:
    atomic_store_explicit(&stop, true, memory_order_release);
    for (uint16_t i = 0; i < config->worker_count; i++) {
        if (rte_eal_get_lcore_state(workers[i].lcore_id) != WAIT)
            rte_eal_wait_lcore(workers[i].lcore_id);
    }
    end = rte_get_tsc_cycles();
    print_summary(workers, config->worker_count, dispatched, end - start);
    result = launch_ok ? 0 : -1;

cleanup:
    for (uint16_t i = 0; i < config->worker_count; i++)
        destroy_worker(&workers[i]);
    if (rte_eth_dev_is_valid_port(config->port_id)) {
        rte_eth_dev_stop(config->port_id);
        rte_eth_dev_close(config->port_id);
    }
    if (mbuf_pool != NULL)
        rte_mempool_free(mbuf_pool);
    return result;
}
