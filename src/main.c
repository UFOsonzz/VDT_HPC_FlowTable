#include "ft_pipeline.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_eal.h>

static void usage(const char *program) {
    printf("Usage: %s EAL_ARGS -- [application options]\n"
           "  --mode MODE        ethdev only; kept for script compatibility\n"
           "  --workers N        Active worker lcores at start (default: 4)\n"
           "  --max-workers N    Launched worker lcores for runtime scale-up\n"
           "  --packets N        Packets to receive, 0 runs until signal\n"
           "  --flow-capacity N  Flow entries per worker (default: 131072)\n"
           "  --ring-size N      Per-worker SPSC ring size (default: 4096)\n"
           "  --timeout N        Flow timeout seconds (default: 5)\n"
           "  --stats-interval N Print live stats every N seconds, 0 disables\n"
           "  --rules PATH       SPI CSV converted from workbook\n"
           "  --directions PATH  Direction strategy CSV\n"
           "  --port N           Ethdev/PCAP PMD port (default: 0)\n"
           "  --rx-queues N      Ethdev RX queues to poll (default: 1)\n"
           "  --rx-mbufs N       RX mbufs for ethdev/PCAP PMD, 0 auto\n"
           "  --dispatchers N    RX dispatcher lcores for ethdev (default: 1)\n"
           "  --tx               Transmit FORWARD packets on per-worker TX queues\n"
           "  --cli              Enable terminal CLI for ethdev mode\n"
           "  --dashboard        Print ANSI realtime dashboard; default interval 1s\n"
           "  --per-dispatcher-limit  Split --packets evenly across dispatchers\n"
           "  --fixed-workers    Disable runtime scaling; hash flows directly to workers\n",
           program);
}

int main(int argc, char **argv) {
    ft_app_config_t config = {
        .worker_count = 4,
        .max_worker_count = 4,
        .flow_capacity_per_worker = 131072,
        .ring_size = 4096,
        .burst_size = 64,
        .aging_budget = 1024,
        .stats_interval_seconds = 0,
        .timeout_seconds = 5,
        .packet_count = 0,
        .rule_path = "config/spi_rules.csv",
        .direction_path = "config/direction_rules.csv",
        .rx_queue_count = 1,
        .dispatcher_count = 1,
    };
    const char *mode = "ethdev";
    static const struct option options[] = {
        {"mode", required_argument, NULL, 'm'},
        {"workers", required_argument, NULL, 'w'},
        {"max-workers", required_argument, NULL, 'W'},
        {"packets", required_argument, NULL, 'p'},
        {"flow-capacity", required_argument, NULL, 'c'},
        {"ring-size", required_argument, NULL, 'r'},
        {"timeout", required_argument, NULL, 't'},
        {"stats-interval", required_argument, NULL, 's'},
        {"rules", required_argument, NULL, 'R'},
        {"directions", required_argument, NULL, 'D'},
        {"port", required_argument, NULL, 'P'},
        {"rx-queues", required_argument, NULL, 'q'},
        {"rx-mbufs", required_argument, NULL, 'M'},
        {"dispatchers", required_argument, NULL, 'd'},
        {"tx", no_argument, NULL, 'T'},
        {"cli", no_argument, NULL, 'C'},
        {"dashboard", no_argument, NULL, 'B'},
        {"per-dispatcher-limit", no_argument, NULL, 'L'},
        {"fixed-workers", no_argument, NULL, 'F'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int consumed;
    int option;
    int result;

    consumed = rte_eal_init(argc, argv);
    if (consumed < 0)
        return EXIT_FAILURE;
    argc -= consumed;
    argv += consumed;
    optind = 1;
    while ((option = getopt_long(argc, argv, "m:w:W:p:c:r:t:s:R:D:P:q:M:d:TCBLFh",
                                 options, NULL)) != -1) {
        switch (option) {
        case 'm':
            mode = optarg;
            break;
        case 'w':
            config.worker_count = (uint16_t)strtoul(optarg, NULL, 10);
            break;
        case 'W':
            config.max_worker_count = (uint16_t)strtoul(optarg, NULL, 10);
            break;
        case 'p':
            config.packet_count = strtoull(optarg, NULL, 10);
            break;
        case 'c':
            config.flow_capacity_per_worker =
                (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'r':
            config.ring_size = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 't':
            config.timeout_seconds = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 's':
            config.stats_interval_seconds = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'R':
            config.rule_path = optarg;
            break;
        case 'D':
            config.direction_path = optarg;
            break;
        case 'P':
            config.port_id = (uint16_t)strtoul(optarg, NULL, 10);
            break;
        case 'q':
            config.rx_queue_count = (uint16_t)strtoul(optarg, NULL, 10);
            break;
        case 'M':
            config.rx_mbuf_count = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'd':
            config.dispatcher_count = (uint16_t)strtoul(optarg, NULL, 10);
            break;
        case 'T':
            config.tx_enabled = true;
            break;
        case 'C':
            config.cli_enabled = true;
            break;
        case 'B':
            config.dashboard_enabled = true;
            break;
        case 'L':
            config.per_dispatcher_limit = true;
            break;
        case 'F':
            config.fixed_workers = true;
            break;
        case 'h':
        default:
            usage(argv[0]);
            rte_eal_cleanup();
            return option == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }
    if (config.max_worker_count == 0)
        config.max_worker_count = config.worker_count;
    if (config.dashboard_enabled && config.stats_interval_seconds == 0)
        config.stats_interval_seconds = 1;
    if (config.fixed_workers)
        config.max_worker_count = config.worker_count;
    if (config.worker_count == 0 ||
        config.max_worker_count == 0 ||
        config.rx_queue_count == 0 ||
        config.dispatcher_count == 0 ||
        config.worker_count > config.max_worker_count ||
        config.max_worker_count > FT_MAX_WORKERS ||
        config.dispatcher_count > FT_MAX_DISPATCHERS) {
        usage(argv[0]);
        rte_eal_cleanup();
        return EXIT_FAILURE;
    }
    if (strcmp(mode, "ethdev") != 0) {
        usage(argv[0]);
        rte_eal_cleanup();
        return EXIT_FAILURE;
    }
    if (config.rx_queue_count != config.dispatcher_count) {
        fprintf(stderr, "--rx-queues must equal --dispatchers\n");
        usage(argv[0]);
        rte_eal_cleanup();
        return EXIT_FAILURE;
    }
    if (config.dispatcher_count > 1 && !config.fixed_workers) {
        fprintf(stderr, "--dispatchers > 1 requires --fixed-workers\n");
        usage(argv[0]);
        rte_eal_cleanup();
        return EXIT_FAILURE;
    }
    result = ft_pipeline_run_ethdev(&config);
    rte_eal_cleanup();
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
