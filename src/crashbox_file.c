/* SPDX-License-Identifier: Apache-2.0 */
#include "crashbox_file.h"
#include <fcntl.h>
#include <stdio.h>                       /* SEEK_SET on libcs whose unistd.h lacks it */
#include <string.h>
#include <sys/stat.h>
#include <stdint.h>
#include <unistd.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

static int at(cbx_file_t *f, uint32_t offset, uint32_t size)
{
    if (f->fd < 0 || offset > f->size || size > f->size - offset) return -1;
    return lseek(f->fd, (off_t)offset, SEEK_SET) == (off_t)offset ? 0 : -1;
}

static int file_read(void *user, uint32_t offset, void *data, uint32_t size)
{
    cbx_file_t *f = user;
    uint8_t *p = data;
    if (at(f, offset, size)) return -1;
    while (size) {
        ssize_t n = read(f->fd, p, size);
        if (n <= 0) return -1;
        p += n;
        size -= (uint32_t)n;
    }
    return 0;
}

static int file_write(void *user, uint32_t offset, const void *data, uint32_t size)
{
    cbx_file_t *f = user;
    const uint8_t *p = data;
    if (at(f, offset, size)) return -1;
    while (size) {
        ssize_t n = write(f->fd, p, size);
        if (n <= 0) return -1;
        p += n;
        size -= (uint32_t)n;
    }
    return fsync(f->fd) == 0 ? 0 : -1;   /* durable before the next step */
}

static int fill(cbx_file_t *f)
{
    uint8_t ones[256];
    memset(ones, 0xff, sizeof(ones));
    if (lseek(f->fd, 0, SEEK_SET) != 0) return -1;
    for (uint32_t done = 0; done < f->size; done += sizeof(ones))
        if (write(f->fd, ones, sizeof(ones)) != (ssize_t)sizeof(ones)) return -1;
    return fsync(f->fd) == 0 ? 0 : -1;
}

int cbx_file_open(cbx_file_t *file, const char *path, uint32_t size, cbx_io_t *io)
{
    struct stat st;
    if (!file || !path || !io || !size || size % CBX_SLOT_SIZE) return CBX_INVALID;
    file->size = size;
    file->fd = open(path, O_RDWR | O_BINARY);
    if (file->fd >= 0 && (fstat(file->fd, &st) != 0 || st.st_size != (off_t)size)) {
        close(file->fd);
        file->fd = -1;
        (void)unlink(path);          /* wrong size: start over */
    }
    if (file->fd < 0) {
        file->fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_BINARY, 0644);
        if (file->fd < 0) return CBX_IO;
        if (fill(file)) {
            cbx_file_close(file);
            return CBX_IO;
        }
    }
    memset(io, 0, sizeof(*io));
    io->user = file;
    io->size = size;
    io->read = file_read;
    io->write = file_write;
    io->erase = NULL;                /* overwrite in place; the core invalidates footers */
    return CBX_OK;
}

/* ---- split mode: one file per slot ---- */

static void slot_path(const cbx_file_t *f, uint32_t slot, char *out, size_t cap)
{
    (void)snprintf(out, cap, "%s/slot%03u.bin", f->dir, (unsigned)slot);
}

static int split_io(cbx_file_t *f, uint32_t offset, uint8_t *data, uint32_t size, int writing)
{
    char path[128];
    if (offset > f->size || size > f->size - offset) return -1;
    while (size) {
        uint32_t slot = offset / CBX_SLOT_SIZE, inner = offset % CBX_SLOT_SIZE;
        uint32_t chunk = CBX_SLOT_SIZE - inner < size ? CBX_SLOT_SIZE - inner : size, done = 0;
        int fd, ok;
        slot_path(f, slot, path, sizeof(path));
        fd = open(path, O_RDWR | O_BINARY);
        if (fd < 0) return -1;
        ok = lseek(fd, (off_t)inner, SEEK_SET) == (off_t)inner;
        while (ok && done < chunk) {
            ssize_t n = writing ? write(fd, data + done, chunk - done) : read(fd, data + done, chunk - done);
            if (n <= 0) ok = 0;
            else done += (uint32_t)n;
        }
        if (ok && writing && fsync(fd) != 0) ok = 0;   /* durable before the next step */
        close(fd);
        if (!ok) return -1;
        offset += chunk;
        data += chunk;
        size -= chunk;
    }
    return 0;
}

static int split_read(void *user, uint32_t offset, void *data, uint32_t size)
{
    return split_io(user, offset, data, size, 0);
}

static int split_write(void *user, uint32_t offset, const void *data, uint32_t size)
{
    return split_io(user, offset, (uint8_t *)(uintptr_t)data, size, 1);
}

/* Writes slot `slot` as `data` (CBX_SLOT_SIZE bytes) unless it already has that size. */
static int slot_create(const cbx_file_t *f, uint32_t slot, const uint8_t *data, int force)
{
    char path[128];
    struct stat st;
    int fd, ok;
    slot_path(f, slot, path, sizeof(path));
    if (!force && stat(path, &st) == 0 && st.st_size == (off_t)CBX_SLOT_SIZE) return 0;
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (fd < 0) return -1;
    ok = write(fd, data, CBX_SLOT_SIZE) == (ssize_t)CBX_SLOT_SIZE && fsync(fd) == 0;
    close(fd);
    return ok ? 0 : -1;
}

int cbx_file_open_split(cbx_file_t *file, const char *dir, uint32_t size, const char *legacy_path, cbx_io_t *io)
{
    static uint8_t block[CBX_SLOT_SIZE];
    struct stat st;
    int fresh, legacy = -1;
    if (!file || !dir || !io || !size || size % CBX_SLOT_SIZE || strlen(dir) >= sizeof(file->dir) - 16)
        return CBX_INVALID;
    memset(file, 0, sizeof(*file));
    file->fd = -1;
    file->size = size;
    strcpy(file->dir, dir);
    fresh = stat(dir, &st) != 0;
    if (fresh && mkdir(dir, 0755) != 0) return CBX_IO;
    if (fresh && legacy_path && stat(legacy_path, &st) == 0 && st.st_size == (off_t)size)
        legacy = open(legacy_path, O_RDONLY | O_BINARY);
    for (uint32_t slot = 0; slot < size / CBX_SLOT_SIZE; slot++) {
        int ok = legacy >= 0 && lseek(legacy, (off_t)slot * CBX_SLOT_SIZE, SEEK_SET) == (off_t)slot * CBX_SLOT_SIZE &&
                 read(legacy, block, CBX_SLOT_SIZE) == (ssize_t)CBX_SLOT_SIZE;
        if (!ok) memset(block, 0xff, sizeof(block));
        if (slot_create(file, slot, block, legacy >= 0)) {
            if (legacy >= 0) close(legacy);
            return CBX_IO;
        }
    }
    if (legacy >= 0) {
        close(legacy);
        (void)unlink(legacy_path);   /* its reports now live in the slot files */
    }
    memset(io, 0, sizeof(*io));
    io->user = file;
    io->size = size;
    io->read = split_read;
    io->write = split_write;
    io->erase = NULL;                /* overwrite in place, as in one-file mode */
    return CBX_OK;
}

void cbx_file_close(cbx_file_t *file)
{
    if (file && file->fd >= 0) {
        close(file->fd);
        file->fd = -1;
    }
}
