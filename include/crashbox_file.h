/* SPDX-License-Identifier: Apache-2.0 */
/*
 * crashbox storage in one fixed-size file (POSIX open/read/write/fsync), for
 * a wear-levelling file system: littlefs (RT-Thread DFS, Zephyr, ...), UBIFS,
 * JFFS2, ext4 on eMMC. The file system spreads the writes; crashbox keeps the
 * file size constant, so it never grows or fragments.
 *
 * Split mode keeps every slot in its own CBX_SLOT_SIZE file under a directory.
 * Use it on littlefs: a write into the middle of one big file rewrites the file
 * from that point on (its blocks form a back-linked list), so a 64-slot file
 * cost the D133 seconds of flash erases per report while holding the file
 * system lock; one file per slot rewrites one block.
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
    char dir[96];                    /* split mode: the slot directory; "" = one file */
} cbx_file_t;

/* Opens (or creates and fills with 0xff) path with `size` bytes, a multiple of
 * CBX_SLOT_SIZE, and fills *io. A file of another size is recreated. */
int cbx_file_open(cbx_file_t *file, const char *path, uint32_t size, cbx_io_t *io);
/* Split mode: `size` / CBX_SLOT_SIZE files dir/slotNNN.bin, created (0xff) when missing. When
 * the directory is new and legacy_path (may be NULL) is a single file of the same size, its slots
 * are copied over and the file is removed, so existing reports survive the switch. */
int cbx_file_open_split(cbx_file_t *file, const char *dir, uint32_t size, const char *legacy_path, cbx_io_t *io);
void cbx_file_close(cbx_file_t *file);

#ifdef __cplusplus
}
#endif

#endif
