/* SPDX-License-Identifier: Apache-2.0 */
/* Host tests: NOR 1->0 write rule, a power cut at every byte, corruption, fault
 * retention, sequence wrap, text, crash records, the file backend, readout. */
#include "crashbox.h"
#include "crashbox_file.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS 64u
static uint8_t flash[CBX_SLOT_SIZE * SLOTS];
static uint8_t saved_flash[sizeof(flash)];
static cbx_store_t store, reopened;
static cbx_report_t report, output;
static cbx_ring_t ring;
static cbx_text_t text;
static unsigned checks, groups, erase_calls, write_calls;
static int write_budget = -1;
static bool read_failure, erase_failure, nor_rule = true;

#define CHECK(expr) do { ++checks; if (!(expr)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #expr); abort(); } } while (0)
#define GROUP() do { ++groups; } while (0)

static int memory_read(void *user, uint32_t offset, void *data, uint32_t size)
{
    (void)user;
    CHECK(offset <= sizeof(flash) && size <= sizeof(flash) - offset);
    if (read_failure) return -1;
    memcpy(data, flash + offset, size);
    return 0;
}

static int memory_write(void *user, uint32_t offset, const void *data, uint32_t size)
{
    (void)user; ++write_calls;
    CHECK(offset <= sizeof(flash) && size <= sizeof(flash) - offset);
    uint32_t amount = size;
    if (write_budget >= 0 && (uint32_t)write_budget < amount) amount = (uint32_t)write_budget;
    const uint8_t *bytes = data;
    for (uint32_t i = 0; i < amount; ++i) {
        if (nor_rule) {
            CHECK((flash[offset + i] & bytes[i]) == bytes[i]);
            flash[offset + i] &= bytes[i];
        } else {
            flash[offset + i] = bytes[i];   /* file: overwrite in place */
        }
    }
    if (write_budget >= 0) write_budget -= (int)amount;
    return amount == size ? 0 : -1;
}

static int memory_erase(void *user, uint32_t offset, uint32_t size)
{
    (void)user; ++erase_calls;
    CHECK(offset % CBX_SLOT_SIZE == 0 && size == CBX_SLOT_SIZE);
    CHECK(offset <= sizeof(flash) && size <= sizeof(flash) - offset);
    if (erase_failure) {
        memset(flash + offset, 255, size / 2);
        return -1;
    }
    memset(flash + offset, 255, size);
    return 0;
}

static cbx_io_t nor_io = {NULL, sizeof(flash), memory_read, memory_write, memory_erase};
static cbx_io_t file_io = {NULL, sizeof(flash), memory_read, memory_write, NULL};
static cbx_io_t io;

static void build_report(uint32_t reason, uint32_t flags, uint32_t reset_flags)
{
    cbx_snapshot_t s = {reason, flags, reset_flags, 7, UINT64_MAX, "firmware-test", &ring, &text, NULL};
    CHECK(cbx_snapshot(&report, &s) == CBX_OK);
}

static void fresh_with(const cbx_io_t *medium)
{
    io = *medium;
    nor_rule = medium->erase != NULL;
    memset(flash, 255, sizeof(flash));
    write_budget = -1; read_failure = false; erase_failure = false;
    erase_calls = write_calls = 0;
    CHECK(cbx_store_open(&store, &io) == CBX_OK);
    cbx_ring_init(&ring);
    cbx_text_init(&text);
    for (unsigned i = 0; i < CBX_EVENT_CAPACITY; ++i) {
        cbx_event_t event = {1000u + i, 3, (uint16_t)i, i, i + 1u, UINT32_MAX};
        CHECK(cbx_ring_push(&ring, &event) == CBX_OK);
    }
    for (unsigned i = 0; i < 200; ++i) CHECK(cbx_text_printf(&text, 1000u * i, "line %u of the log", i) == CBX_OK);
    build_report(CBX_REASON_CHECKPOINT, 0, 0);
}

static void fresh(void) { fresh_with(&nor_io); }

static void test_ring(void)
{
    GROUP();
    cbx_ring_init(&ring);
    for (unsigned i = 0; i < 83; ++i) {
        cbx_event_t event = {i, 6, 2, i, 0, 0};
        CHECK(cbx_ring_push(&ring, &event) == CBX_OK);
    }
    CHECK(ring.count == 83 && ring.total == 83);
    cbx_snapshot_t s = {CBX_REASON_WATCHDOG, 0, CBX_RESET_WATCHDOG, 1, 999, "v1", &ring, NULL, NULL};
    CHECK(cbx_snapshot(&output, &s) == CBX_OK);
    CHECK(output.info.event_count == 64 && output.events[0].time_ms == 19);
    CHECK(output.events[63].a == 82 && output.info.total_events == 83);
    CHECK(output.info.flags & CBX_FLAG_FAULT);
    for (unsigned i = 83; i < 1100; ++i) {
        cbx_event_t event = {i, 6, 2, i, 0, 0};
        CHECK(cbx_ring_push(&ring, &event) == CBX_OK);
    }
    CHECK(ring.count == CBX_RING_CAPACITY && ring.total == 1100);
    s.reason = CBX_REASON_CHECKPOINT; s.reset_flags = 0;
    CHECK(cbx_snapshot(&output, &s) == CBX_OK && !(output.info.flags & CBX_FLAG_FAULT));
    CHECK(output.events[0].a == 1036 && output.events[63].a == 1099);
    cbx_event_t invalid = {0};
    CHECK(cbx_ring_push(&ring, &invalid) == CBX_INVALID);
    ring.total = UINT32_MAX; invalid.type = 1;
    CHECK(cbx_ring_push(&ring, &invalid) == CBX_OK && ring.total == UINT32_MAX);
    s.reason = 0;
    CHECK(cbx_snapshot(&output, &s) == CBX_INVALID);
    char long_version[100];
    memset(long_version, 'a', sizeof(long_version) - 1); long_version[99] = 0;
    s.reason = CBX_REASON_BOOT; s.firmware = long_version;
    CHECK(cbx_snapshot(&output, &s) == CBX_OK && output.info.firmware[31] == 0);
    s.firmware = NULL;
    CHECK(cbx_snapshot(&output, &s) == CBX_OK && output.info.firmware[0] == 0);
}

static void test_text(void)
{
    GROUP();
    cbx_text_init(&text);
    CHECK(cbx_text_printf(&text, 1234, "hello %d", 42) == CBX_OK);
    CHECK(text.count == strlen("[1.234] hello 42\n") && !memcmp(text.data, "[1.234] hello 42\n", text.count));
    cbx_text_write(&text, "raw\x01\n", 5);
    cbx_snapshot_t s = {CBX_REASON_REQUEST, 0, 0, 0, 5, "fw", NULL, &text, NULL};
    CHECK(cbx_snapshot(&output, &s) == CBX_OK);
    CHECK(output.info.text_len == text.count && !memcmp(output.text, "[1.234] hello 42\nraw?\n", 22));
    /* overflow: only whole lines are kept, the newest last */
    for (unsigned i = 0; i < 1000; ++i) CHECK(cbx_text_printf(&text, i, "message number %u", i) == CBX_OK);
    CHECK(text.count == CBX_TEXT_RING_SIZE);
    CHECK(cbx_snapshot(&output, &s) == CBX_OK);
    CHECK(output.info.text_len <= CBX_TEXT_CAPACITY && output.info.text_len > CBX_TEXT_CAPACITY - 40);
    CHECK(output.text[0] == '[');
    CHECK(!memcmp(output.text + output.info.text_len - 20, "message number 999\n", 19) ||
          strstr(output.text, "message number 999\n") != NULL);
    /* long line truncated, still newline terminated */
    char big[400];
    memset(big, 'x', sizeof(big) - 1); big[399] = 0;
    cbx_text_init(&text);
    CHECK(cbx_text_printf(&text, 0, "%s", big) == CBX_OK);
    CHECK(text.count <= 192 && text.data[text.count - 1] == '\n');
    /* one write larger than the ring keeps its tail */
    char huge[CBX_TEXT_RING_SIZE + 100];
    memset(huge, 'a', sizeof(huge)); huge[sizeof(huge) - 1] = 'z';
    cbx_text_write(&text, huge, sizeof(huge));
    CHECK(text.count == CBX_TEXT_RING_SIZE && text.data[(text.head + CBX_TEXT_RING_SIZE - 1) % CBX_TEXT_RING_SIZE] == 'z');
}

static void test_crash(void)
{
    GROUP();
    static cbx_crash_t slot;
    cbx_crash_info_t in, out;
    memset(&slot, 0xa5, sizeof(slot));       /* power-on garbage */
    CHECK(!cbx_crash_take(&slot, &out));
    memset(&in, 0, sizeof(in));
    in.pc = 0x400a1234; in.lr = 0x400b0000; in.sp = 0x30001000; in.cause = 7; in.addr = 8;
    in.reason = CBX_REASON_EXCEPTION; in.uptime_ms = 3600123;
    strcpy(in.task, "box");
    cbx_crash_record(&slot, &in);
    CHECK(cbx_crash_take(&slot, &out) && out.pc == in.pc && !strcmp(out.task, "box"));
    CHECK(!cbx_crash_take(&slot, &out));     /* taken once */
    cbx_crash_record(&slot, &in);
    slot.info.pc ^= 1;                       /* bit flip */
    CHECK(!cbx_crash_take(&slot, &out));

    fresh();
    cbx_snapshot_t s = {CBX_REASON_BOOT, 0, CBX_RESET_SOFTWARE, 9, 1500, "fw", &ring, &text, &in};
    CHECK(cbx_snapshot(&report, &s) == CBX_OK);
    CHECK((report.info.flags & (CBX_FLAG_CRASH | CBX_FLAG_FAULT)) == (CBX_FLAG_CRASH | CBX_FLAG_FAULT));
    uint32_t seq;
    CHECK(cbx_store_append(&store, &report, &seq) == CBX_OK);
    CHECK(store.sequences[48] == seq);       /* fault ring */
    CHECK(cbx_store_open(&reopened, &io) == CBX_OK && cbx_store_read(&reopened, seq, &output) == CBX_OK);
    CHECK(output.info.crash.pc == 0x400a1234 && output.info.crash.uptime_ms == 3600123);
    CHECK(!strcmp(output.info.crash.task, "box") && output.info.boot_count == 9);
}

static void test_geometry_and_empty(void)
{
    GROUP(); fresh();
    CHECK(store.valid_count == 0 && erase_calls == 0 && write_calls == 0);
    CHECK(cbx_store_list(&store, 0, &output.info) == CBX_NOT_FOUND);
    CHECK(cbx_store_read(&store, 1, &output) == CBX_NOT_FOUND);
    cbx_io_t invalid = io;
    invalid.size = CBX_SLOT_SIZE * 2;
    CHECK(cbx_store_open(&reopened, &invalid) == CBX_INVALID);
    invalid.size = CBX_SLOT_SIZE * (CBX_SLOT_LIMIT + 1);
    CHECK(cbx_store_open(&reopened, &invalid) == CBX_INVALID);
    invalid.size = sizeof(flash) - 1;
    CHECK(cbx_store_open(&reopened, &invalid) == CBX_INVALID);
    invalid = io; invalid.read = NULL;
    CHECK(cbx_store_open(&reopened, &invalid) == CBX_INVALID);
    read_failure = true;
    CHECK(cbx_store_open(&reopened, &io) == CBX_IO && !reopened.ready);
    read_failure = false;
}

static void test_roundtrip(const cbx_io_t *medium)
{
    GROUP(); fresh_with(medium);
    uint32_t sequence;
    CHECK(cbx_store_append(&store, &report, &sequence) == CBX_OK && sequence == 1);
    CHECK(cbx_store_open(&reopened, &io) == CBX_OK && reopened.valid_count == 1);
    CHECK(cbx_store_read(&reopened, sequence, &output) == CBX_OK);
    CHECK(output.info.sequence == sequence && output.info.uptime_ms == UINT64_MAX);
    CHECK(output.events[63].c == UINT32_MAX && output.events[17].code == 17);
    CHECK(output.info.text_len == report.info.text_len && !memcmp(output.text, report.text, report.info.text_len));
    unsigned erases_before = erase_calls, writes_before = write_calls;
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(cbx_store_list(&reopened, 0, &output.info) == CBX_OK);
        CHECK(cbx_store_read(&reopened, sequence, &output) == CBX_OK);
    }
    CHECK(erase_calls == erases_before && write_calls == writes_before);
    report.info.event_count = CBX_EVENT_CAPACITY + 1;
    CHECK(cbx_store_append(&store, &report, NULL) == CBX_INVALID);
    report.info.event_count = CBX_EVENT_CAPACITY;
    report.events[1].type = 0;
    CHECK(cbx_store_append(&store, &report, NULL) == CBX_INVALID);
    report.events[1].type = 3;
    report.info.text_len = CBX_TEXT_CAPACITY + 1;
    CHECK(cbx_store_append(&store, &report, NULL) == CBX_INVALID);
    CHECK(erase_calls == erases_before);
}

static void test_every_torn_write(const cbx_io_t *medium)
{
    GROUP(); fresh_with(medium);
    CHECK(cbx_store_append(&store, &report, NULL) == CBX_OK);
    memcpy(saved_flash, flash, sizeof(flash));
    /* cut power at every byte of footer invalidation, payload, CRC and commit */
    unsigned total_bytes = CBX_WIRE_HEADER_SIZE + report.info.event_count * CBX_WIRE_EVENT_SIZE +
                           report.info.text_len + 8u + (medium->erase ? 0u : 8u);
    for (unsigned cut = 0; cut < total_bytes; ++cut) {
        memcpy(flash, saved_flash, sizeof(flash));
        write_budget = -1;
        CHECK(cbx_store_open(&store, &io) == CBX_OK);
        write_budget = (int)cut;
        CHECK(cbx_store_append(&store, &report, NULL) == CBX_IO);
        write_budget = -1;
        CHECK(cbx_store_open(&reopened, &io) == CBX_OK);
        CHECK(reopened.valid_count == 1 && reopened.newest_sequence == 1);
        CHECK(cbx_store_read(&reopened, 1, &output) == CBX_OK);
    }
    if (medium->erase) {
        memcpy(flash, saved_flash, sizeof(flash));
        CHECK(cbx_store_open(&store, &io) == CBX_OK);
        erase_failure = true;
        CHECK(cbx_store_append(&store, &report, NULL) == CBX_IO);
        erase_failure = false;
        CHECK(cbx_store_open(&reopened, &io) == CBX_OK && reopened.newest_sequence == 1);
    }
}

static void test_corruption_and_lengths(void)
{
    GROUP(); fresh();
    CHECK(cbx_store_append(&store, &report, NULL) == CBX_OK);
    CHECK(cbx_store_append(&store, &report, NULL) == CBX_OK);
    memcpy(saved_flash, flash, sizeof(flash));
    /* length, count, text length, firmware terminator, event, text, CRC, commit */
    unsigned offsets[] = {8, 44, 46, 79, CBX_WIRE_HEADER_SIZE + 12,
                          CBX_WIRE_HEADER_SIZE + CBX_EVENT_CAPACITY * CBX_WIRE_EVENT_SIZE + 5,
                          CBX_SLOT_SIZE - 8, CBX_SLOT_SIZE - 4};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        memcpy(flash, saved_flash, sizeof(flash));
        flash[CBX_SLOT_SIZE + offsets[i]] ^= 0x80;
        CHECK(cbx_store_open(&reopened, &io) == CBX_OK);
        CHECK(reopened.newest_sequence == 1 && reopened.valid_count == 1);
    }
    memcpy(flash, saved_flash, sizeof(flash));
    CHECK(cbx_store_open(&reopened, &io) == CBX_OK);
    read_failure = true;
    CHECK(cbx_store_read(&reopened, 2, &output) == CBX_IO);
    read_failure = false;
}

static void test_fault_retention_and_wrap(void)
{
    GROUP(); fresh();
    build_report(CBX_REASON_WATCHDOG, 0, 0);
    CHECK(cbx_store_append(&store, &report, NULL) == CBX_OK);
    build_report(CBX_REASON_APP + 1, 0, 0);
    CHECK(cbx_store_append(&store, &report, NULL) == CBX_OK);
    build_report(CBX_REASON_CHECKPOINT, 0, 0);
    for (unsigned i = 0; i < 110; ++i) CHECK(cbx_store_append(&store, &report, NULL) == CBX_OK);
    CHECK(cbx_store_open(&reopened, &io) == CBX_OK && reopened.valid_count == 50);
    CHECK(cbx_store_read(&reopened, 1, &output) == CBX_OK && output.info.reason == CBX_REASON_WATCHDOG);
    CHECK(cbx_store_read(&reopened, 2, &output) == CBX_OK && output.info.reason == CBX_REASON_APP + 1);
    for (unsigned i = 0; i < reopened.valid_count; ++i) {
        CHECK(cbx_store_list(&reopened, i, &output.info) == CBX_OK);
        CHECK(output.info.sequence == (i < 48 ? 112u - i : 50u - i));
    }
    CHECK(cbx_store_list(&reopened, 50, &output.info) == CBX_NOT_FOUND);
    build_report(CBX_REASON_EXCEPTION, 0, 0);
    for (unsigned i = 0; i < 20; ++i) CHECK(cbx_store_append(&store, &report, NULL) == CBX_OK);
    CHECK(cbx_store_open(&reopened, &io) == CBX_OK && reopened.valid_count == 64);
    CHECK(cbx_store_read(&reopened, 1, &output) == CBX_NOT_FOUND);
    CHECK(cbx_store_read(&reopened, 112, &output) == CBX_OK);
}

static void test_reset_fault_sequence_wrap_and_clear(void)
{
    GROUP(); fresh();
    build_report(CBX_REASON_BOOT, 0, CBX_RESET_WATCHDOG);
    CHECK(cbx_store_append(&store, &report, NULL) == CBX_OK && store.sequences[48] == 1);
    build_report(CBX_REASON_BOOT, 0, CBX_RESET_POWER_ON);
    CHECK(cbx_store_append(&store, &report, NULL) == CBX_OK && store.sequences[0] == 2);
    build_report(CBX_REASON_REQUEST, CBX_FLAG_FAULT, 0);
    CHECK(cbx_store_append(&store, &report, NULL) == CBX_OK && store.sequences[49] == 3);
    CHECK(cbx_store_clear(&store) == CBX_OK && store.valid_count == 0);
    CHECK(cbx_store_open(&reopened, &io) == CBX_OK && reopened.valid_count == 0);
    CHECK(cbx_store_append(&store, &report, NULL) == CBX_OK && store.newest_sequence == 4);

    fresh();
    store.newest_sequence = UINT32_MAX - 1;
    CHECK(cbx_store_append(&store, &report, NULL) == CBX_OK);
    CHECK(cbx_store_append(&store, &report, NULL) == CBX_OK && store.newest_sequence == 1);
    CHECK(cbx_store_open(&reopened, &io) == CBX_OK && reopened.newest_sequence == 1);
    CHECK(cbx_store_list(&reopened, 1, &output.info) == CBX_OK && output.info.sequence == UINT32_MAX);
}

static char doc[65536];
static size_t doc_size;
static bool emit_failure;
static int collect(void *user, const char *data, size_t size)
{
    (void)user;
    if (emit_failure || size > sizeof(doc) - doc_size - 1u) return -1;
    memcpy(doc + doc_size, data, size);
    doc_size += size;
    doc[doc_size] = 0;
    return 0;
}

static const char *event_name(uint16_t type) { return type == 3 ? "bluetooth" : NULL; }

static void test_readout(void)
{
    GROUP(); fresh();
    strcpy(report.info.firmware, "test\"\\build");
    report.info.sequence = 789;
    doc_size = 0; emit_failure = false;
    CHECK(cbx_report_json(&report, collect, NULL) == CBX_OK);
    CHECK(strstr(doc, "\"schemaVersion\":1") && strstr(doc, "\"id\":789"));
    CHECK(strstr(doc, "18446744073709551615") && strstr(doc, "test\\\"\\\\build"));
    CHECK(strstr(doc, "\"text\":\"[") && strstr(doc, "of the log\\n"));
    CHECK(doc[doc_size - 2] == '}' && doc[doc_size - 1] == '\n');
    emit_failure = true;
    CHECK(cbx_report_json(&report, collect, NULL) == CBX_IO);
    CHECK(cbx_report_json(&report, NULL, NULL) == CBX_INVALID);
    emit_failure = false;

    fresh();
    cbx_crash_info_t crash = {0x40001000, 0x40002000, 0x30000000, 2, 0, CBX_REASON_WATCHDOG, 61000, "aic_rx"};
    cbx_snapshot_t s = {CBX_REASON_BOOT, 0, CBX_RESET_WATCHDOG, 3, 2500, "box-1.0", &ring, &text, &crash};
    CHECK(cbx_snapshot(&report, &s) == CBX_OK && cbx_store_append(&store, &report, NULL) == CBX_OK);
    build_report(CBX_REASON_CHECKPOINT, 0, 0);
    CHECK(cbx_store_append(&store, &report, NULL) == CBX_OK);
    cbx_names_t names = {NULL, event_name};
    doc_size = 0;
    CHECK(cbx_store_text(&store, &output, 10, &names, collect, NULL) == CBX_OK);
    CHECK(strstr(doc, "2 reports in 64 slots"));
    char *first = strstr(doc, "==== report #2: checkpoint"), *second = strstr(doc, "==== report #1: boot (1) FAULT");
    CHECK(first && second && first < second);
    CHECK(strstr(doc, "reset watchdog | boot 3 | up 0h00m02.500s | fw box-1.0"));
    CHECK(strstr(doc, "crash: watchdog in task 'aic_rx' at 0h01m01.000s: pc 0x40001000"));
    CHECK(strstr(doc, "  [1.063] bluetooth code 63  63 64 4294967295"));
    CHECK(strstr(doc, "log:\n[") && strstr(doc, "line 199 of the log\n"));
    doc_size = 0;
    CHECK(cbx_store_text(&store, &output, 1, NULL, collect, NULL) == CBX_OK);
    CHECK(strstr(doc, "report #2") && !strstr(doc, "report #1") && strstr(doc, "type3 code"));
}

static void test_file_backend(void)
{
    GROUP();
    const char *path = "crashbox_test.bin";
    cbx_file_t file;
    cbx_io_t fio;
    remove(path);
    CHECK(cbx_file_open(&file, path, 8u * CBX_SLOT_SIZE, &fio) == CBX_OK && fio.erase == NULL);
    CHECK(cbx_store_open(&store, &fio) == CBX_OK && store.valid_count == 0);
    cbx_ring_init(&ring); cbx_text_init(&text);
    cbx_text_printf(&text, 1, "file backend");
    for (unsigned i = 0; i < 20; ++i) {
        build_report(i % 5 ? CBX_REASON_CHECKPOINT : CBX_REASON_EXCEPTION, 0, 0);
        CHECK(cbx_store_append(&store, &report, NULL) == CBX_OK);
    }
    cbx_file_close(&file);
    CHECK(cbx_file_open(&file, path, 8u * CBX_SLOT_SIZE, &fio) == CBX_OK);
    CHECK(cbx_store_open(&reopened, &fio) == CBX_OK && reopened.newest_sequence == 20 && reopened.valid_count == 8);
    CHECK(cbx_store_read(&reopened, 20, &output) == CBX_OK && strstr(output.text, "file backend"));
    cbx_file_close(&file);
    CHECK(cbx_file_open(&file, path, 4u * CBX_SLOT_SIZE, &fio) == CBX_OK);   /* resized: starts empty */
    CHECK(cbx_store_open(&reopened, &fio) == CBX_OK && reopened.valid_count == 0);
    cbx_file_close(&file);
    remove(path);
}

int main(void)
{
    test_ring(); test_text(); test_crash(); test_geometry_and_empty();
    test_roundtrip(&nor_io); test_roundtrip(&file_io);
    test_every_torn_write(&nor_io); test_every_torn_write(&file_io);
    test_corruption_and_lengths(); test_fault_retention_and_wrap();
    test_reset_fault_sequence_wrap_and_clear(); test_readout(); test_file_backend();
    printf("crashbox: %u groups, %u checks passed (report %u B, store %u B)\n", groups, checks,
           (unsigned)sizeof(cbx_report_t), (unsigned)sizeof(cbx_store_t));
    return 0;
}
