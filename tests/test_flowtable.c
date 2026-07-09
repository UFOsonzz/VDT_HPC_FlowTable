#include "ft_config.h"
#include "ft_flow.h"
#include "ft_packet.h"
#include "ft_rule.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_eal.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_tcp.h>
#include <rte_udp.h>

static unsigned int tests_run;
static unsigned int tests_failed;

#define CHECK(condition)                                                       \
    do {                                                                       \
        tests_run++;                                                           \
        if (!(condition)) {                                                    \
            tests_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                    #condition);                                               \
        }                                                                      \
    } while (0)

static void test_bidirectional_key(void) {
    ft_direction_config_t directions;
    ft_packet_t uplink = {
        .src_ip = 0x0a000001,
        .dst_ip = 0x8efa0001,
        .src_port = 40000,
        .dst_port = 443,
        .vlan_id = 100,
        .protocol = IPPROTO_TCP,
    };
    ft_packet_t downlink = {
        .src_ip = 0x8efa0001,
        .dst_ip = 0x0a000001,
        .src_port = 443,
        .dst_port = 40000,
        .vlan_id = 200,
        .protocol = IPPROTO_TCP,
    };
    ft_normalized_flow_t a;
    ft_normalized_flow_t b;

    CHECK(ft_direction_config_load(&directions,
                                   "config/direction_rules.csv") == 0);
    ft_packet_normalize(&uplink, &directions, &a);
    ft_packet_normalize(&downlink, &directions, &b);
    CHECK(memcmp(&a.key, &b.key, sizeof(a.key)) == 0);
    CHECK(a.direction == FT_DIR_UPLINK);
    CHECK(b.direction == FT_DIR_DOWNLINK);
    CHECK(ft_flow_hash(&a.key) == ft_flow_hash(&b.key));
}

static void test_rule_reuse_for_both_directions(void) {
    ft_rule_set_t rules;
    ft_flow_key_t key = {
        .tenant_id = 1,
        .protocol = IPPROTO_TCP,
        .client_ip = 0x0a000001,
        .server_ip = 0x8efa0001,
        .client_port = 40000,
        .server_port = 443,
    };
    const ft_rule_t *rule;

    CHECK(ft_rule_set_load(&rules, "tests/data/spi_rules_test.csv") == 0);
    rule = ft_rule_match(&rules, &key);
    CHECK(rule != NULL);
    CHECK(strcmp(rule->name, "YOUTUBE_ALLOW") == 0);
    CHECK(rule->action == FT_ACTION_FORWARD);

    key.server_ip = 0xc6336401;
    key.server_port = 22;
    rule = ft_rule_match(&rules, &key);
    CHECK(rule != NULL);
    CHECK(strcmp(rule->name, "SSH_BLOCK") == 0);
    CHECK(rule->action == FT_ACTION_DROP);

    key.protocol = IPPROTO_UDP;
    key.server_ip = 0xc0000201;
    key.server_port = 9999;
    rule = ft_rule_match(&rules, &key);
    CHECK(rule != NULL);
    CHECK(strcmp(rule->name, "DEFAULT") == 0);
}

static void test_flow_lifecycle(void) {
    ft_flow_table_t table;
    ft_flow_key_t key = {
        .tenant_id = 1,
        .protocol = IPPROTO_UDP,
        .client_ip = 0x0a000001,
        .server_ip = 0x08080808,
        .client_port = 50000,
        .server_port = 53,
    };
    ft_flow_entry_t *entry;
    bool created;

    CHECK(ft_flow_table_create(&table, "unit_flow_lifecycle", 1024, 0) == 0);
    entry = ft_flow_table_get_or_create(&table, &key, 1000, &created);
    CHECK(entry != NULL && created);
    entry->rule_id = 2;
    entry = ft_flow_table_get_or_create(&table, &key, 1010, &created);
    CHECK(entry != NULL && !created);
    CHECK(entry->rule_id == 2);
    CHECK(table.active == 1 && table.created == 1);
    CHECK(ft_flow_table_age(&table, 1201, 100, table.capacity) == 1);
    CHECK(table.active == 0 && table.deleted == 1 && table.timed_out == 1);
    CHECK(ft_flow_table_lookup(&table, &key) == NULL);
    ft_flow_table_destroy(&table);
}

static void test_worker_distribution(void) {
    uint32_t buckets[4] = {0};
    uint32_t min = UINT32_MAX;
    uint32_t max = 0;

    for (uint32_t i = 0; i < 100000; i++) {
        ft_flow_key_t key = {
            .tenant_id = 1,
            .protocol = IPPROTO_TCP,
            .client_ip = 0x0a000001U + i,
            .server_ip = 0x8efa0001U + (i & 0xffffU),
            .client_port = (uint16_t)(1024 + i % 50000),
            .server_port = 443,
        };
        buckets[ft_flow_hash(&key) % 4]++;
    }
    for (unsigned int i = 0; i < RTE_DIM(buckets); i++) {
        if (buckets[i] < min)
            min = buckets[i];
        if (buckets[i] > max)
            max = buckets[i];
    }
    CHECK(max - min < 1500);
}

static void test_untagged_bidirectional_key(void) {
    ft_direction_config_t directions;
    ft_packet_t uplink = {
        .src_ip = 0x0a000001,
        .dst_ip = 0x8efa0001,
        .src_port = 40000,
        .dst_port = 443,
        .vlan_id = 0,
        .ingress_port = FT_INGRESS_PORT_UNKNOWN,
        .protocol = IPPROTO_TCP,
    };
    ft_packet_t downlink = {
        .src_ip = 0x8efa0001,
        .dst_ip = 0x0a000001,
        .src_port = 443,
        .dst_port = 40000,
        .vlan_id = 0,
        .ingress_port = FT_INGRESS_PORT_UNKNOWN,
        .protocol = IPPROTO_TCP,
    };
    ft_normalized_flow_t a;
    ft_normalized_flow_t b;

    CHECK(ft_direction_config_load(&directions,
                                   "config/direction_rules.csv") == 0);
    ft_packet_normalize(&uplink, &directions, &a);
    ft_packet_normalize(&downlink, &directions, &b);
    CHECK(a.direction == FT_DIR_UPLINK);
    CHECK(b.direction == FT_DIR_DOWNLINK);
    CHECK(a.key.tenant_id == 1 && b.key.tenant_id == 1);
    CHECK(memcmp(&a.key, &b.key, sizeof(a.key)) == 0);
    CHECK(ft_flow_hash(&a.key) == ft_flow_hash(&b.key));
}

static void test_direction_strategy_priority(void) {
    ft_direction_config_t directions = {0};
    uint16_t tenant_id = 0;
    ft_direction_t direction = FT_DIR_UNKNOWN;

    directions.count = 2;
    directions.rules[0] = (ft_direction_rule_t) {
        .match = FT_DIRECTION_MATCH_VLAN,
        .value = 100,
        .tenant_id = 1,
        .direction = FT_DIR_UPLINK,
    };
    directions.rules[1] = (ft_direction_rule_t) {
        .match = FT_DIRECTION_MATCH_INGRESS_PORT,
        .value = 5,
        .tenant_id = 9,
        .direction = FT_DIR_DOWNLINK,
    };
    directions.vlan_rule_index[100] = 1;
    directions.ingress_rule_indices[0] = 1;
    directions.ingress_rule_count = 1;

    CHECK(ft_direction_resolve(&directions, 5, 100,
                               0x0a000001, 0x08080808,
                               0, FT_DIR_UNKNOWN,
                               &tenant_id, &direction));
    CHECK(tenant_id == 9 && direction == FT_DIR_DOWNLINK);

    CHECK(ft_direction_resolve(&directions, 5, 100,
                               0x0a000001, 0x08080808,
                               7, FT_DIR_UPLINK,
                               &tenant_id, &direction));
    CHECK(tenant_id == 7 && direction == FT_DIR_UPLINK);
}

static void test_unknown_direction_and_tenant_isolation(void) {
    ft_direction_config_t directions;
    ft_packet_t forward = {
        .src_ip = 0xac100001,
        .dst_ip = 0xc0000201,
        .src_port = 12345,
        .dst_port = 80,
        .vlan_id = 999,
        .ingress_port = FT_INGRESS_PORT_UNKNOWN,
        .protocol = IPPROTO_TCP,
    };
    ft_packet_t reverse = {
        .src_ip = 0xc0000201,
        .dst_ip = 0xac100001,
        .src_port = 80,
        .dst_port = 12345,
        .vlan_id = 999,
        .ingress_port = FT_INGRESS_PORT_UNKNOWN,
        .protocol = IPPROTO_TCP,
    };
    ft_normalized_flow_t a;
    ft_normalized_flow_t b;
    ft_normalized_flow_t tenant_one;
    ft_normalized_flow_t tenant_two;

    CHECK(ft_direction_config_load(&directions,
                                   "config/direction_rules.csv") == 0);
    ft_packet_normalize(&forward, &directions, &a);
    ft_packet_normalize(&reverse, &directions, &b);
    CHECK(a.direction == FT_DIR_UNKNOWN && b.direction == FT_DIR_UNKNOWN);
    CHECK(memcmp(&a.key, &b.key, sizeof(a.key)) == 0);

    forward.vlan_id = 100;
    ft_packet_normalize(&forward, &directions, &tenant_one);
    forward.vlan_id = 110;
    ft_packet_normalize(&forward, &directions, &tenant_two);
    CHECK(tenant_one.key.tenant_id == 1);
    CHECK(tenant_two.key.tenant_id == 2);
    CHECK(memcmp(&tenant_one.key, &tenant_two.key,
                 sizeof(tenant_one.key)) != 0);
}

static void test_traffic_classification(void) {
    ft_normalized_flow_t flow = {0};

    flow.key.protocol = IPPROTO_TCP;
    flow.key.server_port = 80;
    CHECK(ft_packet_classify(&flow) == FT_TRAFFIC_HTTP);
    flow.key.server_port = 443;
    CHECK(ft_packet_classify(&flow) == FT_TRAFFIC_HTTPS);
    flow.key.server_port = 22;
    CHECK(ft_packet_classify(&flow) == FT_TRAFFIC_TCP);
    flow.key.protocol = IPPROTO_UDP;
    flow.key.server_port = 53;
    CHECK(ft_packet_classify(&flow) == FT_TRAFFIC_DNS);
    flow.key.server_port = 123;
    CHECK(ft_packet_classify(&flow) == FT_TRAFFIC_UDP);
    flow.key.protocol = IPPROTO_ICMP;
    CHECK(ft_packet_classify(&flow) == FT_TRAFFIC_OTHER);
}

static void test_vlan_tcp_parser(void) {
    struct rte_mempool *pool;
    struct rte_mbuf *mbuf;
    struct rte_ether_hdr *ether;
    struct rte_vlan_hdr *vlan;
    struct rte_ipv4_hdr *ipv4;
    struct rte_tcp_hdr *tcp;
    ft_packet_t packet;
    uint8_t *data;
    size_t frame_size = sizeof(*ether) + sizeof(*vlan) +
                        sizeof(*ipv4) + sizeof(*tcp);

    pool = rte_pktmbuf_pool_create("unit_parser_pool", 128, 0, 0,
                                   RTE_MBUF_DEFAULT_BUF_SIZE, 0);
    CHECK(pool != NULL);
    if (pool == NULL)
        return;
    mbuf = rte_pktmbuf_alloc(pool);
    CHECK(mbuf != NULL);
    if (mbuf == NULL) {
        rte_mempool_free(pool);
        return;
    }
    data = (uint8_t *)rte_pktmbuf_append(mbuf, (uint16_t)frame_size);
    CHECK(data != NULL);
    memset(data, 0, frame_size);
    ether = (struct rte_ether_hdr *)data;
    vlan = (struct rte_vlan_hdr *)(ether + 1);
    ipv4 = (struct rte_ipv4_hdr *)(vlan + 1);
    tcp = (struct rte_tcp_hdr *)(ipv4 + 1);
    ether->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_VLAN);
    vlan->vlan_tci = rte_cpu_to_be_16(100);
    vlan->eth_proto = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    ipv4->version_ihl = 0x45;
    ipv4->total_length = rte_cpu_to_be_16(sizeof(*ipv4) + sizeof(*tcp));
    ipv4->next_proto_id = IPPROTO_TCP;
    ipv4->src_addr = rte_cpu_to_be_32(0x0a000001);
    ipv4->dst_addr = rte_cpu_to_be_32(0x8efa0001);
    tcp->src_port = rte_cpu_to_be_16(40000);
    tcp->dst_port = rte_cpu_to_be_16(443);

    CHECK(ft_packet_parse_mbuf(mbuf, &packet) == 0);
    CHECK(packet.vlan_id == 100);
    CHECK(packet.src_ip == 0x0a000001 && packet.dst_ip == 0x8efa0001);
    CHECK(packet.src_port == 40000 && packet.dst_port == 443);
    CHECK(packet.protocol == IPPROTO_TCP);
    rte_pktmbuf_free(mbuf);

    frame_size = sizeof(*ether) + sizeof(*ipv4) + sizeof(struct rte_udp_hdr);
    mbuf = rte_pktmbuf_alloc(pool);
    CHECK(mbuf != NULL);
    if (mbuf == NULL) {
        rte_mempool_free(pool);
        return;
    }
    data = (uint8_t *)rte_pktmbuf_append(mbuf, (uint16_t)frame_size);
    CHECK(data != NULL);
    memset(data, 0, frame_size);
    ether = (struct rte_ether_hdr *)data;
    ipv4 = (struct rte_ipv4_hdr *)(ether + 1); {
        struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ipv4 + 1);
        ether->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
        ipv4->version_ihl = 0x45;
        ipv4->total_length =
            rte_cpu_to_be_16(sizeof(*ipv4) + sizeof(*udp));
        ipv4->next_proto_id = IPPROTO_UDP;
        ipv4->src_addr = rte_cpu_to_be_32(0x0a000002);
        ipv4->dst_addr = rte_cpu_to_be_32(0x08080808);
        udp->src_port = rte_cpu_to_be_16(50000);
        udp->dst_port = rte_cpu_to_be_16(53);
    }
    CHECK(ft_packet_parse_mbuf(mbuf, &packet) == 0);
    CHECK(packet.vlan_id == 0);
    CHECK(packet.src_ip == 0x0a000002 && packet.dst_ip == 0x08080808);
    CHECK(packet.src_port == 50000 && packet.dst_port == 53);
    CHECK(packet.protocol == IPPROTO_UDP);
    rte_pktmbuf_free(mbuf);
    rte_mempool_free(pool);
}

static void test_flow_capacity(void) {
    ft_flow_table_t table;
    bool created;

    CHECK(ft_flow_table_create(&table, "unit_flow_capacity", 64, 0) == 0);
    for (uint32_t i = 0; i < 64; i++) {
        ft_flow_key_t key = {
            .tenant_id = 1,
            .protocol = IPPROTO_TCP,
            .client_ip = 0x0a000001U + i,
            .server_ip = 0xc0000201,
            .client_port = (uint16_t)(10000 + i),
            .server_port = 80,
        };
        CHECK(ft_flow_table_get_or_create(&table, &key, i, &created) != NULL);
    } {
        ft_flow_key_t extra = {
            .tenant_id = 1,
            .protocol = IPPROTO_TCP,
            .client_ip = 0x0a00ffff,
            .server_ip = 0xc0000201,
            .client_port = 65500,
            .server_port = 80,
        };
        CHECK(ft_flow_table_get_or_create(&table, &extra, 100, &created) == NULL);
    }
    CHECK(table.active == 64);
    ft_flow_table_destroy(&table);
}

int main(int argc, char **argv) {
    if (rte_eal_init(argc, argv) < 0)
        return EXIT_FAILURE;
    test_bidirectional_key();
    test_rule_reuse_for_both_directions();
    test_flow_lifecycle();
    test_worker_distribution();
    test_untagged_bidirectional_key();
    test_direction_strategy_priority();
    test_unknown_direction_and_tenant_isolation();
    test_traffic_classification();
    test_vlan_tcp_parser();
    test_flow_capacity();
    printf("tests=%u passed=%u failed=%u\n",
           tests_run, tests_run - tests_failed, tests_failed);
    rte_eal_cleanup();
    return tests_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
