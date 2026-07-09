#ifndef FT_COMMON_H
#define FT_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>

#include <rte_byteorder.h>
#include <rte_common.h>

#define FT_MAX_WORKERS 16
#define FT_MAX_RULES 256
#define FT_MAX_DIRECTION_RULES 128
#define FT_NAME_LEN 64
#define FT_GROUP_LEN 64
#define FT_CACHE_LINE RTE_CACHE_LINE_SIZE
#define FT_INGRESS_PORT_UNKNOWN UINT16_MAX

typedef enum {
    FT_DIR_UNKNOWN = 0,
    FT_DIR_UPLINK = 1,
    FT_DIR_DOWNLINK = 2
} ft_direction_t;

typedef enum {
    FT_ACTION_FORWARD = 0,
    FT_ACTION_DROP = 1,
    FT_ACTION_LOG = 2,
    FT_ACTION_COUNT = 3
} ft_action_t;

typedef enum {
    FT_TRAFFIC_HTTP = 0,
    FT_TRAFFIC_HTTPS,
    FT_TRAFFIC_DNS,
    FT_TRAFFIC_TCP,
    FT_TRAFFIC_UDP,
    FT_TRAFFIC_OTHER,
    FT_TRAFFIC_CLASS_COUNT
} ft_traffic_class_t;

static inline uint64_t ft_elapsed_cycles(uint64_t now, uint64_t then) {
    return now - then;
}

#endif
