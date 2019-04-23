#include "readimage.h"




char get_file_type(struct ext2_dir_entry *dir_entry) {

    char type = '\0';

    if ((dir_entry->file_type & EXT2_FT_DIR) == EXT2_FT_DIR) {
        type = 'd';
    } else if ((dir_entry->file_type & EXT2_FT_REG_FILE) == EXT2_FT_REG_FILE) {
        type = 'f';
    } else if ((dir_entry->file_type & EXT2_FT_SYMLINK) == EXT2_FT_SYMLINK) {
        type = 'l';
    }

    if (type == '\0') {
        fprintf(stderr, "get_file_type: Should not get here.\n");
        exit(EXIT_FAILURE);
    }

    return type;
}


char get_inode_type(struct ext2_inode *inode) {

    char type = '\0';

    if ((inode->i_mode & EXT2_S_IFDIR) == EXT2_S_IFDIR) {
        type = 'd';
    } else if ((inode->i_mode & EXT2_S_IFREG) == EXT2_S_IFREG) {
        type = 'f';
    } else if ((inode->i_mode & EXT2_S_IFLNK) == EXT2_S_IFLNK) {
        type = 'l';
    }

    if (type == '\0') {
        // fprintf(stderr, "get_inode_type: Should not get here.\n");
        // exit(EXIT_FAILURE);
    }

    return type;
}



/**
 * Prints this directories contents.
 */
void print_directory_block(u_char *block_addr) {

    struct ext2_dir_entry *dir_entry;
    int index_byte = 0;
    char file_type;
    while (index_byte < EXT2_BLOCK_SIZE) {

        // Get the directory entry and type of file
        dir_entry = (struct ext2_dir_entry *) (block_addr + index_byte);
        file_type = get_file_type(dir_entry);

        printf("Inode: %d rec_len: %d name_len: %d type= %c name=", dir_entry->inode, dir_entry->rec_len, dir_entry->name_len, file_type);
        printf("%.*s\n", dir_entry->name_len, dir_entry->name);
        // printf("%.*s", stringLength, pointerToString);

        index_byte += dir_entry->rec_len;
    }
}



/**
 * Prints the directory block's contents
 * When printing the blocks, first print the root directory.
 * Then for each inode that is in use, check if it is a directory.
 * If it is a directory, print the contents of the directory block.
 * You do not need to print anything for file blocks.
 */
void print_directory_blocks(struct ext2_inode *inode_tbl, u_char *inode_bitmap, u_int num_inodes) {

    printf("\nDirectory Blocks:\n");

    /** Initialize variables */
    int inode_number = EXT2_ROOT_INO;
    int inode_tbl_index = inode_number - 1;
    struct ext2_inode *inode;                   // ptr to an inode
    int num_blocks;                             // number of blocks belonging to an inode


    /** Print the directory blocks */
    char i_type;
    int block_number;
    u_char *block_addr;
    while (inode_tbl_index < num_inodes) {

        // Get a pointer to the correct inode
        inode = &inode_tbl[inode_tbl_index];
        i_type = get_inode_type(inode);

        // Check if the inode is in use and a directory
        if ((in_use(inode_bitmap, inode_tbl_index)) && i_type == 'd') {
            // We have a directory inode (this inode is a directory)

            num_blocks = inode->i_blocks / 2;
            for (int i = 0; i < num_blocks; i++) {

                // Get the block number for this inode
                block_number = inode->i_block[i];
                printf("   DIR BLOCK NUM: %d (for inode %d)\n", block_number, inode_number);

                // Get a pointer to this block's address
                block_addr = (u_char *) (disk + (EXT2_BLOCK_SIZE * block_number));

                // Print the directory block
                print_directory_block(block_addr);
            }
        }

        // Have to skip reserved inodes
        if (inode_number == EXT2_ROOT_INO) {
            // Then we just printed the root directory. Skip the others
            inode_number = EXT2_GOOD_OLD_FIRST_INO;
            inode_tbl_index = inode_number - 1;
            // inode_tbl_index = EXT2_GOOD_OLD_FIRST_INO - 1;
            // inode_number = inode_tbl_index + 1;
        }

        // Update variables after each iteration
        inode_tbl_index++;
        inode_number++;
    }
}



/** There are 32 inodes, and we are given the <index> of an inode
 * in the range [0 - 31]. Each bit of the bitmap represents whether
 * that particular inode is in use. Thus, we need 4 bytes to represent
 * all 32 inodes.
 * Eg) Given inode index 31, we find which byte by dividing by 8.
 */
int in_use(u_char *inode_bitmap_addr, int index) {

    // Find the proper byte corresponding to the index
    int byte = index / BITS_PER_BYTE;
    // Find the proper bit corresponding to the inode index
    int bit = (index % BITS_PER_BYTE);

    return inode_bitmap_addr[byte] & (1 << bit);
}


/** Takes an inode and it's inode number (which starts at 1) and prints
 * data about this inode.
 */
void print_inode(struct ext2_inode *inode, u_int inode_num) {

    // Find the type of the inode
    char type = get_inode_type(inode);

    printf("[%d] type: %c size: %d links: %d blocks: %d\n", inode_num, type, inode->i_size, inode->i_links_count, inode->i_blocks);


    /** Print the data blocks
     * NOTE: i_blocks is a 32-bit value representing the total number of
     * 512-bytes (sectors) blocks reserved to contain the data of this inode.
     * Since our block size is 1024, i_blocks is actually twice the value
     * of the number of blocks reserved for this inode.
     *
     * "the maximum index of the i_block array should be computed from
     * i_blocks / ((1024 << s_log_block_size) / 512), or once simplified,
     * i_blocks / (2 << s_log_block_size)"
     *
     * i_block[i] is the actual block number (index) of where that data is
     * stored.
     */
    u_int num_blocks = inode->i_blocks / 2;
    printf("[%d] Blocks: ", inode_num);
    for (int i = 0; i < num_blocks; i++) {
        printf(" %d", inode->i_block[i]);
    }
    printf("\n");
}


/**
 * "When the inode table is created, all the reserved inodes are marked as used.
 * In revision 0 this is the first 11 inodes."
 * ! The first 11 inodes are indexed 0 - 10.
 * The first free inode is actually inode 12. EXT2_GOOD_OLD_FIRST_INO is
 * straight up wrong. EXT2_GOOD_OLD_FIRST_INO is actually the first
 * non-reserved inode TABLE INDEX!!
 */
void print_inodes(struct ext2_inode *inode_tbl, u_char *inode_bitmap_addr, u_int num_inodes) {

    printf("\nInodes:\n");

    // Inode and disk block numbering starts at 1 instead of 0 (so subtract 1)
    struct ext2_inode inode = inode_tbl[EXT2_ROOT_INO - 1];
    print_inode(&inode, EXT2_ROOT_INO);

    // NOTE: num_inodes in our case is 32
    int inode_number, inode_tbl_index;
    for (int i = EXT2_GOOD_OLD_FIRST_INO; i < num_inodes; i++) {
        // For clarity: Remember inodes start at 1
        inode_number = i + 1;
        inode_tbl_index = inode_number - 1;             // = i

        if (in_use(inode_bitmap_addr, inode_tbl_index)) {
            inode = inode_tbl[inode_tbl_index];
            print_inode(&inode, inode_number);
        }
    }
}


void print_bitmap(u_char *addr, u_int max_bytes) {
    u_char in_use;

    for (u_char byte = 0; byte < max_bytes; byte++) {
        for (u_int bit = 0; bit < BITS_PER_BYTE; bit++) {
            // in_use = ((addr[byte])[bit]); // does not work, but why?
            in_use = (addr[byte] & (1 << bit)) >> bit;
            printf("%u", in_use);
        }
        printf(" ");
    }
    printf("\n");
}


void print_super_block(struct ext2_super_block *sb) {
    printf("Inodes: %d\n", sb->s_inodes_count);
    printf("Blocks: %d\n", sb->s_blocks_count);
}


void print_block_group(struct ext2_group_desc *bg) {
    printf("Block group:\n");
    // printf("    block bitmap: %d\n", bg[0].bg_block_bitmap);
    printf("    block bitmap: %d\n", bg->bg_block_bitmap);
    printf("    inode bitmap: %d\n", bg->bg_inode_bitmap);
    printf("    inode table: %d\n", bg->bg_inode_table);
    printf("    free blocks: %d\n", bg->bg_free_blocks_count);
    printf("    free inodes: %d\n", bg->bg_free_inodes_count);
    printf("    used_dirs: %d\n", bg->bg_used_dirs_count);
}


int main(int argc, char **argv) {

    if(argc != 2) {
        fprintf(stderr, "Usage: %s <image file name>\n", argv[0]);
        exit(1);
    }
    int fd = open(argv[1], O_RDWR);
	if (fd == -1) {
		perror("open");
		exit(1);
    }

    disk = mmap(NULL, 128 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (disk == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    // Close fd
    if (close(fd) == -1) {
        perror("close fd");
        exit(EXIT_FAILURE);
    }

    /** NOTE: We are doing pointer arithmetic on <disk> which is a pointer
     * to a char. Thus, <disk> + 1 increases the address by 1 byte. We want
     * to access the Superblock, which is the second block (block1) at
     * byte 1024.
     */
    // ------------------------- fs1 -------------------------
    struct ext2_super_block *sb = (struct ext2_super_block *) (disk + EXT2_BLOCK_SIZE);
    print_super_block(sb);

    /** Access the Block Group Descriptor Table */
    struct ext2_group_desc *bg = (struct ext2_group_desc *) (disk + (EXT2_BLOCK_SIZE * 2));
    print_block_group(bg);


    // ------------------------- fs2 -------------------------
    // Find the block address of block usage bitmap from the BGDT
    u_int blk_index = bg->bg_block_bitmap;
    u_int offset = EXT2_BLOCK_SIZE * blk_index;
    // Pointer arithmetic or dereferencing
    u_char *blk_bitmap_addr = (u_char *) (disk + offset);
    // u_char *blk_bitmap_addr = (u_char *) (&disk[EXT2_BLOCK_SIZE * blk_index]);
    // There are 128 blocks. Each byte = 8 bits can represent 8 blocks
    printf("Block bitmap: ");
    print_bitmap(blk_bitmap_addr, sb->s_blocks_count / BITS_PER_BYTE);
    // (sb->s_blocks_count / BITS_PER_BYTE) -----> this is the number of bytes
    // we need to check, each of which has 8 bits that represent a block.


    // Print the inode bitmap
    blk_index = bg->bg_inode_bitmap;
    u_char *inode_bitmap_addr = (u_char *) (disk + (EXT2_BLOCK_SIZE * blk_index));
    // u_char *inode_bitmap_addr = (u_char *) (&disk[EXT2_BLOCK_SIZE * blk_index]);
    printf("Inode bitmap: ");
    print_bitmap(inode_bitmap_addr, sb->s_inodes_count / BITS_PER_BYTE);
    // There are 32 inodes, so we need 32 bits, or 4 bytes


    // Get the inode table
    blk_index = bg->bg_inode_table;
    offset = EXT2_BLOCK_SIZE * blk_index; // inode table offset
    // struct ext2_inode *inode_tbl = (struct ext2_inode *) (disk + offset);
    struct ext2_inode *inode_tbl = (struct ext2_inode *) (&disk[offset]);
    print_inodes(inode_tbl, inode_bitmap_addr, sb->s_inodes_count);



    // ------------------------- fs3 -------------------------
    print_directory_blocks(inode_tbl, inode_bitmap_addr, sb->s_inodes_count);



    return EXIT_SUCCESS;
}



// ./readimage ../a4/helpers/images/
