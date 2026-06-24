#include <stdio.h>
#include <stdlib.h>

// eal & lcore api
#include <rte_eal.h>
#include <rte_lcore.h>

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
    int consumed; // so argument lien quan dpdk duoc eal tieu thu
    
    consumed = rte_eal_init(argc, argv);
    if (consumed < 0) {
        fprintf(stderr, "EAL init failed\n");
        return EXIT_FAILURE;
    } else {
        printf("EAL init success, consumed %d parameters\n", consumed);
    }
    print_eal_info();
    /*
    
    
    
    */
    int cleanup = rte_eal_cleanup();
    if (cleanup < 0) {
        fprintf(stderr, "EAL clean up failed\n");
        return EXIT_FAILURE;
    } else {
        printf("EAL clean up completed\n");
        return EXIT_SUCCESS;
    }
}