#include "app_config.h"
#include "port.h"
#include "rx_loop.h"

#include <getopt.h> // thu vien chuan linux de xu ly tham so dong lenh danh (-p) va (--p)
#include <stdio.h>
#include <stdlib.h>

// eal & lcore api
#include <rte_eal.h>
#include <rte_lcore.h>

static void usage(const char *program) {
    // huong dan su dung
    printf("Usage: %s EAL_ARGS -- APP_ARGS\n"
           "Application options:\n"
           "  --port N        DPDK port id, default 0\n"
           "  --packets N     Stop after N packets, 0 means forever\n"
           "  --burst N       RX burst size, default 32\n"
           "  --promisc       Enable promiscuous mode\n",
           program);
}

void print_eal_info() {
    unsigned int lcore_id;

    printf("DPDK EAL Information:\n");
    printf("Main lcore  :   %u\n", rte_get_main_lcore());
    printf("lcore count :   %u\n", rte_lcore_count());
    printf("Socket count    :   %u\n", rte_socket_count());
    printf("Hugepages:  %s\n", rte_eal_has_hugepages() ? "enabled" : "disabled");

    printf("\nEnabled lcores:\n");

    // RTE_LCORE_FOREACH chi duyet cac lcore da bat
    RTE_LCORE_FOREACH(lcore_id) {
        printf("lcore=%u socket=%u\n", lcore_id, rte_lcore_to_socket_id(lcore_id));
    }
}


int main(int argc, char **argv) {
    app_config_t config = {
        .port_id = 0,
        .rx_queue_id = 0,
        .tx_queue_id = 0,
        .burst_size = 32,
        .max_packets = 10,
        .promiscuous = false,
    };

    app_port_t port;
    int consumed; // so argument lien quan dpdk duoc eal tieu thu
    int opt;
    int ret;

    // dinh nghia cac tham so mo rong dang dai
    static const struct option options[] = {
        {"port", required_argument, NULL, 'p'},
        {"packets", required_argument, NULL, 'n'},
        {"burst", required_argument, NULL, 'b'},
        {"promisc", no_argument, NULL, 'P'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    
    consumed = rte_eal_init(argc, argv);
    if (consumed < 0) {
        fprintf(stderr, "EAL init failed\n");
        return EXIT_FAILURE;
    } else {
        printf("EAL init success, consumed %d parameters\n", consumed);
    }
    print_eal_info();
    
    argc -= consumed; // bo di cac tham so eal, xet cac tham so application
    argv += consumed; // mang argv tinh tien den doan bat dau cac tham so application
    optind = 1;

    // parsing arguments
    while ((opt = getopt_long(argc, argv, "p:n:b:Ph", options, NULL)) != -1) { // quet qua cac tham so nguoi dung nhap vao, co dau : nghia la can tham so
        switch (opt) {
        case 'p':
            // tham so nguoi dung nhap vao duoc luu trong optarg, strtoul voi tham so 10 -> chuyen ve he thap phan
            // "2" -> 2
            config.port_id = (uint16_t)strtoul(optarg, NULL, 10); 
            break;
        case 'n':
            config.max_packets = strtoull(optarg, NULL, 10);
            break;
        case 'b':
            config.burst_size = (uint16_t)strtoul(optarg, NULL, 10);
            break;
        case 'P':
            config.promiscuous = true;
            break;
        case 'h':
        default:
            usage(argv[0]);
            rte_eal_cleanup();
            return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    ret = app_port_init(&port, &config);
    if (ret != 0) {
        rte_eal_cleanup();
        return EXIT_FAILURE;
    }

    ret = app_rx_loop(&port, &config);

    app_port_close(&port);
    rte_eal_cleanup();

    return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}