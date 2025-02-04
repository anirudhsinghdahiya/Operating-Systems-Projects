/*
 * RAID Filesystem Initialization (mkfs)
 * 
 * This program initializes disk images for a RAID-based filesystem.
 * It supports RAID-0 (striping) and RAID-1 (mirroring) configurations
 * with proper block alignment and filesystem structure initialization.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>    // For time() function
#include "wfs.h"

/* Constants for filesystem initialization and error handling */
#define WFS_OK 0
#define WFS_ERROR -1
#define MAX_DISK_COUNT 32

/*
 * Aligns a size value to 32-byte boundary
 * Uses remainder-based calculation instead of bitwise operations
 * Returns: size aligned to next 32-byte boundary
 */
static size_t get_aligned_size(size_t size) {
    size_t remainder = size % 32;
    return remainder ? size + (32 - remainder) : size;
}

/*
 * Calculates filesystem layout and updates superblock pointers
 * Layout order: superblock -> inode bitmap -> data bitmap -> inodes -> data blocks
 * Ensures inode region is block-aligned
 * Returns: total size required for filesystem
 */
static size_t calculate_layout(struct wfs_sb *sb, size_t inodes, size_t blocks) {
    size_t layout_start = sizeof(struct wfs_sb);
    // Calculate bitmap sizes in bytes (8 bits per byte)
    size_t inode_bits = (inodes + 7) >> 3;
    size_t block_bits = (blocks + 7) >> 3;
    
    // Set bitmap region pointers
    sb->i_bitmap_ptr = layout_start;
    sb->d_bitmap_ptr = sb->i_bitmap_ptr + inode_bits;
    
    // Ensure inode region starts on block boundary
    size_t inode_start = sb->d_bitmap_ptr + block_bits;
    sb->i_blocks_ptr = inode_start + (BLOCK_SIZE - (inode_start % BLOCK_SIZE));
    if (inode_start % BLOCK_SIZE == 0) sb->i_blocks_ptr = inode_start;
    
    // Data blocks follow inode region
    sb->d_blocks_ptr = sb->i_blocks_ptr + (inodes * BLOCK_SIZE);
    
    return sb->d_blocks_ptr + (blocks * BLOCK_SIZE);
}

/* Structure to maintain disk initialization state */
struct disk_state {
    int fd;              // Disk file descriptor
    char *name;          // Disk filename
    size_t inode_count;  // Number of inodes
    size_t block_count;  // Number of data blocks
    int raid_mode;       // RAID mode (0 or 1)
    int device_order;        // Disk index in array
    int total_disks;     // Total number of disks in array
    int fs_id;           // Filesystem identifier (unique to the entire FS)
};

/*
 * Creates filesystem on a single disk
 * Initializes superblock, root inode, and bitmaps
 * Uses pwrite for atomic writes at specific offsets
 * Returns: WFS_OK on success, WFS_ERROR on failure
 */
static int create_filesystem(struct disk_state *disk) {
    struct stat st;
    if (fstat(disk->fd, &st) != 0) return WFS_ERROR;

    // Initialize structures with zero
    struct wfs_sb sb = {0};
    struct wfs_inode root = {0};
    uint32_t root_bit = 1;

    // Setup superblock fields
    sb.num_inodes = disk->inode_count;
    sb.num_data_blocks = disk->block_count;
    sb.fs_identifier = disk->fs_id;        // Set filesystem identifier
    sb.raid_mode = disk->raid_mode;        // Set RAID mode
    sb.device_order = (uint64_t)disk->device_order;  // Set device order using device_order

    // total_disks is not stored in the superblock structure in wfs.h as given,
    // but if you needed to store that, you would add a field for it.
    // For now, we only set the three requested variables in the superblock.

    // Debug prints
    printf("Debug: Creating filesystem on disk %d\n", disk->device_order);
    printf("  num_inodes: %zu\n", sb.num_inodes);
    printf("  num_data_blocks: %zu\n", sb.num_data_blocks);
    printf("  fs_identifier: %d\n", sb.fs_identifier);
    printf("  raid_mode: %d\n", sb.raid_mode);
    printf("  device_order: %lu\n", (unsigned long)sb.device_order);

    // Verify disk size
    size_t total_size = calculate_layout(&sb, disk->inode_count, disk->block_count);
    if (st.st_size < total_size) {
        fprintf(stderr, "Error: Disk size too small for requested layout.\n");
        return WFS_ERROR;
    }

    // Setup root directory inode
    root.mode = S_IFDIR | 0755;    // Directory with rwxr-xr-x permissions
    root.nlinks = 1;               // Initial link count
    root.uid = getuid();           // Current user's UID
    root.gid = getgid();           // Current user's GID
    root.size = 0;                 // Empty directory
    root.atim = root.mtim = root.ctim = time(NULL);  // Set all timestamps

    // Write filesystem structures atomically
    if (pwrite(disk->fd, &sb, sizeof(sb), 0) != sizeof(sb)) {
        fprintf(stderr, "Error: Failed to write superblock.\n");
        return WFS_ERROR;
    }

    if (pwrite(disk->fd, &root_bit, sizeof(root_bit), sb.i_bitmap_ptr) != sizeof(root_bit)) {
        fprintf(stderr, "Error: Failed to set root inode bit.\n");
        return WFS_ERROR;
    }

    if (pwrite(disk->fd, &root, sizeof(root), sb.i_blocks_ptr) != sizeof(root)) {
        fprintf(stderr, "Error: Failed to write root inode.\n");
        return WFS_ERROR;
    }

    return WFS_OK;
}

/*
 * Cleanup function for disk arrays
 * Closes all open file descriptors and frees allocated memory
 */
static void cleanup_args(char **names, int *fds, int count) {
    for (int i = 0; i < count; i++) {
        if (fds[i] >= 0) close(fds[i]);
        free(names[i]);
    }
    free(names);
    free(fds);
}

int main(int argc, char *argv[]) {
    // Allocate arrays for disk handling
    char **disk_names = calloc(MAX_DISK_COUNT, sizeof(char*));
    int *disk_fds = calloc(MAX_DISK_COUNT, sizeof(int));
    int disk_count = 0;
    struct disk_state state = {0};

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-' || i + 1 >= argc) {
            cleanup_args(disk_names, disk_fds, disk_count);
            exit(1);
        }

        char *value = argv[++i];
        switch (argv[i-1][1]) {
            case 'r':  // RAID mode
                state.raid_mode = atoi(value);
                if (state.raid_mode != 0 && state.raid_mode != 1) {
                    cleanup_args(disk_names, disk_fds, disk_count);
                    exit(1);
                }
                break;
            case 'i':  // Number of inodes
                state.inode_count = atoi(value);
                if (state.inode_count <= 0) {
                    cleanup_args(disk_names, disk_fds, disk_count);
                    exit(1);
                }
                break;
            case 'b':  // Number of blocks
                state.block_count = atoi(value);
                if (state.block_count <= 0) {
                    cleanup_args(disk_names, disk_fds, disk_count);
                    exit(1);
                }
                break;
            case 'd':  // Disk path
                if (disk_count >= MAX_DISK_COUNT) {
                    cleanup_args(disk_names, disk_fds, disk_count);
                    exit(1);
                }
                disk_fds[disk_count] = open(value, O_RDWR);
                if (disk_fds[disk_count] < 0) {
                    perror("Error opening disk");
                    cleanup_args(disk_names, disk_fds, disk_count);
                    exit(1);
                }
                disk_names[disk_count] = strdup(value);
                disk_count++;
                break;
            default:
                cleanup_args(disk_names, disk_fds, disk_count);
                exit(1);
        }
    }

    // Validate parameters
    if (disk_count < 2 || !state.inode_count || !state.block_count) {
        fprintf(stderr, "Error: RAID requires at least two disks, and both inodes/blocks must be > 0.\n");
        cleanup_args(disk_names, disk_fds, disk_count);
        exit(1);
    }

    // Align values to 32-byte boundaries
    state.inode_count = get_aligned_size(state.inode_count);
    state.block_count = get_aligned_size(state.block_count);

    // After alignment and before initialization loop
    state.total_disks = disk_count;

    // Generate a filesystem identifier using current time
    int f_id = (int)time(NULL);
    state.fs_id = f_id;

    printf("Debug: Initializing filesystem with:\n");
    printf("  raid_mode: %d\n", state.raid_mode);
    printf("  disk_count: %d\n", disk_count);
    printf("  inode_count: %zu\n", state.inode_count);
    printf("  block_count: %zu\n", state.block_count);
    printf("  fs_identifier: %d\n", state.fs_id);

    // Initialize filesystem on each disk
    for (int i = 0; i < disk_count; i++) {
        state.fd = disk_fds[i];
        state.name = disk_names[i];
        state.device_order = i;
        
        if (create_filesystem(&state) != WFS_OK) {
            fprintf(stderr, "Error: Failed to create filesystem on disk %d (%s)\n", i, state.name);
            cleanup_args(disk_names, disk_fds, disk_count);
            exit(-1);
        }
    }

    cleanup_args(disk_names, disk_fds, disk_count);
    return 0;
}