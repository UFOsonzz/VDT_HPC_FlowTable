#include "ft_flow.h"
#include "ft_packet.h"
#include "ft_rule.h"

#include <getopt.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_lcore.h>
#include <rte_pause.h>

typedef struct {
    unsigned int lcore_id;
    uint16_t worker_id;
    uint64_t packet_count;
    uint32_t flow_count;
    ft_rule_set_t *rules;
    ft_flow_table_t table;
    _Atomic unsigned int *ready;
    _Atomic bool *start;
    uint64_t elapsed;
    uint64_t checksum;
} __rte_cache_aligned bench_worker_t;

static uint32_t next_power_of_two(uint32_t value) {
    uint32_t result = 1024;

    while (result < value && result < (1U << 30))
        result <<= 1;
    return result;
}

static int bench_loop(void *argument) {
    bench_worker_t *worker = argument;
    uint64_t begin;
    uint64_t checksum = 0;

    atomic_fetch_add_explicit(worker->ready, 1, memory_order_release);
    while (!atomic_load_explicit(worker->start, memory_order_acquire))
        rte_pause();
    begin = rte_get_tsc_cycles();
    for (uint64_t i = 0; i < worker->packet_count; i++) {
        uint32_t flow_id = (uint32_t)((i / 2) % worker->flow_count);
        ft_flow_key_t key = {
            .tenant_id = 1,
            .protocol = (flow_id % 5 == 0) ? IPPROTO_UDP : IPPROTO_TCP,
            .client_ip = 0x0a000001U + flow_id,
            .server_ip = (flow_id % 5 == 0)
                             ? 0x08080808U
                             : 0x8efa0001U + (flow_id & 0xffffU),
            .client_port = (uint16_t)(1024 + flow_id % 50000),
            .server_port = (flow_id % 5 == 0) ? 53 : 443,
        };
        ft_flow_entry_t *entry;
        bool created;

        entry = ft_flow_table_get_or_create(&worker->table, &key, i, &created);
        if (entry == NULL)
            continue;
        if (created) {
            const ft_rule_t *rule = ft_rule_match(worker->rules, &key);
            entry->rule_id = rule == NULL ? UINT16_MAX : rule->id;
            entry->action = rule == NULL ? FT_ACTION_FORWARD : rule->action;
        }
        entry->last_seen = i;
        entry->packets[i & 1U]++;
        checksum += entry->rule_id + entry->action + 1;
    }
    worker->elapsed = rte_get_tsc_cycles() - begin;
    worker->checksum = checksum;
    return 0;
}

int main(int argc, char **argv) {
    uint16_t worker_count = 1;
    uint64_t packets_per_worker = 2000000;
    uint32_t flow_count = 32768;
    ft_rule_set_t rules;
    bench_worker_t workers[FT_MAX_WORKERS];
    unsigned int lcores[FT_MAX_WORKERS];
    unsigned int available = 0;
    unsigned int lcore_id;
    _Atomic unsigned int ready = 0;
    _Atomic bool start = false;
    uint64_t max_elapsed = 0;
    uint64_t checksum = 0;
    int consumed;
    int option;
    static const struct option options[] = {
        {"workers", required_argument, NULL, 'w'},
        {"packets", required_argument, NULL, 'p'},
        {"flows", required_argument, NULL, 'f'},
        {NULL, 0, NULL, 0},
    };

    consumed = rte_eal_init(argc, argv);
    if (consumed < 0)
        return EXIT_FAILURE;
    argc -= consumed;
    argv += consumed;
    optind = 1;
    while ((option = getopt_long(argc, argv, "w:p:f:", options, NULL)) != -1) {
        if (option == 'w')
            worker_count = (uint16_t)strtoul(optarg, NULL, 10);
        else if (option == 'p')
            packets_per_worker = strtoull(optarg, NULL, 10);
        else if (option == 'f')
            flow_count = (uint32_t)strtoul(optarg, NULL, 10);
    }
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (available < FT_MAX_WORKERS)
            lcores[available++] = lcore_id;
    }
    if (worker_count == 0 || worker_count > available ||
        ft_rule_set_load(&rules, "config/spi_rules.csv") != 0) {
        fprintf(stderr, "Invalid worker count or rule configuration\n");
        rte_eal_cleanup();
        return EXIT_FAILURE;
    }
    memset(workers, 0, sizeof(workers));
    for (uint16_t i = 0; i < worker_count; i++) {
        char name[FT_NAME_LEN];
        workers[i].lcore_id = lcores[i];
        workers[i].worker_id = i;
        workers[i].packet_count = packets_per_worker;
        workers[i].flow_count = flow_count;
        workers[i].rules = &rules;
        workers[i].ready = &ready;
        workers[i].start = &start;
        snprintf(name, sizeof(name), "bench_flow_%u", i);
        if (ft_flow_table_create(&workers[i].table, name,
                                 next_power_of_two(flow_count * 2U),
                                 rte_lcore_to_socket_id(lcores[i])) != 0) {
            fprintf(stderr, "Cannot allocate worker flow table\n");
            return EXIT_FAILURE;
        }
        rte_eal_remote_launch(bench_loop, &workers[i], lcores[i]);
    }
    while (atomic_load_explicit(&ready, memory_order_acquire) < worker_count)
        rte_pause();
    atomic_store_explicit(&start, true, memory_order_release);
    for (uint16_t i = 0; i < worker_count; i++) {
        rte_eal_wait_lcore(lcores[i]);
        if (workers[i].elapsed > max_elapsed)
            max_elapsed = workers[i].elapsed;
        checksum += workers[i].checksum;
    }
    printf("%u,%" PRIu64 ",%u,%.6f,%.0f,%" PRIu64 "\n",
           worker_count, packets_per_worker * worker_count, flow_count,
           (double)max_elapsed / rte_get_tsc_hz(),
           (double)(packets_per_worker * worker_count) * rte_get_tsc_hz() /
               (double)max_elapsed,
           checksum);
    for (uint16_t i = 0; i < worker_count; i++)
        ft_flow_table_destroy(&workers[i].table);
    rte_eal_cleanup();
    return EXIT_SUCCESS;
}
