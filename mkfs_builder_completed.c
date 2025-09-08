// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_builder.c -o mkfs_builder
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>

#define BS 4096u               // block size
#define INODE_SIZE 128u
#define ROOT_INO 1u

uint64_t g_random_seed = 0; // This should be replaced by seed value from the CLI.

// below contains some basic structures you need for your project
// you are free to create more structures as you require

#pragma pack(push, 1)
typedef struct {
    // CREATE YOUR SUPERBLOCK HERE
    // ADD ALL FIELDS AS PROVIDED BY THE SPECIFICATION
    
    // uint32_t is an unsigned 32bit (4 byte) integer
    // uint64_t is an unsigned 64bit (8 byte) integer

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

    // THIS FIELD SHOULD STAY AT THE END
    // ALL OTHER FIELDS SHOULD BE ABOVE THIS
    uint32_t checksum;            // crc32(superblock[0..4091])
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");

#pragma pack(push,1)
typedef struct {
    // CREATE YOUR INODE HERE
    // IF CREATED CORRECTLY, THE STATIC_ASSERT ERROR SHOULD BE GONE

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


    // THIS FIELD SHOULD STAY AT THE END
    // ALL OTHER FIELDS SHOULD BE ABOVE THIS
    uint64_t inode_crc;   // low 4 bytes store crc32 of bytes [0..119]; high 4 bytes 0

} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    // CREATE YOUR DIRECTORY ENTRY STRUCTURE HERE
    // IF CREATED CORRECTLY, THE STATIC_ASSERT ERROR SHOULD BE GONE

    uint32_t inode_no;
    uint8_t type;
    char name[58];

    uint8_t  checksum; // XOR of bytes 0..62
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t)==64, "dirent size mismatch");


// ==========================DO NOT CHANGE THIS PORTION=========================
// These functions are there for your help. You should refer to the specifications to see how you can use them.
// ====================================CRC32====================================
uint32_t CRC32_TAB[256];
void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c = (c&1)?(0xEDB88320u^(c>>1)):(c>>1);
        CRC32_TAB[i]=c;
    }
}
uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}
// ====================================CRC32====================================

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
static uint32_t superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    uint32_t s = crc32((void *) sb, BS - 4);
    sb->checksum = s;
    return s;
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp, ino, INODE_SIZE);
    // zero crc area before computing
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c; // low 4 bytes carry the crc
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];   // covers ino(4) + type(1) + name(58)
    de->checksum = x;
}

int main(int argc, char* argv[]) {
    crc32_init();
    

    if (argc != 7) {
        fprintf(stderr, "Usage: %s --image <image_file> --size-kib <180-4096> --inodes <128-512>\n", argv[0]);
        return 1;
    }

    char *image_file = NULL;
    int size_kib = 0;
    int inodes = 0;

    for (int i = 1; i < argc; i += 2) {
        if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) {
            image_file = argv[i + 1];
        } else if (strcmp(argv[i], "--size-kib") == 0 && i + 1 < argc) {
            size_kib = atoi(argv[i + 1]);
        } else if (strcmp(argv[i], "--inodes") == 0 && i + 1 < argc) {
            inodes = atoi(argv[i + 1]);
        } else {
            fprintf(stderr, "Invalid arguments\n");
            return 1;
        }
    }


    if (!image_file) {
        fprintf(stderr, "Error: image file not specified\n");
        return 1;
    }
    
    if (size_kib < 180 || size_kib > 4096) {
        fprintf(stderr, "Error: size must be between 180 and 4096\n");
        return 1;
    }
    
    if (inodes < 128 || inodes > 512) {
        fprintf(stderr, "Error: inodes must be between 128 and 512\n");
        return 1;
    }
    
    if (size_kib % 4 != 0) {
        fprintf(stderr, "Error: size must be a multiple of 4\n");
        return 1;
    }

    uint64_t total_blocks = (size_kib * 1024) / BS;
    uint64_t inode_bitmap_start = 1;  // Block 1
    uint64_t inode_bitmap_blocks = 1;
    uint64_t data_bitmap_start = 2;   // Block 2
    uint64_t data_bitmap_blocks = 1;
    uint64_t inode_table_start = 3;   // Block 3
    uint64_t inode_table_blocks = (inodes * INODE_SIZE + BS - 1) / BS;  // Round up
    uint64_t data_region_start = inode_table_start + inode_table_blocks;
    uint64_t data_region_blocks = total_blocks - data_region_start;

    
    if (data_region_start >= total_blocks) {
        fprintf(stderr, "Error: insufficient space for filesystem layout\n");
        return 1;
    }

    // Open output file
    FILE *fp = fopen(image_file, "wb");
    if (!fp) {
        fprintf(stderr, "Failed to open image file: %s\n", image_file);
        return 1;
    }

    time_t now = time(NULL);

    // Initialize superblock
    superblock_t superblock = {0};
    superblock.magic = 0x4D565346;
    superblock.version = 1;
    superblock.block_size = BS;
    superblock.total_blocks = total_blocks;
    superblock.inode_count = inodes;
    superblock.inode_bitmap_start = inode_bitmap_start;
    superblock.inode_bitmap_blocks = inode_bitmap_blocks;
    superblock.data_bitmap_start = data_bitmap_start;
    superblock.data_bitmap_blocks = data_bitmap_blocks;
    superblock.inode_table_start = inode_table_start;
    superblock.inode_table_blocks = inode_table_blocks;
    superblock.data_region_start = data_region_start;
    superblock.data_region_blocks = data_region_blocks;
    superblock.root_inode = ROOT_INO;
    superblock.mtime_epoch = now;
    superblock.flags = 0;

    superblock_crc_finalize(&superblock);


    if (fwrite(&superblock, sizeof(superblock_t), 1, fp) != 1) {
        fprintf(stderr, "Error: failed to write superblock\n");
        fclose(fp);
        return 1;
    }
    
    // Padding
    uint8_t padding[BS - sizeof(superblock_t)] = {0};
    if (fwrite(padding, sizeof(padding), 1, fp) != 1) {
        fprintf(stderr, "Error: failed to pad superblock\n");
        fclose(fp);
        return 1;
    }

    // Write inode bitmap (block 1)
    uint8_t inode_bitmap[BS] = {0};
    inode_bitmap[0] = 0x01;  // Mark inode 1 (root) as allocated
    if (fwrite(inode_bitmap, BS, 1, fp) != 1) {
        fprintf(stderr, "Error: failed to write inode bitmap\n");
        fclose(fp);
        return 1;
    }

    // Writing Data bitmap
    uint8_t data_bitmap[BS] = {0};
    data_bitmap[0] = 0x01;  // Mark first data block as allocated for root directory
    if (fwrite(data_bitmap, BS, 1, fp) != 1) {
        fprintf(stderr, "Error: failed to write data bitmap\n");
        fclose(fp);
        return 1;
    }

    // Write inode table
    for (uint64_t block = 0; block < inode_table_blocks; block++) {
        uint8_t inode_block[BS] = {0};
        
        if (block == 0) { 
            inode_t root_inode = {0};
            root_inode.mode = 040755;  
            root_inode.links = 2; 
            root_inode.uid = 0;
            root_inode.gid = 0;
            root_inode.size_bytes = 2 * sizeof(dirent64_t);  
            root_inode.atime = now;
            root_inode.mtime = now;
            root_inode.ctime = now;
            root_inode.direct[0] = data_region_start;  // First data block
            for (int i = 1; i < 12; i++) {
                root_inode.direct[i] = 0; 
            }
            root_inode.reserved_0 = 0;
            root_inode.reserved_1 = 0;
            root_inode.reserved_2 = 0;
            root_inode.proj_id = 2; 
            root_inode.uid16_gid16 = 0;
            root_inode.xattr_ptr = 0;
            
            inode_crc_finalize(&root_inode);
            
            // Copy root inode to the block (at position 0 for inode 1)
            memcpy(inode_block, &root_inode, sizeof(inode_t));
        }
        
        if (fwrite(inode_block, BS, 1, fp) != 1) {
            fprintf(stderr, "Error: failed to write inode table block %"PRIu64"\n", block);
            fclose(fp);
            return 1;
        }
    }

    // Writing root directory data block
    uint8_t root_dir_block[BS] = {0};
    
    // Creating "." entry
    dirent64_t dot_entry = {0};
    dot_entry.inode_no = ROOT_INO;
    dot_entry.type = 2;  
    strcpy(dot_entry.name, ".");
    dirent_checksum_finalize(&dot_entry);
    
    // Creating ".." entry
    dirent64_t dotdot_entry = {0};
    dotdot_entry.inode_no = ROOT_INO;
    dotdot_entry.type = 2;  
    strcpy(dotdot_entry.name, "..");
    dirent_checksum_finalize(&dotdot_entry);
    
    // Copying entries to root directory block
    memcpy(root_dir_block, &dot_entry, sizeof(dirent64_t));
    memcpy(root_dir_block + sizeof(dirent64_t), &dotdot_entry, sizeof(dirent64_t));
    
    if (fwrite(root_dir_block, BS, 1, fp) != 1) {
        fprintf(stderr, "Error: failed to write root directory\n");
        fclose(fp);
        return 1;
    }

    // Writing remaining data blocks (all zeros)
    uint8_t zero_block[BS] = {0};
    for (uint64_t i = 1; i < data_region_blocks; i++) {
        if (fwrite(zero_block, BS, 1, fp) != 1) {
            fprintf(stderr, "Error: failed to write data block %"PRIu64"\n", i);
            fclose(fp);
            return 1;
        }
    }

    fclose(fp);
    
    printf("Successfully created MiniVSFS image: %s\n", image_file);
    printf("Total blocks: %" PRIu64 "\n", total_blocks);
    printf("Inodes: %d\n", inodes);
    printf("Inode table blocks: %" PRIu64 "\n", inode_table_blocks);
    printf("Data region starts at block: %" PRIu64 "\n", data_region_start);
    printf("Data region blocks: %" PRIu64 "\n", data_region_blocks);
    
    return 0;
}