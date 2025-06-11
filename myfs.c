#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <libgen.h>

// Filesystem Constants
#define BLOCK_SIZE 4096
#define MAX_INODES 512
#define MAX_DATA_BLOCKS 8192
#define MAX_FILENAME_LEN 255
#define INODE_DIRECT_POINTERS 12
#define ROOT_INODE_NUM 0
#define MAX_PATH_DEPTH 64
#define UNUSED_BLOCK ((uint32_t)-1) //clear sentinel for unused blocks

//disk Structure Layout
#define SUPERBLOCK_BLOCK 0
#define INODE_BITMAP_BLOCK 1
#define DATA_BITMAP_BLOCK 2
#define INODE_TABLE_START_BLOCK 3

// Data Structures
typedef struct {
    uint32_t total_size;
    uint32_t num_inodes;
    uint32_t num_data_blocks;
    uint32_t inode_bitmap_block;
    uint32_t data_bitmap_block;
    uint32_t inode_table_start_block;
    uint32_t data_blocks_start_block;
} Superblock;

typedef struct {
    uint16_t mode; // 0 for file, 1 for directory
    uint32_t size;
    uint32_t link_count;
    time_t creation_time;
    time_t modification_time;
    uint32_t direct_blocks[INODE_DIRECT_POINTERS];
} Inode;

typedef struct {
    char name[MAX_FILENAME_LEN + 1];
    uint32_t inode_number;
} DirectoryEntry;

// Global Variables
FILE *virtual_disk = NULL;
Superblock sb;
unsigned char inode_bitmap[MAX_INODES / 8];
unsigned char data_block_bitmap[MAX_DATA_BLOCKS / 8];
int current_working_directory_inode = ROOT_INODE_NUM; // For CWD support

// Forward Declarations
void do_mkfs(const char *disk_path, long size_bytes);
int get_path_inode(const char* path);
void read_inode(int inode_num, Inode* inode);
int find_entry_in_dir(int dir_inode_num, const char* name);

// Bitmap Helpers
void set_bit(unsigned char* bitmap, int n) { bitmap[n/8] |= (1 << (n%8)); }
void clear_bit(unsigned char* bitmap, int n) { bitmap[n/8] &= ~(1 << (n%8)); }
int get_bit(unsigned char* bitmap, int n) { return (bitmap[n/8] & (1 << (n%8))) != 0; }

// Low-Level I/O
void read_block(int block_num, void* buffer) {
    if (fseek(virtual_disk, block_num * BLOCK_SIZE, SEEK_SET) != 0) {
        perror("fseek failed");
        exit(1);
    }
    if (fread(buffer, BLOCK_SIZE, 1, virtual_disk) != 1) {
        // In testing with pipes, feof might be set before a read error.
        if (feof(virtual_disk)) return;
        perror("fread failed");
        exit(1);
    }
}

void write_block(int block_num, void* buffer) {
    if (fseek(virtual_disk, block_num * BLOCK_SIZE, SEEK_SET) != 0) {
        perror("fseek failed");
        exit(1);
    }
    if (fwrite(buffer, BLOCK_SIZE, 1, virtual_disk) != 1) {
        perror("fwrite failed");
        exit(1);
    }
}

void read_inode(int inode_num, Inode* inode) {
    int block_num = sb.inode_table_start_block + (inode_num * sizeof(Inode)) / BLOCK_SIZE;
    int offset = (inode_num * sizeof(Inode)) % BLOCK_SIZE;
    char buffer[BLOCK_SIZE];
    read_block(block_num, buffer);
    memcpy(inode, buffer + offset, sizeof(Inode));
}

void write_inode(int inode_num, Inode* inode) {
    int block_num = sb.inode_table_start_block + (inode_num * sizeof(Inode)) / BLOCK_SIZE;
    int offset = (inode_num * sizeof(Inode)) % BLOCK_SIZE;
    char buffer[BLOCK_SIZE];
    read_block(block_num, buffer);
    memcpy(buffer + offset, inode, sizeof(Inode));
    write_block(block_num, buffer);
}

void sync_bitmaps() {
    char buffer[BLOCK_SIZE];
    memset(buffer, 0, BLOCK_SIZE);
    memcpy(buffer, inode_bitmap, sizeof(inode_bitmap));
    write_block(sb.inode_bitmap_block, buffer);

    memset(buffer, 0, BLOCK_SIZE);
    memcpy(buffer, data_block_bitmap, sizeof(data_block_bitmap));
    write_block(sb.data_bitmap_block, buffer);
}

// Core Filesystem Logic
int alloc_inode() {
    for (int i = 0; i < sb.num_inodes; i++) {
        if (!get_bit(inode_bitmap, i)) {
            set_bit(inode_bitmap, i);
            return i;
        }
    }
    return -1;
}

void free_inode(int inode_num) {
    clear_bit(inode_bitmap, inode_num);
}

int alloc_data_block() {
    for (int i = 0; i < sb.num_data_blocks; i++) {
        if (!get_bit(data_block_bitmap, i)) {
            set_bit(data_block_bitmap, i);
            return i;
        }
    }
    return -1;
}

void free_data_block(int block_num) {
    clear_bit(data_block_bitmap, block_num);
}

// FIXED: Corrected loop logic
int find_entry_in_dir(int dir_inode_num, const char* name) {
    Inode dir_inode;
    read_inode(dir_inode_num, &dir_inode);
    if (dir_inode.mode != 1) return -1;

    char buffer[BLOCK_SIZE];
    int total_valid_entries = dir_inode.size / sizeof(DirectoryEntry);
    int entries_found = 0;

    for (int i = 0; i < INODE_DIRECT_POINTERS; i++) {
        if (dir_inode.direct_blocks[i] == UNUSED_BLOCK || entries_found >= total_valid_entries)
            break;

        read_block(sb.data_blocks_start_block + dir_inode.direct_blocks[i], buffer);
        int entries_in_block = BLOCK_SIZE / sizeof(DirectoryEntry);
        
        DirectoryEntry* de = (DirectoryEntry*)buffer;
        for (int j = 0; j < entries_in_block; j++) {
            if (entries_found >= total_valid_entries) break;
            
            if (de[j].name[0] != '\0') {
                entries_found++;
                if (strcmp(de[j].name, name) == 0) {
                    return de[j].inode_number;
                }
            }
        }
    }
    return -1;
}

void add_entry_to_dir(int dir_inode_num, const char* name, int new_inode_num) {
    Inode dir_inode;
    read_inode(dir_inode_num, &dir_inode);

    DirectoryEntry new_entry;
    strncpy(new_entry.name, name, MAX_FILENAME_LEN);
    new_entry.name[MAX_FILENAME_LEN] = '\0';
    new_entry.inode_number = new_inode_num;

    char buffer[BLOCK_SIZE];
    int entries_per_block = BLOCK_SIZE / sizeof(DirectoryEntry);

    for (int i = 0; i < INODE_DIRECT_POINTERS; i++) {
        int current_block_num;
        if (dir_inode.direct_blocks[i] == UNUSED_BLOCK) {
            current_block_num = alloc_data_block();
            if (current_block_num == -1) {
                printf("Error: Out of data blocks.\n");
                return;
            }
            dir_inode.direct_blocks[i] = current_block_num;
            memset(buffer, 0, BLOCK_SIZE); 
        } else {
            current_block_num = dir_inode.direct_blocks[i];
            read_block(sb.data_blocks_start_block + current_block_num, buffer);
        }

        DirectoryEntry* de = (DirectoryEntry*)buffer;
        for (int j = 0; j < entries_per_block; j++) {
            if (de[j].name[0] == '\0') {
                memcpy(&de[j], &new_entry, sizeof(DirectoryEntry));
                write_block(sb.data_blocks_start_block + current_block_num, buffer);

                int current_entry_offset = (i * entries_per_block) + j;
                if (current_entry_offset * sizeof(DirectoryEntry) >= dir_inode.size) {
                    dir_inode.size += sizeof(DirectoryEntry);
                }

                dir_inode.modification_time = time(NULL);
                write_inode(dir_inode_num, &dir_inode);
                return;
            }
        }
    }
    printf("Error: Directory is full.\n");
}


int get_path_inode(const char* path) {
    if (path == NULL || path[0] == '\0') return -1;

    if (strcmp(path, ".") == 0) return current_working_directory_inode;

    char path_copy[strlen(path) + 1];
    strcpy(path_copy, path);

    char *token;
    char *rest = path_copy;
    int start_inode;

    if (path[0] == '/') {
        start_inode = ROOT_INODE_NUM;
        if (strlen(rest) > 1) rest++;
    } else {
        start_inode = current_working_directory_inode;
    }

    if (strlen(path_copy) == 1 && path_copy[0] == '/')
        return ROOT_INODE_NUM;

    int current_inode = start_inode;
    while ((token = strtok_r(rest, "/", &rest))) {
        if (strlen(token) == 0) continue;

        current_inode = find_entry_in_dir(current_inode, token);
        if (current_inode == -1) {
            return -1;
        }

        Inode temp_inode;
        read_inode(current_inode, &temp_inode);
        if (temp_inode.mode != 1 && rest && *rest != '\0') {
            return -1;
        }
    }
    return current_inode;
}

void do_mkdir(const char *path) {
    char dname_path[strlen(path) + 1];
    char bname_path[strlen(path) + 1];
    strcpy(dname_path, path);
    strcpy(bname_path, path);

    char *parent_path = dirname(dname_path);
    char *child_name = basename(bname_path);

    int parent_inode_num = get_path_inode(parent_path);
    if (parent_inode_num == -1) { printf("Error: Parent directory not found for '%s'.\n", path); return; }
    if (find_entry_in_dir(parent_inode_num, child_name) != -1) { printf("Error: Name '%s' already exists.\n", child_name); return; }

    int new_inode_num = alloc_inode();
    if (new_inode_num == -1) { printf("Error: Out of inodes.\n"); return; }

    int new_block_num = alloc_data_block();
    if (new_block_num == -1) {
        printf("Error: Out of data blocks.\n");
        free_inode(new_inode_num);
        return;
    }

    Inode new_inode;
    new_inode.mode = 1;
    new_inode.size = 2 * sizeof(DirectoryEntry);
    new_inode.link_count = 2;
    new_inode.creation_time = new_inode.modification_time = time(NULL);
    for(int i = 0; i < INODE_DIRECT_POINTERS; i++) new_inode.direct_blocks[i] = UNUSED_BLOCK;
    new_inode.direct_blocks[0] = new_block_num;
    write_inode(new_inode_num, &new_inode);

    DirectoryEntry entries[2];
    strcpy(entries[0].name, ".");
    entries[0].inode_number = new_inode_num;
    strcpy(entries[1].name, "..");
    entries[1].inode_number = parent_inode_num;

    char buffer[BLOCK_SIZE] = {0};
    memcpy(buffer, entries, 2 * sizeof(DirectoryEntry));
    write_block(sb.data_blocks_start_block + new_block_num, buffer);

    add_entry_to_dir(parent_inode_num, child_name, new_inode_num);

    Inode parent_inode;
    read_inode(parent_inode_num, &parent_inode);
    parent_inode.link_count++;
    write_inode(parent_inode_num, &parent_inode);

    sync_bitmaps();
    printf("Directory created: %s\n", path);
}

// FIXED: Corrected loop logic
void do_ls(const char *path) {
    if (path == NULL || path[0] == '\0')
        path = ".";

    int inode_num = get_path_inode(path);
    if (inode_num == -1) {
        printf("ls: cannot access '%s': No such file or directory\n", path);
        return;
    }

    Inode inode;
    read_inode(inode_num, &inode);
    if (inode.mode != 1) {
        char temp_path[strlen(path)+1];
        strcpy(temp_path, path);
        printf("f\t%u\t\t%s\n", inode.size, basename(temp_path));
        return;
    }

    printf("Contents of %s:\n", path);
    printf("Type\tSize\t\tName\n");
    printf("----\t----\t\t----\n");

    char buffer[BLOCK_SIZE];
    int total_valid_entries = inode.size / sizeof(DirectoryEntry);
    int entries_found = 0;

    for (int i = 0; i < INODE_DIRECT_POINTERS; i++) {
        if (inode.direct_blocks[i] == UNUSED_BLOCK || entries_found >= total_valid_entries)
            break;

        read_block(sb.data_blocks_start_block + inode.direct_blocks[i], buffer);
        int entries_in_block = BLOCK_SIZE / sizeof(DirectoryEntry);

        DirectoryEntry *de = (DirectoryEntry*)buffer;
        for (int j = 0; j < entries_in_block; j++) {
            if (entries_found >= total_valid_entries) break;

            if (de[j].name[0] != '\0') {
                entries_found++;
                Inode entry_inode;
                read_inode(de[j].inode_number, &entry_inode);
                printf("%s\t%u\t\t%s\n",
                       (entry_inode.mode == 1 ? "d" : "f"),
                       entry_inode.size,
                       de[j].name);
            }
        }
    }
}

void do_cp_to_vdisk(const char* host_path, const char* vdisk_path) {
    FILE *src_file = fopen(host_path, "rb");
    if (!src_file) { printf("Error: Cannot open host file %s\n", host_path); return; }

    fseek(src_file, 0, SEEK_END);
    long file_size = ftell(src_file);
    fseek(src_file, 0, SEEK_SET);

    if (file_size > INODE_DIRECT_POINTERS * BLOCK_SIZE) {
        printf("Error: File is too large for this simple filesystem.\n");
        fclose(src_file);
        return;
    }

    char dname_path[strlen(vdisk_path) + 1];
    char bname_path[strlen(vdisk_path) + 1];
    strcpy(dname_path, vdisk_path);
    strcpy(bname_path, vdisk_path);
    char *parent_path = dirname(dname_path);
    char *child_name = basename(bname_path);

    int parent_inode_num = get_path_inode(parent_path);
    if (parent_inode_num == -1) { printf("Error: Parent directory not found.\n"); fclose(src_file); return; }
    if (find_entry_in_dir(parent_inode_num, child_name) != -1) { printf("Error: Name already exists.\n"); fclose(src_file); return; }

    int new_inode_num = alloc_inode();
    if (new_inode_num == -1) { printf("Error: Out of inodes.\n"); fclose(src_file); return; }

    Inode new_inode;
    new_inode.mode = 0; // File
    new_inode.size = file_size;
    new_inode.link_count = 1;
    new_inode.creation_time = new_inode.modification_time = time(NULL);
    for(int i = 0; i < INODE_DIRECT_POINTERS; i++) new_inode.direct_blocks[i] = UNUSED_BLOCK;

    char buffer[BLOCK_SIZE];
    long bytes_left = file_size;
    int blocks_allocated = 0;
    for (int i = 0; i < INODE_DIRECT_POINTERS && bytes_left > 0; i++) {
        int new_block = alloc_data_block();
        if (new_block == -1) {
            printf("Error: Out of data blocks during copy. Cleaning up.\n");
            for (int j = 0; j < blocks_allocated; j++) {
                free_data_block(new_inode.direct_blocks[j]);
            }
            free_inode(new_inode_num);
            sync_bitmaps();
            fclose(src_file);
            return;
        }
        new_inode.direct_blocks[i] = new_block;
        blocks_allocated++;

        size_t bytes_to_read = bytes_left > BLOCK_SIZE ? BLOCK_SIZE : (size_t)bytes_left;
        fread(buffer, bytes_to_read, 1, src_file);
        memset(buffer + bytes_to_read, 0, BLOCK_SIZE - bytes_to_read); // Zero-pad
        write_block(sb.data_blocks_start_block + new_block, buffer);
        bytes_left -= bytes_to_read;
    }

    write_inode(new_inode_num, &new_inode);
    add_entry_to_dir(parent_inode_num, child_name, new_inode_num);
    sync_bitmaps();
    fclose(src_file);
    printf("Copied %s to %s\n", host_path, vdisk_path);
}

void do_cp_from_vdisk(const char* vdisk_path, const char* host_path) {
    int inode_num = get_path_inode(vdisk_path);
    if (inode_num == -1) { printf("Error: File not found on virtual disk.\n"); return; }

    Inode inode;
    read_inode(inode_num, &inode);
    if(inode.mode != 0) { printf("Error: Not a file.\n"); return; }

    FILE *dest_file = fopen(host_path, "wb");
    if (!dest_file) { printf("Error: Cannot create host file %s\n", host_path); return; }

    char buffer[BLOCK_SIZE];
    long bytes_left = inode.size;
    for (int i = 0; i < INODE_DIRECT_POINTERS && bytes_left > 0; i++) {
        if (inode.direct_blocks[i] == UNUSED_BLOCK) break;
        read_block(sb.data_blocks_start_block + inode.direct_blocks[i], buffer);
        size_t bytes_to_write = bytes_left > BLOCK_SIZE ? BLOCK_SIZE : (size_t)bytes_left;
        fwrite(buffer, bytes_to_write, 1, dest_file);
        bytes_left -= bytes_to_write;
    }

    fclose(dest_file);
    printf("Copied %s to %s\n", vdisk_path, host_path);
}

void do_rm_entry(int parent_inode_num, const char* child_name) {
    Inode parent_inode;
    read_inode(parent_inode_num, &parent_inode);
    char buffer[BLOCK_SIZE];

    int total_entries = parent_inode.size / sizeof(DirectoryEntry);
    int entries_found = 0;

    for (int i = 0; i < INODE_DIRECT_POINTERS; i++) {
        if (parent_inode.direct_blocks[i] == UNUSED_BLOCK || entries_found >= total_entries)
            break;

        read_block(sb.data_blocks_start_block + parent_inode.direct_blocks[i], buffer);
        int entries_in_block = BLOCK_SIZE / sizeof(DirectoryEntry);
        
        DirectoryEntry* de = (DirectoryEntry*)buffer;
        for (int j = 0; j < entries_in_block; j++) {
            if (entries_found >= total_entries) break;
            if (de[j].name[0] != '\0') {
                 entries_found++;
                 if (strcmp(de[j].name, child_name) == 0) {
                    memset(&de[j], 0, sizeof(DirectoryEntry));
                    write_block(sb.data_blocks_start_block + parent_inode.direct_blocks[i], buffer);
                    return;
                }
            }
        }
    }
}

void do_rm(const char* path) {
    char dname_path[strlen(path) + 1];
    char bname_path[strlen(path) + 1];
    strcpy(dname_path, path);
    strcpy(bname_path, path);
    char *parent_path = dirname(dname_path);
    char *child_name = basename(bname_path);

    int parent_inode_num = get_path_inode(parent_path);
    if (parent_inode_num == -1) { printf("Error: Parent directory not found.\n"); return; }

    int child_inode_num = find_entry_in_dir(parent_inode_num, child_name);
    if (child_inode_num == -1) { printf("Error: File or link not found.\n"); return; }

    Inode child_inode;
    read_inode(child_inode_num, &child_inode);
    if (child_inode.mode == 1) { printf("Error: Cannot remove directory with 'rm'. Use 'rmdir'.\n"); return; }

    do_rm_entry(parent_inode_num, child_name);

    Inode parent_inode;
    read_inode(parent_inode_num, &parent_inode);
    parent_inode.size -= sizeof(DirectoryEntry);
    write_inode(parent_inode_num, &parent_inode);

    child_inode.link_count--;
    write_inode(child_inode_num, &child_inode);

    if (child_inode.link_count == 0) {
        for (int i = 0; i < INODE_DIRECT_POINTERS; i++) {
            if (child_inode.direct_blocks[i] != UNUSED_BLOCK) {
                free_data_block(child_inode.direct_blocks[i]);
            }
        }
        free_inode(child_inode_num);
    }

    sync_bitmaps();
    printf("Removed %s\n", path);
}

void do_rmdir(const char* path) {
    if (strcmp(path, "/") == 0) { printf("Error: Cannot remove root directory.\n"); return; }

    int inode_num = get_path_inode(path);
    if (inode_num == -1) { printf("Error: Directory not found.\n"); return; }

    Inode inode;
    read_inode(inode_num, &inode);
    if (inode.mode != 1) { printf("Error: Not a directory.\n"); return; }

    int entry_count = 0;
    char buffer[BLOCK_SIZE];
    int total_entries = inode.size / sizeof(DirectoryEntry);
    int entries_found = 0;

    for (int i = 0; i < INODE_DIRECT_POINTERS; i++) {
        if (inode.direct_blocks[i] == UNUSED_BLOCK || entries_found >= total_entries)
            break;

        read_block(sb.data_blocks_start_block + inode.direct_blocks[i], buffer);
        int entries_in_block = BLOCK_SIZE / sizeof(DirectoryEntry);
        
        DirectoryEntry* de = (DirectoryEntry*)buffer;
        for (int j = 0; j < entries_in_block; j++) {
            if (entries_found >= total_entries) break;
            if (de[j].name[0] != '\0') {
                 entries_found++;
                 entry_count++;
            }
        }
    }

    if (entry_count > 2) { printf("Error: Directory not empty.\n"); return; }

    char dname_path[strlen(path) + 1];
    char bname_path[strlen(path) + 1];
    strcpy(dname_path, path);
    strcpy(bname_path, path);
    char *parent_path = dirname(dname_path);
    char *child_name = basename(bname_path);

    int parent_inode_num = get_path_inode(parent_path);

    do_rm_entry(parent_inode_num, child_name);

    Inode parent_inode;
    read_inode(parent_inode_num, &parent_inode);
    parent_inode.size -= sizeof(DirectoryEntry);
    parent_inode.link_count--;
    write_inode(parent_inode_num, &parent_inode);

    if(inode.direct_blocks[0] != UNUSED_BLOCK) {
        free_data_block(inode.direct_blocks[0]);
    }
    free_inode(inode_num);

    sync_bitmaps();
    printf("Removed directory %s\n", path);
}

void do_ln(const char* target_path, const char* link_path) {
    int target_inode_num = get_path_inode(target_path);
    if (target_inode_num == -1) { printf("Error: Target does not exist.\n"); return; }

    Inode target_inode;
    read_inode(target_inode_num, &target_inode);
    if (target_inode.mode == 1) { printf("Error: Hard links to directories not supported.\n"); return; }

    char dname_path[strlen(link_path) + 1];
    char bname_path[strlen(link_path) + 1];
    strcpy(dname_path, link_path);
    strcpy(bname_path, link_path);
    char *parent_path = dirname(dname_path);
    char *child_name = basename(bname_path);

    int parent_inode_num = get_path_inode(parent_path);
    if (parent_inode_num == -1) { printf("Error: Parent directory for link not found.\n"); return; }

    if (find_entry_in_dir(parent_inode_num, child_name) != -1) {
        printf("Error: Link name '%s' already exists.\n", child_name);
        return;
    }

    add_entry_to_dir(parent_inode_num, child_name, target_inode_num);
    target_inode.link_count++;
    write_inode(target_inode_num, &target_inode);

    printf("Created hard link %s -> %s\n", link_path, target_path);
}

void do_df() {
    int used_inodes = 0;
    for (int i = 0; i < sb.num_inodes; i++) {
        if (get_bit(inode_bitmap, i)) used_inodes++;
    }

    int used_data_blocks = 0;
    for (int i = 0; i < sb.num_data_blocks; i++) {
        if (get_bit(data_block_bitmap, i)) used_data_blocks++;
    }

    printf("Disk Usage:\n");
    printf("  Inodes:      %d used, %d free, %d total\n", used_inodes, sb.num_inodes - used_inodes, sb.num_inodes);
    printf("  Data Blocks: %d used, %d free, %d total\n", used_data_blocks, sb.num_data_blocks - used_data_blocks, sb.num_data_blocks);
    printf("  Disk Space:  %ld bytes used, %ld bytes free, %u bytes total\n",
           (long)used_data_blocks * BLOCK_SIZE,
           (long)(sb.num_data_blocks - used_data_blocks) * BLOCK_SIZE, sb.total_size);
}

void do_append(const char *path, int n_bytes) {
    if (n_bytes <= 0) { printf("Error: Must append a positive number of bytes.\n"); return; }
    int inode_num = get_path_inode(path);
    if (inode_num == -1) { printf("Error: File not found.\n"); return; }

    Inode inode;
    read_inode(inode_num, &inode);
    if (inode.mode != 0) { printf("Error: Not a file.\n"); return; }

    long original_size = inode.size;
    long new_size = original_size + n_bytes;
    if (new_size > INODE_DIRECT_POINTERS * BLOCK_SIZE) {
        printf("Error: Appending would exceed maximum file size.\n");
        return;
    }

    char buffer[BLOCK_SIZE] = {0};
    long bytes_to_add = n_bytes;

    int last_block_idx = (original_size > 0) ? (original_size - 1) / BLOCK_SIZE : -1;
    if (original_size > 0 && original_size % BLOCK_SIZE != 0) {
        int last_block_num = inode.direct_blocks[last_block_idx];
        read_block(sb.data_blocks_start_block + last_block_num, buffer);

        int offset = original_size % BLOCK_SIZE;
        int space_in_block = BLOCK_SIZE - offset;
        int bytes_to_write = (bytes_to_add < space_in_block) ? (int)bytes_to_add : space_in_block;

        memset(buffer + offset, 0, bytes_to_write);
        write_block(sb.data_blocks_start_block + last_block_num, buffer);
        bytes_to_add -= bytes_to_write;
        last_block_idx++;
    }

    while (bytes_to_add > 0) {
        if (last_block_idx >= INODE_DIRECT_POINTERS) break;

        int new_block_num = alloc_data_block();
        if (new_block_num == -1) {
            printf("Error: Out of data blocks.\n");
            break;
        }
        inode.direct_blocks[last_block_idx] = new_block_num;

        int bytes_to_write = (bytes_to_add > BLOCK_SIZE) ? BLOCK_SIZE : (int)bytes_to_add;
        memset(buffer, 0, BLOCK_SIZE);
        write_block(sb.data_blocks_start_block + new_block_num, buffer);
        bytes_to_add -= bytes_to_write;
        last_block_idx++;
    }

    inode.size = new_size - bytes_to_add;
    inode.modification_time = time(NULL);
    write_inode(inode_num, &inode);
    sync_bitmaps();
    printf("Appended %ld bytes to %s.\n", n_bytes - bytes_to_add, path);
}

void do_truncate(const char *path, int n_bytes) {
    if (n_bytes <= 0) { printf("Error: Must shorten by a positive number of bytes.\n"); return; }
    int inode_num = get_path_inode(path);
    if (inode_num == -1) { printf("Error: File not found.\n"); return; }

    Inode inode;
    read_inode(inode_num, &inode);
    if (inode.mode != 0) { printf("Error: Not a file.\n"); return; }

    long original_size = inode.size;
    long new_size = (n_bytes >= original_size) ? 0 : original_size - n_bytes;

    int last_block_idx_to_keep = (new_size > 0) ? (int)((new_size - 1) / BLOCK_SIZE) : -1;

    for (int i = last_block_idx_to_keep + 1; i < INODE_DIRECT_POINTERS; i++) {
        if (inode.direct_blocks[i] != UNUSED_BLOCK) {
            free_data_block(inode.direct_blocks[i]);
            inode.direct_blocks[i] = UNUSED_BLOCK;
        }
    }

    inode.size = new_size;
    inode.modification_time = time(NULL);
    write_inode(inode_num, &inode);
    sync_bitmaps();

    if (new_size == 0 && original_size > 0) {
        printf("Truncated %s to 0 bytes.\n", path);
    } else {
        printf("Shortened %s to %ld bytes.\n", path, new_size);
    }
}

int find_name_for_inode(int parent_inode_num, int child_inode_num, char* name_buffer) {
    Inode parent_inode;
    read_inode(parent_inode_num, &parent_inode);
    if (parent_inode.mode != 1) return -1;

    int total_entries = parent_inode.size / sizeof(DirectoryEntry);
    int entries_found = 0;
    char block_buffer[BLOCK_SIZE];

    for (int i = 0; i < INODE_DIRECT_POINTERS; i++) {
        if (parent_inode.direct_blocks[i] == UNUSED_BLOCK || entries_found >= total_entries)
            break;

        read_block(sb.data_blocks_start_block + parent_inode.direct_blocks[i], block_buffer);
        int entries_in_block = BLOCK_SIZE / sizeof(DirectoryEntry);
        
        DirectoryEntry* de = (DirectoryEntry*)block_buffer;
        for (int j = 0; j < entries_in_block; j++) {
            if (entries_found >= total_entries) break;
            if (de[j].name[0] != '\0') {
                 entries_found++;
                if (de[j].inode_number == child_inode_num &&
                    strcmp(de[j].name, ".") != 0 && strcmp(de[j].name, "..") != 0) {
                    strcpy(name_buffer, de[j].name);
                    return 0;
                }
            }
        }
    }
    return -1;
}

void do_pwd() {
    if (current_working_directory_inode == ROOT_INODE_NUM) {
        printf("/\n");
        return;
    }

    char components[MAX_PATH_DEPTH][MAX_FILENAME_LEN + 1];
    int depth = 0;
    int current_inode = current_working_directory_inode;

    while (current_inode != ROOT_INODE_NUM) {
        int parent_inode = find_entry_in_dir(current_inode, "..");
        if (depth >= MAX_PATH_DEPTH) {
            printf("/<path too deep>\n");
            return;
        }

        if (find_name_for_inode(parent_inode, current_inode, components[depth]) != 0) {
            printf("/<error: fs inconsistent>\n");
            return;
        }
        depth++;
        if (parent_inode == current_inode) break;
        current_inode = parent_inode;
    }

    printf("/");
    for (int i = depth - 1; i >= 0; i--) {
        printf("%s", components[i]);
        if (i > 0) printf("/");
    }
    printf("\n");
}

void do_cd(const char *path) {
    if (path[0] == '\0') {
        return;
    }

    int target_inode_num = get_path_inode(path);
    if (target_inode_num == -1) {
        printf("cd: no such file or directory: %s\n", path);
        return;
    }

    Inode target_inode;
    read_inode(target_inode_num, &target_inode);
    if (target_inode.mode != 1) {
        printf("cd: not a directory: %s\n", path);
        return;
    }
    current_working_directory_inode = target_inode_num;
}

// FIXED: Corrected initialization of root directory entries
void do_mkfs(const char *disk_path, long size_bytes) {
    FILE* temp_disk = fopen(disk_path, "w+b");
    if (!temp_disk) { perror("Error creating virtual disk file"); exit(1); }

    if (ftruncate(fileno(temp_disk), size_bytes) != 0) {
        perror("Error setting disk size");
        fclose(temp_disk);
        exit(1);
    }

    int num_inode_blocks = (MAX_INODES * sizeof(Inode) + BLOCK_SIZE - 1) / BLOCK_SIZE;
    int num_total_blocks = size_bytes / BLOCK_SIZE;

    Superblock temp_sb;
    temp_sb.total_size = size_bytes;
    temp_sb.num_inodes = MAX_INODES;
    temp_sb.inode_bitmap_block = INODE_BITMAP_BLOCK;
    temp_sb.data_bitmap_block = DATA_BITMAP_BLOCK;
    temp_sb.inode_table_start_block = INODE_TABLE_START_BLOCK;
    temp_sb.data_blocks_start_block = temp_sb.inode_table_start_block + num_inode_blocks;
    temp_sb.num_data_blocks = num_total_blocks - temp_sb.data_blocks_start_block;
    if (temp_sb.num_data_blocks > MAX_DATA_BLOCKS) temp_sb.num_data_blocks = MAX_DATA_BLOCKS;

    char buffer[BLOCK_SIZE] = {0};

    memcpy(buffer, &temp_sb, sizeof(Superblock));
    fseek(temp_disk, SUPERBLOCK_BLOCK * BLOCK_SIZE, SEEK_SET);
    fwrite(buffer, BLOCK_SIZE, 1, temp_disk);

    unsigned char local_inode_bitmap[MAX_INODES / 8] = {0};
    unsigned char local_data_block_bitmap[MAX_DATA_BLOCKS / 8] = {0};

    set_bit(local_inode_bitmap, ROOT_INODE_NUM);
    set_bit(local_data_block_bitmap, 0);

    fseek(temp_disk, temp_sb.inode_bitmap_block * BLOCK_SIZE, SEEK_SET);
    fwrite(local_inode_bitmap, sizeof(local_inode_bitmap), 1, temp_disk);

    fseek(temp_disk, temp_sb.data_bitmap_block * BLOCK_SIZE, SEEK_SET);
    fwrite(local_data_block_bitmap, sizeof(local_data_block_bitmap), 1, temp_disk);

    Inode root_inode;
    root_inode.mode = 1;
    root_inode.size = 2 * sizeof(DirectoryEntry);
    root_inode.link_count = 2;
    root_inode.creation_time = root_inode.modification_time = time(NULL);
    for(int i = 0; i < INODE_DIRECT_POINTERS; i++)
        root_inode.direct_blocks[i] = UNUSED_BLOCK;
    root_inode.direct_blocks[0] = 0;

    memset(buffer, 0, BLOCK_SIZE);
    memcpy(buffer, &root_inode, sizeof(Inode));
    fseek(temp_disk, temp_sb.inode_table_start_block * BLOCK_SIZE, SEEK_SET);
    fwrite(buffer, BLOCK_SIZE, 1, temp_disk);

    DirectoryEntry entries[2];
    strcpy(entries[0].name, ".");
    entries[0].inode_number = ROOT_INODE_NUM;
    strcpy(entries[1].name, "..");
    entries[1].inode_number = ROOT_INODE_NUM;

    memset(buffer, 0, BLOCK_SIZE);
    memcpy(buffer, entries, 2 * sizeof(DirectoryEntry));
    fseek(temp_disk, (temp_sb.data_blocks_start_block) * BLOCK_SIZE, SEEK_SET);
    fwrite(buffer, BLOCK_SIZE, 1, temp_disk);

    fclose(temp_disk);
    if(isatty(fileno(stdout))) {
        printf("Virtual disk created successfully: %s (%ld bytes)\n", disk_path, size_bytes);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <virtual_disk_file>\n", argv[0]);
        return 1;
    }
    
    int is_interactive = isatty(fileno(stdin));
    char *disk_path = argv[1];
    virtual_disk = fopen(disk_path, "r+b");

    if (!virtual_disk) {
        char input_buffer[128];
        char answer = 'n';

        if (is_interactive) printf("Virtual disk file '%s' not found. Create it? (y/n): ", disk_path);
        
        while(fgets(input_buffer, sizeof(input_buffer), stdin)) {
            if(input_buffer[0] == '#' || input_buffer[0] == '\n' || input_buffer[0] == '\r') continue;
            sscanf(input_buffer, " %c", &answer);
            break;
        }

        if (answer == 'y' || answer == 'Y') {
            long size = 0;
            if (is_interactive) printf("Enter size in bytes (e.g., 10485760 for 10MB): ");
            
            while(fgets(input_buffer, sizeof(input_buffer), stdin)) {
                if(input_buffer[0] == '#' || input_buffer[0] == '\n' || input_buffer[0] == '\r') continue;
                size = atol(input_buffer);
                break;
            }

            if (size <= 0) {
                fprintf(stderr, "Invalid size provided.\n");
                return 1;
            }

            do_mkfs(disk_path, size);
            virtual_disk = fopen(disk_path, "r+b");
            if (!virtual_disk) {
                perror("Failed to open newly created disk");
                return 1;
            }
        } else {
            if (is_interactive) printf("Exiting.\n");
            return 0;
        }
    }

    char buffer[BLOCK_SIZE];
    read_block(SUPERBLOCK_BLOCK, buffer);
    memcpy(&sb, buffer, sizeof(Superblock));

    read_block(sb.inode_bitmap_block, buffer);
    memcpy(inode_bitmap, buffer, sizeof(inode_bitmap));

    read_block(sb.data_bitmap_block, buffer);
    memcpy(data_block_bitmap, buffer, sizeof(data_block_bitmap));

    if (is_interactive) printf("Virtual File System Initialized. Type 'help' for commands.\n");
    
    char line[1024];
    char cmd[16] = {0}, arg1[512] = {0}, arg2[512] = {0};

    while (1) {
        if (is_interactive) printf("vfs> ");
        if (!fgets(line, sizeof(line), stdin)) break;

        if (line[0] == '\n' || line[0] == '#' || line[0] == '\r') continue;

        cmd[0] = arg1[0] = arg2[0] = '\0';
        sscanf(line, "%15s %511s %511s", cmd, arg1, arg2);
        if (strlen(cmd) == 0) continue;

        if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
            break;
        } else if (strcmp(cmd, "cd") == 0) {
            if (arg1[0] == '\0') do_cd("/"); else do_cd(arg1);
        } else if (strcmp(cmd, "pwd") == 0) {
            do_pwd();
        } else if (strcmp(cmd, "ls") == 0) {
            if (arg1[0] == '\0') do_ls("."); else do_ls(arg1);
        } else if (strcmp(cmd, "mkdir") == 0) {
            if (arg1[0] == '\0') { printf("Usage: mkdir <path>\n"); continue; }
            do_mkdir(arg1);
        } else if (strcmp(cmd, "cp-to") == 0) {
            if (arg1[0] == '\0' || arg2[0] == '\0') { printf("Usage: cp-to <host_path> <vdisk_path>\n"); continue; }
            do_cp_to_vdisk(arg1, arg2);
        } else if (strcmp(cmd, "cp-from") == 0) {
            if (arg1[0] == '\0' || arg2[0] == '\0') { printf("Usage: cp-from <vdisk_path> <host_path>\n"); continue; }
            do_cp_from_vdisk(arg1, arg2);
        } else if (strcmp(cmd, "rm") == 0) {
            if (arg1[0] == '\0') { printf("Usage: rm <path>\n"); continue; }
            do_rm(arg1);
        } else if (strcmp(cmd, "rmdir") == 0) {
            if (arg1[0] == '\0') { printf("Usage: rmdir <path>\n"); continue; }
            do_rmdir(arg1);
        } else if (strcmp(cmd, "ln") == 0) {
             if (arg1[0] == '\0' || arg2[0] == '\0') { printf("Usage: ln <target_path> <link_path>\n"); continue; }
            do_ln(arg1, arg2);
        } else if (strcmp(cmd, "df") == 0) {
            do_df();
        } else if (strcmp(cmd, "append") == 0) {
            if (arg1[0] == '\0' || arg2[0] == '\0') { printf("Usage: append <path> <bytes>\n"); continue; }
            do_append(arg1, atoi(arg2));
        } else if (strcmp(cmd, "truncate") == 0) {
            if (arg1[0] == '\0' || arg2[0] == '\0') { printf("Usage: truncate <path> <bytes>\n"); continue; }
            do_truncate(arg1, atoi(arg2));
        } else if (strcmp(cmd, "help") == 0) {
            printf("Available commands:\n");
            printf("  ls [path]                - List directory contents (default: current dir)\n");
            printf("  cd [path]                - Change current directory (.. is supported)\n");
            printf("  pwd                      - Print current directory path\n");
            printf("  mkdir <path>             - Create a directory\n");
            printf("  rmdir <path>             - Remove an empty directory\n");
            printf("  cp-to <host> <vdisk>     - Copy file from host to virtual disk\n");
            printf("  cp-from <vdisk> <host>   - Copy file from virtual disk to host\n");
            printf("  rm <path>                - Remove a file or link\n");
            printf("  ln <target> <link_name>  - Create a hard link\n");
            printf("  append <path> <bytes>    - Add N null bytes to a file\n");
            printf("  truncate <path> <bytes>  - Shorten a file by N bytes (or to 0)\n");
            printf("  df                       - Display disk usage information\n");
            printf("  exit/quit                - Exit the program\n");
        } else {
            if (strlen(cmd) > 0) printf("Unknown command: %s\n", cmd);
        }
    }

    if (is_interactive) printf("Exiting.\n");
    if (virtual_disk) {
        fclose(virtual_disk);
    }
    
    return 0;
}
