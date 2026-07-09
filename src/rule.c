#include "ft_rule.h"

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

static bool is_any(const char *text) {
    return text == NULL || *text == '\0' || strcmp(text, "*") == 0 ||
           strcasecmp(text, "any") == 0 || strcasecmp(text, "NA") == 0 ||
           strcasecmp(text, "N/A") == 0;
}

static int parse_ipv4(const char *text, uint32_t *ip) {
    struct in_addr address;

    if (inet_pton(AF_INET, text, &address) != 1)
        return -1;
    *ip = ntohl(address.s_addr);
    return 0;
}

static int parse_ipv4_match(const char *prefix_text,
                 const char *exact_text,
                 ft_ipv4_match_t *match) {
    char copy[64];
    char *slash;
    unsigned long bits;
    uint32_t ip;

    memset(match, 0, sizeof(*match));
    match->any = true;
    if (!is_any(exact_text)) {
        if (parse_ipv4(exact_text, &ip) != 0)
            return -1;
        match->network = ip;
        match->mask = UINT32_MAX;
        match->any = false;
        return 0;
    }
    if (is_any(prefix_text))
        return 0;
    snprintf(copy, sizeof(copy), "%s", prefix_text);
    slash = strchr(copy, '/');
    if (slash == NULL)
        return -1;
    *slash++ = '\0';
    bits = strtoul(slash, NULL, 10);
    if (bits > 32 || parse_ipv4(copy, &ip) != 0)
        return -1;
    match->mask = bits == 0 ? 0 : UINT32_MAX << (32 - bits);
    match->network = ip & match->mask;
    match->any = false;
    return 0;
}

static void parse_port(const char *text, ft_port_match_t *port) {
    port->any = is_any(text);
    port->port = port->any ? 0 : (uint16_t)strtoul(text, NULL, 10);
}

static ft_action_t parse_action(const char *text) {
    if (text != NULL && strcasecmp(text, "DROP") == 0)
        return FT_ACTION_DROP;
    if (text != NULL && strcasecmp(text, "LOG") == 0)
        return FT_ACTION_LOG;
    if (text != NULL && strcasecmp(text, "COUNT") == 0)
        return FT_ACTION_COUNT;
    return FT_ACTION_FORWARD;
}

static int compare_rule(const void *left, const void *right) {
    const ft_rule_t *a = left;
    const ft_rule_t *b = right;

    if (a->precedence < b->precedence)
        return -1;
    if (a->precedence > b->precedence)
        return 1;
    return (int)a->id - (int)b->id;
}

int ft_rule_set_load(ft_rule_set_t *set, const char *path) {
    FILE *file;
    char line[1024];
    unsigned int line_number = 0;

    if (set == NULL || path == NULL)
        return -1;
    memset(set, 0, sizeof(*set));
    set->default_rule_id = UINT16_MAX;
    file = fopen(path, "r");
    if (file == NULL)
        return -1;

    while (fgets(line, sizeof(line), file) != NULL) {
        char *fields[11] = {0};
        char *save = NULL;
        char *field;
        unsigned int count = 0;
        ft_rule_t *rule;

        line_number++;
        field = strtok_r(line, ",", &save);
        while (field != NULL && count < RTE_DIM(fields)) {
            fields[count++] = trim(field);
            field = strtok_r(NULL, ",", &save);
        }
        if (count == 0 || *fields[0] == '\0' || *fields[0] == '#')
            continue;
        if (strcasecmp(fields[0], "precedence") == 0)
            continue;
        if (count < RTE_DIM(fields) || set->count == FT_MAX_RULES) {
            fclose(file);
            return -(int)line_number;
        }
        rule = &set->rules[set->count];
        memset(rule, 0, sizeof(*rule));
        rule->id = set->count;
        rule->precedence = (uint16_t)strtoul(fields[0], NULL, 10);
        snprintf(rule->name, sizeof(rule->name), "%s", fields[1]);
        snprintf(rule->group, sizeof(rule->group), "%s", fields[2]);
        if (parse_ipv4_match(fields[7], fields[8], &rule->src_ip) != 0 ||
            parse_ipv4_match(fields[3], fields[4], &rule->dst_ip) != 0) {
            fclose(file);
            return -(int)line_number;
        }
        parse_port(fields[9], &rule->src_port);
        parse_port(fields[5], &rule->dst_port);
        rule->protocol_any = is_any(fields[6]);
        if (!rule->protocol_any) {
            if (strcasecmp(fields[6], "tcp") == 0)
                rule->protocol = IPPROTO_TCP;
            else if (strcasecmp(fields[6], "udp") == 0)
                rule->protocol = IPPROTO_UDP;
            else
                rule->protocol = (uint8_t)strtoul(fields[6], NULL, 10);
        }
        rule->action = parse_action(fields[10]);
        if (strcasecmp(rule->name, "DEFAULT") == 0)
            set->default_rule_id = rule->id;
        set->count++;
    }
    fclose(file);
    qsort(set->rules, set->count, sizeof(set->rules[0]), compare_rule);
    set->default_rule_id = UINT16_MAX;
    for (uint16_t i = 0; i < set->count; i++) {
        set->rules[i].id = i;
        if (strcasecmp(set->rules[i].name, "DEFAULT") == 0)
            set->default_rule_id = i;
    }
    if (set->default_rule_id == UINT16_MAX && set->count < FT_MAX_RULES) {
        ft_rule_t *rule = &set->rules[set->count];
        memset(rule, 0, sizeof(*rule));
        rule->id = set->count;
        rule->precedence = UINT16_MAX;
        snprintf(rule->name, sizeof(rule->name), "DEFAULT");
        snprintf(rule->group, sizeof(rule->group), "default");
        rule->src_ip.any = true;
        rule->dst_ip.any = true;
        rule->src_port.any = true;
        rule->dst_port.any = true;
        rule->protocol_any = true;
        rule->action = FT_ACTION_FORWARD;
        set->default_rule_id = set->count++;
    }
    return 0;
}

static bool ip_matches(const ft_ipv4_match_t *match, uint32_t ip) {
    return match->any || (ip & match->mask) == match->network;
}

const ft_rule_t *ft_rule_match(const ft_rule_set_t *set, const ft_flow_key_t *key) {
    uint16_t i;

    if (set == NULL || key == NULL)
        return NULL;
    for (i = 0; i < set->count; i++) {
        const ft_rule_t *rule = &set->rules[i];
        if (!rule->protocol_any && rule->protocol != key->protocol)
            continue;
        if (!ip_matches(&rule->src_ip, key->client_ip) ||
            !ip_matches(&rule->dst_ip, key->server_ip))
            continue;
        if (!rule->src_port.any && rule->src_port.port != key->client_port)
            continue;
        if (!rule->dst_port.any && rule->dst_port.port != key->server_port)
            continue;
        return rule;
    }
    return NULL;
}

const char *ft_action_name(ft_action_t action) {
    switch (action) {
    case FT_ACTION_DROP:
        return "DROP";
    case FT_ACTION_LOG:
        return "LOG";
    case FT_ACTION_COUNT:
        return "COUNT";
    case FT_ACTION_FORWARD:
    default:
        return "FORWARD";
    }
}
