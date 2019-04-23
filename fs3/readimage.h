#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "ext2.h"

#define BITS_PER_BYTE 8

u_char *disk;



/** Functions */
char get_file_type(struct ext2_dir_entry *dir_entry);
char get_inode_type(struct ext2_inode *inode);
void print_directory_block(u_char *block_addr);
void print_directory_blocks(struct ext2_inode *inode_tbl, u_char *inode_bitmap, u_int num_inodes);
int in_use(u_char *inode_bitmap_addr, int index);
void print_inode(struct ext2_inode *inode, u_int inode_num);
void print_inodes(struct ext2_inode *inode_tbl, u_char *inode_bitmap_addr, u_int num_inodes);
void print_bitmap(u_char *addr, u_int max_bytes);
void print_super_block(struct ext2_super_block *sb);
void print_block_group(struct ext2_group_desc *bg);
