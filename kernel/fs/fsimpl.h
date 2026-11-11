/*
 * fsimpl.h -- inside the file systems: what a driver (fat.c, exfat.c)
 * gives fs.c, which hands the public calls in fs.h on to it.
 *
 * A driver's volume begins with a struct vol and its open files with a
 * struct fnode. Its calls take the lock themselves (fs_lock nests).
 */
#ifndef OS1993_FSIMPL_H
#define OS1993_FSIMPL_H

#include "fs.h"

struct fs_ops {
    int  (*open)(struct vol *v, const char *path, uint32_t disposition, bool write, uint8_t attr,
                 struct fnode **out, bool *created);
    int  (*read)(struct fnode *n, uint64_t pos, void *buf, uint32_t len);
    int  (*write)(struct fnode *n, uint64_t pos, const void *buf, uint32_t len);
    int  (*set_size)(struct fnode *n, uint64_t size);
    uint64_t (*size)(const struct fnode *n);
    void (*get_time)(const struct fnode *n, uint16_t *date, uint16_t *time);
    int  (*set_time)(struct fnode *n, uint16_t date, uint16_t time);
    uint8_t (*attr)(const struct fnode *n);
    void (*name_of)(const struct fnode *n, uint8_t fcb[11]);
    int  (*flush)(struct fnode *n);
    void (*close)(struct fnode *n);
    void (*dup)(struct fnode *n);
    int  (*stat)(struct vol *v, const char *path, struct fs_entry *e);
    int  (*set_attr)(struct vol *v, const char *path, uint8_t attr);
    int  (*del)(struct vol *v, const char *path);
    int  (*mkdir)(struct vol *v, const char *path);
    int  (*rmdir)(struct vol *v, const char *path);
    int  (*rename)(struct vol *v, const char *from, const char *to);
    int  (*short_path)(struct vol *v, const char *path, char *out, uint32_t size);
    int  (*find_first)(struct vol *v, const char *dir, const char *pattern, struct fs_find *f,
                       struct fs_entry *e);
    int  (*find_dos)(struct vol *v, const char *dir, const uint8_t fcb[11], uint8_t attr,
                     struct fs_find *f, struct fs_entry *e);
    int  (*find_next)(struct fs_find *f, struct fs_entry *e);
    int  (*space)(struct vol *v, uint32_t *spc, uint32_t *bps, uint32_t *free, uint32_t *total);
    void (*sync)(struct vol *v);                /* open files' entries, then the cache */
    int  (*link)(struct vol *v, const char *existing, const char *path);    /* NULL: not supported */
    int  (*symlink)(struct vol *v, const char *path, const char *target);
    void (*commit)(struct vol *v);              /* a journal's transaction, now (NULL: none) */
};

struct vol {
    const struct fs_ops *ops;
    struct disk *disk;                          /* where it is */
    uint32_t start;
    const char *type;                           /* "FAT32", "exFAT" ... */
    char     letter;
    uint32_t serial;
    char     label[12];                         /* 8-bit, as DOS shows it */
};

struct fnode {
    struct vol *v;
};

void fs_add_volume(struct vol *v);

struct partition;
struct vol *fat_mount(struct partition *p);
struct vol *exfat_mount(struct partition *p);

/* Shared by the drivers (fs.c) */
char fs_up(char c);                             /* ASCII and Latin-1 upper case */
bool fs_name_eq(const char *a, const char *b);
bool fs_wild(const char *pattern, const char *name);     /* * and ?, any case */
bool fs_fcb_match(const uint8_t *tmpl, const uint8_t *name);
int  fs_clean_name(const char *in, char *out);  /* -> length, or -ERROR_* */
bool fs_short_char(uint8_t c);
/* DOS's name for a long one: the name in upper case if it fits 8.3, else
 * "XXhhhh~1.EXT" from hash (fcb as in a directory entry, alias "NAME.EXT"). */
bool fs_make_alias(const char *name, uint16_t hash, uint8_t fcb[11], char *alias);  /* -> hashed */
void fs_alias_number(uint8_t fcb[11], char *alias, int n);      /* "~1" -> "~n" */
bool fs_alias_parse(const char *name, uint8_t base[11], int *n); /* "~n" -> its "~1" form */
uint16_t fs_hash16(const char *name);
void fs_unix_to_dos(uint32_t t, uint16_t *date, uint16_t *time);
uint32_t fs_dos_to_unix(uint16_t date, uint16_t time);
struct vol *ext2_mount(struct partition *p);
struct vol *btrfs_mount(struct partition *p);
uint32_t fs_crc32c(uint32_t crc, const void *data, uint32_t len);   /* no final inversion */

#endif
