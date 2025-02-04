#define FUSE_USE_VERSION 26

#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <libgen.h>
#include <stdlib.h>
#include <fuse.h>
#include <assert.h>
#include <string.h>
#include "wfs.h"

#define MAX_DISKS 10

// global variables
void *fs_memory_regions[MAX_DISKS];    // memory-mapped regions for each disk image
uint64_t device_orders[MAX_DISKS];     // order of device_order
int raid_mode;                         // RAID mode
int total_devices;                     // number of disks in fs
int error_code;                        // rc of error

// State management structures 
enum RaidState {
    INIT,
    READ_READY,
    WRITE_READY,
    DELETE_READY,
    CREATE_READY,
    ERROR
};

enum WriteState {
    WRITE_INIT,
    BUFFER_READY,
    WRITE_IN_PROGRESS,
    UPDATE_SIZE,
    WRITE_COMPLETE,
    WRITE_ERROR
};

struct RaidManager {
    enum RaidState state;
    int current_disk;
    int total_disks;
    int raid_type;       // RAID0, RAID1, RAID1V
    void *current_buffer;
    size_t buffer_size;
    uint64_t operation_id;
};

// Global raid manager
static struct RaidManager raid_mgr;

// Basic Helper Functions

// State transition function
void transition_state(enum RaidState new_state) {
    raid_mgr.state = new_state;
}

// Initialize manager
void init_raid_manager(void) {
    raid_mgr.state = INIT;
    raid_mgr.current_disk = 0;
    raid_mgr.total_disks = total_devices;
    raid_mgr.raid_type = raid_mode;
    raid_mgr.current_buffer = NULL;
    raid_mgr.buffer_size = 0;
    raid_mgr.operation_id = 0;
}

void release_bitmap(uint32_t position, uint32_t *bitmap)//ch
{
    uint32_t mask = ~(1U << (position & 31));  // Using bitwise AND for modulo
    bitmap[position >> 5] &= mask;  // Using shift for division, AND instead of XOR
}

void release_block(off_t blk, int disk)///ch
{
    struct wfs_sb *sb = (struct wfs_sb *)fs_memory_regions[disk];
    // Zero out the block
    void *block_address = (char *)fs_memory_regions[disk] + blk;
    memset(block_address, 0, BLOCK_SIZE);

    // Calculate position in data block bitmap
    uint32_t position = (blk - sb->d_blocks_ptr) / BLOCK_SIZE;
    uint32_t *bitmap = (uint32_t *)((char *)fs_memory_regions[disk] + sb->d_bitmap_ptr);

    release_bitmap(position, bitmap);
}

void release_inode(struct wfs_inode *inode, int disk) {///ch
    struct wfs_sb *sb = (struct wfs_sb *)fs_memory_regions[disk];
    
    // Clear inode contents
    memset((char *)inode, 0, BLOCK_SIZE);
    
    // Calculate bitmap position
    uint32_t bit_position = ((char *)inode - (char *)fs_memory_regions[disk] + sb->i_blocks_ptr) / BLOCK_SIZE;
    uint32_t *bitmap = (uint32_t *)((char *)fs_memory_regions[disk] + sb->i_bitmap_ptr);
    
    // Use our improved release_bitmap function
    release_bitmap(bit_position, bitmap);
}

struct wfs_inode *get_inode(int num, int disk) {
    struct wfs_sb *sb = (struct wfs_sb *)((char *)fs_memory_regions[disk]);
    uint32_t *bitmap = (uint32_t *)((char *)fs_memory_regions[disk] + sb->i_bitmap_ptr);

    int block = num / 32;
    int position = num % 32;

    if (bitmap[block] & (0x1 << position)) {
        return (struct wfs_inode *)((char *)fs_memory_regions[disk] + sb->i_blocks_ptr + num * BLOCK_SIZE);
    }
    return NULL;
}

ssize_t allocate_bitmap_block(uint32_t *bitmap, size_t len) {
    // Scan each word first
    for (size_t word_idx = 0; word_idx < len; word_idx++) {
        uint32_t word = bitmap[word_idx];
        if (word != 0xFFFFFFFF) {  // Check if word has any free bits
            // Find first zero bit in the word
            for (int bit = 0; bit < 32; bit++) {
                if (!(word & (1U << bit))) {
                    bitmap[word_idx] |= (1U << bit);
                    return word_idx * 32 + bit;
                }
            }
        }
    }
    return -1;  // No free bits found
}

off_t allocate_block(int disk) {
    struct wfs_sb *sb = (struct wfs_sb *)fs_memory_regions[disk];
    uint32_t *bitmap = (uint32_t *)((char *)fs_memory_regions[disk] + sb->d_bitmap_ptr);

    off_t block_num = allocate_bitmap_block(bitmap, sb->num_data_blocks / 32);
    if (block_num < 0) {
        return 0;
    }
    
    return sb->d_blocks_ptr + BLOCK_SIZE * block_num;
}

struct wfs_inode *create_inode(int disk) {
    struct wfs_sb *sb = (struct wfs_sb *)fs_memory_regions[disk];
    uint32_t *bitmap = (uint32_t *)((char *)fs_memory_regions[disk] + sb->i_bitmap_ptr);
    
    off_t block_num = allocate_bitmap_block(bitmap, sb->num_inodes / 32);
    if (block_num < 0) {
        error_code = -ENOSPC;
        return NULL;
    }
    
    struct wfs_inode *inode = (struct wfs_inode *)((char *)fs_memory_regions[disk] + 
                             sb->i_blocks_ptr + BLOCK_SIZE * block_num);
    inode->num = block_num;
    
    return inode;
}

void setup_inode(struct wfs_inode *inode, mode_t mode)///ch
{
    struct timespec time;
    clock_gettime(CLOCK_REALTIME, &time);
    
    // Set timestamps all at once
    time_t curr_time = time.tv_sec;
    inode->mode = mode;
    inode->uid = getuid();
    inode->gid = getgid();
    inode->size = 0;
    inode->nlinks = 1;
    inode->atim = curr_time;
    inode->mtim = curr_time;
    inode->ctim = curr_time;
}

// Helper functions
char *get_block_location(struct wfs_inode *inode, off_t offset, int alloc, int disk) {
    int block_num = offset / BLOCK_SIZE; 
    off_t *blocks;

    // Step 1: block number within valid range
    if (block_num > D_BLOCK + (BLOCK_SIZE / sizeof(off_t))) {
        return NULL;
    }

    // Step 2: handle RAID0
    if (raid_mode == RAID0) {
        int target_disk = block_num % total_devices; // target disk for this block
        if (block_num > D_BLOCK) { // indirect block
            block_num -= IND_BLOCK;
            if (inode->blocks[IND_BLOCK] == 0) {
                // allocate indirect block for all disks
                for (int i = 0; i < total_devices; i++) {
                    struct wfs_inode *w = get_inode(inode->num, i);
                    w->blocks[IND_BLOCK] = allocate_block(i);
                }
            }
            // calculate block array for indirect blocks
            blocks = (off_t *)((char *)fs_memory_regions[disk] + inode->blocks[IND_BLOCK]);
        } else { // direct block
            blocks = inode->blocks;
        }

        // Step 3: allocate block
        if (alloc && *(blocks + block_num) == 0) {
            off_t new_block_offset = allocate_block(target_disk);
            for (int i = 0; i < total_devices; i++) {
                struct wfs_inode *w = get_inode(inode->num, i);
                if (blocks != inode->blocks) { // indirect block
                    off_t *indirect = (off_t *)((char *)fs_memory_regions[i] + w->blocks[IND_BLOCK]);
                    *(indirect + block_num) = new_block_offset;
                } else { // direct block
                    w->blocks[block_num] = new_block_offset;
                }
            }
        }

        return (char *)fs_memory_regions[target_disk] + blocks[block_num] + (offset % BLOCK_SIZE);
    } else {
        // Step 4: handle RAID1/RAID1v (direct and indirect blocks)
        if (block_num > D_BLOCK) { // indirect block
            block_num -= IND_BLOCK;
            if (inode->blocks[IND_BLOCK] == 0) {
                inode->blocks[IND_BLOCK] = allocate_block(disk);
            }

            blocks = (off_t *)((char *)fs_memory_regions[disk] + inode->blocks[IND_BLOCK]);
        } else { // direct block
            blocks = inode->blocks;
        }

        // Step 5: allocate block 
        if (alloc && *(blocks + block_num) == 0) {
            *(blocks + block_num) = allocate_block(disk);
        }
        if (*(blocks + block_num) == 0) {
            return NULL;
        }

        return (char *)fs_memory_regions[disk] + blocks[block_num] + (offset % BLOCK_SIZE);
    }
}

int locate_inode(struct wfs_inode *enclosing, char *path, struct wfs_inode **inode, int disk)
{
    // base case: If the path is empty, return the current inode
    if (!strcmp(path, "")) {
        *inode = enclosing;
        return 0;
    }

    // extract the next component of the path
    char *next = path;
    while (*path != '/' && *path != '\0') {
        path++;
    }
    if (*path != '\0') {
        *path++ = '\0'; // null-terminate the current component
    }

    // search for the directory entry matching 'next' within 'enclosing'
    size_t sz = enclosing->size;
    struct wfs_dentry *dentries;
    int inum = -1; // inode number of the matching directory entry

    for (off_t off = 0; off < sz; off += sizeof(struct wfs_dentry)) {
        // retrieve the directory entry for the current offset
        dentries = (struct wfs_dentry *)get_block_location(enclosing, off, 0, disk);

        // check for a matching directory entry
        if (dentries->num != 0 && !strcmp(dentries->name, next)) {
            inum = dentries->num;
            break;
        }
    }
    if (inum < 0)
    {
        error_code = -ENOENT;
        return -1;
    }
    return locate_inode(get_inode(inum, disk), path, inode, disk);
}

int find_inode_by_path(char *path, struct wfs_inode **inode, int disk) {///ch
    struct wfs_inode *root = get_inode(0, disk);
    if (!root) {
        return -1;
    }
    return locate_inode(root, path + 1, inode, disk);
}

int insert_dir_entry(struct wfs_inode *parent_inode, int num, char *name, int disk)
{
    // insert dentry if there is an empty slot
    struct wfs_dentry *dentries;
    off_t offset = 0;

    while (offset < parent_inode->size)
    {
        dentries = (struct wfs_dentry *)get_block_location(parent_inode, offset, 0, disk);

        if (dentries->num == 0)
        {
            dentries->num = num;
            strncpy(dentries->name, name, MAX_NAME);
            if (raid_mode == RAID0)
            {
                for (int i = 0; i < total_devices; i++)
                {
                    struct wfs_inode *w = get_inode(parent_inode->num, i);
                    w->nlinks++;
                }
            }
            else
            {
                parent_inode->nlinks++;
            }
            return 0;
        }
        offset += sizeof(struct wfs_dentry);
    }

    // careful this will not work with indirect blocks for now
    // We will not do indirect blocks with directories
    dentries = (struct wfs_dentry *)get_block_location(parent_inode, parent_inode->size, 1, disk);
    if (!dentries)
    {
        printf("mknod error\n");
        return -1;
    }
    dentries->num = num;
    strncpy(dentries->name, name, MAX_NAME);
    if (raid_mode == RAID0)
    {
        for (int i = 0; i < total_devices; i++)
        {
            struct wfs_inode *w = get_inode(parent_inode->num, i);
            w->nlinks++;
            w->size += BLOCK_SIZE;
        }
    }
    else
    {
        parent_inode->nlinks++;
        parent_inode->size += BLOCK_SIZE;
    }

    return 0;
}

int delete_dir_entry(struct wfs_inode *inode, int inum, int disk)
{
    size_t sz = inode->size;
    struct wfs_dentry *dentries;

    for (off_t off = 0; off < sz; off += sizeof(struct wfs_dentry))
    {
        dentries = (struct wfs_dentry *)get_block_location(inode, off, 0, disk);

        if (dentries->num == inum)
        { // match
            dentries->num = 0;
            return 0;
        }
    }
    return -1; // not found
}

// Memory Management Helpers
void initialize_memory_mapping(int *fds, struct stat *file_stat) {///ch
    struct wfs_sb *first_sb = NULL;
    
    // Map disks and verify superblocks in same loop
    for (int i = 0; i < total_devices; i++) {
        fs_memory_regions[i] = mmap(NULL, file_stat->st_size, 
                                  PROT_READ | PROT_WRITE, 
                                  MAP_SHARED, fds[i], 0);
        if (fs_memory_regions[i] == MAP_FAILED) {
            perror("mmap");
            exit(EXIT_FAILURE);
        }
        
        struct wfs_sb *current_sb = (struct wfs_sb *)fs_memory_regions[i];
        device_orders[current_sb->device_order] = i;
        
        if (!first_sb) {
            first_sb = current_sb;
            raid_mode = first_sb->raid_mode;
        } else if (first_sb->fs_identifier != current_sb->fs_identifier || 
                  first_sb->raid_mode != current_sb->raid_mode ||
                  memcmp(first_sb, current_sb, 48)) {
            exit(EXIT_FAILURE);
        }
    }
}

void sort_device_order() {///ch
    // Create temporary buffer and copy pointers
    void *temp_regions[MAX_DISKS];
    for (int i = 0; i < total_devices; i++) {
        temp_regions[device_orders[i]] = fs_memory_regions[i];
    }

    // Copy back into original array
    memcpy(fs_memory_regions, temp_regions, total_devices * sizeof(void *));
}

int verify_root_inodes() {///ch
    struct wfs_inode *root_inode = NULL;
    
    for (int i = 0; i < total_devices; i++) {
        if (!(root_inode = get_inode(0, i))) {
            return -1;
        }
        if ((root_inode->mode & S_IFMT) != S_IFDIR) {
            return -1;
        }
    }
    return 0;
}

void cleanup_resources(int *fds, struct stat *file_stat, char **fuse_argv) {///ch
    // Clean up memory regions
    for (int i = 0; i < total_devices; i++) {
        if (fs_memory_regions[i] != NULL && fs_memory_regions[i] != MAP_FAILED) {
            munmap(fs_memory_regions[i], file_stat->st_size);
        }
        if (fds[i] >= 0) {
            close(fds[i]);
        }
    }
    // Free FUSE args if present
    if (fuse_argv) {
        free(fuse_argv);
    }
}

// FUSE Operations
int wfs_getattr(const char *path, struct stat *statbuf)
{
    struct wfs_inode *inode;
    char *path_copy = strdup(path);
    
    if (find_inode_by_path(path_copy, &inode, 0) < 0)
    {
        printf("Cannot get inode from path!\n");
        free(path_copy);
        return error_code;  // Changed from err_rc to error_code
    }

    // Fill in the stat buffer from the inode
    statbuf->st_mode = inode->mode;
    statbuf->st_uid = inode->uid;
    statbuf->st_gid = inode->gid;
    statbuf->st_size = inode->size;
    statbuf->st_atime = inode->atim;
    statbuf->st_mtime = inode->mtim;
    statbuf->st_ctime = inode->ctim;
    statbuf->st_nlink = inode->nlinks;

    free(path_copy);
    return 0;
}

int wfs_mknod(const char* path, mode_t mode, dev_t dev, int disk) {
    printf("START: wfs_mknod \n");

    struct wfs_inode* parent_inode = NULL;
    char *base = strdup(path);
    char *name = strdup(path);

    // Retrieve parent inode
    if (find_inode_by_path(dirname(base), &parent_inode, disk) < 0) {
        free(base);
        free(name);
        return -ENOENT;
    }

    // Allocate inode
    struct wfs_inode* inode = NULL;
    if (raid_mode == RAID0) {
        for (int i = 0; i < total_devices; i++) {
            inode = create_inode(i);
            if (!inode) {
                free(base);
                free(name);
                return -ENOSPC;
            }
            setup_inode(inode, S_IFREG | mode);
        }
    } else {
        inode = create_inode(disk);
        if (!inode) {
            free(base);
            free(name);
            return -ENOSPC;
        }
        setup_inode(inode, S_IFREG | mode);
    }

    // Add directory entry
    if (insert_dir_entry(parent_inode, inode->num, basename(name), disk) < 0) {
        free(base);
        free(name);
        return -EEXIST;
    }

    free(base);
    free(name);
    return 0;
}

int WFS_MKNOD(const char *path, mode_t mode, dev_t dev) {
    // handle RAID0 case (one disk only)
    if (raid_mode == RAID0) {
        int result = wfs_mknod(path, mode, dev, 0);
        if (result != 0) {
            return result;
        }
        return 0;
    }

    // handle other RAID modes (all disks)
    for (int i = 0; i < total_devices; i++) {
        int result = wfs_mknod(path, mode, dev, i);
        if (result != 0) {
            return result;
        }
    }
    return 0;
}

int wfs_mkdir(const char *path, mode_t mode, int disk) {
   printf("START: wfs_mkdir: \n");

   struct wfs_inode *parent_inode = NULL;
   char *base = strdup(path);
   char *name = strdup(path);

   // Step 1: find the parent directory inode
   if (find_inode_by_path(dirname(base), &parent_inode, disk) < 0) {
       free(base);
       free(name);
       return error_code;
   }

   // Step 2: allocate and initialize the new directory inode
   struct wfs_inode *inode = NULL;
   if (raid_mode == RAID0) {
       for (int i = 0; i < total_devices; i++) {
           inode = create_inode(i);
           if (inode == NULL) {
               free(base);
               free(name);
               return error_code;
           }
           setup_inode(inode, S_IFDIR | mode);
       }
   } else {
       // RAID1 or RAID1v: allocate inode on the specific disk
       inode = create_inode(disk);
       if (inode == NULL) {
           free(base);
           free(name);
           return error_code;
       }
       setup_inode(inode, S_IFDIR | mode);
   }

   // Step 3: add a directory entry to the parent directory
   if (insert_dir_entry(parent_inode, inode->num, basename(name), disk) < 0) {
       free(base);
       free(name);
       return error_code;
   }

   free(name);
   free(base);
   return 0;
}

int WFS_MKDIR(const char *path, mode_t mode) {
   // RAID0: create the directory only on disk 0
   if (raid_mode == RAID0) {
       if (wfs_mkdir(path, mode, 0) != 0) {
           return error_code;
       }
   } 
   else { // RAID1 or RAID1v: create the directory on all disks
       for (int i = 0; i < total_devices; i++) {
           if (wfs_mkdir(path, mode, i) != 0) {
               printf("Error: Failed to create directory '%s' on disk %d.\n", path, i);
               return error_code;
           }
       }
   }
   
   return 0;
}

static int prepare_read_operation(const char *path, size_t length) {
    raid_mgr.buffer_size = length;
    raid_mgr.operation_id++;
    
    switch(raid_mgr.raid_type) {
        case RAID0:
        case RAID1:
            raid_mgr.current_disk = 0;
            transition_state(READ_READY);
            break;
        case RAID1V:
            raid_mgr.current_disk = 0;
            transition_state(INIT);  // Need to check all disks
            break;
        default:
            transition_state(ERROR);
            return -1;
    }
    return 0;
}

// Main read function that replaces the previous three functions
int wfs_read(const char *path, char *buf, size_t length, off_t offset, struct fuse_file_info *fi) {
    int result = prepare_read_operation(path, length);
    if (result < 0) return result;

    // For RAID1V, we need checksums
    int checksums[MAX_DISKS];
    int max_checksum = -1;
    int best_disk = 0;

    while(1) {
        switch(raid_mgr.state) {
            case INIT: {
                // For RAID1V: Calculate checksums for all disks
                struct wfs_inode *inode;
                char *path_copy = strdup(path);
                
                if (find_inode_by_path(path_copy, &inode, raid_mgr.current_disk) < 0) {
                    free(path_copy);
                    raid_mgr.current_disk++;
                    if (raid_mgr.current_disk >= raid_mgr.total_disks) {
                        transition_state(READ_READY);
                    }
                    continue;
                }

                // Read and calculate checksum
                size_t bytes_read = 0;
                size_t position = offset;
                checksums[raid_mgr.current_disk] = 0;

                while (bytes_read < length && position < inode->size) {
                    size_t to_read = BLOCK_SIZE - (position % BLOCK_SIZE);
                    size_t remaining = inode->size - position;
                    if (to_read > remaining) to_read = remaining;

                    char *addr = get_block_location(inode, position, 0, raid_mgr.current_disk);
                    for (size_t i = 0; i < to_read; i++) {
                        checksums[raid_mgr.current_disk] += addr[i];
                    }

                    position += to_read;
                    bytes_read += to_read;
                }

                free(path_copy);

                // Check if this disk has highest checksum count
                int count = 1;
                for (int j = 0; j < raid_mgr.current_disk; j++) {
                    if (checksums[j] == checksums[raid_mgr.current_disk]) {
                        count++;
                    }
                }
                if (count > max_checksum) {
                    max_checksum = count;
                    best_disk = raid_mgr.current_disk;
                }

                raid_mgr.current_disk++;
                if (raid_mgr.current_disk >= raid_mgr.total_disks) {
                    raid_mgr.current_disk = best_disk;
                    transition_state(READ_READY);
                }
                break;
            }

            case READ_READY: {
                // Actual read operation
                struct wfs_inode *inode;
                char *path_copy = strdup(path);

                if (find_inode_by_path(path_copy, &inode, raid_mgr.current_disk) < 0) {
                    free(path_copy);
                    transition_state(ERROR);
                    return error_code;
                }

                size_t bytes_read = 0;
                size_t position = offset;

                while (bytes_read < length && position < inode->size) {
                    size_t to_read = BLOCK_SIZE - (position % BLOCK_SIZE);
                    size_t remaining = inode->size - position;
                    if (to_read > remaining) to_read = remaining;
                    if (to_read > length - bytes_read) to_read = length - bytes_read;

                    char *addr = get_block_location(inode, position, 0, raid_mgr.current_disk);
                    if (!addr) {
                        free(path_copy);
                        transition_state(ERROR);
                        return -EIO;
                    }

                    memcpy(buf + bytes_read, addr, to_read);
                    position += to_read;
                    bytes_read += to_read;
                }

                free(path_copy);
                transition_state(INIT);
                return bytes_read;
            }

            case ERROR:
                return error_code;

            default:
                transition_state(ERROR);
                return -EINVAL;
        }
    }
}

static int prepare_write_operation(const char *path, size_t length) {
    raid_mgr.buffer_size = length;
    raid_mgr.operation_id++;
    raid_mgr.current_disk = 0;

    if (raid_mgr.raid_type == RAID0) {
        transition_state(WRITE_READY);
    } else {
        transition_state(INIT);
    }
    return 0;
}

int wfs_write(const char *path, const char *buf, size_t length, off_t offset, struct fuse_file_info *fi) {
    int result = prepare_write_operation(path, length);
    if (result < 0) return result;

    ssize_t new_data_len = 0;
    size_t written_bytes = 0;
    size_t position = offset;
    char *path_copy = NULL;
    struct wfs_inode *inode = NULL;

    while(1) {
        switch(raid_mgr.state) {
            case INIT:
                // For non-RAID0, prepare each disk
                if (raid_mgr.current_disk >= raid_mgr.total_disks) {
                    transition_state(WRITE_READY);
                    raid_mgr.current_disk = 0;
                    continue;
                }
                transition_state(WRITE_READY);
                break;

            case WRITE_READY:
                path_copy = strdup(path);
                if (find_inode_by_path(path_copy, &inode, raid_mgr.current_disk) < 0) {
                    free(path_copy);
                    return error_code;
                }

                // Calculate additional data length needed
                new_data_len = length - (inode->size - offset);
                written_bytes = 0;
                position = offset;
                transition_state(READ_READY);  // Move to actual write state
                break;

            case READ_READY:  // Using READ_READY as write state to match with read operation
                while (written_bytes < length) {
                    size_t to_write = BLOCK_SIZE - (position % BLOCK_SIZE);
                    if (to_write + written_bytes > length) {
                        to_write = length - written_bytes;
                    }

                    char *addr = get_block_location(inode, position, 1, raid_mgr.current_disk);
                    if (!addr) {
                        free(path_copy);
                        return error_code;
                    }

                    memcpy(addr, buf + written_bytes, to_write);
                    position += to_write;
                    written_bytes += to_write;
                }

                // Update file size if needed
                if (new_data_len > 0) {
                    inode->size += new_data_len;
                    
                    // For RAID0, update size on all disks
                    if (raid_mgr.raid_type == RAID0) {
                        for (int i = 0; i < raid_mgr.total_disks; i++) {
                            struct wfs_inode *disk_inode = get_inode(inode->num, i);
                            disk_inode->size = inode->size;
                        }
                    }
                }

                free(path_copy);
                path_copy = NULL;

                if (raid_mgr.raid_type == RAID0 || 
                    raid_mgr.current_disk == raid_mgr.total_disks - 1) {
                    transition_state(INIT);
                    return written_bytes;
                }

                raid_mgr.current_disk++;
                transition_state(INIT);
                break;

            case ERROR:
                if (path_copy) free(path_copy);
                return error_code;

            default:
                if (path_copy) free(path_copy);
                transition_state(ERROR);
                return -EINVAL;
        }
    }
}

int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    // Add the standard directory entries
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    struct wfs_inode *inode;
    char *path_copy = strdup(path);

    // Find the directory's inode
    if (find_inode_by_path(path_copy, &inode, 0) < 0) {
        free(path_copy);
        return error_code;
    }

    // Read through all directory entries
    size_t dir_size = inode->size;
    struct wfs_dentry *dir_entry;

    for (off_t offset = 0; offset < dir_size; offset += sizeof(struct wfs_dentry)) {
        // Get pointer to current directory entry
        dir_entry = (struct wfs_dentry *)get_block_location(inode, offset, 0, 0);
        
        // Add valid entries to the buffer
        if (dir_entry->num != 0) {
            filler(buf, dir_entry->name, NULL, 0);
        }
    }

    free(path_copy);
    return 0;
}

// Helper function to prepare delete operation
static int prepare_delete_operation(const char *path) {
    raid_mgr.operation_id++;
    raid_mgr.current_disk = 0;

    if (raid_mgr.raid_type == RAID0) {
        transition_state(DELETE_READY);
    } else {
        transition_state(INIT);
    }
    return 0;
}

int wfs_unlink(const char *path) {
    int result = prepare_delete_operation(path);
    if (result < 0) return result;

    struct wfs_inode *parent_inode = NULL;
    struct wfs_inode *inode = NULL;
    char *base = NULL;
    char *path_copy = NULL;
    off_t *blocks = NULL;

    while(1) {
        switch(raid_mgr.state) {
            case INIT:
                if (raid_mgr.current_disk >= raid_mgr.total_disks) {
                    transition_state(DELETE_READY);
                    raid_mgr.current_disk = 0;
                    continue;
                }
                transition_state(DELETE_READY);
                break;

            case DELETE_READY:
                base = strdup(path);
                path_copy = strdup(path);

                // Find parent and target inodes
                if (find_inode_by_path(dirname(base), &parent_inode, raid_mgr.current_disk) < 0 ||
                    find_inode_by_path(path_copy, &inode, raid_mgr.current_disk) < 0) {
                    result = error_code;
                    goto cleanup;
                }

                // Free blocks
                if (inode->blocks[IND_BLOCK] != 0) {
                    blocks = (off_t *)(((char *)fs_memory_regions[raid_mgr.current_disk] + 
                             inode->blocks[IND_BLOCK]));
                    for (int i = 0; i < BLOCK_SIZE / sizeof(off_t); i++) {
                        if (blocks[i] != 0) {
                            release_block(blocks[i], raid_mgr.current_disk);
                        }
                    }
                }

                blocks = inode->blocks;
                for (int i = 0; i < N_BLOCKS; i++) {
                    if (blocks[i] != 0) {
                        if (raid_mgr.raid_type == RAID0) {
                            release_block(blocks[i], i % raid_mgr.total_disks);
                        } else {
                            release_block(blocks[i], raid_mgr.current_disk);
                        }
                    }
                }

                // Remove directory entry and free inode
                if (delete_dir_entry(parent_inode, inode->num, raid_mgr.current_disk) < 0) {
                    result = error_code;
                    goto cleanup;
                }

                if (raid_mgr.raid_type == RAID0) {
                    int inum = inode->num;
                    for (int i = 0; i < raid_mgr.total_disks; i++) {
                        struct wfs_inode *w = get_inode(inum, i);
                        release_inode(w, i);
                    }
                } else {
                    release_inode(inode, raid_mgr.current_disk);
                }

                free(base);
                free(path_copy);
                base = path_copy = NULL;

                if (raid_mgr.raid_type == RAID0 || 
                    raid_mgr.current_disk == raid_mgr.total_disks - 1) {
                    transition_state(INIT);
                    return 0;
                }

                raid_mgr.current_disk++;
                transition_state(INIT);
                break;

            case ERROR:
                result = error_code;
                goto cleanup;

            default:
                result = -EINVAL;
                goto cleanup;
        }
        continue;

cleanup:
        free(base);
        free(path_copy);
        return result;
    }
}

int wfs_rmdir(const char *path)
{
    printf("START: wfs_rmdir \n");
    return wfs_unlink(path);  
}

static struct fuse_operations wfs_oper = {
    .getattr = wfs_getattr,
    .mknod = WFS_MKNOD,    // Changed from wfs_mknod to WFS_MKNOD
    .mkdir = WFS_MKDIR,     // Changed from wfs_mkdir to WFS_MKDIR 
    .unlink = wfs_unlink,    
    .rmdir = wfs_rmdir,
    .read = wfs_read,
    .write = wfs_write,
    .readdir = wfs_readdir,
};

// Helper function for disk count validation
static int validate_disk_count(int argc, char *argv[]) {///ch
    int count = 0;
    while (count < argc - 1 && argv[count + 1][0] != '-') {
        count++;
    }
    if (count == 0) {
        exit(EXIT_FAILURE);
    }
    if (count < 2) {
        exit(EXIT_FAILURE);
    }
    return count;
}

// Helper function to open disk images
static void open_disk_images(int *fds, struct stat *file_stat, char *argv[]) {///ch
    for (int i = 0; i < total_devices; i++) {
        fds[i] = open(argv[i + 1], O_RDWR, 0666);
        if (fds[i] == -1) {
            perror("Failed to open disk image");
            exit(EXIT_FAILURE);
        }
        if (fstat(fds[i], file_stat) == -1) {
            perror("stat");
            exit(EXIT_FAILURE);
        }
    }
}

// Helper function to prepare FUSE arguments
static char** prepare_fuse_args(int argc, char *argv[], int *fuse_argc) {///ch
    char **fuse_argv = malloc((argc - total_devices) * sizeof(char *));
    if (!fuse_argv) {
        perror("Failed to allocate memory for FUSE arguments");
        exit(EXIT_FAILURE);
    }
    
    fuse_argv[0] = argv[0];
    for (int i = total_devices + 1; i < argc; i++) {
        fuse_argv[i - total_devices] = argv[i];
    }
    *fuse_argc = argc - total_devices;
    
    return fuse_argv;
}

int main(int argc, char *argv[]) {///ch
    // Initialize filesystem
    total_devices = validate_disk_count(argc, argv);
    
    // Set up disk access
    int fds[MAX_DISKS];
    struct stat file_stat;
    open_disk_images(fds, &file_stat, argv);
    
    // Initialize filesystem structures
    initialize_memory_mapping(fds, &file_stat);
    init_raid_manager();
    sort_device_order();
    
    // Verify filesystem state
    if (verify_root_inodes() != 0) {
        cleanup_resources(fds, &file_stat, NULL);
        return EXIT_FAILURE;
    }
    
    // Set up and run FUSE
    int fuse_argc;
    char **fuse_argv = prepare_fuse_args(argc, argv, &fuse_argc);
    int fuse_ret = fuse_main(fuse_argc, fuse_argv, &wfs_oper, NULL);
    
    if (fuse_ret != 0) {
        printf("FUSE mount failed with return code %d\n", fuse_ret);
    }
    
    cleanup_resources(fds, &file_stat, fuse_argv);
    return fuse_ret;
}