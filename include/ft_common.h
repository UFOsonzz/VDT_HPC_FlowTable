#ifndef FT_COMMON_H
#define FT_COMMON_H

// dinh nghia header dung chung cho ca project
#include <stdint.h>
#include <stdbool.h>

#include <rte_common.h>

#define FT_INGRESS_PORT_UNKNOWN UINT16_MAX

typedef enum {
    FT_DIR_UNKNOWN = 0,
    FT_DIR_UPLINK = 1,
    FT_DIR_DOWNLINK = 2
} ft_direction_t;

/*
    uplink: src = client, dst = server
    downlink: nguoc lai
    unknown: kbt
*/

#endif