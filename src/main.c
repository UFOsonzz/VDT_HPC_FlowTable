#include "ft_pipeline.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_eal.h>

static void usage(const char *program) {
    printf("Usage: %s EAL_ARGS -- [application options]\n"
           "  --mode MODE        synthetic or ethdev (default: synthetic)\n"
           "  --workers N        Active worker lcores at start (default: 4)\n"
           "  --max-workers N    Launched worker lcores for runtime scale-up\n"
           "  --packets N        Synthetic packets (default: 1000000)\n"
           "  --flows N          Concurrent synthetic flows (default: 100000)\n"
           "  --flow-capacity N  Flow entries per worker (default: 131072)\n"
           "  --ring-size N      Per-worker SPSC ring size (default: 4096)\n"
           "  --timeout N        Flow timeout seconds (default: 5)\n"
           "  --stats-interval N Print live stats every N seconds, 0 disables\n"
           "  --scale-interval N Synthetic auto scale-up every N dispatched packets\n"
           "  --rules PATH       SPI CSV converted from workbook\n"
           "  --directions PATH  Direction strategy CSV\n"
           "  --port N           Ethdev/PCAP PMD port (default: 0)\n"
           "  --tx               Transmit FORWARD packets on per-worker TX queues\n"
           "  --cli              Enable terminal CLI for ethdev mode\n",
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
        .packet_count = 1000000,
        .scale_interval_packets = 0,
        .synthetic_flow_count = 100000,
        .rule_path = "config/spi_rules.csv",
        .direction_path = "config/direction_rules.csv",
    };
    const char *mode = "synthetic";
    static const struct option options[] = {
        {"mode", required_argument, NULL, 'm'},
        {"workers", required_argument, NULL, 'w'},
        {"max-workers", required_argument, NULL, 'W'},
        {"packets", required_argument, NULL, 'p'},
        {"flows", required_argument, NULL, 'f'},
        {"flow-capacity", required_argument, NULL, 'c'},
        {"ring-size", required_argument, NULL, 'r'},
        {"timeout", required_argument, NULL, 't'},
        {"stats-interval", required_argument, NULL, 's'},
        {"scale-interval", required_argument, NULL, 'S'},
        {"rules", required_argument, NULL, 'R'},
        {"directions", required_argument, NULL, 'D'},
        {"port", required_argument, NULL, 'P'},
        {"tx", no_argument, NULL, 'T'},
        {"cli", no_argument, NULL, 'C'},
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
    while ((option = getopt_long(argc, argv, "m:w:W:p:f:c:r:t:s:S:R:D:P:TCh",
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
        case 'f':
            config.synthetic_flow_count = (uint32_t)strtoul(optarg, NULL, 10);
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
        case 'S':
            config.scale_interval_packets = strtoull(optarg, NULL, 10);
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
        case 'T':
            config.tx_enabled = true;
            break;
        case 'C':
            config.cli_enabled = true;
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
    if (config.synthetic_flow_count == 0 ||
        config.worker_count == 0 ||
        config.max_worker_count == 0 ||
        config.worker_count > config.max_worker_count ||
        config.max_worker_count > FT_MAX_WORKERS) {
        usage(argv[0]);
        rte_eal_cleanup();
        return EXIT_FAILURE;
    }
    if (strcmp(mode, "ethdev") == 0)
        result = ft_pipeline_run_ethdev(&config);
    else if (strcmp(mode, "synthetic") == 0)
        result = ft_pipeline_run_synthetic(&config);
    else {
        usage(argv[0]);
        result = -1;
    }
    rte_eal_cleanup();
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
