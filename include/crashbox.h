/* SPDX-License-Identifier: Apache-2.0 */
/*
 * crashbox - a small flight recorder for embedded products.
 *
 * Keeps the last events and log text in RAM, takes a report on boot, on a
 * timer and on faults, and stores reports in a ring of 4 KiB slots on a raw
 * flash partition or in one fixed-size file. Reports survive reboots and are
 * read out as plain text or JSON, typically over the product's own Wi-Fi web
 * server ("GET /log.txt").
 *
 *  - Wear: every report goes to the next slot of its ring, so erases spread
 *    evenly over the whole area; a report every 30 minutes on a 256 KiB area
 *    costs each sector about one erase a day.
 *  - Power loss: payload, CRC and a separate commit word are written in that
 *    order and read back; torn or corrupt slots are skipped, the newest
 *    committed report is never the one being overwritten.
 *  - Faults are kept apart: a quarter of the slots (at least two) take fault
 *    reports only, so routine checkpoints never push a crash out.
 *  - Crashes: cbx_crash_record() only writes RAM (no locks, heap or flash) and
 *    is meant for exception and watchdog-interrupt handlers. Put the record in
 *    RAM that survives a warm reset; the next boot folds it into its report.
 *
 * The core is portable C99 with no heap, no locks and no OS calls. The owner
 * serialises calls on one store, ring or text buffer (one task, or a mutex).
 * Platform glue supplies the storage callbacks, the time, the firmware name
 * and the reset cause; see ports/ for RT-Thread, Linux and Jieli examples.
 */
#ifndef CRASHBOX_H
#define CRASHBOX_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CBX_VERSION_STRING "1.0.0"

#define CBX_SLOT_SIZE 4096u
#ifndef CBX_SLOT_LIMIT
#define CBX_SLOT_LIMIT 256u          /* up to 1 MiB of slots */
#endif
#ifndef CBX_EVENT_CAPACITY
#define CBX_EVENT_CAPACITY 64u       /* events kept per report */
#endif
#ifndef CBX_RING_CAPACITY
#define CBX_RING_CAPACITY 256u       /* events kept in RAM */
#endif
#ifndef CBX_TEXT_CAPACITY
#define CBX_TEXT_CAPACITY 2048u      /* log text kept per report (0: numeric only) */
#endif
#ifndef CBX_TEXT_RING_SIZE
#define CBX_TEXT_RING_SIZE 4096u     /* log text kept in RAM */
#endif
#define CBX_NAME_SIZE 32u            /* firmware name, NUL terminated */
#define CBX_TASK_SIZE 16u

#define CBX_WIRE_HEADER_SIZE 128u
#define CBX_WIRE_EVENT_SIZE 24u
#define CBX_WIRE_MAX_SIZE \
    (CBX_WIRE_HEADER_SIZE + CBX_EVENT_CAPACITY * CBX_WIRE_EVENT_SIZE + CBX_TEXT_CAPACITY)

enum {
    CBX_OK = 0,
    CBX_INVALID = -1,
    CBX_IO = -2,
    CBX_NOT_FOUND = -3,
    CBX_BUSY = -4,
    CBX_TOO_SMALL = -5
};

/* Report reasons 1..15 are fixed below; the product may use 16 and up. */
enum {
    CBX_REASON_BOOT = 1,
    CBX_REASON_CHECKPOINT = 2,
    CBX_REASON_REQUEST = 3,          /* asked for by a user / service tool */
    CBX_REASON_WATCHDOG = 4,         /* a supervisor saw a hang */
    CBX_REASON_EXCEPTION = 5,
    CBX_REASON_PLATFORM_FAILURE = 6,
    CBX_REASON_SHUTDOWN = 7,         /* orderly stop */
    CBX_REASON_APP = 16
};

/* Reset cause bits as the platform reports them. */
enum {
    CBX_RESET_LOW_VOLTAGE = 1u << 0,
    CBX_RESET_WATCHDOG = 1u << 1,
    CBX_RESET_SOFTWARE = 1u << 2,
    CBX_RESET_EXCEPTION = 1u << 3,
    CBX_RESET_POWER_ON = 1u << 4,
    CBX_RESET_PIN = 1u << 5
};
#define CBX_RESET_UNKNOWN 0x80000000u

/* Report flags. */
enum {
    CBX_FLAG_FAULT = 1u << 0,        /* stored in the fault ring */
    CBX_FLAG_CRASH = 1u << 1         /* crash block is valid */
};

/* One event: a time, a product-defined type (non-zero) and code, three numbers.
 * Keep secrets and personal data out of events and text. */
typedef struct {
    uint64_t time_ms;
    uint16_t type;
    uint16_t code;
    uint32_t a, b, c;
} cbx_event_t;

typedef struct {
    cbx_event_t events[CBX_RING_CAPACITY];
    uint32_t total;                  /* saturates, does not wrap */
    uint16_t head, count;
} cbx_ring_t;

/* Log text kept in RAM; cbx_text_printf() adds "[seconds.ms] message\n". */
typedef struct {
    char data[CBX_TEXT_RING_SIZE];
    uint32_t head;                   /* next write position */
    uint32_t count;                  /* bytes held, <= CBX_TEXT_RING_SIZE */
} cbx_text_t;

/* What an exception / watchdog handler knows. Registers are as wide as the
 * product needs them; 64-bit cores truncate to the low 32 bits. */
typedef struct {
    uint32_t pc, lr, sp;
    uint32_t cause;                  /* mcause, CFSR, signal number, ... */
    uint32_t addr;                   /* mtval, MMFAR/BFAR, si_addr, ... */
    uint32_t reason;                 /* CBX_REASON_EXCEPTION / _WATCHDOG / product */
    uint64_t uptime_ms;
    char task[CBX_TASK_SIZE];        /* thread / task name */
} cbx_crash_info_t;

/* Crash record for RAM that survives a warm reset (a .noinit section, a
 * retention register file, ...). Zeroed or garbage contents read as empty. */
typedef struct {
    uint32_t magic;
    cbx_crash_info_t info;
    uint32_t crc;
} cbx_crash_t;

typedef struct {
    uint32_t sequence;
    uint32_t reason;
    uint32_t flags;
    uint32_t reset_flags;
    uint32_t boot_count;
    uint64_t uptime_ms;
    uint32_t total_events;
    uint16_t event_count;
    uint16_t text_len;
    char firmware[CBX_NAME_SIZE];
    cbx_crash_info_t crash;          /* valid with CBX_FLAG_CRASH */
} cbx_report_info_t;

typedef struct {
    cbx_report_info_t info;
    cbx_event_t events[CBX_EVENT_CAPACITY];
    char text[CBX_TEXT_CAPACITY ? CBX_TEXT_CAPACITY : 1];
} cbx_report_t;

/*
 * Storage. Offsets are relative to the area; size is a multiple of
 * CBX_SLOT_SIZE between 4 slots and CBX_SLOT_LIMIT slots. Each call returns 0
 * on full success and must be durable when it returns (flush/fsync for files).
 * erase() sets a slot to 0xff like NOR flash; it may be NULL for media that
 * overwrite in place (files): the core then invalidates a slot by writing
 * 0xff over its footer before reusing it.
 */
typedef struct {
    void *user;
    uint32_t size;
    int (*read)(void *user, uint32_t offset, void *data, uint32_t size);
    int (*write)(void *user, uint32_t offset, const void *data, uint32_t size);
    int (*erase)(void *user, uint32_t offset, uint32_t size);
} cbx_io_t;

typedef struct {
    cbx_io_t io;
    uint32_t sequences[CBX_SLOT_LIMIT];
    uint32_t newest_sequence;
    uint16_t slot_count, newest_slot, valid_count;
    bool ready;
    uint8_t scratch[CBX_WIRE_MAX_SIZE];
} cbx_store_t;

/* ---- RAM side ---- */
void cbx_ring_init(cbx_ring_t *ring);
int cbx_ring_push(cbx_ring_t *ring, const cbx_event_t *event);

void cbx_text_init(cbx_text_t *text);
void cbx_text_write(cbx_text_t *text, const char *data, size_t size);
int cbx_text_printf(cbx_text_t *text, uint64_t time_ms, const char *format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
int cbx_text_vprintf(cbx_text_t *text, uint64_t time_ms, const char *format, va_list args);
/* "[s.ms] message" plus a newline into line (truncated to cap); returns the
 * length, 0 on error */
#define CBX_LINE_MAX 192u
size_t cbx_text_format(char *line, size_t cap, uint64_t time_ms, const char *format, va_list args);

/* Exception / interrupt safe: plain stores and a CRC, nothing else. */
void cbx_crash_record(cbx_crash_t *slot, const cbx_crash_info_t *info);
/* Returns true and clears the slot if it holds a valid record. */
bool cbx_crash_take(cbx_crash_t *slot, cbx_crash_info_t *info);

typedef struct {
    uint32_t reason;
    uint32_t flags;                  /* CBX_FLAG_FAULT forces the fault ring */
    uint32_t reset_flags;
    uint32_t boot_count;
    uint64_t uptime_ms;
    const char *firmware;
    const cbx_ring_t *ring;          /* may be NULL */
    const cbx_text_t *text;          /* may be NULL */
    const cbx_crash_info_t *crash;   /* may be NULL */
} cbx_snapshot_t;

/* Fault if: flags has CBX_FLAG_FAULT, a crash is attached, the reason is not
 * boot / checkpoint / request / shutdown, or a boot follows a watchdog,
 * exception or low-voltage reset. */
int cbx_snapshot(cbx_report_t *report, const cbx_snapshot_t *args);

/* ---- storage ---- */
int cbx_store_open(cbx_store_t *store, const cbx_io_t *io);
int cbx_store_append(cbx_store_t *store, const cbx_report_t *report, uint32_t *sequence);
/* Index 0 is the newest report. Read-only calls never change the area. */
int cbx_store_list(cbx_store_t *store, unsigned newest_index, cbx_report_info_t *info);
int cbx_store_read(cbx_store_t *store, uint32_t sequence, cbx_report_t *report);
/* Erase every slot (service tool "clear log"). */
int cbx_store_clear(cbx_store_t *store);

/* ---- readout: streaming, no heap; emit returns 0 on success ---- */
typedef int (*cbx_emit_fn)(void *user, const char *text, size_t size);

/* Optional names for the text output; any member may be NULL. */
typedef struct {
    const char *(*reason)(uint32_t reason);
    const char *(*event)(uint16_t type);
} cbx_names_t;

int cbx_report_text(const cbx_report_t *report, const cbx_names_t *names, cbx_emit_fn emit, void *user);
int cbx_report_json(const cbx_report_t *report, cbx_emit_fn emit, void *user);
/* Up to max_reports reports, newest first, as one text document. report is
 * caller-provided working memory. */
int cbx_store_text(cbx_store_t *store, cbx_report_t *report, unsigned max_reports,
                   const cbx_names_t *names, cbx_emit_fn emit, void *user);

const char *cbx_reason_name(uint32_t reason);

#ifdef __cplusplus
}
#endif

#endif
