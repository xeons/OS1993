/*
 * files.h -- file access for ring-3 processes (files.c): DOS's drives
 * through DOS, the VMM's own (fs.h) directly.
 */
#ifndef OS1993_FILES_H
#define OS1993_FILES_H

#include <stdbool.h>
#include <stdint.h>

#define FILE_MAX    8                   /* open files per process */
#define FIND_MAX    4                   /* searches in progress per process */
#define PATH_MAX    260
#define DOS_PATH_MAX 128                /* what DOS takes */

struct fnode;
struct fs_find;

struct proc_files {
    int16_t dos[FILE_MAX];              /* DOS handles, -1 free */
    struct fnode *nf[FILE_MAX];         /* ... or a file on the VMM's own disks */
    uint32_t pos[FILE_MAX];             /* ... and its file pointer */
    bool    wr[FILE_MAX];               /* ... opened for writing */
    struct { bool used; uint8_t dta[43]; struct fs_find *nat; } find[FIND_MAX];
    char    cwd[PATH_MAX];              /* "C:\OS1993" */
};

struct file_args;
void files_init(struct proc_files *pf, const char *cwd);
/* Console thread only, with the process's address space installed. */
int  file_op(struct proc_files *pf, uint32_t op, struct file_args *a);
void files_close_all(struct proc_files *pf);

#endif
