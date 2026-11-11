/*
 * fs.h -- the VMM's own file systems (kernel/fs): volumes on the disks it
 * owns (disk.h), each with a drive letter. OS1993 programs reach them
 * directly (files.c), DOS through the redirector (redir.c).
 *
 * Paths here are relative to the volume's root, '\'-separated, already
 * normalized ("DIR\SUB\FILE.TXT"; "" is the root). Names compare without
 * regard to case and match either a long name or its 8.3 alias. Errors
 * are negative DOS/Win32 error codes (ERROR_* in user.h).
 *
 * Every call takes the file system lock itself; a caller that needs
 * several calls to be atomic holds fs_lock() around them (it nests).
 */
#ifndef OS1993_FS_H
#define OS1993_FS_H

#include <stdbool.h>
#include <stdint.h>

#include "user.h"

#define FS_NAME_MAX 255
#define VOL_MAX     8


struct vol;
struct fnode;
struct vm;
struct regs;
struct partition;

struct fs_entry {                       /* a directory entry */
    char     name[FS_NAME_MAX + 1];     /* the long name, or the 8.3 one in its case */
    char     short_name[13];            /* "NAME.EXT", upper case as DOS sees it */
    uint8_t  fcb_name[11];              /* "NAME    EXT" */
    uint8_t  attr;
    uint32_t size;
    uint16_t time, date;                /* modified, DOS format */
    uint16_t ctime, cdate, adate;       /* created, last access */
    uint32_t cluster;
    bool     lfn;                       /* name is a long name (short_name its alias) */
};

struct fs_find {                        /* a search in progress */
    struct vol *v;
    uint32_t dir;                       /* directory cluster (0: the root) */
    uint32_t next;                      /* next entry index */
    uint8_t  attr;                      /* DOS mode: the search attributes */
    bool     dos;                       /* DOS mode: FCB template, DOS's attribute rules */
    uint8_t  fcb[11];                   /* DOS mode's template */
    char     pattern[FS_NAME_MAX + 1];  /* otherwise: * and ? against either name */
};

void fs_lock(void);
void fs_unlock(void);

void fs_init(void);                     /* probe, mount, letters (before the VM snapshot) */
void fs_shutdown(void);                 /* write everything back; disks back to the BIOS */

struct vol *fs_volume(char letter);     /* the native volume with that letter, or NULL */
char fs_letter(const struct vol *v);
int  fs_volume_count(void);
struct vol *fs_volume_at(int i);
void fs_set_letter(struct vol *v, char letter);
uint32_t fs_serial(const struct vol *v);
const char *fs_label(const struct vol *v);
const char *fs_type_name(const struct vol *v);  /* "FAT12", "FAT16", "FAT32" */

/* Files. disposition: CREATE_NEW .. TRUNCATE_EXISTING (user.h). */
int  fs_open(struct vol *v, const char *path, uint32_t disposition, bool write, uint8_t attr,
             struct fnode **out, bool *created);
int  fs_read(struct fnode *n, uint32_t pos, void *buf, uint32_t len);       /* -> bytes */
int  fs_write(struct fnode *n, uint32_t pos, const void *buf, uint32_t len);/* -> bytes */
int  fs_set_size(struct fnode *n, uint32_t size);
uint32_t fs_size(const struct fnode *n);
void fs_get_time(const struct fnode *n, uint16_t *date, uint16_t *time);
int  fs_set_time(struct fnode *n, uint16_t date, uint16_t time);
uint8_t fs_node_attr(const struct fnode *n);
void fs_name_of(const struct fnode *n, uint8_t fcb[11]);
int  fs_flush(struct fnode *n);
void fs_close(struct fnode *n);
struct fnode *fs_dup(struct fnode *n);

/* Names. */
int  fs_stat(struct vol *v, const char *path, struct fs_entry *e);  /* the root: a directory */
int  fs_set_attr(struct vol *v, const char *path, uint8_t attr);
int  fs_delete(struct vol *v, const char *path);
int  fs_mkdir(struct vol *v, const char *path);
int  fs_rmdir(struct vol *v, const char *path);
int  fs_rename(struct vol *v, const char *from, const char *to);
int  fs_short_path(struct vol *v, const char *path, char *out, uint32_t size);

/* Searches: the directory is a path (""), the pattern a name with * and ?.
 * fs_find_dos: an FCB template ("????????TXT") and DOS's attribute rules. */
int  fs_find_first(struct vol *v, const char *dir, const char *pattern, struct fs_find *f,
                   struct fs_entry *e);
int  fs_find_dos(struct vol *v, const char *dir, const uint8_t fcb[11], uint8_t attr,
                 struct fs_find *f, struct fs_entry *e);
int  fs_find_next(struct fs_find *f, struct fs_entry *e);

int  fs_space(struct vol *v, uint32_t *spc, uint32_t *bps, uint32_t *free, uint32_t *total);

/* redir.c: DOS's view */
void redir_init(void);                  /* drive letters for DOS (System VM, before the snapshot) */
bool redir_int2f(struct vm *vm, struct regs *r);    /* true: handled */
void redir_restore(void);               /* DOS's own drives back (shutdown) */
void redir_vm_end(struct vm *vm);       /* close what a VM left open */

/* fat.c, for fs.c */
struct vol *fat_mount(struct partition *p);
void fat_sync_all(void);

/* user.c */
void rtc_dos_time(uint16_t *date, uint16_t *time);

#endif
