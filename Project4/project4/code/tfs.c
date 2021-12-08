/*
 *  Copyright (C) 2019 CS416 Spring 2019
 *	
 *	Tiny File System
 *
 *	File:	tfs.c
 *  Author: Yujie REN
 *	Date:	April 2019
 *
 */

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/time.h>
#include <libgen.h>
#include <limits.h>

#include "block.h"
#include "tfs.h"

#define FILE 1
#define DIRECTORY 2 // Directory
char diskfile_path[PATH_MAX];

struct superblock SuperBlock;
bitmap_t DataBitMap;
bitmap_t INodeBitMap;
struct inode* INodeTable;

int blockCount = 0;

/*

	DATA REGION SHOULD BE THE SIZE OF 
	32*1024*1024 - ((sizeof(SuperBlock) + Sizeof(DataBitMap) +sizeof(INodeBitMap)+ sizeof(INodeTable)))

*/

void SuperBlockInit(){


	SuperBlock.magic_num = MAGIC_NUM;
	SuperBlock.max_inum  = MAX_INUM;
	SuperBlock.max_dnum  = MAX_DNUM;
	SuperBlock.d_start_blk = 99; // START OF THE DATA REGION
	SuperBlock.i_start_blk = 35;
	SuperBlock.d_bitmap_blk = 1;
	SuperBlock.i_bitmap_blk = 33;

	
}


void writeDataBlockBitMapInit(){

	for(int i = 1; i < 33; i++){
		DataBitMap  = calloc('0', sizeof(bitmap_t) * (MAX_DNUM/32));
		bio_write(i, (void*)&DataBitMap);
		free(DataBitMap);
	}
}

void writeInodeBitMapInit(){

	for(int i = 33; i < 35; i++){
		INodeBitMap = calloc('0', sizeof(bitmap_t) * (MAX_INUM/2));
		bio_write(i, (void*)&INodeBitMap);
		free(INodeBitMap);
	}
}


void writeInodeTableInit(){

	signed long accum = 0;
	int ino = 0;
	for(int i = 35; i < 99; i++){

		INodeTable = malloc(sizeof(struct inode) * 16);

		for(int j = 0; i < 16; j++){
			INodeTable->ino = ino+1;
			if(i == 1 || i == 2){
				INodeTable->valid = -1;
				if(i == 2){
					INodeTable->type = DIRECTORY;
				}
			}else{
				INodeTable->valid = 1;
			}
			INodeTable->size = -1;
			INodeTable->link = 0;
			memset(&(INodeTable->vstat), 0, sizeof(struct stat));
			memset(&(INodeTable->direct_ptr), 0, sizeof(int) * 16);
			memset(&(INodeTable->indirect_ptr), 0, sizeof(int) * 8);
			ino++;
		}

		bio_write(i, (void*)&INodeTable);
		free(INodeTable);

	}

}

// Declare your in-memory data structures here
/*

	BECAUSE	OF THE NATURE OF THIS THE INDEX RETURN TREATS EACH BLOCK AS IF ITS ONE BLOCK. THEREFORE
	THE INDEX RETURNED IS GOING TO BE OUT OF BOUNDS IF YOU SIMPLY USE IT TO EXTRACT INFORMATION 
	OUT OF THE ARRAY. YOU MUST THEREFORE USE THE MODUS TO AVOID THIS. IF MORE QUESTIONS JUST MESSAGE
	ME!

	THESE RETURN -1 IF THERE IS NO AVAILBLE DATA REGION OR INODE. IDK HOW TO HANDLE THAT ERROR ANY OTHER
	WAY.
*/


/* 
 * Get available inode number from bitmap
 */
int get_avail_ino() {

	// Step 1: Read inode bitmap from disk
        unsigned char inodeBitmap[BLOCK_SIZE] = {0};
        int ret = bio_read(SuperBlock.i_bitmap_blk, (bitmap_t) inodeBitmap);
	if (ret < 0) {return -1;}
        
        // Step 2: Traverse inode bitmap to find an available slot
        for (int i = 0; i <= MAX_INUM; i++) {
                if (get_bitmap((bitmap_t) inodeBitmap, i) == 0) {
                
                        // Step 3: Update inode bitmap and write to disk 
                        set_bitmap((bitmap_t) inodeBitmap, i);
                        ret = bio_write(SuperBlock.i_bitmap_blk, (bitmap_t) inodeBitmap);
                        if (ret < 0) {
                                return -1;
                        } else {
                                // The available inode number
                                return i; 
                        }
                }
                
        }
        // Failed to find an available inode number.
	return -1;
}

/* 
 * Get available data block number from bitmap
 */
int get_avail_blkno() {

	// Step 1: Read data block bitmap from disk
	unsigned char blockBitmap[BLOCK_SIZE] = {0};
        int ret = bio_read(SuperBlock.d_bitmap_blk, (bitmap_t) blockBitmap);
        if (ret < 0) {return -1;}
        
	// Step 2: Traverse data block bitmap to find an available slot
        for (int i = 0; i < MAX_DNUM; i++) {
                if(get_bitmap((bitmap_t) blockBitmap, i) == 0) {
                        blockCount += 1;
                        
                        // Step 3: Update data block bitmap and write to disk 
                        set_bitmap((bitmap_t) blockBitmap, i);
                        ret = bio_write(SuperBlock.d_bitmap_blk, (bitmap_t) blockBitmap);
                        if (ret < 0) {
                                return -1;
                        } else {
                                // The available block number
                                return i;
                        }
                }
        }
        // Failed to find an available block number
	return -1;
}

/* 
 * inode operations
 */
int readi(uint16_t ino, struct inode *inode) {
        // Step 1: Get the inode's on-disk block number
        
        // Note: each inode is sizeof(struct inode) bytes large.
        // Byte offset into inode region is ino * sizeof(struct inode)
        // Block offset into inode region is (ino * sizeof(struct inode)) / BLOCK_SIZE
        int inodeBlockNumber = SuperBlock.i_start_blk + ((ino * sizeof(struct inode)) / BLOCK_SIZE);
        
        // Create a block to read into.
        char inodeBlock[BLOCK_SIZE] = {0};
        
        // Read block from disk.
        int ret = bio_read(inodeBlockNumber, inodeBlock);
        if (ret < 0) {return -1;}
        
        // Step 2: Get offset of the inode in the inode on-disk block
        int blockOffset = (ino * sizeof(struct inode)) % BLOCK_SIZE;
        
        // Step 3: Read the block from disk and then copy into inode structure
        memcpy(inode, inodeBlock + blockOffset, sizeof(struct inode));

	return 0;
}

int writei(uint16_t ino, struct inode *inode) {
        // Step 1: Get the block number where this inode resides on disk
        
        // Note: each inode is sizeof(struct inode) bytes large.
        // Byte offset into inode region is ino * sizeof(struct inode)
        // Block offset into inode region is (ino * sizeof(struct inode)) / BLOCK_SIZE
        int inodeBlockNumber = SuperBlock.i_start_blk + ((ino * sizeof(struct inode)) / BLOCK_SIZE);
        
        // Create a block to read into.
        char inodeBlock[BLOCK_SIZE] = {0};
        
        // Read block from disk.
        int ret = bio_read(inodeBlockNumber, inodeBlock);
        if (ret < 0) {return -1;}
        
        // Step 2: Get the offset in the block where this inode resides on disk
        int blockOffset = (ino * sizeof(struct inode)) % BLOCK_SIZE;
        
        // Write the inode to the block
        memcpy(inodeBlock + blockOffset, inode, sizeof(struct inode));
        
        // Step 3: Write the block the inode is in to disk 
        ret = bio_write(inodeBlockNumber, inodeBlock);
        if (ret < 0) {return -1;}

	return 0;
}


/* 
 * directory operations
 */
int dir_find(uint16_t ino, const char *fname, size_t name_len, struct dirent *dirent) {
        // Step 1: Call readi() to get the inode using ino (inode number of current directory)
        struct inode directoryInode = {0};
        int ret = readi(ino, &directoryInode);
        if (ret < 0) {return -1;}
        
        // Step 2: Get data block of current directory from inode
        char dataBlock[BLOCK_SIZE] = {0};
        int directoryEntryCount = BLOCK_SIZE / sizeof(struct dirent);
        
        // Iterate over all the direct blocks
        for (int i = 0; i < 16; i++) {
                // check if pointer is valid. Directory entries are allocated in order,
                // so the first invalid block indicates no more directory entries.
                if (directoryInode.direct_ptr[i] == 0) break;
                
                // Read the block from disk
                ret = bio_read(directoryInode.direct_ptr[i], dataBlock);    
                if (ret < 0) {return -1;}
                
                // Step 3: Read directory's data block and check each directory entry.
                // If the name matches, then copy directory entry to dirent structure
                for (int j = 0; j < directoryEntryCount; j++) {
                        
                        // Get a pointer to a directory entry inside the datablock. We cast raw bytes into a pointer type.
                        struct dirent *workingDirent = (struct dirent *) (dataBlock + (j * sizeof(struct dirent)));
                        
                        // Check if dirent is valid and the names match
                        if (workingDirent->valid == 1 && !memcmp(workingDirent->name, fname, name_len)) {
                                memcpy(dirent, workingDirent, sizeof(struct dirent));
                                return 0;
                        }
                }
        }
        
        // Could not find the name.
	return -1;
}

int dir_add(struct inode dir_inode, uint16_t f_ino, const char *fname, size_t name_len) {
	// Step 1: Read dir_inode's data block
        char dataBlock[BLOCK_SIZE] = {0};
        int directoryEntryCount = BLOCK_SIZE / sizeof(struct dirent);
        int ret = 0;
        
        // The blocks that correspond to the direct pointers.
        int blockIndex = 0;
        
        // Step 2: Check if fname (directory name) is already used in other entries
        // Iterate over the 16 direct pointers
        for (blockIndex = 0; blockIndex < 16; blockIndex++) {
                
                // Check if pointer is valid.
                if (dir_inode.direct_ptr[blockIndex] == 0) break;
                
                // Read the block from the disk
                ret = bio_read(dir_inode.direct_ptr[blockIndex], dataBlock);
                if (ret < 0) {return -1;}
                
                // Iterate over the directory entries in the block.
                for (int j = 0; j < directoryEntryCount; j++) {
                        
                        // Get a pointer to a directory entry in the datablock. We cast raw bytes into a pointer type.
                        struct dirent *workingDirent = (struct dirent *) (dataBlock + (j * sizeof(struct dirent)));
                        
                        // Check if dirent is valid and the names match. Can't add dir if duplicate.
                        if (workingDirent->valid == 1 && !memcmp(workingDirent->name, fname, name_len)) {
                                return -1;
                        }
                        
                        // Invalid entry means we can write to it.
                        if (workingDirent->valid == 0) {
                                
                                // Sanitize the workingDirent struct.
                                memset(workingDirent, 0, sizeof(struct dirent));
                                
                                // Copy data into workingDirect
                                workingDirent->ino   = f_ino;
                                workingDirent->valid = 1;
                                memcpy(workingDirent->name, fname, name_len);
                                
                                // Write block to disk. Note that inode is not updated
                                ret = bio_write(dir_inode.direct_ptr[blockIndex], dataBlock);
                                if (ret < 0) {return -1;}
                                
                                return 0;
                        }
                }
        }
        
        // Case where directory has maximum number of files
        if (blockIndex == 16) {
                return -1;
        }
        
        // Allocate a new block to the directory
        int blockPointer = SuperBlock.d_start_blk + (get_avail_blkno() * BLOCK_SIZE);
        char newBlock[BLOCK_SIZE] = {0};
        
        // Update the inode and write it to disk.
        dir_inode.direct_ptr[blockIndex] = blockPointer;
        ret = writei(dir_inode.ino, &dir_inode);
        if (ret < 0) {return -1;}
        
        // Write the directory entry to the block.
        // Note that the directory entry will always be at the start of the block.
        struct dirent newEntry = {0};
        newEntry.ino = f_ino;
        newEntry.valid = 1;
        memcpy(newEntry.name, fname, name_len);
        memcpy(newBlock, &newEntry, sizeof(struct dirent));
        
        // Write the block to the disk
        ret = bio_write(blockPointer, newBlock);
        if (ret < 0) {return -1;}
        
        return 0;
        

	// Step 3: Add directory entry in dir_inode's data block and write to disk
}

int dir_remove(struct inode dir_inode, const char *fname, size_t name_len) {

	// Step 1: Read dir_inode's data block and checks each directory entry of dir_inode
	
	// Step 2: Check if fname exist

	// Step 3: If exist, then remove it from dir_inode's data block and write to disk

	return 0;
}

/* 
 * namei operation
 */
int get_node_by_path(const char *path, uint16_t ino, struct inode *inode) {
	
	// Step 1: Resolve the path name, walk through path, and finally, find its inode.
	// Note: You could either implement it in a iterative way or recursive way

	return 0;
}

/* 
 * Make file system
 */
int tfs_mkfs() {

	// Call dev_init() to initialize (Create) Diskfile

	dev_init("./FileSystem");

	SuperBlockInit();
	bio_write(0, (void*)&SuperBlock);
	writeInodeTableInit();
	writeDataBlockBitMapInit();
	// write superblock information - Done

	// initialize inode bitmap - Done

	// initialize data block bitmap - Done

	// update bitmap information for root directory

	// update inode for root directory

	return 0;
}


/* 
 * FUSE file operations
 */
static void *tfs_init(struct fuse_conn_info *conn) {

	// Step 1a: If disk file is not found, call mkfs

	if(dev_open("./FileSystem") < 0){
		tfs_mkfs();
	}else{
		bio_read(0, (void*)&SuperBlock);
		if(SuperBlock.magic_num != MAGIC_NUM){
			printf("ERROR! File Super Block corrupted.");
			return;
		}else{
			// FILE BLOCK FOUND. READ ALL NEEDED DATA;

			bio_read(1, (void*)&DataBitMap);
			bio_read(2, (void*)&INodeBitMap);
		}
	}
  // Step 1b: If disk file is found, just initialize in-memory data structures
  // and read superblock from disk

	return NULL;
}

static void tfs_destroy(void *userdata) {

	// Step 1: De-allocate in-memory data structures

	// Step 2: Close diskfile

}

static int tfs_getattr(const char *path, struct stat *stbuf) {

	// Step 1: call get_node_by_path() to get inode from path

	// Step 2: fill attribute of file into stbuf from inode

		stbuf->st_mode   = S_IFDIR | 0755;
		stbuf->st_nlink  = 2;
		time(&stbuf->st_mtime);

	return 0;
}

static int tfs_opendir(const char *path, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path

	// Step 2: If not find, return -1

    return 0;
}

static int tfs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path

	// Step 2: Read directory entries from its data blocks, and copy them to filler

	return 0;
}


static int tfs_mkdir(const char *path, mode_t mode) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name

	// Step 2: Call get_node_by_path() to get inode of parent directory

	// Step 3: Call get_avail_ino() to get an available inode number

	// Step 4: Call dir_add() to add directory entry of target directory to parent directory

	// Step 5: Update inode for target directory

	// Step 6: Call writei() to write inode to disk
	

	return 0;
}

static int tfs_rmdir(const char *path) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name

	// Step 2: Call get_node_by_path() to get inode of target directory

	// Step 3: Clear data block bitmap of target directory

	// Step 4: Clear inode bitmap and its data block

	// Step 5: Call get_node_by_path() to get inode of parent directory

	// Step 6: Call dir_remove() to remove directory entry of target directory in its parent directory

	return 0;
}

static int tfs_releasedir(const char *path, struct fuse_file_info *fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int tfs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target file name

	// Step 2: Call get_node_by_path() to get inode of parent directory

	// Step 3: Call get_avail_ino() to get an available inode number

	// Step 4: Call dir_add() to add directory entry of target file to parent directory

	// Step 5: Update inode for target file

	// Step 6: Call writei() to write inode to disk

	return 0;
}

static int tfs_open(const char *path, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path

	// Step 2: If not find, return -1

	return 0;
}

static int tfs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {

	// Step 1: You could call get_node_by_path() to get inode from path

	// Step 2: Based on size and offset, read its data blocks from disk

	// Step 3: copy the correct amount of data from offset to buffer

	// Note: this function should return the amount of bytes you copied to buffer
	return 0;
}

static int tfs_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {
	// Step 1: You could call get_node_by_path() to get inode from path

	// Step 2: Based on size and offset, read its data blocks from disk

	// Step 3: Write the correct amount of data from offset to disk

	// Step 4: Update the inode info and write it to disk

	// Note: this function should return the amount of bytes you write to disk
	return size;
}

static int tfs_unlink(const char *path) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target file name

	// Step 2: Call get_node_by_path() to get inode of target file

	// Step 3: Clear data block bitmap of target file

	// Step 4: Clear inode bitmap and its data block

	// Step 5: Call get_node_by_path() to get inode of parent directory

	// Step 6: Call dir_remove() to remove directory entry of target file in its parent directory

	return 0;
}

static int tfs_truncate(const char *path, off_t size) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int tfs_release(const char *path, struct fuse_file_info *fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int tfs_flush(const char * path, struct fuse_file_info * fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int tfs_utimens(const char *path, const struct timespec tv[2]) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}


static struct fuse_operations tfs_ope = {
	.init		= tfs_init,
	.destroy	= tfs_destroy,

	.getattr	= tfs_getattr,
	.readdir	= tfs_readdir,
	.opendir	= tfs_opendir,
	.releasedir	= tfs_releasedir,
	.mkdir		= tfs_mkdir,
	.rmdir		= tfs_rmdir,

	.create		= tfs_create,
	.open		= tfs_open,
	.read 		= tfs_read,
	.write		= tfs_write,
	.unlink		= tfs_unlink,

	.truncate   = tfs_truncate,
	.flush      = tfs_flush,
	.utimens    = tfs_utimens,
	.release	= tfs_release
};


int main(int argc, char *argv[]) {
	int fuse_stat;

	getcwd(diskfile_path, PATH_MAX);
	strcat(diskfile_path, "/DISKFILE");

	fuse_stat = fuse_main(argc, argv, &tfs_ope, NULL);

	return fuse_stat;
}

