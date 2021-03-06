/*
 * The MIT License (MIT)
 *
 * Copyright © 2015 Franklin "Snaipe" Mathieu <http://snai.pe/>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#define CRITERION_LOGGING_COLORS
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <csptr/smalloc.h>
#include <valgrind/valgrind.h>
#include "criterion/criterion.h"
#include "criterion/options.h"
#include "criterion/ordered-set.h"
#include "criterion/logging.h"
#include "compat/time.h"
#include "compat/posix.h"
#include "compat/processor.h"
#include "wrappers/wrap.h"
#include "string/i18n.h"
#include "io/event.h"
#include "runner_coroutine.h"
#include "stats.h"
#include "runner.h"
#include "report.h"
#include "worker.h"
#include "abort.h"
#include "config.h"
#include "common.h"

#ifdef HAVE_PCRE
#include "string/extmatch.h"
#endif

typedef const char *const msg_t;

#ifdef ENABLE_NLS
static msg_t msg_valgrind_early_exit = N_("%1$sWarning! Criterion has detected "
        "that it is running under valgrind, but the no_early_exit option is "
        "explicitely disabled. Reports will not be accurate!%2$s\n");

static msg_t msg_valgrind_jobs = N_("%1$sWarning! Criterion has detected "
        "that it is running under valgrind, but the number of jobs have been "
        "explicitely set. Reports might appear confusing!%2$s\n");
#else
static msg_t msg_valgrind_early_exit = "%sWarning! Criterion has detected "
        "that it is running under valgrind, but the no_early_exit option is "
        "explicitely disabled. Reports will not be accurate!%s\n";

static msg_t msg_valgrind_jobs = "%sWarning! Criterion has detected "
        "that it is running under valgrind, but the number of jobs have been "
        "explicitely set. Reports might appear confusing!%s\n";
#endif


#ifdef _MSC_VER
struct criterion_test  *CR_SECTION_START_(cr_tst);
struct criterion_suite *CR_SECTION_START_(cr_sts);
struct criterion_test  *CR_SECTION_END_(cr_tst);
struct criterion_suite *CR_SECTION_END_(cr_sts);
#endif

CR_IMPL_SECTION_LIMITS(struct criterion_test*, cr_tst);
CR_IMPL_SECTION_LIMITS(struct criterion_suite*, cr_sts);

// This is here to make the test suite & test sections non-empty
TestSuite();
Test(,) {};

static INLINE void nothing(void) {}

int cmp_suite(void *a, void *b) {
    struct criterion_suite *s1 = a, *s2 = b;
    return strcmp(s1->name, s2->name);
}

int cmp_test(void *a, void *b) {
    struct criterion_test *s1 = a, *s2 = b;
    return strcmp(s1->name, s2->name);
}

static void dtor_suite_set(void *ptr, CR_UNUSED void *meta) {
    struct criterion_suite_set *s = ptr;
    sfree(s->tests);
}

static void dtor_test_set(void *ptr, CR_UNUSED void *meta) {
    struct criterion_test_set *t = ptr;
    sfree(t->suites);
}

void criterion_register_test(struct criterion_test_set *set,
                                    struct criterion_test *test) {

    struct criterion_suite_set css = {
        .suite = { .name = test->category },
    };
    struct criterion_suite_set *s = insert_ordered_set(set->suites, &css, sizeof (css));
    if (!s->tests)
        s->tests = new_ordered_set(cmp_test, NULL);

    insert_ordered_set(s->tests, test, sizeof(*test));
    ++set->tests;
}

struct criterion_test_set *criterion_init(void) {
    struct criterion_ordered_set *suites = new_ordered_set(cmp_suite, dtor_suite_set);

    FOREACH_SUITE_SEC(s) {
        if (!*s || !*(*s)->name)
            continue;

        struct criterion_suite_set css = {
            .suite = **s,
        };
        insert_ordered_set(suites, &css, sizeof (css));
    }

    struct criterion_test_set *set = smalloc(
            .size = sizeof (struct criterion_test_set),
            .dtor = dtor_test_set
        );

    *set = (struct criterion_test_set) {
        suites,
        0,
    };

    FOREACH_TEST_SEC(test) {
        if (!*test)
            continue;

        if (!*(*test)->category || !*(*test)->name)
            continue;

        criterion_register_test(set, *test);
    }

    return set;
}

f_wrapper *g_wrappers[] = {
    [CR_LANG_C]     = c_wrap,
    [CR_LANG_CPP]   = cpp_wrap,
};

void run_test_child(struct criterion_test *test,
                    struct criterion_suite *suite) {

#ifndef ENABLE_VALGRIND_ERRORS
    VALGRIND_ENABLE_ERROR_REPORTING;
#endif

    if (suite->data && suite->data->timeout != 0 && test->data->timeout == 0)
        setup_timeout((uint64_t) (suite->data->timeout * 1e9));
    else if (test->data->timeout != 0)
        setup_timeout((uint64_t) (test->data->timeout * 1e9));

    g_wrappers[test->data->lang_](test, suite);
}

#define push_event(Kind, ...)                                       \
    do {                                                            \
        stat_push_event(ctx->stats,                                 \
                ctx->suite_stats,                                   \
                ctx->test_stats,                                    \
                &(struct event) { .kind = Kind, __VA_ARGS__ });     \
        report(Kind, ctx->test_stats);                              \
    } while (0)

s_pipe_handle *g_worker_pipe;

static void handle_worker_terminated(struct event *ev,
        struct execution_context *ctx) {

    struct worker_status *ws = ev->data;
    struct process_status status = ws->status;

    if (status.kind == SIGNAL) {
        if (status.status == SIGPROF) {
            ctx->test_stats->timed_out = true;
            double elapsed_time = ctx->test->data->timeout;
            if (elapsed_time == 0 && ctx->suite->data)
                elapsed_time = ctx->suite->data->timeout;
            push_event(POST_TEST, .data = &elapsed_time);
            push_event(POST_FINI);
            log(test_timeout, ctx->test_stats);
            return;
        }

        if (ctx->normal_finish || !ctx->test_started) {
            log(other_crash, ctx->test_stats);
            if (!ctx->test_started) {
                stat_push_event(ctx->stats,
                        ctx->suite_stats,
                        ctx->test_stats,
                        &(struct event) { .kind = TEST_CRASH });
            }
            return;
        }
        ctx->test_stats->signal = status.status;
        if (ctx->test->data->signal == 0) {
            push_event(TEST_CRASH);
            log(test_crash, ctx->test_stats);
        } else {
            double elapsed_time = 0;
            push_event(POST_TEST, .data = &elapsed_time);
            log(post_test, ctx->test_stats);
            push_event(POST_FINI);
            log(post_fini, ctx->test_stats);
        }
    } else {
        if (ctx->aborted) {
            if (!ctx->normal_finish) {
                double elapsed_time = 0;
                push_event(POST_TEST, .data = &elapsed_time);
                log(post_test, ctx->test_stats);
            }
            if (!ctx->cleaned_up) {
                push_event(POST_FINI);
                log(post_fini, ctx->test_stats);
            }
            return;
        }
        if ((ctx->normal_finish && !ctx->cleaned_up) || !ctx->test_started) {
            log(abnormal_exit, ctx->test_stats);
            if (!ctx->test_started) {
                stat_push_event(ctx->stats,
                        ctx->suite_stats,
                        ctx->test_stats,
                        &(struct event) { .kind = TEST_CRASH });
            }
            return;
        }
        ctx->test_stats->exit_code = status.status;
        if (!ctx->normal_finish) {
            if (ctx->test->data->exit_code == 0) {
                push_event(TEST_CRASH);
                log(abnormal_exit, ctx->test_stats);
            } else {
                double elapsed_time = 0;
                push_event(POST_TEST, .data = &elapsed_time);
                log(post_test, ctx->test_stats);
                push_event(POST_FINI);
                log(post_fini, ctx->test_stats);
            }
        }
    }
}

static void handle_event(struct event *ev) {
    struct execution_context *ctx = &ev->worker->ctx;
    if (ev->kind < WORKER_TERMINATED)
        stat_push_event(ctx->stats, ctx->suite_stats, ctx->test_stats, ev);
    switch (ev->kind) {
        case PRE_INIT:
            report(PRE_INIT, ctx->test);
            log(pre_init, ctx->test);
            break;
        case PRE_TEST:
            report(PRE_TEST, ctx->test);
            log(pre_test, ctx->test);
            ctx->test_started = true;
            break;
        case THEORY_FAIL: {
            struct criterion_theory_stats ths = {
                .formatted_args = (char*) ev->data,
                .stats = ctx->test_stats,
            };
            report(THEORY_FAIL, &ths);
            log(theory_fail, &ths);
        } break;
        case ASSERT:
            report(ASSERT, ev->data);
            log(assert, ev->data);
            break;
        case TEST_ABORT:
            log(test_abort, ctx->test_stats, ev->data);
            ctx->test_stats->failed = 1;
            ctx->aborted = true;
            break;
        case POST_TEST:
            report(POST_TEST, ctx->test_stats);
            log(post_test, ctx->test_stats);
            ctx->normal_finish = true;
            break;
        case POST_FINI:
            report(POST_FINI, ctx->test_stats);
            log(post_fini, ctx->test_stats);
            ctx->cleaned_up = true;
            break;
        case WORKER_TERMINATED:
            handle_worker_terminated(ev, ctx);
            break;
    }
}

#ifdef HAVE_PCRE
void disable_unmatching(struct criterion_test_set *set) {
    FOREACH_SET(struct criterion_suite_set *s, set->suites) {
        if ((s->suite.data && s->suite.data->disabled) || !s->tests)
            continue;

        FOREACH_SET(struct criterion_test *test, s->tests) {
            const char *errmsg;
            int ret = extmatch(criterion_options.pattern, test->data->identifier_, &errmsg);
            if (ret == -10) {
                printf("pattern error: %s\n", errmsg);
                exit(1);
            } else if (ret < 0) {
                test->data->disabled = true;
            }
        }
    }
}
#endif

struct criterion_test_set *criterion_initialize(void) {
    init_i18n();

#ifndef ENABLE_VALGRIND_ERRORS
    VALGRIND_DISABLE_ERROR_REPORTING;
#endif
    if (RUNNING_ON_VALGRIND) {
        criterion_options.no_early_exit = 1;
        criterion_options.jobs = 1;
    }

    if (resume_child()) // (windows only) resume from the fork
        exit(0);

    return criterion_init();
}

void criterion_finalize(struct criterion_test_set *set) {
    sfree(set);

#ifndef ENABLE_VALGRIND_ERRORS
    VALGRIND_ENABLE_ERROR_REPORTING;
#endif
}

static void run_tests_async(struct criterion_test_set *set,
                            struct criterion_global_stats *stats) {

    ccrContext ctx = 0;

    size_t nb_workers = DEF(criterion_options.jobs, get_processor_count());
    struct worker_set workers = {
        .max_workers = nb_workers,
        .workers = calloc(nb_workers, sizeof (struct worker*)),
    };

    size_t active_workers = 0;

    s_pipe_file_handle *event_pipe = pipe_in_handle(g_worker_pipe, PIPE_DUP);
    struct event *ev = NULL;

    // initialization of coroutine
    run_next_test(set, stats, &ctx);

    for (size_t i = 0; i < nb_workers; ++i) {
        workers.workers[i] = run_next_test(NULL, NULL, &ctx);
        if (!is_runner())
            goto cleanup;

        if (!ctx)
            break;
        ++active_workers;
    }

    if (!active_workers)
        goto cleanup;

    while ((ev = worker_read_event(&workers, event_pipe)) != NULL) {
        handle_event(ev);
        size_t wi = ev->worker_index;
        if (ev->kind == WORKER_TERMINATED) {
            sfree(workers.workers[wi]);
            workers.workers[wi] = ctx ? run_next_test(NULL, NULL, &ctx) : NULL;

            if (!is_runner())
                goto cleanup;

            if (workers.workers[wi] == NULL)
                --active_workers;
        }
        sfree(ev);
        if (!active_workers)
            break;
    }
    ev = NULL;

cleanup:
    sfree(event_pipe);
    sfree(ev);
    for (size_t i = 0; i < nb_workers; ++i)
        sfree(workers.workers[i]);
    free(workers.workers);
    ccrAbort(ctx);
}

static int criterion_run_all_tests_impl(struct criterion_test_set *set) {
    report(PRE_ALL, set);
    log(pre_all, set);

    if (RUNNING_ON_VALGRIND) {
        if (!criterion_options.no_early_exit)
            criterion_pimportant(CRITERION_PREFIX_DASHES,
                    _(msg_valgrind_early_exit), CR_FG_BOLD, CR_RESET);
        if (criterion_options.jobs != 1)
            criterion_pimportant(CRITERION_PREFIX_DASHES,
                    _(msg_valgrind_jobs), CR_FG_BOLD, CR_RESET);
    }

    fflush(NULL); // flush everything before forking

    g_worker_pipe = stdpipe();
    if (g_worker_pipe == NULL) {
        criterion_perror("Could not initialize the event pipe: %s.\n",
                strerror(errno));
        abort();
    }

    struct criterion_global_stats *stats = stats_init();
    run_tests_async(set, stats);

    int result = is_runner() ? stats->tests_failed == 0 : -1;

    if (!is_runner())
        goto cleanup;

    report(POST_ALL, stats);
    log(post_all, stats);

cleanup:
    sfree(g_worker_pipe);
    sfree(stats);
    return result;
}

int criterion_run_all_tests(struct criterion_test_set *set) {
    #ifdef HAVE_PCRE
    if (criterion_options.pattern)
        disable_unmatching(set);
    #endif

    set_runner_process();
    int res = criterion_run_all_tests_impl(set);
    unset_runner_process();

    if (res == -1) {
        criterion_finalize(set);
        exit(0);
    }

    return criterion_options.always_succeed || res;
}
