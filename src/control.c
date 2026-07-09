#include "pipeline_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_pause.h>

#define FT_ANSI_CLEAR "\033[2J\033[H"
#define FT_ANSI_RESET "\033[0m"
#define FT_ANSI_BOLD "\033[1m"
#define FT_ANSI_CYAN "\033[36m"

volatile sig_atomic_t force_quit;
static volatile sig_atomic_t reload_signal;
static volatile sig_atomic_t scale_up_signal;
static volatile sig_atomic_t scale_down_signal;

static void handle_signal(int signal_number) {
    (void)signal_number;
    force_quit = 1;
}

static void handle_reload_signal(int signal_number) {
    (void)signal_number;
    reload_signal = 1;
}

static void handle_scale_up_signal(int signal_number) {
    (void)signal_number;
    scale_up_signal = 1;
}

static void handle_scale_down_signal(int signal_number) {
    (void)signal_number;
    scale_down_signal = 1;
}

void ft_control_reset_signals(void) {
    force_quit = 0;
    reload_signal = 0;
    scale_up_signal = 0;
    scale_down_signal = 0;
}

void ft_control_install_signal_handlers(void) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_reload_signal);
    signal(SIGUSR1, handle_scale_up_signal);
    signal(SIGUSR2, handle_scale_down_signal);
}

static ft_rule_set_t *load_rule_snapshot(const char *path) {
    ft_rule_set_t *rules = calloc(1, sizeof(*rules));

    if (rules == NULL)
        return NULL;
    if (ft_rule_set_load(rules, path) != 0) {
        free(rules);
        return NULL;
    }
    return rules;
}

int ft_rule_store_init(ft_rule_store_t *store, const char *path) {
    ft_rule_set_t *rules;

    memset(store, 0, sizeof(*store));
    rules = load_rule_snapshot(path);
    if (rules == NULL)
        return -1;
    atomic_init(&store->current, rules);
    store->version = 1;
    return 0;
}

ft_rule_set_t *ft_rule_store_current(ft_rule_store_t *store) {
    return atomic_load_explicit(&store->current, memory_order_acquire);
}

int ft_rule_store_reload(ft_rule_store_t *store, const char *path) {
    ft_rule_set_t *new_rules;
    ft_rule_set_t *old_rules;

    if (store->retired_count == FT_RULE_RETIRED_MAX)
        return -1;
    new_rules = load_rule_snapshot(path);
    if (new_rules == NULL)
        return -1;
    old_rules = atomic_exchange_explicit(&store->current, new_rules,
                                         memory_order_acq_rel);
    if (old_rules != NULL)
        store->retired[store->retired_count++] = old_rules;
    store->version++;
    printf("rules reloaded: path=%s version=%" PRIu64 " count=%u\n",
           path, store->version, new_rules->count);
    return 0;
}

void ft_rule_store_destroy(ft_rule_store_t *store) {
    ft_rule_set_t *current = ft_rule_store_current(store);

    free(current);
    for (uint16_t i = 0; i < store->retired_count; i++)
        free(store->retired[i]);
    memset(store, 0, sizeof(*store));
}

static void request_scale(_Atomic int *scale_delta, int delta) {
    atomic_fetch_add_explicit(scale_delta, delta, memory_order_release);
}

static void request_show_value(_Atomic int *show_request,
                               _Atomic bool *stop,
                               int request) {
    atomic_store_explicit(show_request, request, memory_order_release);
    while (!force_quit &&
           !atomic_load_explicit(stop, memory_order_acquire) &&
           atomic_load_explicit(show_request, memory_order_acquire) !=
               FT_SHOW_NONE)
        rte_pause();
}

static void request_show(_Atomic int *show_request,
                         _Atomic bool *stop,
                         ft_show_request_t request) {
    request_show_value(show_request, stop, (int)request);
}

static bool parse_worker_detail(const char *line, unsigned long *worker_id) {
    char *end = NULL;

    *worker_id = strtoul(line + 12, &end, 10);
    return end != line + 12 && *end == '\0' && *worker_id < FT_MAX_WORKERS;
}

void *ft_cli_loop(void *argument) {
    ft_cli_context_t *context = argument;
    char line[128];

    printf(FT_ANSI_CLEAR);
    printf(FT_ANSI_BOLD FT_ANSI_CYAN "FlowTable CLI" FT_ANSI_RESET "\n");
    printf("commands: help | show statistics | show benchmark | show flow |"
           " show worker | show worker N | show traffic | show dashboard |"
           " rules | reload | scale up | scale down | quit\n\n");
    printf("flowtable> ");
    fflush(stdout);
    while (!atomic_load_explicit(context->stop, memory_order_acquire) &&
           fgets(line, sizeof(line), stdin) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strcmp(line, "help") == 0) {
            ft_stats_print_title("help");
            printf("+-----------------+-------------------------------------------+\n");
            printf("| command         | description                               |\n");
            printf("+-----------------+-------------------------------------------+\n");
            printf("| show statistics | total packet, byte, flow and rule stats    |\n");
            printf("| show benchmark  | realtime PPS, flow rate and drop counters  |\n");
            printf("| show flow       | per-worker flow lifecycle counters         |\n");
            printf("| show worker     | per-worker queue and packet counters       |\n");
            printf("| show worker N   | one worker core traffic/class counters     |\n");
            printf("| show traffic    | direction, class and rule-hit tables        |\n");
            printf("| show dashboard  | dashboard with active/pps/drop graphs       |\n");
            printf("| rules           | rule version and loaded rule count          |\n");
            printf("| reload          | reload SPI rules for new flows              |\n");
            printf("| scale up/down   | adjust active workers in dynamic mode       |\n");
            printf("| quit            | stop the pipeline                           |\n");
            printf("+-----------------+-------------------------------------------+\n");
        } else if (strcmp(line, "show") == 0 ||
                   strcmp(line, "show statistics") == 0) {
            request_show(context->show_request, context->stop,
                         FT_SHOW_STATISTICS);
        } else if (strcmp(line, "show benchmark") == 0) {
            request_show(context->show_request, context->stop,
                         FT_SHOW_BENCHMARK);
        } else if (strcmp(line, "show flow") == 0) {
            request_show(context->show_request, context->stop, FT_SHOW_FLOW);
        } else if (strcmp(line, "show worker") == 0) {
            request_show(context->show_request, context->stop, FT_SHOW_WORKER);
        } else if (strncmp(line, "show worker ", 12) == 0) {
            unsigned long worker_id;

            if (!parse_worker_detail(line, &worker_id)) {
                printf("invalid worker id: %s\n", line + 12);
            } else {
                request_show_value(context->show_request, context->stop,
                                   FT_SHOW_WORKER_DETAIL_BASE +
                                   (int)worker_id);
            }
        } else if (strcmp(line, "show traffic") == 0) {
            request_show(context->show_request, context->stop, FT_SHOW_TRAFFIC);
        } else if (strcmp(line, "show dashboard") == 0) {
            request_show(context->show_request, context->stop,
                         FT_SHOW_DASHBOARD);
        } else if (strcmp(line, "rules") == 0) {
            ft_rule_set_t *rules = ft_rule_store_current(context->rule_store);

            printf("rules version=%" PRIu64 " count=%u path=%s\n",
                   context->rule_store->version,
                   rules == NULL ? 0 : rules->count,
                   context->rule_path);
        } else if (strcmp(line, "reload") == 0) {
            atomic_store_explicit(context->reload_requested, true,
                                  memory_order_release);
        } else if (strcmp(line, "scale up") == 0) {
            request_scale(context->scale_delta, 1);
        } else if (strcmp(line, "scale down") == 0) {
            request_scale(context->scale_delta, -1);
        } else if (strcmp(line, "quit") == 0) {
            force_quit = 1;
            atomic_store_explicit(context->stop, true, memory_order_release);
            break;
        } else {
            printf("unknown command: %s\n", line);
        }
        printf("flowtable> ");
        fflush(stdout);
    }
    return NULL;
}

void ft_apply_control_events(const ft_app_config_t *config,
                             ft_worker_t *workers,
                             uint16_t worker_count,
                             _Atomic uint16_t *active_worker_count,
                             ft_rule_store_t *rule_store,
                             _Atomic bool *reload_requested,
                             _Atomic int *scale_delta,
                             _Atomic int *show_request,
                             ft_dashboard_state_t *dashboard_state,
                             uint64_t dispatched) {
    int delta;
    int requested_show;
    uint16_t active;

    if (reload_signal) {
        reload_signal = 0;
        atomic_store_explicit(reload_requested, true, memory_order_release);
    }
    if (scale_up_signal) {
        scale_up_signal = 0;
        if (!config->fixed_workers)
            request_scale(scale_delta, 1);
    }
    if (scale_down_signal) {
        scale_down_signal = 0;
        if (!config->fixed_workers)
            request_scale(scale_delta, -1);
    }
    if (atomic_exchange_explicit(reload_requested, false,
                                 memory_order_acq_rel)) {
        if (ft_rule_store_reload(rule_store, config->rule_path) != 0)
            fprintf(stderr, "rule reload failed: %s\n", config->rule_path);
    }
    if (config->fixed_workers)
        atomic_store_explicit(scale_delta, 0, memory_order_release);
    delta = config->fixed_workers ?
        0 : atomic_exchange_explicit(scale_delta, 0, memory_order_acq_rel);
    if (delta != 0) {
        uint16_t previous;

        active = atomic_load_explicit(active_worker_count, memory_order_acquire);
        previous = active;
        while (delta > 0 && active < worker_count) {
            active++;
            delta--;
        }
        while (delta < 0 && active > 1) {
            active--;
            delta++;
        }
        atomic_store_explicit(active_worker_count, active,
                              memory_order_release);
        if (active != previous)
            printf("active worker count now %u/%u\n", active, worker_count);
    }

    requested_show = atomic_exchange_explicit(show_request, FT_SHOW_NONE,
                                              memory_order_acq_rel);
    if (requested_show == FT_SHOW_NONE)
        return;

    active = atomic_load_explicit(active_worker_count, memory_order_acquire);
    if (requested_show >= FT_SHOW_WORKER_DETAIL_BASE) {
        ft_stats_print_worker_detail(workers, worker_count, active,
                                     (uint16_t)(requested_show -
                                                FT_SHOW_WORKER_DETAIL_BASE));
        return;
    }
    switch ((ft_show_request_t)requested_show) {
    case FT_SHOW_FLOW:
        ft_stats_print_flow(workers, worker_count, active, dispatched);
        break;
    case FT_SHOW_WORKER:
        ft_stats_print_worker(workers, worker_count, active);
        break;
    case FT_SHOW_TRAFFIC:
        ft_stats_print_traffic(workers, worker_count,
                               ft_rule_store_current(rule_store));
        break;
    case FT_SHOW_DASHBOARD:
        ft_stats_print_dashboard(workers, worker_count, active, dispatched,
                                 ft_rule_store_current(rule_store),
                                 dashboard_state, false);
        break;
    case FT_SHOW_BENCHMARK:
        ft_stats_print_benchmark(workers, worker_count, active, dispatched,
                                 ft_rule_store_current(rule_store),
                                 dashboard_state);
        break;
    case FT_SHOW_STATISTICS:
    default:
        ft_stats_print_statistics(workers, worker_count, active, dispatched,
                                  ft_rule_store_current(rule_store));
        break;
    }
}
