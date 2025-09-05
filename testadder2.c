#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/stat.h>

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

// Function prototypes
int parse_arguments(int argc, char* argv[], char** input_name, char** output_name, char** file_name);
int find_free_inode(FILE* img_file, superblock_t* sb);
int find_free_data_block(FILE* img_file, superblock_t* sb);
int find_free_directory_entry(FILE* img_file, superblock_t* sb);
void mark_inode_allocated(FILE* img_file, superblock_t* sb, int inode_num);
void mark_data_block_allocated(FILE* img_file, superblock_t* sb, int block_num);
int add_file_to_filesystem(FILE* img_file, superblock_t* sb, const char* filename);
int copy_file_to_output(const char* input_name, const char* output_name);

// Checksum functions (placeholders)
void superblock_crc_finalize(superblock_t* sb) {
    sb->checksum = 0xDEADBEEF;
}

void inode_crc_finalize(inode_t* inode) {
    inode->inode_crc = 0xDEADBEEFDEADBEEF;
}

void dirent_checksum_finalize(dirent64_t* dirent) {
    dirent->checksum = 0xAB;
}

// Parse command line arguments
int parse_arguments(int argc, char* argv[], char** input_name, char** output_name, char** file_name) {
    int opt;
    static struct option long_options[] = {
        {"input", required_argument, 0, 'i'},
        {"output", required_argument, 0, 'o'},
        {"file", required_argument, 0, 'f'},
        {0, 0, 0, 0}
    };

    *input_name = NULL;
    *output_name = NULL;
    *file_name = NULL;

    while ((opt = getopt_long(argc, argv, "i:o:f:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'i':
                *input_name = optarg;
                break;
            case 'o':
                *output_name = optarg;
                break;
            case 'f':
                *file_name = optarg;
                break;
            default:
                fprintf(stderr, "Usage: %s --input <file> --output <file> --file <file>\n", argv[0]);
                return -1;
        }
    }

    if (!*input_name || !*output_name || !*file_name) {
        fprintf(stderr, "Error: All arguments (--input, --output, --file) are required\n");
        return -1;
    }

    return 0;
}

// Find the first free inode
int find_free_inode(FILE* img_file, superblock_t* sb) {
    fseek(img_file, sb->inode_bitmap_start * BLOCK_SIZE, SEEK_SET);
    char inode_bitmap[BLOCK_SIZE];
    if (fread(inode_bitmap, 1, BLOCK_SIZE, img_file) != BLOCK_SIZE) {
        fprintf(stderr, "Error: Failed to read inode bitmap\n");
        return -1;
    }

    for (size_t byte_idx = 0; byte_idx < BLOCK_SIZE; byte_idx++) {
        for (int bit_idx = 0; bit_idx < 8; bit_idx++) {
            uint64_t inode_num = byte_idx * 8 + bit_idx + 1; // 1-indexed
            if (inode_num > sb->inode_count) {
                return -1; // No more inodes available
            }
            
            if (!(inode_bitmap[byte_idx] & (1 << bit_idx))) {
                return (int)inode_num;
            }
        }
    }
    return -1; // No free inodes
}

// Find the first free data block
int find_free_data_block(FILE* img_file, superblock_t* sb) {
    fseek(img_file, sb->data_bitmap_start * BLOCK_SIZE, SEEK_SET);
    char data_bitmap[BLOCK_SIZE];
    if (fread(data_bitmap, 1, BLOCK_SIZE, img_file) != BLOCK_SIZE) {
        fprintf(stderr, "Error: Failed to read data bitmap\n");
        return -1;
    }

    for (size_t byte_idx = 0; byte_idx < BLOCK_SIZE; byte_idx++) {
        for (int bit_idx = 0; bit_idx < 8; bit_idx++) {
            uint64_t block_num = byte_idx * 8 + bit_idx;
            if (block_num >= sb->data_region_blocks) {
                return -1; // No more data blocks available
            }
            
            if (!(data_bitmap[byte_idx] & (1 << bit_idx))) {
                return (int)block_num;
            }
        }
    }
    return -1; // No free data blocks
}

// Find a free directory entry in root directory
int find_free_directory_entry(FILE* img_file, superblock_t* sb) {
    uint64_t root_data_offset = sb->data_region_start * BLOCK_SIZE;
    fseek(img_file, root_data_offset, SEEK_SET);
    
    dirent64_t entries[BLOCK_SIZE / sizeof(dirent64_t)];
    size_t entries_count = BLOCK_SIZE / sizeof(dirent64_t);
    
    if (fread(entries, sizeof(dirent64_t), entries_count, img_file) != entries_count) {
        fprintf(stderr, "Error: Failed to read root directory\n");
        return -1;
    }

    // Skip . and .. entries (positions 0 and 1)
    for (size_t i = 2; i < entries_count; i++) {
        if (entries[i].inode_no == 0) {
            return (int)i;
        }
    }
    return -1; // Directory is full
}

// Mark an inode as allocated in the bitmap
void mark_inode_allocated(FILE* img_file, superblock_t* sb, int inode_num) {
    fseek(img_file, sb->inode_bitmap_start * BLOCK_SIZE, SEEK_SET);
    char inode_bitmap[BLOCK_SIZE];
    fread(inode_bitmap, 1, BLOCK_SIZE, img_file);

    int byte_idx = (inode_num - 1) / 8;
    int bit_idx = (inode_num - 1) % 8;
    inode_bitmap[byte_idx] |= (1 << bit_idx);

    fseek(img_file, sb->inode_bitmap_start * BLOCK_SIZE, SEEK_SET);
    fwrite(inode_bitmap, 1, BLOCK_SIZE, img_file);
}

// Mark a data block as allocated in the bitmap
void mark_data_block_allocated(FILE* img_file, superblock_t* sb, int block_num) {
    fseek(img_file, sb->data_bitmap_start * BLOCK_SIZE, SEEK_SET);
    char data_bitmap[BLOCK_SIZE];
    fread(data_bitmap, 1, BLOCK_SIZE, img_file);

    int byte_idx = block_num / 8;
    int bit_idx = block_num % 8;
    data_bitmap[byte_idx] |= (1 << bit_idx);

    fseek(img_file, sb->data_bitmap_start * BLOCK_SIZE, SEEK_SET);
    fwrite(data_bitmap, 1, BLOCK_SIZE, img_file);
}

// Copy input file to output file
int copy_file_to_output(const char* input_name, const char* output_name) {
    FILE* input_file = fopen(input_name, "rb");
    FILE* output_file = fopen(output_name, "wb");
    
    if (!input_file) {
        fprintf(stderr, "Error: Cannot open input file '%s'\n", input_name);
        return -1;
    }
    
    if (!output_file) {
        fprintf(stderr, "Error: Cannot create output file '%s'\n", output_name);
        fclose(input_file);
        return -1;
    }

    // Copy file contents
    char buffer[BLOCK_SIZE];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, BLOCK_SIZE, input_file)) > 0) {
        if (fwrite(buffer, 1, bytes_read, output_file) != bytes_read) {
            fprintf(stderr, "Error: Failed to write to output file\n");
            fclose(input_file);
            fclose(output_file);
            return -1;
        }
    }

    fclose(input_file);
    fclose(output_file);
    return 0;
}

// Add a file to the filesystem
int add_file_to_filesystem(FILE* img_file, superblock_t* sb, const char* filename) {
    // Check if file exists in current directory
    struct stat file_stat;
    if (stat(filename, &file_stat) != 0) {
        fprintf(stderr, "Error: File '%s' not found in current directory\n", filename);
        return -1;
    }

    // Check file size limit
    uint64_t file_size = (uint64_t)file_stat.st_size;
    uint64_t max_file_size = 12 * BLOCK_SIZE;
    if (file_size > max_file_size) {
        fprintf(stderr, "Error: File '%s' is too large (%lu bytes, max %lu bytes)\n", 
                filename, file_size, max_file_size);
        return -1;
    }

    // Check if filename is too long
    if (strlen(filename) > 57) {
        fprintf(stderr, "Error: Filename '%s' is too long (max 57 characters)\n", filename);
        return -1;
    }

    // Check if file already exists in directory
    uint64_t root_data_offset = sb->data_region_start * BLOCK_SIZE;
    fseek(img_file, root_data_offset, SEEK_SET);
    
    dirent64_t entries[BLOCK_SIZE / sizeof(dirent64_t)];
    size_t entries_count = BLOCK_SIZE / sizeof(dirent64_t);
    fread(entries, sizeof(dirent64_t), entries_count, img_file);

    for (size_t i = 2; i < entries_count; i++) {
        if (entries[i].inode_no != 0 && strcmp(entries[i].name, filename) == 0) {
            fprintf(stderr, "Error: File '%s' already exists in filesystem\n", filename);
            return -1;
        }
    }

    // Find free resources
    int free_entry = find_free_directory_entry(img_file, sb);
    if (free_entry == -1) {
        fprintf(stderr, "Error: Root directory is full\n");
        return -1;
    }

    int new_inode_num = find_free_inode(img_file, sb);
    if (new_inode_num == -1) {
        fprintf(stderr, "Error: No free inodes available\n");
        return -1;
    }

    // Calculate blocks needed
    uint64_t blocks_needed = file_size > 0 ? (file_size + BLOCK_SIZE - 1) / BLOCK_SIZE : 1;
    if (blocks_needed > 12) {
        fprintf(stderr, "Error: File requires more than 12 blocks\n");
        return -1;
    }

    // Allocate data blocks
    int data_blocks[12] = {0};
    for (size_t i = 0; i < blocks_needed; i++) {
        data_blocks[i] = find_free_data_block(img_file, sb);
        if (data_blocks[i] == -1) {
            fprintf(stderr, "Error: Not enough free data blocks (need %lu)\n", blocks_needed);
            return -1;
        }
        mark_data_block_allocated(img_file, sb, data_blocks[i]);
    }

    // Create and write inode
    inode_t new_inode;
    memset(&new_inode, 0, sizeof(inode_t));
    
    time_t now = time(NULL);
    new_inode.mode = 0100000;  // Regular file (octal 100000)
    new_inode.links = 1;
    new_inode.uid = 0;
    new_inode.gid = 0;
    new_inode.size_bytes = file_size;
    new_inode.atime = now;
    new_inode.mtime = file_stat.st_mtime;
    new_inode.ctime = now;
    
    for (size_t i = 0; i < blocks_needed; i++) {
        new_inode.direct[i] = (uint32_t)data_blocks[i];
    }
    new_inode.proj_id = 0; // Set to your group ID if needed
    
    inode_crc_finalize(&new_inode);

    // Write inode to table
    uint64_t inode_offset = sb->inode_table_start * BLOCK_SIZE + (new_inode_num - 1) * INODE_SIZE;
    fseek(img_file, inode_offset, SEEK_SET);
    if (fwrite(&new_inode, sizeof(inode_t), 1, img_file) != 1) {
        fprintf(stderr, "Error: Failed to write inode\n");
        return -1;
    }

    // Mark inode as allocated
    mark_inode_allocated(img_file, sb, new_inode_num);

    // Write file data
    FILE* input_file = fopen(filename, "rb");
    if (!input_file) {
        fprintf(stderr, "Error: Cannot open input file '%s'\n", filename);
        return -1;
    }

    for (size_t i = 0; i < blocks_needed; i++) {
        char buffer[BLOCK_SIZE];
        memset(buffer, 0, BLOCK_SIZE);
        size_t bytes_read = fread(buffer, 1, BLOCK_SIZE, input_file);
        
        uint64_t data_block_offset = (sb->data_region_start + data_blocks[i]) * BLOCK_SIZE;
        fseek(img_file, data_block_offset, SEEK_SET);
        if (fwrite(buffer, 1, BLOCK_SIZE, img_file) != BLOCK_SIZE) {
            fprintf(stderr, "Error: Failed to write file data\n");
            fclose(input_file);
            return -1;
        }
    }
    fclose(input_file);

    // Update root directory
    fseek(img_file, root_data_offset, SEEK_SET);
    fread(entries, sizeof(dirent64_t), entries_count, img_file);

    entries[free_entry].inode_no = (uint32_t)new_inode_num;
    entries[free_entry].type = 1; // Regular file
    strncpy(entries[free_entry].name, filename, 57);
    entries[free_entry].name[57] = '\0';
    dirent_checksum_finalize(&entries[free_entry]);

    fseek(img_file, root_data_offset, SEEK_SET);
    if (fwrite(entries, sizeof(dirent64_t), entries_count, img_file) != entries_count) {
        fprintf(stderr, "Error: Failed to update root directory\n");
        return -1;
    }

    // Update root inode (increment link count)
    uint64_t root_inode_offset = sb->inode_table_start * BLOCK_SIZE;
    fseek(img_file, root_inode_offset, SEEK_SET);
    inode_t root_inode;
    fread(&root_inode, sizeof(inode_t), 1, img_file);
    
    root_inode.links++;
    root_inode.mtime = now;
    inode_crc_finalize(&root_inode);
    
    fseek(img_file, root_inode_offset, SEEK_SET);
    fwrite(&root_inode, sizeof(inode_t), 1, img_file);

    printf("Successfully added file '%s' (inode %d, %lu blocks, %lu bytes)\n", 
           filename, new_inode_num, blocks_needed, file_size);

    return 0;
}

int main(int argc, char* argv[]) {
    char* input_name = NULL;
    char* output_name = NULL;
    char* file_name = NULL;

    // Parse command line arguments
    if (parse_arguments(argc, argv, &input_name, &output_name, &file_name) != 0) {
        return 1;
    }

    // Verify input file exists
    FILE* test_file = fopen(input_name, "rb");
    if (!test_file) {
        fprintf(stderr, "Error: Cannot open input image '%s'\n", input_name);
        return 1;
    }

    // Read and verify superblock
    superblock_t superblock;
    if (fread(&superblock, sizeof(superblock_t), 1, test_file) != 1) {
        fprintf(stderr, "Error: Cannot read superblock from '%s'\n", input_name);
        fclose(test_file);
        return 1;
    }
    fclose(test_file);

    // Verify magic number
    if (superblock.magic != MAGIC_NUMBER) {
        fprintf(stderr, "Error: Invalid filesystem magic number (expected 0x%08X, got 0x%08X)\n", 
                MAGIC_NUMBER, superblock.magic);
        return 1;
    }

    // Copy input to output
    if (copy_file_to_output(input_name, output_name) != 0) {
        return 1;
    }

    // Open output file for modification
    FILE* output_file = fopen(output_name, "r+b");
    if (!output_file) {
        fprintf(stderr, "Error: Cannot open output file '%s' for modification\n", output_name);
        return 1;
    }

    // Add file to filesystem
    int result = add_file_to_filesystem(output_file, &superblock, file_name);
    fclose(output_file);

    if (result == 0) {
        printf("File system updated successfully in '%s'\n", output_name);
    }

    return result;
}