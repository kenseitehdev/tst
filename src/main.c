#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <dirent.h>
#include <pthread.h>
#include <math.h>
#include <errno.h>

/* ─── constants ──────────────────────────────────────────────────── */

#define TST_VERSION       "0.1.0"
#define MAX_TESTS         1024
#define MAX_PATH          512
#define BENCH_RUNS        100
#define LOAD_RAMP_STEPS   10
#define LOAD_STEP_SECS    3
#define LOAD_MAX_WORKERS  20
#define OUTPUT_BUF        4096

/* ─── colors ─────────────────────────────────────────────────────── */

#define C_RESET   "\033[0m"
#define C_BOLD    "\033[1m"
#define C_GREEN   "\033[32m"
#define C_RED     "\033[31m"
#define C_YELLOW  "\033[33m"
#define C_CYAN    "\033[36m"
#define C_DIM     "\033[2m"

/* ─── structs ────────────────────────────────────────────────────── */

typedef struct {
    char path[MAX_PATH];   /* full path to executable */
    char name[MAX_PATH];   /* display name (relative) */
    bool pass;
    int  exit_code;
    double wall_ms;
    double mem_kb;
    char stdout_buf[OUTPUT_BUF];
} TestResult;

typedef struct {
    double runs;
    double avg_ms;
    double min_ms;
    double max_ms;
    double stddev_ms;
    double avg_mem_kb;
    /* timeline: each run's ms, up to BENCH_RUNS */
    double timeline[BENCH_RUNS];
} BenchResult;

typedef struct {
    int    workers;
    double rps;
    double p50_ms;
    double p95_ms;
    double p99_ms;
    double error_pct;
    int    total_runs;
    int    errors;
} LoadStep;

/* ─── globals ────────────────────────────────────────────────────── */

static char   g_tests_dir[MAX_PATH] = "tests";
static char   g_test_paths[MAX_TESTS][MAX_PATH];
static int    g_test_count = 0;
static TestResult g_results[MAX_TESTS];

/* ─── utility: time ──────────────────────────────────────────────── */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ─── utility: comparison for qsort ─────────────────────────────── */

static int cmp_double(const void *a, const void *b) {
    double da = *(const double*)a, db = *(const double*)b;
    return (da > db) - (da < db);
}

/* ─── discovery ──────────────────────────────────────────────────── */

static void discover(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "tst: cannot open tests dir '%s': %s\n", dir, strerror(errno));
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char full[MAX_PATH];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            discover(full);  /* recurse */
        } else if (S_ISREG(st.st_mode) && (st.st_mode & S_IXUSR)) {
            if (g_test_count < MAX_TESTS) {
                strncpy(g_test_paths[g_test_count++], full, MAX_PATH - 1);
            }
        }
    }
    closedir(d);
}

/* ─── run one test ───────────────────────────────────────────────── */

static TestResult run_one(const char *path) {
    TestResult r;
    memset(&r, 0, sizeof(r));
    strncpy(r.path, path, MAX_PATH - 1);

    /* name = relative path from cwd */
    strncpy(r.name, path, MAX_PATH - 1);

    int pipefd[2];
    if (pipe(pipefd) != 0) { r.pass = false; return r; }

    double t0 = now_ms();
    pid_t pid = fork();
    if (pid == 0) {
        /* child */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execl(path, path, NULL);
        _exit(127);
    }
    close(pipefd[1]);

    /* read stdout */
    ssize_t n = 0, total = 0;
    while ((n = read(pipefd[0], r.stdout_buf + total,
                     OUTPUT_BUF - 1 - total)) > 0) {
        total += n;
    }
    r.stdout_buf[total] = '\0';
    close(pipefd[0]);

    /* wait + rusage */
    struct rusage usage;
    int status;
    wait4(pid, &status, 0, &usage);
    r.wall_ms  = now_ms() - t0;
    r.mem_kb   = usage.ru_maxrss;  /* KB on Linux, bytes on macOS */
#ifdef __APPLE__
    r.mem_kb /= 1024.0;
#endif

    r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    /* pass logic: exit 0 AND no "FAIL" in stdout */
    bool exit_ok   = (r.exit_code == 0);
    bool stdout_ok = (strstr(r.stdout_buf, "FAIL") == NULL);
    bool stdout_pass = (strstr(r.stdout_buf, "PASS") != NULL);

    /* if stdout says PASS explicitly, trust it; else rely on exit code */
    if (stdout_pass) {
        r.pass = true;
    } else if (strstr(r.stdout_buf, "FAIL")) {
        r.pass = false;
    } else {
        r.pass = exit_ok;
    }
    (void)stdout_ok;

    return r;
}

/* ─── output: per-test line ──────────────────────────────────────── */

static void print_result(const TestResult *r) {
    const char *dot   = r->pass ? C_GREEN "●" C_RESET : C_RED "○" C_RESET;
    const char *label = r->pass ? C_GREEN "pass" C_RESET : C_RED "FAIL" C_RESET;
    printf("  %s  %-40s  %s  %6.0fms  %6.0fkb\n",
           dot, r->name, label, r->wall_ms, r->mem_kb);
    if (!r->pass) {
        printf("       " C_DIM "└─ exit: %d\n" C_RESET, r->exit_code);
        if (r->stdout_buf[0]) {
            /* print first line of stdout */
            char tmp[256];
            strncpy(tmp, r->stdout_buf, 255);
            tmp[255] = '\0';
            char *nl = strchr(tmp, '\n');
            if (nl) *nl = '\0';
            printf("       " C_DIM "└─ stdout: \"%s\"\n" C_RESET, tmp);
        }
    }
}

/* ─── output: dot grid ───────────────────────────────────────────── */

#define GRID_COLS 20

static void print_dot_grid(TestResult *results, int count) {
    printf("\n" C_BOLD "  results grid" C_RESET C_DIM "  (● pass  ○ fail)\n" C_RESET);
    printf("  ┌");
    for (int i = 0; i < GRID_COLS * 2 + 1; i++) printf("─");
    printf("┐\n");

    for (int i = 0; i < count; i++) {
        if (i % GRID_COLS == 0) printf("  │ ");
        if (results[i].pass)
            printf(C_GREEN "●" C_RESET " ");
        else
            printf(C_RED "○" C_RESET " ");
        if ((i + 1) % GRID_COLS == 0 || i == count - 1) {
            /* pad remaining */
            int rem = GRID_COLS - ((i % GRID_COLS) + 1);
            for (int j = 0; j < rem; j++) printf("  ");
            printf("│\n");
        }
    }

    printf("  └");
    for (int i = 0; i < GRID_COLS * 2 + 1; i++) printf("─");
    printf("┘\n");
}

/* ─── output: bench timeline bar ────────────────────────────────── */

#define TIMELINE_WIDTH 50

static void print_bench_timeline(const BenchResult *b, int runs) {
    printf("\n" C_BOLD "  bench timeline" C_RESET C_DIM "  (each dot = 1 run, height = relative duration)\n" C_RESET);

    double max_ms = 0;
    for (int i = 0; i < runs; i++)
        if (b->timeline[i] > max_ms) max_ms = b->timeline[i];
    if (max_ms == 0) return;

    /* 5 rows */
    int rows = 5;
    printf("  ");
    for (int r = rows; r >= 1; r--) {
        if (r < rows) printf("  ");
        for (int i = 0; i < runs && i < TIMELINE_WIDTH; i++) {
            int height = (int)ceil((b->timeline[i] / max_ms) * rows);
            printf(height >= r ? C_CYAN "█" C_RESET : " ");
        }
        printf("\n");
    }
    printf("  " C_DIM);
    for (int i = 0; i < (runs < TIMELINE_WIDTH ? runs : TIMELINE_WIDTH); i++)
        printf("▔");
    printf(C_RESET "\n");
    printf("  " C_DIM "0ms" C_RESET "%*s" C_DIM "%.0fms\n" C_RESET,
           (runs < TIMELINE_WIDTH ? runs : TIMELINE_WIDTH) - 6, "", max_ms);
}

/* ─── subcommand: test ───────────────────────────────────────────── */

static int cmd_test(const char *filter) {
    discover(g_tests_dir);
    if (g_test_count == 0) {
        printf("tst: no executable tests found in '%s'\n", g_tests_dir);
        return 1;
    }

    printf(C_BOLD "\ntst v%s" C_RESET "  " C_DIM "test runner\n\n" C_RESET, TST_VERSION);

    int pass = 0, fail = 0;
    double total_ms = 0;

    for (int i = 0; i < g_test_count; i++) {
        if (filter && strstr(g_test_paths[i], filter) == NULL) continue;
        g_results[i] = run_one(g_test_paths[i]);
        print_result(&g_results[i]);
        if (g_results[i].pass) pass++; else fail++;
        total_ms += g_results[i].wall_ms;
    }

    /* separator */
    printf("\n  " C_DIM);
    for (int i = 0; i < 60; i++) printf("─");
    printf(C_RESET "\n");

    /* summary */
    printf("  " C_BOLD "%d tests" C_RESET "  "
           C_GREEN "%d passed" C_RESET "  "
           C_RED "%d failed" C_RESET "  "
           C_DIM "%.0fms total\n" C_RESET,
           pass + fail, pass, fail, total_ms);

    /* dot grid */
    print_dot_grid(g_results, pass + fail);

    printf("\n");
    return fail > 0 ? 1 : 0;
}

/* ─── subcommand: bench ──────────────────────────────────────────── */

static int cmd_bench(const char *filter) {
    discover(g_tests_dir);
    if (g_test_count == 0) {
        printf("tst: no executable tests found in '%s'\n", g_tests_dir);
        return 1;
    }

    printf(C_BOLD "\ntst v%s" C_RESET "  " C_DIM "benchmark\n\n" C_RESET, TST_VERSION);

    for (int i = 0; i < g_test_count; i++) {
        if (filter && strstr(g_test_paths[i], filter) == NULL) continue;

        BenchResult b;
        memset(&b, 0, sizeof(b));
        b.min_ms = 1e18;

        double mem_total = 0;
        double times[BENCH_RUNS];

        printf("  " C_BOLD "[bench]" C_RESET " %s\n", g_test_paths[i]);

        for (int r = 0; r < BENCH_RUNS; r++) {
            TestResult tr = run_one(g_test_paths[i]);
            times[r] = tr.wall_ms;
            b.timeline[r] = tr.wall_ms;
            mem_total += tr.mem_kb;
            if (tr.wall_ms < b.min_ms) b.min_ms = tr.wall_ms;
            if (tr.wall_ms > b.max_ms) b.max_ms = tr.wall_ms;
            b.avg_ms += tr.wall_ms;
        }
        b.runs      = BENCH_RUNS;
        b.avg_ms   /= BENCH_RUNS;
        b.avg_mem_kb = mem_total / BENCH_RUNS;

        /* stddev */
        double var = 0;
        for (int r = 0; r < BENCH_RUNS; r++)
            var += (times[r] - b.avg_ms) * (times[r] - b.avg_ms);
        b.stddev_ms = sqrt(var / BENCH_RUNS);

        printf("  runs: %d  │  avg: %.1fms  │  min: %.1fms  │  max: %.1fms  │  "
               "σ: %.1fms  │  mem: %.0fkb avg\n",
               BENCH_RUNS, b.avg_ms, b.min_ms, b.max_ms, b.stddev_ms, b.avg_mem_kb);

        print_bench_timeline(&b, BENCH_RUNS);
        printf("\n");
    }
    return 0;
}

/* ─── subcommand: load ───────────────────────────────────────────── */

typedef struct {
    const char *path;
    double     *out_ms;
    bool       *out_ok;
    int         idx;
} WorkerArg;

static void *load_worker(void *arg) {
    WorkerArg *wa = (WorkerArg*)arg;
    double t0 = now_ms();
    TestResult tr = run_one(wa->path);
    wa->out_ms[wa->idx] = now_ms() - t0;
    wa->out_ok[wa->idx] = tr.pass;
    return NULL;
}

static LoadStep run_load_step(const char *path, int workers) {
    LoadStep s = {0};
    s.workers = workers;

    double   *ms  = calloc(workers, sizeof(double));
    bool     *ok  = calloc(workers, sizeof(bool));
    pthread_t *th = calloc(workers, sizeof(pthread_t));
    WorkerArg *wa = calloc(workers, sizeof(WorkerArg));

    double t0 = now_ms();
    for (int i = 0; i < workers; i++) {
        wa[i].path   = path;
        wa[i].out_ms = ms;
        wa[i].out_ok = ok;
        wa[i].idx    = i;
        pthread_create(&th[i], NULL, load_worker, &wa[i]);
    }
    for (int i = 0; i < workers; i++)
        pthread_join(th[i], NULL);
    double elapsed = now_ms() - t0;

    s.total_runs = workers;
    for (int i = 0; i < workers; i++)
        if (!ok[i]) s.errors++;

    /* percentiles */
    double *sorted = malloc(workers * sizeof(double));
    memcpy(sorted, ms, workers * sizeof(double));
    qsort(sorted, workers, sizeof(double), cmp_double);

    s.p50_ms    = sorted[(int)(workers * 0.50)];
    s.p95_ms    = sorted[(int)(workers * 0.95)];
    s.p99_ms    = sorted[(int)(workers * 0.99)];
    s.rps       = workers / (elapsed / 1000.0);
    s.error_pct = 100.0 * s.errors / workers;

    free(ms); free(ok); free(th); free(wa); free(sorted);
    return s;
}

static int cmd_load(const char *filter) {
    discover(g_tests_dir);
    if (g_test_count == 0) {
        printf("tst: no executable tests found in '%s'\n", g_tests_dir);
        return 1;
    }

    printf(C_BOLD "\ntst v%s" C_RESET "  " C_DIM "load runner\n\n" C_RESET, TST_VERSION);

    for (int i = 0; i < g_test_count; i++) {
        if (filter && strstr(g_test_paths[i], filter) == NULL) continue;

        printf("  " C_BOLD "[load]" C_RESET " %s  "
               C_DIM "ramp 1→%d workers over %ds\n" C_RESET,
               g_test_paths[i], LOAD_MAX_WORKERS,
               LOAD_RAMP_STEPS * LOAD_STEP_SECS);

        printf("  %-8s  %-8s  %-8s  %-8s  %-8s  %-8s\n",
               "workers", "rps", "p50", "p95", "p99", "errors");
        printf("  " C_DIM);
        for (int j = 0; j < 56; j++) printf("─");
        printf(C_RESET "\n");

        /* timeline data for chart */
        double tl_rps[LOAD_RAMP_STEPS * 2];
        int    tl_n = 0;

        /* ramp UP */
        for (int step = 1; step <= LOAD_RAMP_STEPS; step++) {
            int w = (int)(1 + (double)(LOAD_MAX_WORKERS - 1) *
                          step / LOAD_RAMP_STEPS);
            LoadStep s = run_load_step(g_test_paths[i], w);
            tl_rps[tl_n++] = s.rps;
            printf("  %-8d  %-8.1f  %-7.1fms  %-7.1fms  %-7.1fms  %.1f%%\n",
                   w, s.rps, s.p50_ms, s.p95_ms, s.p99_ms, s.error_pct);
            sleep(LOAD_STEP_SECS);
        }
        /* ramp DOWN */
        for (int step = LOAD_RAMP_STEPS - 1; step >= 1; step--) {
            int w = (int)(1 + (double)(LOAD_MAX_WORKERS - 1) *
                          step / LOAD_RAMP_STEPS);
            LoadStep s = run_load_step(g_test_paths[i], w);
            tl_rps[tl_n++] = s.rps;
            printf("  %-8d  %-8.1f  %-7.1fms  %-7.1fms  %-7.1fms  %.1f%%\n",
                   w, s.rps, s.p50_ms, s.p95_ms, s.p99_ms, s.error_pct);
            sleep(LOAD_STEP_SECS);
        }

        /* rps timeline chart */
        printf("\n  " C_BOLD "rps timeline" C_RESET C_DIM
               "  (↑ ramp up  ↓ ramp down)\n" C_RESET);
        double max_rps = 0;
        for (int j = 0; j < tl_n; j++)
            if (tl_rps[j] > max_rps) max_rps = tl_rps[j];

        int rows = 5;
        for (int r = rows; r >= 1; r--) {
            printf("  ");
            for (int j = 0; j < tl_n; j++) {
                int h = (int)ceil((tl_rps[j] / max_rps) * rows);
                printf(h >= r ? C_CYAN "█ " C_RESET : "  ");
            }
            printf("\n");
        }
        printf("  " C_DIM);
        for (int j = 0; j < tl_n; j++) printf("▔▔");
        printf(C_RESET "\n");
        printf("  " C_DIM "ramp up →                    ← ramp down\n" C_RESET);
        printf("\n");
    }
    return 0;
}

/* ─── help ───────────────────────────────────────────────────────── */

static void usage(void) {
    printf(C_BOLD "tst v%s" C_RESET " — test runner, benchmarker, load tester\n\n", TST_VERSION);
    printf("  " C_BOLD "usage:" C_RESET "\n");
    printf("    tst                     run all tests\n");
    printf("    tst test [filter]       run tests (optional name filter)\n");
    printf("    tst bench [filter]      benchmark tests\n");
    printf("    tst load [filter]       load test with ramp up/down\n");
    printf("    tst help                show this help\n\n");
    printf("  " C_BOLD "options:" C_RESET "\n");
    printf("    -d <dir>                tests directory (default: ./tests)\n\n");
    printf("  " C_BOLD "test discovery:" C_RESET "\n");
    printf("    recursively finds all executable files in the tests dir\n");
    printf("    works with any language: sh, py, js, ts, lua, compiled C...\n\n");
    printf("  " C_BOLD "pass/fail:" C_RESET "\n");
    printf("    exit code 0 = pass, non-zero = fail\n");
    printf("    stdout containing FAIL overrides to fail\n");
    printf("    stdout containing PASS overrides to pass\n\n");
}

/* ─── main ───────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    int i = 1;

    /* parse -d flag */
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            strncpy(g_tests_dir, argv[++i], MAX_PATH - 1);
        }
        i++;
    }

    if (i >= argc) {
        return cmd_test(NULL);
    }

    const char *cmd = argv[i++];
    const char *filter = (i < argc) ? argv[i] : NULL;

    if (strcmp(cmd, "test")  == 0) return cmd_test(filter);
    if (strcmp(cmd, "bench") == 0) return cmd_bench(filter);
    if (strcmp(cmd, "load")  == 0) return cmd_load(filter);
    if (strcmp(cmd, "help")  == 0) { usage(); return 0; }

    fprintf(stderr, "tst: unknown command '%s'. Try: tst help\n", cmd);
    return 1;
}