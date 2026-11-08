/* SPDX-License-Identifier: Apache-2.0 */
/*
 * crashbox storage in one fixed-size file (POSIX open/read/write/fsync), for
 * a wear-levelling file system: littlefs (RT-Thread DFS, Zephyr, ...), UBIFS,
 * JFFS2, ext4 on eMMC. The file system spreads the writes; crashbox keeps the
 * file size constant, so it never grows or fragments.
 */
#ifndef CRASHBOX_FILE_H
#define CRASHBOX_FILE_H

#include "crashbox.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int fd;
    uint32_t size;
} cbx_file_t;

/* Opens (or creates and fills with 0xff) path with `size` bytes, a multiple of
 * CBX_SLOT_SIZE, and fills *io. A file of another size is recreated. */
int cbx_file_open(cbx_file_t *file, const char *path, uint32_t size, cbx_io_t *io);
void cbx_file_close(cbx_file_t *file);

#ifdef __cplusplus
}
#endif

#endif
