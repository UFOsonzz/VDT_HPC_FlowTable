#include "ft_config.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *text) {
    char *end;

    while (isspace((unsigned char)*text))
        text++;
    if (*text == '\0')
        return text;
    end = text + strlen(text) - 1;
    while (end > text && isspace((unsigned char)*end))
        *end-- = '\0';
    return text;
}

static int parse_direction(const char *text, ft_direction_t *direction) {
    if (strcasecmp(text, "UPLINK") == 0)
        *direction = FT_DIR_UPLINK;
    else if (strcasecmp(text, "DOWNLINK") == 0)
        *direction = FT_DIR_DOWNLINK;
    else if (strcasecmp(text, "UNKNOWN") == 0)
        *direction = FT_DIR_UNKNOWN;
    else
        return -1;
    return 0;
}

static int parse_prefix(const char *text, uint32_t *network, uint32_t *mask) {
    char copy[64];
    char *slash;
    char *end = NULL;
    struct in_addr address;
    unsigned long bits;

    snprintf(copy, sizeof(copy), "%s", text);
    slash = strchr(copy, '/');
    if (slash == NULL)
        return -1;
    *slash++ = '\0';
    bits = strtoul(slash, &end, 10);
    if (*slash == '\0' || *end != '\0' || bits > 32 ||
        inet_pton(AF_INET, copy, &address) != 1)
        return -1;
    *mask = bits == 0 ? 0 : UINT32_MAX << (32 - bits);
    *network = ntohl(address.s_addr) & *mask;
    return 0;
}

int ft_direction_config_load(ft_direction_config_t *config, const char *path) {
    FILE *file;
    char line[256];
    unsigned int line_number = 0;

    if (config == NULL || path == NULL)
        return -1;
    memset(config, 0, sizeof(*config));
    file = fopen(path, "r");
    if (file == NULL)
        return -1;

    while (fgets(line, sizeof(line), file) != NULL) {
        char *type_text;
        char *value_text;
        char *tenant_text;
        char *direction_text;
        char *save = NULL;
        ft_direction_rule_t *rule;

        line_number++;
        type_text = trim(strtok_r(line, ",", &save));
        if (*type_text == '\0' || *type_text == '#')
            continue;
        value_text = strtok_r(NULL, ",", &save);
        tenant_text = strtok_r(NULL, ",", &save);
        direction_text = strtok_r(NULL, ",", &save);
        if (value_text == NULL || tenant_text == NULL ||
            direction_text == NULL) {
            fclose(file);
            return -(int)line_number;
        }
        if (strcasecmp(type_text, "match_type") == 0)
            continue;
        if (config->count == FT_MAX_DIRECTION_RULES) {
            fclose(file);
            return -2;
        }

        rule = &config->rules[config->count];
        memset(rule, 0, sizeof(*rule));
        value_text = trim(value_text);
        tenant_text = trim(tenant_text);
        direction_text = trim(direction_text);
        rule->tenant_id = (uint16_t)strtoul(tenant_text, NULL, 10);
        if (parse_direction(direction_text, &rule->direction) != 0) {
            fclose(file);
            return -(int)line_number;
        }

        if (strcasecmp(type_text, "INGRESS_PORT") == 0) {
            rule->match = FT_DIRECTION_MATCH_INGRESS_PORT;
            rule->value = (uint16_t)strtoul(value_text, NULL, 10);
        } else if (strcasecmp(type_text, "VLAN") == 0) {
            rule->match = FT_DIRECTION_MATCH_VLAN;
            rule->value = (uint16_t)strtoul(value_text, NULL, 10);
        } else if (strcasecmp(type_text, "SRC_PREFIX") == 0) {
            rule->match = FT_DIRECTION_MATCH_SRC_PREFIX;
            if (parse_prefix(value_text, &rule->network, &rule->mask) != 0) {
                fclose(file);
                return -(int)line_number;
            }
        } else if (strcasecmp(type_text, "DST_PREFIX") == 0) {
            rule->match = FT_DIRECTION_MATCH_DST_PREFIX;
            if (parse_prefix(value_text, &rule->network, &rule->mask) != 0) {
                fclose(file);
                return -(int)line_number;
            }
        } else {
            fclose(file);
            return -(int)line_number;
        }
        if (rule->match == FT_DIRECTION_MATCH_INGRESS_PORT) {
            config->ingress_rule_indices[config->ingress_rule_count++] =
                config->count;
        } else if (rule->match == FT_DIRECTION_MATCH_VLAN) {
            if (rule->value < RTE_DIM(config->vlan_rule_index) &&
                config->vlan_rule_index[rule->value] == 0)
                config->vlan_rule_index[rule->value] = config->count + 1;
        } else if (rule->match == FT_DIRECTION_MATCH_SRC_PREFIX) {
            config->src_prefix_rule_indices[config->src_prefix_rule_count++] =
                config->count;
        } else if (rule->match == FT_DIRECTION_MATCH_DST_PREFIX) {
            config->dst_prefix_rule_indices[config->dst_prefix_rule_count++] =
                config->count;
        }
        config->count++;
    }
    fclose(file);
    return 0;
}

static bool rule_matches(const ft_direction_rule_t *rule,
             uint16_t ingress_port,
             uint16_t vlan_id,
             uint32_t src_ip,
             uint32_t dst_ip) {
    switch (rule->match) {
    case FT_DIRECTION_MATCH_INGRESS_PORT:
        return ingress_port != FT_INGRESS_PORT_UNKNOWN &&
               rule->value == ingress_port;
    case FT_DIRECTION_MATCH_VLAN:
        return vlan_id != 0 && rule->value == vlan_id;
    case FT_DIRECTION_MATCH_SRC_PREFIX:
        return (src_ip & rule->mask) == rule->network;
    case FT_DIRECTION_MATCH_DST_PREFIX:
        return (dst_ip & rule->mask) == rule->network;
    default:
        return false;
    }
}

bool ft_direction_resolve(const ft_direction_config_t *config,
                     uint16_t ingress_port,
                     uint16_t vlan_id,
                     uint32_t src_ip,
                     uint32_t dst_ip,
                     uint16_t tenant_hint,
                     ft_direction_t direction_hint,
                     uint16_t *tenant_id,
                     ft_direction_t *direction) {
    const ft_direction_rule_t *rule;

    if (direction_hint != FT_DIR_UNKNOWN) {
        if (tenant_id != NULL)
            *tenant_id = tenant_hint;
        if (direction != NULL)
            *direction = direction_hint;
        return true;
    }
    if (config == NULL)
        return false;

    for (uint16_t i = 0; i < config->ingress_rule_count; i++) {
        rule = &config->rules[config->ingress_rule_indices[i]];
        if (rule_matches(rule, ingress_port, vlan_id, src_ip, dst_ip))
            goto matched;
    }
    if (vlan_id < RTE_DIM(config->vlan_rule_index) &&
        config->vlan_rule_index[vlan_id] != 0) {
        rule = &config->rules[config->vlan_rule_index[vlan_id] - 1];
        goto matched;
    }
    for (uint16_t i = 0; i < config->src_prefix_rule_count; i++) {
        rule = &config->rules[config->src_prefix_rule_indices[i]];
        if (rule_matches(rule, ingress_port, vlan_id, src_ip, dst_ip))
            goto matched;
    }
    for (uint16_t i = 0; i < config->dst_prefix_rule_count; i++) {
        rule = &config->rules[config->dst_prefix_rule_indices[i]];
        if (rule_matches(rule, ingress_port, vlan_id, src_ip, dst_ip))
            goto matched;
    }
    return false;

matched:
    if (tenant_id != NULL)
        *tenant_id = rule->tenant_id;
    if (direction != NULL)
        *direction = rule->direction;
    return true;
}
