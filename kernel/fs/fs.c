/*
 * fs.c -- bringing up the VMM's file systems: probe the disks, read the
 * partition tables, mount what has a FAT, and give DOS its drive letters
 * (redir.c, which also picks the letters).
 */
#include "kernel.h"
#include "disk.h"
#include "fs.h"

void fs_init(void)
{
    cache_init();
    ata_init();
    fs_lock();
    part_scan();
    for (int i = 0; i < nparts; i++)
        fat_mount(&parts[i]);
    fs_unlock();
    redir_init();
    ata_claim();
}

void fs_shutdown(void)
{
    fat_sync_all();
    ata_release();
}
