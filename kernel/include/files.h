/*
 * files.h -- file access for ring-3 processes (files.c), through DOS.
 */
#ifndef OS1993_FILES_H
#define OS1993_FILES_H

#include <stdbool.h>
#include <stdint.h>

#define FILE_MAX    8                   /* open files per process */
#define FIND_MAX    4                   /* searches in progress per process */
#define PATH_MAX    128

struct proc_files {
    int16_t dos[FILE_MAX];              /* DOS handles, -1 free */
    struct { bool used; uint8_t dta[43]; } find[FIND_MAX];
    char    cwd[PATH_MAX];              /* "C:\OS1993" */
};

struct file_args;
void files_init(struct proc_files *pf, const char *cwd);
/* Console thread only, with the process's address space installed. */
int  file_op(struct proc_files *pf, uint32_t op, struct file_args *a);
void files_close_all(struct proc_files *pf);

#endif
