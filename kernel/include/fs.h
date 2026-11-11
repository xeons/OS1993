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
#define VOL_MAX     24


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
    uint64_t size;
    uint16_t time, date;                /* modified, DOS format */
    uint16_t ctime, cdate, adate;       /* created, last access */
    uint32_t cluster;
    bool     lfn;                       /* name is a long name (short_name its alias) */
};

struct fs_find {                        /* a search in progress */
    struct vol *v;
    uint32_t dir;                       /* directory cluster (0: the root) */
    uint32_t next;                      /* next entry index */
    uint32_t dirlen;                    /* the directory's length in clusters, if the driver needs it */
    uint8_t  attr;                      /* DOS mode: the search attributes */
    bool     dos;                       /* DOS mode: FCB template, DOS's attribute rules */
    uint8_t  fcb[11];                   /* DOS mode's template */
    char     pattern[FS_NAME_MAX + 1];  /* otherwise: * and ? against either name */
    uint32_t seen[32];                  /* hashed aliases met so far (by hash, 1024 bits) */
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
void fs_drop_volume(struct vol *v);     /* DOS keeps it (redir.c) */
void fs_commit_all(void);               /* journals' transactions, committed now */
int  fs_boot_sector(struct vol *v, uint8_t *out);   /* its first sector */
uint32_t fs_serial(const struct vol *v);
const char *fs_label(const struct vol *v);
const char *fs_type_name(const struct vol *v);  /* "FAT12", "FAT16", "FAT32" */

/* Files. disposition: CREATE_NEW .. TRUNCATE_EXISTING (user.h). */
int  fs_open(struct vol *v, const char *path, uint32_t disposition, bool write, uint8_t attr,
             struct fnode **out, bool *created);
int  fs_read(struct fnode *n, uint64_t pos, void *buf, uint32_t len);       /* -> bytes */
int  fs_write(struct fnode *n, uint64_t pos, const void *buf, uint32_t len);/* -> bytes */
int  fs_set_size(struct fnode *n, uint64_t size);
uint64_t fs_size(const struct fnode *n);
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
int  fs_link(struct vol *v, const char *existing, const char *path);        /* a hard link */
int  fs_symlink(struct vol *v, const char *path, const char *target);       /* a symbolic link */

/* Searches: the directory is a path (""), the pattern a name with * and ?.
 * fs_find_dos: an FCB template ("????????TXT") and DOS's attribute rules. */
int  fs_find_first(struct vol *v, const char *dir, const char *pattern, struct fs_find *f,
                   struct fs_entry *e);
int  fs_find_dos(struct vol *v, const char *dir, const uint8_t fcb[11], uint8_t attr,
                 struct fs_find *f, struct fs_entry *e);
int  fs_find_next(struct fs_find *f, struct fs_entry *e);

int  fs_space(struct vol *v, uint32_t *spc, uint32_t *bps, uint32_t *free, uint32_t *total);

/* Share modes and byte-range locks, between DOS's and OS1993's opens.
 * owner: who opened it (a redirector file, a process's handle). */
int  fs_share_open(struct fnode *n, uintptr_t owner, bool rd, bool wr, int share);
void fs_share_close(struct fnode *n, uintptr_t owner);         /* and its locks */
int  fs_lock_range(struct fnode *n, uintptr_t owner, uint64_t off, uint64_t len, bool unlock);
int  fs_lock_check(struct fnode *n, uintptr_t owner, uint64_t off, uint64_t len);

uint64_t fs_div64(uint64_t n, uint32_t d, uint32_t *rem);
uint64_t fs_dos_to_filetime(uint16_t date, uint16_t time);
void fs_filetime_to_dos(uint64_t ft, uint16_t *date, uint16_t *time);

/* redir.c: DOS's view */
void redir_init(void);                  /* drive letters for DOS (System VM, before the snapshot) */
bool redir_int2f(struct vm *vm, struct regs *r);    /* true: handled */
void redir_restore(void);               /* DOS's own drives back (shutdown) */
void redir_vm_end(struct vm *vm);       /* close what a VM left open */
int  redir_cds_path(int drive, char *out);  /* DOS's current directory on a drive (0 = A:) */
int  redir_current_drive(void);
int  redir_handle_node(uint16_t handle, struct fnode **n);    /* a DOS handle of the caller -> our file */
int  redir_search_new(struct vm *vm, uint32_t *stamp);        /* a slot for a search in progress */
struct fs_find *redir_search(int slot, uint32_t stamp);
void redir_search_free(int slot);

/* lfn.c: INT 21h AX=71xxh, the long-name calls, on the VMM's drives */
#define LFN_PASS    0                   /* not ours: on to DOS */
#define LFN_DONE    1                   /* answered */
#define LFN_REWRITE 2                   /* rewritten as a DOS call: run lfn_stub()'s routine */
int  lfn_int21(struct vm *vm, struct regs *r);
uint16_t lfn_stub(uint16_t *seg);
void lfn_vm_end(struct vm *vm);

/* user.c */
void rtc_dos_time(uint16_t *date, uint16_t *time);
void rtc_dos_time_odd(uint16_t *date, uint16_t *time, uint8_t *odd);

/* The time zone: the RTC keeps local time; UTC = local + bias minutes
 * (Win32's sign). ext2 keeps UTC, exFAT local time with its offset. */
void fs_set_timezone(const struct fs_timezone *z);
void fs_get_timezone(struct fs_timezone *z);
bool fs_timezone_known(void);
int32_t fs_timezone(void);              /* the bias now */
int32_t fs_bias_local(uint16_t date, uint16_t time);
int32_t fs_bias_utc(uint32_t unix_utc);

#endif
