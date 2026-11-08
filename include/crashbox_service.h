/* SPDX-License-Identifier: Apache-2.0 */
/*
 * crashbox service: one store, the event ring and the log text wired together
 * with the usual policy, so a port only supplies storage, locks and a clock.
 *
 *   start    open the store, take the crash record, write the boot report
 *            (with the boot count, the reset cause, the crash and the text the
 *            previous run left in retained RAM)
 *   event / log   add to RAM (cheap; log text may come from interrupts if the
 *            port's text_lock masks them)
 *   request  ask for a report from any task context; no IO, no waiting
 *   poll     run from one low-priority task: writes requested reports and a
 *            checkpoint every checkpoint_minutes
 *   http     the readout routes for crashbox_http (or any web server)
 */
#ifndef CRASHBOX_SERVICE_H
#define CRASHBOX_SERVICE_H

#include "crashbox.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    cbx_io_t io;
    const char *firmware;
    uint32_t reset_flags;            /* this boot's reset cause */
    uint32_t checkpoint_minutes;     /* 0: no periodic checkpoints */
    cbx_crash_t *crash;              /* retained RAM, or NULL */
    cbx_text_t *text;                /* log text (retained RAM if possible), or NULL */
    bool text_retained;              /* text held the previous run's log: keep it */
    uint64_t (*uptime_ms)(void);
    /* store / ring / report lock (a mutex); NULL when only one task uses the service */
    void (*lock)(void *user);
    void (*unlock)(void *user);
    /* text lock: must exclude every text writer (mask interrupts if the console
     * tee writes from them); NULL to use lock */
    void (*text_lock)(void *user);
    void (*text_unlock)(void *user);
    void *lock_user;
    const cbx_names_t *names;
} cbx_service_config_t;

typedef struct {
    cbx_service_config_t cfg;
    cbx_store_t store;
    cbx_ring_t ring;
    cbx_report_t report;
    uint32_t boot_count;
    uint64_t next_checkpoint_ms;
    volatile uint32_t pending_reason;  /* 0: none */
    volatile uint32_t pending_flags;
    int last_error;
    bool ready;
} cbx_service_t;

int cbx_service_start(cbx_service_t *svc, const cbx_service_config_t *cfg);
void cbx_service_event(cbx_service_t *svc, uint16_t type, uint16_t code, uint32_t a, uint32_t b, uint32_t c);
void cbx_service_log(cbx_service_t *svc, const char *format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
/* Report on the next poll; a fault request is not replaced by a routine one. */
void cbx_service_request(cbx_service_t *svc, uint32_t reason, uint32_t flags);
/* Returns CBX_OK, or the store error of a report it tried to write. */
int cbx_service_poll(cbx_service_t *svc);
/* Write a report now (task context). */
int cbx_service_save(cbx_service_t *svc, uint32_t reason, uint32_t flags);

/*
 * Readout routes, for crashbox_http or another server:
 *   /  /log.txt [?n=N]   newest N reports as text (default 20)
 *   /log.json [?n=N]     the same as a JSON array
 *   /save                write a report now (reason "request")
 *   /clear?confirm=yes   erase all reports
 * Returns CBX_NOT_FOUND for other paths; sets *type before emitting.
 */
int cbx_service_http(void *svc, const char *path, const char *query, cbx_emit_fn emit, void *emit_user,
                     const char **type);

#ifdef __cplusplus
}
#endif

#endif
