#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12

#pragma pack(push, 1)
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
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");

#pragma pack(push,1)
typedef struct {
    uint16_t mode;                
    uint16_t links;                
    uint32_t uid;                  
    uint32_t gid;                
    uint64_t size_bytes;           
    uint64_t atime;                
    uint64_t mtime;                
    uint64_t ctime;                
    uint32_t direct[DIRECT_MAX];   
    uint32_t reserved_0;          
    uint32_t reserved_1;          
    uint32_t reserved_2;         
    uint32_t proj_id;           
    uint32_t uid16_gid16;          
    uint64_t xattr_ptr;            
    uint64_t inode_crc;            
} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;             
    uint8_t type;                  
    char name[58];                
    uint8_t checksum;              
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
    uint8_t tmp[INODE_SIZE]; 
    memcpy(tmp, ino, INODE_SIZE);
    // zero crc area before computing
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c; // low 4 bytes carry the crc
}
// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i]; // covers ino(4) + type(1) + name(58)
    de->checksum = x;
}

// Helper function to read a block from the image
int read_block(FILE *fp, uint64_t block_num, void *buffer) {
    if (fseek(fp, block_num * BS, SEEK_SET) != 0) {
        return -1;
    }
    if (fread(buffer, BS, 1, fp) != 1) {
        return -1;
    }
    return 0;
}

// Helper function to write a block to the image
int write_block(FILE *fp, uint64_t block_num, const void *buffer) {
    if (fseek(fp, block_num * BS, SEEK_SET) != 0) {
        return -1;
    }
    if (fwrite(buffer, BS, 1, fp) != 1) {
        return -1;
    }
    return 0;
}

// Find the first free inode
int find_free_inode(FILE *fp, const superblock_t *sb) {
    uint8_t bitmap[BS];
    if (read_block(fp, sb->inode_bitmap_start, bitmap) != 0) {
        return -1;
    }
    
    for (uint64_t i = 0; i < sb->inode_count; i++) {
        uint64_t byte_idx = i / 8;
        uint64_t bit_idx = i % 8;
        if (!(bitmap[byte_idx] & (1 << bit_idx))) {
            return i + 1; // Return 1-indexed inode number
        }
    }
    return -1; 
}

// Find the first free data block
int find_free_data_block(FILE *fp, const superblock_t *sb) {
    uint8_t bitmap[BS];
    if (read_block(fp, sb->data_bitmap_start, bitmap) != 0) {
        return -1;
    }
    
    for (uint64_t i = 0; i < sb->data_region_blocks; i++) {
        uint64_t byte_idx = i / 8;
        uint64_t bit_idx = i % 8;
        if (!(bitmap[byte_idx] & (1 << bit_idx))) {
            return sb->data_region_start + i; // Return actual block number
        }
    }
    return -1; 
}

// Set bit in bitmap
void set_bit(uint8_t *bitmap, uint64_t bit_num) {
    uint64_t byte_idx = bit_num / 8;
    uint64_t bit_idx = bit_num % 8;
    bitmap[byte_idx] |= (1 << bit_idx);
}

// Check if file already exists in root directory
int file_exists(FILE *fp, const superblock_t *sb, const char *filename) {
    // Read root inode
    inode_t root_inode;
    uint64_t root_inode_offset = (sb->inode_table_start * BS) + ((ROOT_INO - 1) * INODE_SIZE);
    if (fseek(fp, root_inode_offset, SEEK_SET) != 0) {
        return -1;
    }
    if (fread(&root_inode, sizeof(inode_t), 1, fp) != 1) {
        return -1;
    }
    
    // Check all data blocks of root directory
    for (int i = 0; i < DIRECT_MAX && root_inode.direct[i] != 0; i++) {
        uint8_t block_data[BS];
        if (read_block(fp, root_inode.direct[i], block_data) != 0) {
            return -1;
        }
        
        // Check each directory entry in this block
        for (int j = 0; j < BS / sizeof(dirent64_t); j++) {
            dirent64_t *entry = (dirent64_t *)(block_data + j * sizeof(dirent64_t));
            if (entry->inode_no != 0 && strcmp(entry->name, filename) == 0) {
                return 1; // File exists
            }
        }
    }
    return 0; 
}

int main(int argc, char *argv[]) {
    crc32_init();
    

    char *input_file = NULL;
    char *output_file = NULL;
    char *file_to_add = NULL;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            input_file = argv[++i];
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_file = argv[++i];
        } else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            file_to_add = argv[++i];
        }
    }
    
    if (!input_file || !output_file || !file_to_add) {
        fprintf(stderr, "Usage: %s --input <input.img> --output <output.img> --file <filename>\n", argv[0]);
        return 1;
    }
    
 
    struct stat file_stat;
    if (stat(file_to_add, &file_stat) != 0) {
        fprintf(stderr, "Error: File '%s' not found in current directory\n", file_to_add);
        return 1;
    }
    
    if (!S_ISREG(file_stat.st_mode)) {
        fprintf(stderr, "Error: '%s' is not a regular file\n", file_to_add);
        return 1;
    }
    
    // Check if input file has .img extension
    const char *ext = strrchr(input_file, '.');
    if (!ext || strcmp(ext, ".img") != 0) {
        fprintf(stderr, "Error: Input file must have .img extension\n");
        return 1;
    }


    FILE *input_fp = fopen(input_file, "rb");
    if (!input_fp) {
        fprintf(stderr, "Error: Cannot open input file '%s': %s\n", input_file, strerror(errno));
        return 1;
    }
    
    // Read superblock
    superblock_t sb;
    if (fread(&sb, sizeof(superblock_t), 1, input_fp) != 1) {
        fprintf(stderr, "Error: Cannot read superblock\n");
        fclose(input_fp);
        return 1;
    }
    
    if (sb.magic != 0x4D565346) {
        fprintf(stderr, "Error: Invalid file system magic number\n");
        fclose(input_fp);
        return 1;
    }
    
    // Check if file already exists
    int exists = file_exists(input_fp, &sb, file_to_add);
    if (exists == -1) {
        fprintf(stderr, "Error: Cannot check if file exists\n");
        fclose(input_fp);
        return 1;
    }
    if (exists == 1) {
        fprintf(stderr, "Error: File '%s' already exists in the file system\n", file_to_add);
        fclose(input_fp);
        return 1;
    }
    
    // Copy input to output
    fclose(input_fp);
    
    char copy_cmd[1024];
    snprintf(copy_cmd, sizeof(copy_cmd), "cp '%s' '%s'", input_file, output_file);
    if (system(copy_cmd) != 0) {
        fprintf(stderr, "Error: Cannot copy input file to output file\n");
        return 1;
    }
    
 
    FILE *output_fp = fopen(output_file, "r+b");
    if (!output_fp) {
        fprintf(stderr, "Error: Cannot open output file '%s': %s\n", output_file, strerror(errno));
        return 1;
    }
    
 
    if (fread(&sb, sizeof(superblock_t), 1, output_fp) != 1) {
        fprintf(stderr, "Error: Cannot read superblock from output file\n");
        fclose(output_fp);
        return 1;
    }
    

    uint64_t blocks_needed = (file_stat.st_size + BS - 1) / BS;
    if (blocks_needed > DIRECT_MAX) {
        fprintf(stderr, "Error: 12 blocks exceeded\n");
        fclose(output_fp);
        return 1;
    }
    
    // Find free inode
    int new_inode_num = find_free_inode(output_fp, &sb);
    if (new_inode_num == -1) {
        fprintf(stderr, "Error: No free inodes available\n");
        fclose(output_fp);
        return 1;
    }
    
    // Find free data blocks for the file
    uint32_t file_blocks[DIRECT_MAX] = {0};
    for (uint64_t i = 0; i < blocks_needed; i++) {
        int block_num = find_free_data_block(output_fp, &sb);
        if (block_num == -1) {
            fprintf(stderr, "Error: No free data blocks available\n");
            fclose(output_fp);
            return 1;
        }
        file_blocks[i] = block_num;
    }
    
    // Update bitmaps
    uint8_t inode_bitmap[BS];
    uint8_t data_bitmap[BS];
    
    // Read and update inode bitmap
    if (read_block(output_fp, sb.inode_bitmap_start, inode_bitmap) != 0) {
        fprintf(stderr, "Error: Cannot read inode bitmap\n");
        fclose(output_fp);
        return 1;
    }
    set_bit(inode_bitmap, new_inode_num - 1); // Convert to 0-indexed
    if (write_block(output_fp, sb.inode_bitmap_start, inode_bitmap) != 0) {
        fprintf(stderr, "Error: Cannot write inode bitmap\n");
        fclose(output_fp);
        return 1;
    }
    
    // Read and update data bitmap
    if (read_block(output_fp, sb.data_bitmap_start, data_bitmap) != 0) {
        fprintf(stderr, "Error: Cannot read data bitmap\n");
        fclose(output_fp);
        return 1;
    }
    for (uint64_t i = 0; i < blocks_needed; i++) {
        set_bit(data_bitmap, file_blocks[i] - sb.data_region_start);
    }
    if (write_block(output_fp, sb.data_bitmap_start, data_bitmap) != 0) {
        fprintf(stderr, "Error: Cannot write data bitmap\n");
        fclose(output_fp);
        return 1;
    }
    
    // Create new inode
    inode_t new_inode = {0};
    new_inode.mode = 0100000;
    new_inode.links = 1;
    new_inode.uid = 0;
    new_inode.gid = 0;
    new_inode.size_bytes = file_stat.st_size;
    time_t now = time(NULL);
    new_inode.atime = now;
    new_inode.mtime = now;
    new_inode.ctime = now;
    for (uint64_t i = 0; i < blocks_needed; i++) {
        new_inode.direct[i] = file_blocks[i];
    }
    new_inode.proj_id = 2;
    inode_crc_finalize(&new_inode);
    
    // Write new inode
    uint64_t inode_offset = (sb.inode_table_start * BS) + ((new_inode_num - 1) * INODE_SIZE);
    if (fseek(output_fp, inode_offset, SEEK_SET) != 0) {
        fprintf(stderr, "Error: Cannot seek to inode position\n");
        fclose(output_fp);
        return 1;
    }
    if (fwrite(&new_inode, sizeof(inode_t), 1, output_fp) != 1) {
        fprintf(stderr, "Error: Cannot write new inode\n");
        fclose(output_fp);
        return 1;
    }
    
    // Copy file data to allocated blocks
    FILE *file_fp = fopen(file_to_add, "rb");
    if (!file_fp) {
        fprintf(stderr, "Error: Cannot open file '%s' for reading: %s\n", file_to_add, strerror(errno));
        fclose(output_fp);
        return 1;
    }
    
    for (uint64_t i = 0; i < blocks_needed; i++) {
        uint8_t block_data[BS] = {0};
        size_t bytes_to_read = BS;
        if (i == blocks_needed - 1) {
  
            bytes_to_read = file_stat.st_size - (i * BS);
        }
        
        if (fread(block_data, 1, bytes_to_read, file_fp) != bytes_to_read) {
            fprintf(stderr, "Error: Cannot read file data\n");
            fclose(file_fp);
            fclose(output_fp);
            return 1;
        }
        
        if (write_block(output_fp, file_blocks[i], block_data) != 0) {
            fprintf(stderr, "Error: Cannot write file data block\n");
            fclose(file_fp);
            fclose(output_fp);
            return 1;
        }
    }
    fclose(file_fp);
    
    // Update root directory
    inode_t root_inode;
    uint64_t root_inode_offset = (sb.inode_table_start * BS) + ((ROOT_INO - 1) * INODE_SIZE);
    if (fseek(output_fp, root_inode_offset, SEEK_SET) != 0) {
        fprintf(stderr, "Error: Cannot seek to root inode\n");
        fclose(output_fp);
        return 1;
    }
    if (fread(&root_inode, sizeof(inode_t), 1, output_fp) != 1) {
        fprintf(stderr, "Error: Cannot read root inode\n");
        fclose(output_fp);
        return 1;
    }
    
    // Find space for new directory entry
    int entry_added = 0;
    for (int i = 0; i < DIRECT_MAX && root_inode.direct[i] != 0 && !entry_added; i++) {
        uint8_t block_data[BS];
        if (read_block(output_fp, root_inode.direct[i], block_data) != 0) {
            fprintf(stderr, "Error: Cannot read root directory block\n");
            fclose(output_fp);
            return 1;
        }
        
        // Find empty directory entry
        for (int j = 0; j < BS / sizeof(dirent64_t); j++) {
            dirent64_t *entry = (dirent64_t *)(block_data + j * sizeof(dirent64_t));
            if (entry->inode_no == 0) {
    
                entry->inode_no = new_inode_num;
                entry->type = 1;
                strncpy(entry->name, file_to_add, 57);
                entry->name[57] = '\0'; // Ensure null termination
                dirent_checksum_finalize(entry);
                
                if (write_block(output_fp, root_inode.direct[i], block_data) != 0) {
                    fprintf(stderr, "Error: Cannot write root directory block\n");
                    fclose(output_fp);
                    return 1;
                }
                entry_added = 1;
                break;
            }
        }
    }
    
    if (!entry_added) {
        fprintf(stderr, "Error: Root directory is full\n");
        fclose(output_fp);
        return 1;
    }
    
    root_inode.mtime = now;
    root_inode.ctime = now;
    inode_crc_finalize(&root_inode);
    
    if (fseek(output_fp, root_inode_offset, SEEK_SET) != 0) {
        fprintf(stderr, "Error: Cannot seek to root inode for update\n");
        fclose(output_fp);
        return 1;
    }
    if (fwrite(&root_inode, sizeof(inode_t), 1, output_fp) != 1) {
        fprintf(stderr, "Error: Cannot write updated root inode\n");
        fclose(output_fp);
        return 1;
    }

    sb.mtime_epoch = now;
    superblock_crc_finalize(&sb);
    
    if (fseek(output_fp, 0, SEEK_SET) != 0) {
        fprintf(stderr, "Error: Cannot seek to superblock\n");
        fclose(output_fp);
        return 1;
    }
    if (fwrite(&sb, sizeof(superblock_t), 1, output_fp) != 1) {
        fprintf(stderr, "Error: Cannot write updated superblock\n");
        fclose(output_fp);
        return 1;
    }
    
    fclose(output_fp);
    printf("Successfully added file '%s' to the file system\n", file_to_add);
    return 0;
}