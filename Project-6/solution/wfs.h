#ifndef WFS_H
#define WFS_H

#include <time.h>
#include <sys/stat.h>
#include <stdint.h>

#define BLOCK_SIZE (512)
#define MAX_NAME   (28)

#define D_BLOCK    (6)
#define IND_BLOCK  (D_BLOCK+1)
#define N_BLOCKS   (IND_BLOCK+1)

// Define constants for RAID modes
#define RAID0 0
#define RAID1 1
#define RAID1V 2

/*
  The fields in the superblock should reflect the structure of the filesystem.
  `mkfs` writes the superblock to offset 0 of the disk image. 
  The disk image will have this format:

          d_bitmap_ptr       d_blocks_ptr
               v                  v
+----+---------+---------+--------+--------------------------+
| SB | IBITMAP | DBITMAP | INODES |       DATA BLOCKS        |
+----+---------+---------+--------+--------------------------+
0    ^                   ^
i_bitmap_ptr        i_blocks_ptr

*/

// Superblock structure
struct wfs_sb {
    size_t num_inodes;
    size_t num_data_blocks;
    off_t i_bitmap_ptr;
    off_t d_bitmap_ptr;
    off_t i_blocks_ptr;
    off_t d_blocks_ptr;

    // RAID-specific fields
    int fs_identifier;     // unique id for the filesystem (renamed from f_id)
    int raid_mode;        // the RAID mode of the filesystem (renamed from raid)
    uint64_t device_order; // Unique ID or order of this disk in the RAID array (renamed from disk_id)
};

// Inode structure
struct wfs_inode {
    int     num;      /* Inode number */
    mode_t  mode;     /* File type and mode */
    uid_t   uid;      /* User ID of owner */
    gid_t   gid;      /* Group ID of owner */
    off_t   size;     /* Total size, in bytes */
    int     nlinks;   /* Number of links */

    time_t atim;      /* Time of last access */
    time_t mtim;      /* Time of last modification */
    time_t ctim;      /* Time of last status change */

    off_t blocks[N_BLOCKS]; /* Direct and indirect block pointers */
};

// Directory entry structure
struct wfs_dentry {
    char name[MAX_NAME];
    int num;
};

#endif // WFS_H