/* SPDX-License-Identifier: Apache-2.0 */
#include "crashbox_service.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void lock(cbx_service_t *s)
{
    if (s->cfg.lock) s->cfg.lock(s->cfg.lock_user);
}

static void unlock(cbx_service_t *s)
{
    if (s->cfg.unlock) s->cfg.unlock(s->cfg.lock_user);
}

static void text_lock(cbx_service_t *s)
{
    if (s->cfg.text_lock) s->cfg.text_lock(s->cfg.lock_user);
    else lock(s);
}

static void text_unlock(cbx_service_t *s)
{
    if (s->cfg.text_unlock) s->cfg.text_unlock(s->cfg.lock_user);
    else unlock(s);
}

static uint64_t now(const cbx_service_t *s)
{
    return s->cfg.uptime_ms ? s->cfg.uptime_ms() : 0;
}

/* caller holds lock */
static int write_report(cbx_service_t *s, uint32_t reason, uint32_t flags, const cbx_crash_info_t *crash)
{
    cbx_snapshot_t snap;
    int result;

    memset(&snap, 0, sizeof(snap));
    snap.reason = reason;
    snap.flags = flags;
    snap.reset_flags = s->cfg.reset_flags;
    snap.boot_count = s->boot_count;
    snap.uptime_ms = now(s);
    snap.firmware = s->cfg.firmware;
    snap.ring = &s->ring;
    snap.text = NULL;
    snap.crash = crash;
    if (s->cfg.text) {
        /* the copy is short (at most CBX_TEXT_CAPACITY bytes); without a separate
         * text lock the store lock we already hold covers the text too */
        if (s->cfg.text_lock) s->cfg.text_lock(s->cfg.lock_user);
        snap.text = s->cfg.text;
        result = cbx_snapshot(&s->report, &snap);
        if (s->cfg.text_unlock) s->cfg.text_unlock(s->cfg.lock_user);
    } else {
        result = cbx_snapshot(&s->report, &snap);
    }
    if (result == CBX_OK) result = cbx_store_append(&s->store, &s->report, NULL);
    s->last_error = result;
    return result;
}

int cbx_service_start(cbx_service_t *svc, const cbx_service_config_t *cfg)
{
    cbx_crash_info_t crash;
    cbx_report_info_t newest;
    bool crashed;
    int result;

    if (!svc || !cfg) return CBX_INVALID;
    memset(svc, 0, sizeof(*svc));
    svc->cfg = *cfg;
    cbx_ring_init(&svc->ring);
    if (cfg->text && !cfg->text_retained) cbx_text_init(cfg->text);
    crashed = cfg->crash && cbx_crash_take(cfg->crash, &crash);

    lock(svc);
    result = cbx_store_open(&svc->store, &cfg->io);
    if (result == CBX_OK) {
        svc->boot_count = cbx_store_list(&svc->store, 0, &newest) == CBX_OK ? newest.boot_count + 1u : 1u;
        svc->ready = true;
        result = write_report(svc, CBX_REASON_BOOT, 0, crashed ? &crash : NULL);
    }
    svc->last_error = result;
    unlock(svc);

    svc->next_checkpoint_ms = now(svc) + (uint64_t)cfg->checkpoint_minutes * 60000u;
    if (crashed)
        cbx_service_log(svc, "crashbox: previous run ended in %s (task %s, pc 0x%08lx)",
                        cbx_reason_name(crash.reason), crash.task, (unsigned long)crash.pc);
    cbx_service_log(svc, "crashbox: boot %lu, reset 0x%lx, store %s (%u reports)", (unsigned long)svc->boot_count,
                    (unsigned long)cfg->reset_flags, result == CBX_OK ? "ok" : "FAILED",
                    (unsigned)svc->store.valid_count);
    return result;
}

void cbx_service_event(cbx_service_t *svc, uint16_t type, uint16_t code, uint32_t a, uint32_t b, uint32_t c)
{
    cbx_event_t e;
    if (!svc) return;
    e.time_ms = now(svc);
    e.type = type;
    e.code = code;
    e.a = a;
    e.b = b;
    e.c = c;
    lock(svc);
    (void)cbx_ring_push(&svc->ring, &e);
    unlock(svc);
}

void cbx_service_log(cbx_service_t *svc, const char *format, ...)
{
    va_list args;
    if (!svc || !svc->cfg.text) return;
    char line[CBX_LINE_MAX];
    size_t n;
    va_start(args, format);
    n = cbx_text_format(line, sizeof(line), now(svc), format, args);   /* format outside the lock */
    va_end(args);
    if (!n) return;
    text_lock(svc);
    cbx_text_write(svc->cfg.text, line, n);
    text_unlock(svc);
}

void cbx_service_request(cbx_service_t *svc, uint32_t reason, uint32_t flags)
{
    if (!svc || !reason) return;
    if (reason != CBX_REASON_BOOT && reason != CBX_REASON_CHECKPOINT && reason != CBX_REASON_REQUEST &&
        reason != CBX_REASON_SHUTDOWN)
        flags |= CBX_FLAG_FAULT;
    /* a pending fault wins over a routine request */
    if (svc->pending_reason && (svc->pending_flags & CBX_FLAG_FAULT) && !(flags & CBX_FLAG_FAULT)) return;
    svc->pending_flags = flags;
    svc->pending_reason = reason;
}

int cbx_service_save(cbx_service_t *svc, uint32_t reason, uint32_t flags)
{
    int result;
    if (!svc || !svc->ready) return CBX_INVALID;
    lock(svc);
    result = write_report(svc, reason, flags, NULL);
    unlock(svc);
    return result;
}

int cbx_service_poll(cbx_service_t *svc)
{
    int result = CBX_OK;
    uint32_t reason, flags;
    if (!svc || !svc->ready) return CBX_INVALID;
    reason = svc->pending_reason;
    flags = svc->pending_flags;
    if (reason) {
        svc->pending_reason = 0;
        result = cbx_service_save(svc, reason, flags);
    }
    if (svc->cfg.checkpoint_minutes && now(svc) >= svc->next_checkpoint_ms) {
        svc->next_checkpoint_ms = now(svc) + (uint64_t)svc->cfg.checkpoint_minutes * 60000u;
        if (!reason) result = cbx_service_save(svc, CBX_REASON_CHECKPOINT, 0);
    }
    return result;
}

/* ---- readout ---- */

static unsigned query_n(const char *query, unsigned fallback)
{
    const char *n = query ? strstr(query, "n=") : NULL;
    long v = n ? strtol(n + 2, NULL, 10) : 0;
    return v > 0 && v <= (long)CBX_SLOT_LIMIT ? (unsigned)v : fallback;
}

static int json_all(cbx_service_t *s, unsigned max, cbx_emit_fn emit, void *user)
{
    uint32_t seqs[CBX_SLOT_LIMIT];
    unsigned count = 0;
    int result = CBX_OK;
    cbx_report_info_t info;

    while (count < max && cbx_store_list(&s->store, count, &info) == CBX_OK) seqs[count++] = info.sequence;
    if (emit(user, "[", 1)) return CBX_IO;
    for (unsigned i = 0; i < count && result == CBX_OK; ++i) {
        if (i && emit(user, ",", 1)) return CBX_IO;
        result = cbx_store_read(&s->store, seqs[i], &s->report);
        if (result == CBX_OK) result = cbx_report_json(&s->report, emit, user);
    }
    if (result == CBX_OK && emit(user, "]\n", 2)) return CBX_IO;
    return result;
}

static int say(cbx_emit_fn emit, void *user, const char *text)
{
    return emit(user, text, strlen(text)) ? CBX_IO : CBX_OK;
}

int cbx_service_http(void *service, const char *path, const char *query, cbx_emit_fn emit, void *emit_user,
                     const char **type)
{
    cbx_service_t *s = service;
    int result;
    if (!s || !path || !emit || !type) return CBX_INVALID;
    *type = "text/plain; charset=utf-8";
    if (!strcmp(path, "/") || !strcmp(path, "/log.txt")) {
        if (!s->ready) return say(emit, emit_user, "crashbox: store not available\n");
        lock(s);
        result = cbx_store_text(&s->store, &s->report, query_n(query, 20), s->cfg.names, emit, emit_user);
        unlock(s);
        return result;
    }
    if (!strcmp(path, "/log.json")) {
        *type = "application/json";
        if (!s->ready) return say(emit, emit_user, "[]\n");
        lock(s);
        result = json_all(s, query_n(query, 20), emit, emit_user);
        unlock(s);
        return result;
    }
    if (!strcmp(path, "/save")) {
        char line[64];
        result = cbx_service_save(s, CBX_REASON_REQUEST, 0);
        snprintf(line, sizeof(line), "save: %s (report #%lu)\n", result == CBX_OK ? "ok" : "FAILED",
                 (unsigned long)s->store.newest_sequence);
        return say(emit, emit_user, line);
    }
    if (!strcmp(path, "/clear")) {
        if (!query || !strstr(query, "confirm=yes"))
            return say(emit, emit_user, "clear: add ?confirm=yes to erase all reports\n");
        lock(s);
        result = s->ready ? cbx_store_clear(&s->store) : CBX_INVALID;
        unlock(s);
        return say(emit, emit_user, result == CBX_OK ? "clear: ok\n" : "clear: FAILED\n");
    }
    return CBX_NOT_FOUND;
}
