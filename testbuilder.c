#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>

#define BLOCK_SIZE 4096
#define INODE_SIZE 128
#define MAGIC_NUMBER 0x4D565346

// Structure definitions
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t inode_count;
    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;
    uint64_t data_bitmap_start;
    uint64_t data_bitmap_blocks;
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t data_region_start;
    uint64_t data_region_blocks;
    uint64_t root_inode;
    uint64_t mtime_epoch;
    uint32_t flags;
    uint32_t checksum;
} superblock_t;

typedef struct {
    uint16_t mode;
    uint16_t links;
    uint32_t uid;
    uint32_t gid;
    uint64_t size_bytes;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t direct[12];
    uint32_t reserved_0;
    uint32_t reserved_1;
    uint32_t reserved_2;
    uint32_t proj_id;
    uint32_t uid16_gid16;
    uint64_t xattr_ptr;
    uint64_t inode_crc;
} inode_t;

typedef struct {
    uint32_t inode_no;
    uint8_t type;
    char name[58];
    uint8_t checksum;
} dirent64_t;

// Checksum functions (placeholders - you'll need actual implementations)
void superblock_crc_finalize(superblock_t* sb) {
    // Calculate CRC32 of superblock excluding checksum field
    // This is a placeholder - implement actual CRC32 calculation
    sb->checksum = 0xDEADBEEF;
}

void inode_crc_finalize(inode_t* inode) {
    // Calculate CRC64 of inode excluding inode_crc field
    // This is a placeholder - implement actual CRC64 calculation
    inode->inode_crc = 0xDEADBEEFDEADBEEF;
}

void dirent_checksum_finalize(dirent64_t* dirent) {
    // Calculate checksum of directory entry
    // This is a placeholder - implement actual checksum calculation
    dirent->checksum = 0xAB;
}

// Function to parse command line arguments
int parse_arguments(int argc, char* argv[], char** image_name, int* size_kib, int* inodes) {
    int opt;
    static struct option long_options[] = {
        {"image", required_argument, 0, 'i'},
        {"size-kib", required_argument, 0, 's'},
        {"inodes", required_argument, 0, 'n'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "i:s:n:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'i':
                *image_name = optarg;
                break;
            case 's':
                *size_kib = atoi(optarg);
                if (*size_kib < 180 || *size_kib > 4096 || *size_kib % 4 != 0) {
                    fprintf(stderr, "Error: size-kib must be between 180-4096 and multiple of 4\n");
                    return -1;
                }
                break;
            case 'n':
                *inodes = atoi(optarg);
                if (*inodes < 128 || *inodes > 512) {
                    fprintf(stderr, "Error: inodes must be between 128-512\n");
                    return -1;
                }
                break;
            default:
                fprintf(stderr, "Usage: %s --image <file> --size-kib <180-4096> --inodes <128-512>\n", argv[0]);
                return -1;
        }
    }

    if (!*image_name || *size_kib == 0 || *inodes == 0) {
        fprintf(stderr, "Error: All arguments are required\n");
        return -1;
    }

    return 0;
}

// Function to create and initialize superblock
void create_superblock(superblock_t* sb, int size_kib, int inodes) {
    time_t now = time(NULL);
    uint64_t total_blocks = (size_kib * 1024) / BLOCK_SIZE;
    uint64_t inode_table_blocks = (inodes * INODE_SIZE + BLOCK_SIZE - 1) / BLOCK_SIZE;

    memset(sb, 0, sizeof(superblock_t));
    
    sb->magic = MAGIC_NUMBER;
    sb->version = 1;
    sb->block_size = BLOCK_SIZE;
    sb->total_blocks = total_blocks;
    sb->inode_count = inodes;
    sb->inode_bitmap_start = 1;
    sb->inode_bitmap_blocks = 1;
    sb->data_bitmap_start = 2;
    sb->data_bitmap_blocks = 1;
    sb->inode_table_start = 3;
    sb->inode_table_blocks = inode_table_blocks;
    sb->data_region_start = 3 + inode_table_blocks;
    sb->data_region_blocks = total_blocks - sb->data_region_start;
    sb->root_inode = 1;
    sb->mtime_epoch = now;
    sb->flags = 0;
    
    superblock_crc_finalize(sb);
}

// Function to create root directory inode
void create_root_inode(inode_t* root_inode) {
    time_t now = time(NULL);
    
    memset(root_inode, 0, sizeof(inode_t));
    
    root_inode->mode = 0040000;  // Directory mode (octal 040000)
    root_inode->links = 2;       // . and ..
    root_inode->uid = 0;
    root_inode->gid = 0;
    root_inode->size_bytes = BLOCK_SIZE;  // One block for directory entries
    root_inode->atime = now;
    root_inode->mtime = now;
    root_inode->ctime = now;
    root_inode->direct[0] = 0;   // First data block (relative to data region)
    for (int i = 1; i < 12; i++) {
        root_inode->direct[i] = 0;  // Unused blocks
    }
    root_inode->proj_id = 0;     // Set to your group ID
    
    inode_crc_finalize(root_inode);
}

// Function to create root directory entries
void create_root_directory_entries(dirent64_t* entries) {
    // Create "." entry
    memset(&entries[0], 0, sizeof(dirent64_t));
    entries[0].inode_no = 1;
    entries[0].type = 2;  // Directory
    strcpy(entries[0].name, ".");
    dirent_checksum_finalize(&entries[0]);
    
    // Create ".." entry
    memset(&entries[1], 0, sizeof(dirent64_t));
    entries[1].inode_no = 1;
    entries[1].type = 2;  // Directory
    strcpy(entries[1].name, "..");
    dirent_checksum_finalize(&entries[1]);
    
    // Initialize remaining entries as free
    for (int i = 2; i < BLOCK_SIZE / sizeof(dirent64_t); i++) {
        memset(&entries[i], 0, sizeof(dirent64_t));
        entries[i].inode_no = 0;  // Free entry
    }
}

int main(int argc, char* argv[]) {
    char* image_name = NULL;
    int size_kib = 0;
    int inodes = 0;

    // Parse command line arguments
    if (parse_arguments(argc, argv, &image_name, &size_kib, &inodes) != 0) {
        return 1;
    }

    // Calculate filesystem parameters
    uint64_t total_blocks = (size_kib * 1024) / BLOCK_SIZE;
    uint64_t inode_table_blocks = (inodes * INODE_SIZE + BLOCK_SIZE - 1) / BLOCK_SIZE;

    // Create output file
    FILE* img_file = fopen(image_name, "wb");
    if (!img_file) {
        perror("Error opening output file");
        return 1;
    }

    // Create and write superblock
    superblock_t superblock;
    create_superblock(&superblock, size_kib, inodes);
    
    if (fwrite(&superblock, sizeof(superblock_t), 1, img_file) != 1) {
        fprintf(stderr, "Error writing superblock\n");
        fclose(img_file);
        return 1;
    }
    
    // Pad superblock to full block
    char padding[BLOCK_SIZE - sizeof(superblock_t)] = {0};
    fwrite(padding, 1, BLOCK_SIZE - sizeof(superblock_t), img_file);

    // Create and write inode bitmap
    char inode_bitmap[BLOCK_SIZE] = {0};
    inode_bitmap[0] = 0x01;  // Mark inode 1 (root) as allocated
    fwrite(inode_bitmap, 1, BLOCK_SIZE, img_file);

    // Create and write data bitmap
    char data_bitmap[BLOCK_SIZE] = {0};
    data_bitmap[0] = 0x01;  // Mark first data block as allocated for root directory
    fwrite(data_bitmap, 1, BLOCK_SIZE, img_file);

    // Create and write inode table
    for (uint64_t i = 0; i < inode_table_blocks; i++) {
        char inode_block[BLOCK_SIZE] = {0};
        
        if (i == 0) {
            // First inode block contains root directory inode
            inode_t root_inode;
            create_root_inode(&root_inode);
            memcpy(inode_block, &root_inode, sizeof(inode_t));
        }
        
        fwrite(inode_block, 1, BLOCK_SIZE, img_file);
    }

    // Create and write data blocks
    uint64_t data_blocks = total_blocks - superblock.data_region_start;
    for (uint64_t i = 0; i < data_blocks; i++) {
        char data_block[BLOCK_SIZE] = {0};
        
        if (i == 0) {
            // First data block contains root directory entries
            dirent64_t root_entries[BLOCK_SIZE / sizeof(dirent64_t)];
            create_root_directory_entries(root_entries);
            memcpy(data_block, root_entries, BLOCK_SIZE);
        }
        
        fwrite(data_block, 1, BLOCK_SIZE, img_file);
    }

    fclose(img_file);
    printf("File system image '%s' created successfully\n", image_name);
    return 0;
}