/* SPDX-License-Identifier: Apache-2.0 */
/*
 * crashboxd - crashbox for an embedded Linux product, as one small daemon.
 *
 *   crashboxd --store /mnt/data/crashbox.bin [--size-kb 256] [--port 8090]
 *             [--log NAME=PATH]... [--kmsg] [--checkpoint-min 10]
 *             [--firmware NAME] [--clean-marker PATH]
 *
 * - Follows the given log files (new lines only; a file that shrinks was
 *   truncated and is read from the start) and the kernel log (warnings and
 *   worse) into the crashbox text ring, prefixed with the source name.
 * - A line that reports a crash or a dying process (segfault, oom-kill,
 *   kernel oops/panic, "exited (N)" with N != 0, "FAIL ") asks for a fault
 *   report a moment later, at most one a minute.
 * - Writes a boot report at start, a checkpoint every --checkpoint-min
 *   minutes, a report on SIGTERM/SIGINT; the clean marker written at a clean
 *   stop tells the next start whether the previous run ended abruptly
 *   (power loss, kernel panic, hardware watchdog, kill -9).
 * - Serves /log.txt, /log.json, /save, /clear on --port (crashbox_http).
 *
 * Reports go to one fixed-size file: put it on a wear-levelling file system
 * (UBIFS, JFFS2, F2FS, ext4 on eMMC), not on tmpfs.
 */
#define _DEFAULT_SOURCE
#include "crashbox_file.h"
#include "crashbox_http.h"
#include "crashbox_service.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_LOGS 12
#define FAULT_GAP_MS 60000u
#define FAULT_DELAY_MS 1500u        /* let the lines after the trigger arrive */

enum { REASON_LOG_FAULT = CBX_REASON_APP, REASON_KERNEL = CBX_REASON_APP + 1, REASON_STOP = CBX_REASON_SHUTDOWN };
enum { EVENT_LOG_FAULT = 1, EVENT_KERNEL_ERROR = 2 };   /* code: source index (kernel: MAX_LOGS), a: fault count */

typedef struct {
    char name[24];
    const char *path;
    off_t offset;
    char partial[256];
    size_t partial_len;
} source_t;

static source_t sources[MAX_LOGS];
static unsigned source_count;
static cbx_service_t svc;
static cbx_text_t text;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t stop_signal;
static uint64_t fault_at, last_fault;

static uint64_t uptime_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void lock(void *u) { (void)u; pthread_mutex_lock(&mutex); }
static void unlock(void *u) { (void)u; pthread_mutex_unlock(&mutex); }

static const char *reason_name(uint32_t reason)
{
    switch (reason) {
    case REASON_LOG_FAULT: return "log-fault";
    case REASON_KERNEL: return "kernel";
    default: return NULL;
    }
}

static const char *event_name(uint16_t type)
{
    return type == EVENT_LOG_FAULT ? "log-fault" : type == EVENT_KERNEL_ERROR ? "kernel-error" : NULL;
}

static const cbx_names_t names = {reason_name, event_name};

static bool fault_line(const char *line)
{
    static const char *const keys[] = {"segfault", "Out of memory", "oom-kill", "Kernel panic",
                                       "Unable to handle", "Internal error: Oops", "FAIL "};
    const char *e;
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
        if (strstr(line, keys[i])) return true;
    e = strstr(line, "exited (");
    return e && strncmp(e + 8, "0)", 2) != 0;
}

static bool replaying;               /* reading what was logged before the start: history only */
static uint32_t faults_seen;

/* source: index into sources[], or MAX_LOGS for the kernel */
static void add_line(unsigned source, const char *line, bool kernel_error)
{
    cbx_service_log(&svc, "%s: %s", source < MAX_LOGS ? sources[source].name : "kernel", line);
    if (replaying || !(kernel_error || fault_line(line))) return;
    uint64_t now = uptime_ms();
    ++faults_seen;
    cbx_service_event(&svc, kernel_error ? EVENT_KERNEL_ERROR : EVENT_LOG_FAULT, (uint16_t)source, faults_seen,
                      0, 0);
    if (!fault_at && (!last_fault || now - last_fault >= FAULT_GAP_MS)) fault_at = now + FAULT_DELAY_MS;
}

static void follow(source_t *s)
{
    struct stat st;
    char buf[1024];
    int fd;

    if (stat(s->path, &st) != 0) return;
    if (st.st_size < s->offset) s->offset = 0;     /* truncated (log trimming) */
    if (st.st_size == s->offset) return;
    fd = open(s->path, O_RDONLY);
    if (fd < 0) return;
    if (lseek(fd, s->offset, SEEK_SET) == s->offset) {
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            s->offset += n;
            for (ssize_t i = 0; i < n; ++i) {
                char ch = buf[i];
                if (ch == '\n' || s->partial_len == sizeof(s->partial) - 1) {
                    s->partial[s->partial_len] = 0;
                    if (s->partial_len) add_line((unsigned)(s - sources), s->partial, false);
                    s->partial_len = 0;
                    if (ch == '\n') continue;
                }
                if (ch != '\r') s->partial[s->partial_len++] = ch;
            }
        }
    }
    close(fd);
}

/* /dev/kmsg records: "prio,seq,usec,flags;message" (one per read) */
static void read_kmsg(int fd)
{
    char rec[1024];
    for (;;) {
        ssize_t n = read(fd, rec, sizeof(rec) - 1);
        if (n < 0 && errno == EPIPE) continue;     /* overwritten records */
        if (n <= 0) return;
        rec[n] = 0;
        char *msg = strchr(rec, ';');
        char *nl;
        if (!msg) continue;
        *msg++ = 0;
        if ((nl = strchr(msg, '\n'))) *nl = 0;
        int prio = atoi(rec) & 7;
        if (prio <= 4) add_line(MAX_LOGS, msg, prio <= 3);   /* warning and worse; err and worse is a fault */
    }
}

static void *http_thread(void *arg)
{
    uint16_t port = (uint16_t)(uintptr_t)arg;
    for (;;) {
        if (cbx_http_serve(port, cbx_service_http, &svc) != CBX_OK) sleep(5);   /* address not up yet */
    }
    return NULL;
}

static void on_signal(int sig) { stop_signal = sig; }

static void usage(void)
{
    fprintf(stderr, "usage: crashboxd --store FILE [--size-kb 256] [--port 8090] [--log NAME=PATH]... [--kmsg]\n"
                    "                 [--checkpoint-min 10] [--firmware NAME] [--clean-marker PATH]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *store = NULL, *firmware = "linux", *marker = NULL;
    unsigned size_kb = 256, checkpoint = 10, port = 8090;
    bool kmsg = false;
    static cbx_file_t file;
    cbx_service_config_t cfg;
    int kfd = -1;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--kmsg")) { kmsg = true; continue; }
        if (!v) usage();
        ++i;
        if (!strcmp(a, "--store")) store = v;
        else if (!strcmp(a, "--size-kb")) size_kb = (unsigned)atoi(v);
        else if (!strcmp(a, "--port")) port = (unsigned)atoi(v);
        else if (!strcmp(a, "--checkpoint-min")) checkpoint = (unsigned)atoi(v);
        else if (!strcmp(a, "--firmware")) firmware = v;
        else if (!strcmp(a, "--clean-marker")) marker = v;
        else if (!strcmp(a, "--log") && source_count < MAX_LOGS) {
            const char *eq = strchr(v, '=');
            source_t *s = &sources[source_count++];
            snprintf(s->name, sizeof(s->name), "%.*s", eq ? (int)(eq - v) : 3, eq ? v : "log");
            s->path = eq ? eq + 1 : v;
        } else usage();
    }
    if (!store || size_kb * 1024u % CBX_SLOT_SIZE) usage();

    memset(&cfg, 0, sizeof(cfg));
    if (cbx_file_open(&file, store, size_kb * 1024u, &cfg.io) != CBX_OK) {
        fprintf(stderr, "crashboxd: cannot open %s: %s\n", store, strerror(errno));
        return 1;
    }
    /* clean marker present: the previous run stopped cleanly */
    cfg.reset_flags = marker && access(marker, F_OK) == 0 ? CBX_RESET_SOFTWARE : CBX_RESET_UNKNOWN;
    if (marker) unlink(marker);
    cfg.firmware = firmware;
    cfg.checkpoint_minutes = checkpoint;
    cfg.text = &text;
    cfg.text_retained = true;                              /* holds the kernel replay below */
    cfg.uptime_ms = uptime_ms;
    cfg.lock = lock;
    cfg.unlock = unlock;
    cfg.names = &names;
    /* this boot's kernel warnings so far go into the boot report, as context (no faults) */
    cbx_text_init(&text);
    if (kmsg && (kfd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK)) >= 0) {
        svc.cfg = cfg;                                     /* add_line() logs through the service */
        replaying = true;
        read_kmsg(kfd);
        replaying = false;
    }
    if (cbx_service_start(&svc, &cfg) != CBX_OK)
        fprintf(stderr, "crashboxd: boot report not stored (%d)\n", svc.last_error);
    printf("crashboxd: boot %lu (%s), %u reports, http port %u\n", (unsigned long)svc.boot_count,
           cfg.reset_flags & CBX_RESET_UNKNOWN ? "previous run ended abruptly" : "clean",
           (unsigned)svc.store.valid_count, port);
    fflush(stdout);

    for (unsigned i = 0; i < source_count; ++i) {         /* only what comes after the start */
        struct stat st;
        if (stat(sources[i].path, &st) == 0) sources[i].offset = st.st_size;
    }
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    pthread_t t;
    pthread_attr_t attr;
    int err;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 128u * 1024u);   /* the default (8 MiB) fails on small boards */
    err = pthread_create(&t, &attr, http_thread, (void *)(uintptr_t)port);
    pthread_attr_destroy(&attr);
    if (err) fprintf(stderr, "crashboxd: no HTTP thread: %s\n", strerror(err));

    while (!stop_signal) {
        for (unsigned i = 0; i < source_count; ++i) follow(&sources[i]);
        if (kfd >= 0) read_kmsg(kfd);
        if (fault_at && uptime_ms() >= fault_at) {
            fault_at = 0;
            last_fault = uptime_ms();
            cbx_service_request(&svc, REASON_LOG_FAULT, CBX_FLAG_FAULT);
        }
        (void)cbx_service_poll(&svc);
        usleep(500000);
    }
    cbx_service_log(&svc, "crashboxd: stopping on signal %d", (int)stop_signal);
    (void)cbx_service_save(&svc, REASON_STOP, 0);
    if (marker) {
        int fd = open(marker, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            fsync(fd);
            close(fd);
        }
    }
    return 0;
}
