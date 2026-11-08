/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Storage algorithm and power-cut handling derived from the dashboard
 * diagnostics journal of the tesla_ble_ac7925b firmware (Apache-2.0).
 */
#include "crashbox.h"
#include <stdio.h>
#include <string.h>

#define CBX_MAGIC 0x31584243u        /* "CBX1" */
#define CBX_WIRE_VERSION 1u
#define CBX_COMMIT 0x5458424bu       /* "KBXT" */
#define CBX_CRASH_MAGIC 0x48535243u  /* "CRSH" */
#define CBX_FOOTER_OFFSET (CBX_SLOT_SIZE - 8u)

#if CBX_WIRE_MAX_SIZE > CBX_FOOTER_OFFSET
#error "events + text do not fit in a slot: lower CBX_EVENT_CAPACITY or CBX_TEXT_CAPACITY"
#endif

/* wire header offsets (little endian) */
enum {
    W_MAGIC = 0, W_VERSION = 4, W_SIZE = 8, W_SEQUENCE = 12, W_REASON = 16, W_FLAGS = 20,
    W_RESET = 24, W_BOOTS = 28, W_UPTIME = 32, W_TOTAL = 40, W_COUNT = 44, W_TEXTLEN = 46,
    W_FIRMWARE = 48, W_CRASH = 80,   /* pc lr sp cause addr reason, uptime, task[16] */
    W_CRASH_UPTIME = 104, W_CRASH_TASK = 112
};

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8u * i));
}
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const uint8_t *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t size)
{
    crc = ~crc;
    while (size--) {
        crc ^= *data++;
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static bool newer(uint32_t a, uint32_t b)
{
    /* sequences in one bounded journal differ by far less than half the range */
    return a != b && (uint32_t)(a - b) < 0x80000000u;
}

static void copy_name(char *out, size_t cap, const char *in)
{
    size_t i = 0;
    memset(out, 0, cap);
    for (; in && in[i] && i + 1u < cap; ++i) {
        unsigned char ch = (unsigned char)in[i];
        out[i] = ch >= 32 && ch < 127 ? (char)ch : '?';
    }
}

/* ---- events ---- */

void cbx_ring_init(cbx_ring_t *ring)
{
    if (ring) memset(ring, 0, sizeof(*ring));
}

int cbx_ring_push(cbx_ring_t *ring, const cbx_event_t *event)
{
    if (!ring || !event || !event->type) return CBX_INVALID;
    ring->events[ring->head] = *event;
    ring->head = (uint16_t)((ring->head + 1u) % CBX_RING_CAPACITY);
    if (ring->count < CBX_RING_CAPACITY) ++ring->count;
    if (ring->total != UINT32_MAX) ++ring->total;
    return CBX_OK;
}

/* ---- text ---- */

void cbx_text_init(cbx_text_t *text)
{
    if (text) memset(text, 0, sizeof(*text));
}

void cbx_text_write(cbx_text_t *text, const char *data, size_t size)
{
    if (!text || !data) return;
    if (size > CBX_TEXT_RING_SIZE) {  /* only the tail can be kept */
        data += size - CBX_TEXT_RING_SIZE;
        size = CBX_TEXT_RING_SIZE;
    }
    for (size_t i = 0; i < size; ++i) {
        text->data[text->head] = data[i];
        text->head = (text->head + 1u) % CBX_TEXT_RING_SIZE;
    }
    text->count = text->count + size > CBX_TEXT_RING_SIZE ? CBX_TEXT_RING_SIZE : text->count + (uint32_t)size;
}

size_t cbx_text_format(char *line, size_t cap, uint64_t time_ms, const char *format, va_list args)
{
    int head, body;
    size_t n;

    if (!line || cap < 16 || !format) return 0;
    head = snprintf(line, cap, "[%lu.%03u] ", (unsigned long)(time_ms / 1000u), (unsigned)(time_ms % 1000u));
    if (head < 0 || (size_t)head >= cap - 2u) return 0;
    body = vsnprintf(line + head, cap - (size_t)head, format, args);
    if (body < 0) return 0;
    n = (size_t)head + (size_t)body;
    if (n > cap - 2u) n = cap - 2u;  /* truncated */
    if (line[n - 1] != '\n') line[n++] = '\n';
    return n;
}

int cbx_text_vprintf(cbx_text_t *text, uint64_t time_ms, const char *format, va_list args)
{
    char line[CBX_LINE_MAX];
    size_t n;

    if (!text || !format) return CBX_INVALID;
    n = cbx_text_format(line, sizeof(line), time_ms, format, args);
    if (!n) return CBX_INVALID;
    cbx_text_write(text, line, n);
    return CBX_OK;
}

int cbx_text_printf(cbx_text_t *text, uint64_t time_ms, const char *format, ...)
{
    va_list args;
    int result;

    va_start(args, format);
    result = cbx_text_vprintf(text, time_ms, format, args);
    va_end(args);
    return result;
}

/* ---- crash record ---- */

static uint32_t crash_crc(const cbx_crash_info_t *info)
{
    return crc32_update(CBX_CRASH_MAGIC, (const uint8_t *)info, sizeof(*info));
}

void cbx_crash_record(cbx_crash_t *slot, const cbx_crash_info_t *info)
{
    volatile uint8_t *dst = (volatile uint8_t *)&slot->info;
    const uint8_t *src = (const uint8_t *)info;

    if (!slot || !info) return;
    slot->magic = 0;                 /* invalid while it changes */
    for (size_t i = 0; i < sizeof(*info); ++i) dst[i] = src[i];
    slot->info.task[CBX_TASK_SIZE - 1u] = 0;
    slot->crc = crash_crc(&slot->info);
    slot->magic = CBX_CRASH_MAGIC;
}

bool cbx_crash_take(cbx_crash_t *slot, cbx_crash_info_t *info)
{
    bool valid;

    if (!slot) return false;
    valid = slot->magic == CBX_CRASH_MAGIC && slot->crc == crash_crc(&slot->info) &&
            memchr(slot->info.task, 0, CBX_TASK_SIZE) != NULL;
    if (valid && info) *info = slot->info;
    memset(slot, 0, sizeof(*slot));
    return valid;
}

/* ---- reports ---- */

static bool routine_reason(uint32_t reason)
{
    return reason == CBX_REASON_BOOT || reason == CBX_REASON_CHECKPOINT || reason == CBX_REASON_REQUEST ||
           reason == CBX_REASON_SHUTDOWN;
}

int cbx_snapshot(cbx_report_t *report, const cbx_snapshot_t *args)
{
    if (!report || !args || !args->reason) return CBX_INVALID;
    memset(report, 0, sizeof(*report));
    cbx_report_info_t *info = &report->info;
    info->reason = args->reason;
    info->flags = args->flags & CBX_FLAG_FAULT;
    info->reset_flags = args->reset_flags;
    info->boot_count = args->boot_count;
    info->uptime_ms = args->uptime_ms;
    copy_name(info->firmware, CBX_NAME_SIZE, args->firmware);
    if (args->crash) {
        info->crash = *args->crash;
        copy_name(info->crash.task, CBX_TASK_SIZE, args->crash->task);
        info->flags |= CBX_FLAG_CRASH;
    }
    if ((info->flags & CBX_FLAG_CRASH) || !routine_reason(info->reason) ||
        (info->reason == CBX_REASON_BOOT &&
         (info->reset_flags & (CBX_RESET_WATCHDOG | CBX_RESET_EXCEPTION | CBX_RESET_LOW_VOLTAGE))))
        info->flags |= CBX_FLAG_FAULT;

    const cbx_ring_t *ring = args->ring;
    if (ring && ring->count <= CBX_RING_CAPACITY && ring->head < CBX_RING_CAPACITY) {
        unsigned count = ring->count < CBX_EVENT_CAPACITY ? ring->count : CBX_EVENT_CAPACITY;
        unsigned first = (ring->head + CBX_RING_CAPACITY - count) % CBX_RING_CAPACITY;
        info->total_events = ring->total;
        info->event_count = (uint16_t)count;
        for (unsigned i = 0; i < count; ++i) report->events[i] = ring->events[(first + i) % CBX_RING_CAPACITY];
    }

#if CBX_TEXT_CAPACITY
    const cbx_text_t *text = args->text;
    if (text && text->count <= CBX_TEXT_RING_SIZE && text->head < CBX_TEXT_RING_SIZE && text->count) {
        uint32_t n = text->count < CBX_TEXT_CAPACITY ? text->count : CBX_TEXT_CAPACITY;
        uint32_t start = (text->head + CBX_TEXT_RING_SIZE - n) % CBX_TEXT_RING_SIZE, skip = 0;
        if (n < text->count) {       /* cut inside the ring: start at the next line */
            while (skip < n && text->data[(start + skip) % CBX_TEXT_RING_SIZE] != '\n') ++skip;
            skip = skip < n ? skip + 1u : 0u;
        }
        for (uint32_t i = skip; i < n; ++i) {
            unsigned char ch = (unsigned char)text->data[(start + i) % CBX_TEXT_RING_SIZE];
            report->text[i - skip] = (ch >= 32 && ch < 127) || ch == '\n' || ch == '\t' ? (char)ch : '?';
        }
        info->text_len = (uint16_t)(n - skip);
    }
#endif
    return CBX_OK;
}

static bool report_valid(const cbx_report_t *report)
{
    const cbx_report_info_t *info = &report->info;
    if (!info->reason || info->event_count > CBX_EVENT_CAPACITY || info->text_len > CBX_TEXT_CAPACITY ||
        !memchr(info->firmware, 0, CBX_NAME_SIZE) || !memchr(info->crash.task, 0, CBX_TASK_SIZE))
        return false;
    for (unsigned i = 0; i < info->event_count; ++i)
        if (!report->events[i].type) return false;
    return true;
}

static uint32_t serialize(uint8_t *w, const cbx_report_t *report, uint32_t sequence)
{
    const cbx_report_info_t *info = &report->info;
    uint32_t events = CBX_WIRE_HEADER_SIZE + info->event_count * CBX_WIRE_EVENT_SIZE;
    uint32_t size = events + info->text_len;

    memset(w, 0, CBX_WIRE_HEADER_SIZE);
    put32(w + W_MAGIC, CBX_MAGIC); put32(w + W_VERSION, CBX_WIRE_VERSION);
    put32(w + W_SIZE, size); put32(w + W_SEQUENCE, sequence);
    put32(w + W_REASON, info->reason); put32(w + W_FLAGS, info->flags);
    put32(w + W_RESET, info->reset_flags); put32(w + W_BOOTS, info->boot_count);
    put64(w + W_UPTIME, info->uptime_ms); put32(w + W_TOTAL, info->total_events);
    put16(w + W_COUNT, info->event_count); put16(w + W_TEXTLEN, info->text_len);
    memcpy(w + W_FIRMWARE, info->firmware, CBX_NAME_SIZE);
    if (info->flags & CBX_FLAG_CRASH) {
        const cbx_crash_info_t *c = &info->crash;
        put32(w + W_CRASH, c->pc); put32(w + W_CRASH + 4, c->lr); put32(w + W_CRASH + 8, c->sp);
        put32(w + W_CRASH + 12, c->cause); put32(w + W_CRASH + 16, c->addr);
        put32(w + W_CRASH + 20, c->reason); put64(w + W_CRASH_UPTIME, c->uptime_ms);
        memcpy(w + W_CRASH_TASK, c->task, CBX_TASK_SIZE);
    }
    for (unsigned i = 0; i < info->event_count; ++i) {
        const cbx_event_t *e = &report->events[i];
        uint8_t *p = w + CBX_WIRE_HEADER_SIZE + i * CBX_WIRE_EVENT_SIZE;
        put64(p, e->time_ms); put16(p + 8, e->type); put16(p + 10, e->code);
        put32(p + 12, e->a); put32(p + 16, e->b); put32(p + 20, e->c);
    }
    if (info->text_len) memcpy(w + events, report->text, info->text_len);
    return size;
}

/* Invalid or torn records read as CBX_NOT_FOUND; physical IO errors as CBX_IO. */
static int load_slot(cbx_store_t *store, unsigned slot, bool committed, cbx_report_info_t *info,
                     cbx_report_t *report)
{
    uint8_t footer[8];
    uint8_t *w = store->scratch;
    uint32_t base = slot * CBX_SLOT_SIZE;

    if (store->io.read(store->io.user, base, w, CBX_WIRE_HEADER_SIZE)) return CBX_IO;
    uint16_t count = get16(w + W_COUNT), text_len = get16(w + W_TEXTLEN);
    uint32_t size = get32(w + W_SIZE);
    if (get32(w + W_MAGIC) != CBX_MAGIC || get32(w + W_VERSION) != CBX_WIRE_VERSION ||
        !get32(w + W_SEQUENCE) || !get32(w + W_REASON) || count > CBX_EVENT_CAPACITY ||
        text_len > CBX_TEXT_CAPACITY ||
        size != CBX_WIRE_HEADER_SIZE + count * CBX_WIRE_EVENT_SIZE + text_len ||
        !memchr(w + W_FIRMWARE, 0, CBX_NAME_SIZE) || !memchr(w + W_CRASH_TASK, 0, CBX_TASK_SIZE))
        return CBX_NOT_FOUND;
    if ((size > CBX_WIRE_HEADER_SIZE &&
         store->io.read(store->io.user, base + CBX_WIRE_HEADER_SIZE, w + CBX_WIRE_HEADER_SIZE,
                        size - CBX_WIRE_HEADER_SIZE)) ||
        store->io.read(store->io.user, base + CBX_FOOTER_OFFSET, footer, sizeof(footer)))
        return CBX_IO;
    if ((committed && get32(footer + 4) != CBX_COMMIT) || get32(footer) != crc32_update(0, w, size))
        return CBX_NOT_FOUND;
    for (unsigned i = 0; i < count; ++i)
        if (!get16(w + CBX_WIRE_HEADER_SIZE + i * CBX_WIRE_EVENT_SIZE + 8)) return CBX_NOT_FOUND;

    cbx_report_info_t local;
    memset(&local, 0, sizeof(local));
    local.sequence = get32(w + W_SEQUENCE); local.reason = get32(w + W_REASON);
    local.flags = get32(w + W_FLAGS); local.reset_flags = get32(w + W_RESET);
    local.boot_count = get32(w + W_BOOTS); local.uptime_ms = get64(w + W_UPTIME);
    local.total_events = get32(w + W_TOTAL); local.event_count = count; local.text_len = text_len;
    memcpy(local.firmware, w + W_FIRMWARE, CBX_NAME_SIZE);
    if (local.flags & CBX_FLAG_CRASH) {
        cbx_crash_info_t *c = &local.crash;
        c->pc = get32(w + W_CRASH); c->lr = get32(w + W_CRASH + 4); c->sp = get32(w + W_CRASH + 8);
        c->cause = get32(w + W_CRASH + 12); c->addr = get32(w + W_CRASH + 16);
        c->reason = get32(w + W_CRASH + 20); c->uptime_ms = get64(w + W_CRASH_UPTIME);
        memcpy(c->task, w + W_CRASH_TASK, CBX_TASK_SIZE);
    }
    if (info) *info = local;
    if (report) {
        memset(report, 0, sizeof(*report));
        report->info = local;
        for (unsigned i = 0; i < count; ++i) {
            const uint8_t *p = w + CBX_WIRE_HEADER_SIZE + i * CBX_WIRE_EVENT_SIZE;
            cbx_event_t *e = &report->events[i];
            e->time_ms = get64(p); e->type = get16(p + 8); e->code = get16(p + 10);
            e->a = get32(p + 12); e->b = get32(p + 16); e->c = get32(p + 20);
        }
#if CBX_TEXT_CAPACITY
        if (text_len) memcpy(report->text, w + CBX_WIRE_HEADER_SIZE + count * CBX_WIRE_EVENT_SIZE, text_len);
#endif
    }
    return CBX_OK;
}

int cbx_store_open(cbx_store_t *store, const cbx_io_t *io)
{
    if (!store || !io || !io->read || !io->write || io->size < 4u * CBX_SLOT_SIZE ||
        io->size > CBX_SLOT_LIMIT * CBX_SLOT_SIZE || io->size % CBX_SLOT_SIZE)
        return CBX_INVALID;
    memset(store, 0, sizeof(*store));
    store->io = *io;
    store->slot_count = (uint16_t)(io->size / CBX_SLOT_SIZE);
    for (unsigned slot = 0; slot < store->slot_count; ++slot) {
        cbx_report_info_t info;
        int result = load_slot(store, slot, true, &info, NULL);
        if (result == CBX_NOT_FOUND) continue;
        if (result) return result;
        store->sequences[slot] = info.sequence;
        ++store->valid_count;
        if (!store->newest_sequence || newer(info.sequence, store->newest_sequence)) {
            store->newest_sequence = info.sequence;
            store->newest_slot = (uint16_t)slot;
        }
    }
    store->ready = true;
    return CBX_OK;
}

static int wipe_slot(cbx_store_t *store, uint32_t base)
{
    static const uint8_t ones[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    if (store->io.erase) return store->io.erase(store->io.user, base, CBX_SLOT_SIZE);
    return store->io.write(store->io.user, base + CBX_FOOTER_OFFSET, ones, sizeof(ones));
}

int cbx_store_append(cbx_store_t *store, const cbx_report_t *report, uint32_t *sequence)
{
    if (!store || !store->ready || !report || !report_valid(report)) return CBX_INVALID;
    unsigned fault_slots = store->slot_count / 4u;
    if (fault_slots < 2) fault_slots = 2;
    bool fault = (report->info.flags & CBX_FLAG_FAULT) != 0;
    unsigned first = fault ? store->slot_count - fault_slots : 0;
    unsigned limit = fault ? store->slot_count : store->slot_count - fault_slots;
    unsigned slot = first;
    uint32_t latest = 0;
    for (unsigned i = first; i < limit; ++i) {
        if (store->sequences[i] && (!latest || newer(store->sequences[i], latest))) {
            latest = store->sequences[i];
            slot = i + 1u;
        }
    }
    if (slot >= limit) slot = first;
    uint32_t seq = store->newest_sequence + 1u;
    if (!seq) seq = 1;
    uint32_t base = slot * CBX_SLOT_SIZE;
    uint32_t size = serialize(store->scratch, report, seq);
    uint8_t footer[8];
    put32(footer, crc32_update(0, store->scratch, size));
    put32(footer + 4, CBX_COMMIT);
    /* only the oldest report of this ring can be lost; the newest is in another slot */
    if (store->sequences[slot]) {
        store->sequences[slot] = 0;
        --store->valid_count;
    }
    if (wipe_slot(store, base) || store->io.write(store->io.user, base, store->scratch, size) ||
        store->io.write(store->io.user, base + CBX_FOOTER_OFFSET, footer, 4))
        return CBX_IO;
    cbx_report_info_t verify;
    int result = load_slot(store, slot, false, &verify, NULL);
    if (result || verify.sequence != seq) return CBX_IO;
    if (store->io.write(store->io.user, base + CBX_FOOTER_OFFSET + 4, footer + 4, 4)) return CBX_IO;
    result = load_slot(store, slot, true, &verify, NULL);
    if (result || verify.sequence != seq) return CBX_IO;
    store->sequences[slot] = seq;
    ++store->valid_count;
    store->newest_sequence = seq;
    store->newest_slot = (uint16_t)slot;
    if (sequence) *sequence = seq;
    return CBX_OK;
}

/* newest slot with a sequence older than `previous` (0: the newest); -1 if none */
static int older_slot(const cbx_store_t *store, uint32_t previous)
{
    uint32_t candidate = 0;
    int selected = -1;
    for (unsigned slot = 0; slot < store->slot_count; ++slot) {
        uint32_t seq = store->sequences[slot];
        if (!seq || (previous && !newer(previous, seq))) continue;
        if (!candidate || newer(seq, candidate)) {
            candidate = seq;
            selected = (int)slot;
        }
    }
    return selected;
}

int cbx_store_list(cbx_store_t *store, unsigned newest_index, cbx_report_info_t *info)
{
    if (!store || !store->ready || !info) return CBX_INVALID;
    if (newest_index >= store->valid_count) return CBX_NOT_FOUND;
    uint32_t previous = 0;
    int slot = -1;
    for (unsigned rank = 0; rank <= newest_index; ++rank) {
        slot = older_slot(store, previous);
        if (slot < 0) return CBX_NOT_FOUND;
        previous = store->sequences[slot];
    }
    return load_slot(store, (unsigned)slot, true, info, NULL);
}

int cbx_store_read(cbx_store_t *store, uint32_t sequence, cbx_report_t *report)
{
    if (!store || !store->ready || !sequence || !report) return CBX_INVALID;
    for (unsigned slot = 0; slot < store->slot_count; ++slot)
        if (store->sequences[slot] == sequence) return load_slot(store, slot, true, NULL, report);
    return CBX_NOT_FOUND;
}

int cbx_store_clear(cbx_store_t *store)
{
    if (!store || !store->ready) return CBX_INVALID;
    for (unsigned slot = 0; slot < store->slot_count; ++slot) {
        if (!store->sequences[slot]) continue;
        store->sequences[slot] = 0;
        --store->valid_count;
        if (wipe_slot(store, slot * CBX_SLOT_SIZE)) return CBX_IO;
    }
    /* keep newest_sequence: new reports stay newer than anything a torn wipe left */
    return CBX_OK;
}

/* ---- readout ---- */

const char *cbx_reason_name(uint32_t reason)
{
    switch (reason) {
    case CBX_REASON_BOOT: return "boot";
    case CBX_REASON_CHECKPOINT: return "checkpoint";
    case CBX_REASON_REQUEST: return "request";
    case CBX_REASON_WATCHDOG: return "watchdog";
    case CBX_REASON_EXCEPTION: return "exception";
    case CBX_REASON_PLATFORM_FAILURE: return "platform-failure";
    case CBX_REASON_SHUTDOWN: return "shutdown";
    default: return reason >= CBX_REASON_APP ? "app" : "unknown";
    }
}

static int emit_str(cbx_emit_fn emit, void *user, const char *text)
{
    return emit(user, text, strlen(text)) ? CBX_IO : CBX_OK;
}

static int emit_fmt(cbx_emit_fn emit, void *user, char *line, size_t cap, const char *format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 5, 6)))
#endif
    ;
static int emit_fmt(cbx_emit_fn emit, void *user, char *line, size_t cap, const char *format, ...)
{
    va_list args;
    int n;
    va_start(args, format);
    n = vsnprintf(line, cap, format, args);
    va_end(args);
    if (n < 0) return CBX_INVALID;
    if ((size_t)n >= cap) n = (int)cap - 1;
    return emit(user, line, (size_t)n) ? CBX_IO : CBX_OK;
}

static void reset_text(uint32_t flags, char *out, size_t cap)
{
    static const char *const names[] = {"low-voltage", "watchdog", "software", "exception", "power-on", "pin"};
    size_t n = 0;
    out[0] = 0;
    for (unsigned i = 0; i < 6; ++i) {
        if (!(flags & (1u << i))) continue;
        int w = snprintf(out + n, cap - n, "%s%s", n ? "+" : "", names[i]);
        if (w < 0 || (size_t)w >= cap - n) return;
        n += (size_t)w;
    }
    if ((flags & CBX_RESET_UNKNOWN) && n + 9 < cap) snprintf(out + n, cap - n, "%sunknown", n ? "+" : "");
    if (!out[0]) snprintf(out, cap, "-");
}

static void duration(uint64_t ms, char *out, size_t cap)
{
    uint64_t s = ms / 1000u;
    snprintf(out, cap, "%luh%02um%02u.%03us", (unsigned long)(s / 3600u), (unsigned)(s / 60u % 60u),
             (unsigned)(s % 60u), (unsigned)(ms % 1000u));
}

int cbx_report_text(const cbx_report_t *report, const cbx_names_t *names, cbx_emit_fn emit, void *user)
{
    char line[200], up[32], resets[80];
    int r;
    if (!report || !emit || !report_valid(report)) return CBX_INVALID;
    const cbx_report_info_t *info = &report->info;
    const char *reason = names && names->reason ? names->reason(info->reason) : NULL;
    if (!reason) reason = cbx_reason_name(info->reason);
    duration(info->uptime_ms, up, sizeof(up));
    reset_text(info->reset_flags, resets, sizeof(resets));
    if ((r = emit_fmt(emit, user, line, sizeof(line),
                      "==== report #%lu: %s (%lu)%s | reset %s | boot %lu | up %s | fw %s\n",
                      (unsigned long)info->sequence, reason, (unsigned long)info->reason,
                      (info->flags & CBX_FLAG_FAULT) ? " FAULT" : "", resets,
                      (unsigned long)info->boot_count, up, info->firmware)))
        return r;
    if (info->flags & CBX_FLAG_CRASH) {
        const cbx_crash_info_t *c = &info->crash;
        duration(c->uptime_ms, up, sizeof(up));
        if ((r = emit_fmt(emit, user, line, sizeof(line),
                          "crash: %s in task '%s' at %s: pc 0x%08lx lr 0x%08lx sp 0x%08lx cause 0x%lx addr 0x%08lx\n",
                          cbx_reason_name(c->reason), c->task, up, (unsigned long)c->pc, (unsigned long)c->lr,
                          (unsigned long)c->sp, (unsigned long)c->cause, (unsigned long)c->addr)))
            return r;
    }
    if (info->event_count &&
        (r = emit_fmt(emit, user, line, sizeof(line), "events: last %u of %lu\n", (unsigned)info->event_count,
                      (unsigned long)info->total_events)))
        return r;
    for (unsigned i = 0; i < info->event_count; ++i) {
        const cbx_event_t *e = &report->events[i];
        const char *name = names && names->event ? names->event(e->type) : NULL;
        char type[16];
        if (!name) {
            snprintf(type, sizeof(type), "type%u", (unsigned)e->type);
            name = type;
        }
        if ((r = emit_fmt(emit, user, line, sizeof(line), "  [%lu.%03u] %s code %u  %lu %lu %lu\n",
                          (unsigned long)(e->time_ms / 1000u), (unsigned)(e->time_ms % 1000u), name,
                          (unsigned)e->code, (unsigned long)e->a, (unsigned long)e->b, (unsigned long)e->c)))
            return r;
    }
#if CBX_TEXT_CAPACITY
    if (info->text_len) {
        if ((r = emit_str(emit, user, "log:\n"))) return r;
        if (emit(user, report->text, info->text_len)) return CBX_IO;
        if (report->text[info->text_len - 1] != '\n' && (r = emit_str(emit, user, "\n"))) return r;
    }
#endif
    return emit_str(emit, user, "\n");
}

static int emit_json_string(cbx_emit_fn emit, void *user, const char *s, size_t n)
{
    char esc[8];
    if (emit(user, "\"", 1)) return CBX_IO;
    for (size_t i = 0; i < n && s[i]; ++i) {
        unsigned char ch = (unsigned char)s[i];
        const char *out = NULL;
        size_t len = 1;
        if (ch == '"') out = "\\\"";
        else if (ch == '\\') out = "\\\\";
        else if (ch == '\n') out = "\\n";
        else if (ch == '\t') out = "\\t";
        else if (ch < 32 || ch > 126) {
            snprintf(esc, sizeof(esc), "\\u%04x", ch);
            out = esc;
        }
        if (out) len = strlen(out);
        if (emit(user, out ? out : (const char *)&s[i], len)) return CBX_IO;
    }
    return emit(user, "\"", 1) ? CBX_IO : CBX_OK;
}

int cbx_report_json(const cbx_report_t *report, cbx_emit_fn emit, void *user)
{
    char line[256];
    int r;
    if (!report || !emit || !report_valid(report)) return CBX_INVALID;
    const cbx_report_info_t *info = &report->info;
    if ((r = emit_fmt(emit, user, line, sizeof(line),
                      "{\"schemaVersion\":1,\"id\":%lu,\"reason\":%lu,\"flags\":%lu,\"resetFlags\":%lu,"
                      "\"bootCount\":%lu,\"uptimeMs\":%llu,\"totalEvents\":%lu,\"firmware\":",
                      (unsigned long)info->sequence, (unsigned long)info->reason, (unsigned long)info->flags,
                      (unsigned long)info->reset_flags, (unsigned long)info->boot_count,
                      (unsigned long long)info->uptime_ms, (unsigned long)info->total_events)))
        return r;
    if ((r = emit_json_string(emit, user, info->firmware, CBX_NAME_SIZE))) return r;
    if (info->flags & CBX_FLAG_CRASH) {
        const cbx_crash_info_t *c = &info->crash;
        if ((r = emit_fmt(emit, user, line, sizeof(line),
                          ",\"crash\":{\"reason\":%lu,\"pc\":%lu,\"lr\":%lu,\"sp\":%lu,\"cause\":%lu,\"addr\":%lu,"
                          "\"uptimeMs\":%llu,\"task\":",
                          (unsigned long)c->reason, (unsigned long)c->pc, (unsigned long)c->lr, (unsigned long)c->sp,
                          (unsigned long)c->cause, (unsigned long)c->addr, (unsigned long long)c->uptime_ms)))
            return r;
        if ((r = emit_json_string(emit, user, c->task, CBX_TASK_SIZE)) || (r = emit_str(emit, user, "}")))
            return r;
    }
    if ((r = emit_str(emit, user, ",\"events\":["))) return r;
    for (unsigned i = 0; i < info->event_count; ++i) {
        const cbx_event_t *e = &report->events[i];
        if ((r = emit_fmt(emit, user, line, sizeof(line),
                          "%s{\"timeMs\":%llu,\"type\":%u,\"code\":%u,\"a\":%lu,\"b\":%lu,\"c\":%lu}", i ? "," : "",
                          (unsigned long long)e->time_ms, (unsigned)e->type, (unsigned)e->code, (unsigned long)e->a,
                          (unsigned long)e->b, (unsigned long)e->c)))
            return r;
    }
    if ((r = emit_str(emit, user, "]"))) return r;
#if CBX_TEXT_CAPACITY
    if (info->text_len) {
        if ((r = emit_str(emit, user, ",\"text\":"))) return r;
        if ((r = emit_json_string(emit, user, report->text, info->text_len))) return r;
    }
#endif
    return emit_str(emit, user, "}\n");
}

int cbx_store_text(cbx_store_t *store, cbx_report_t *report, unsigned max_reports, const cbx_names_t *names,
                   cbx_emit_fn emit, void *user)
{
    char line[160];
    uint32_t previous = 0;
    int r;
    if (!store || !store->ready || !report || !emit) return CBX_INVALID;
    if ((r = emit_fmt(emit, user, line, sizeof(line), "crashbox %s: %u reports in %u slots (newest first)\n\n",
                      CBX_VERSION_STRING, (unsigned)store->valid_count, (unsigned)store->slot_count)))
        return r;
    for (unsigned n = 0; n < max_reports; ++n) {
        int slot = older_slot(store, previous);
        if (slot < 0) break;
        previous = store->sequences[slot];
        r = load_slot(store, (unsigned)slot, true, NULL, report);
        if (r == CBX_NOT_FOUND) continue;
        if (r) return r;
        if ((r = cbx_report_text(report, names, emit, user))) return r;
    }
    return CBX_OK;
}
