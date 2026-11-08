/* SPDX-License-Identifier: Apache-2.0 */
#include "crashbox_file.h"
#include <fcntl.h>
#include <stdio.h>                       /* SEEK_SET on libcs whose unistd.h lacks it */
#include <string.h>
#include <sys/stat.h>
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

void cbx_file_close(cbx_file_t *file)
{
    if (file && file->fd >= 0) {
        close(file->fd);
        file->fd = -1;
    }
}
