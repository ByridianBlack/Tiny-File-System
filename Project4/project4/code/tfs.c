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

//bitmap_t DataBitMap[MAX_DNUM/4] = {'0'};
//bitmap_t INodeBitMap;
//struct inode* INodeTable;

// Below are Paul's macros and globals
#define ROOT_INODE 2


/*

	DATA REGION SHOULD BE THE SIZE OF 
	32*1024*1024 - ((sizeof(SuperBlock) + Sizeof(DataBitMap) +sizeof(INodeBitMap)+ sizeof(INodeTable)))

*/

void SuperBlockInit(){


	SuperBlock.magic_num = MAGIC_NUM;
	SuperBlock.max_inum  = MAX_INUM;
	SuperBlock.max_dnum  = MAX_DNUM;
        SuperBlock.i_bitmap_blk = 1;
	SuperBlock.d_bitmap_blk = 2;
        SuperBlock.i_start_blk = 3;  // START OF INODE BLOCKS
	SuperBlock.d_start_blk = 67; // START OF THE DATA BLOCKS
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
        int inodeBlockIndex = SuperBlock.i_start_blk + ((ino * sizeof(struct inode)) / BLOCK_SIZE);
        
        // Create a block to read into.
        unsigned char inodeBlock[BLOCK_SIZE] = {0};
        
        // Read block from disk.
        int ret = bio_read(inodeBlockIndex, inodeBlock);
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
        int inodeBlockIndex = SuperBlock.i_start_blk + ((ino * sizeof(struct inode)) / BLOCK_SIZE);
        
        // Create a block to read into.
        unsigned char inodeBlock[BLOCK_SIZE] = {0};
        
        // Read block from disk.
        int ret = bio_read(inodeBlockIndex, inodeBlock);
        if (ret < 0) {return -1;}
        
        // Step 2: Get the offset in the block where this inode resides on disk
        int blockOffset = (ino * sizeof(struct inode)) % BLOCK_SIZE;
        
        // Write the inode to the block
        memcpy(inodeBlock + blockOffset, inode, sizeof(struct inode));
        
        // Step 3: Write the block the inode is in to disk 
        ret = bio_write(inodeBlockIndex, inodeBlock);
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
        unsigned char dataBlock[BLOCK_SIZE] = {0};
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
        unsigned char dataBlock[BLOCK_SIZE] = {0};
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
        int blkno = get_avail_blkno();
        int newBlockIndex = SuperBlock.d_start_blk + blkno;
        unsigned char newBlock[BLOCK_SIZE] = {0};
        
        // Update the inode and write it to disk.
        dir_inode.direct_ptr[blockIndex] = newBlockIndex;
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
        ret = bio_write(newBlockIndex, newBlock);
        if (ret < 0) {return -1;}
        
        return 0;

	// Step 3: Add directory entry in dir_inode's data block and write to disk
}

int dir_remove(struct inode dir_inode, const char *fname, size_t name_len) {
	// Step 1: Read dir_inode's data block and checks each directory entry of dir_inode
	unsigned char dataBlock[BLOCK_SIZE] = {0};
        int directoryEntryCount = BLOCK_SIZE / sizeof(struct dirent);
        int ret = 0;
        
        // Iterate over each of the data blocks.
        for (int i = 0; i < 16; i++) {
                
                // Check if pointer is valid. Blocks are used in order.
                if (dir_inode.direct_ptr[i] == 0) return -1;
                
                // Read the block from the disk
                ret = bio_read(dir_inode.direct_ptr[i], dataBlock);
                if (ret < 0) {return -1;}
                
                // Iterate over the directory entries in the block
                for (int j = 0; j < directoryEntryCount; j++) {
                        
                        // Get a pointer to a directory entry in the datablock. We cast raw bytes into a pointer type.
                        struct dirent *workingDirent = (struct dirent *) (dataBlock + (j * sizeof(struct dirent)));
                        
                        // Step 2: Check if fname exist
                        // Step 3: If exist, then remove it from dir_inode's data block and write to disk
                        if (workingDirent->valid == 1 && !memcmp(workingDirent->name, fname, name_len)) {
                                // Set bytes to 0
                                memset(workingDirent, 0, sizeof(struct dirent));
                                
                                // Write block to the disk
                                ret = bio_write(dir_inode.direct_ptr[i], dataBlock);
                                if (ret < 0) {
                                        return -1;
                                } else {
                                        // Successfully deleted the directory entry
                                        return 0;
                                }
                        }  
                }
        }
        
        // We iterated through all the directory entries but didn't find the name.
        return -1;
}

/* 
 * namei operation
 */
int get_node_by_path(const char *path, uint16_t ino, struct inode *inode) {
	// Step 1: Resolve the path name, walk through path, and finally, find its inode.
	// Note: You could either implement it in a iterative way or recursive way
        
        int ret = 0;
        // We begin resolving the path from ino. This allows us to support relative paths
        // instead of always having an absolute path from root.
        int atIno = ino; 
        
        // Path will be like /foo/bar. The start of the filename is "f", not "/"
        const char *atFilename = path;
        int lengthFileName = 0;
        
        while (*atFilename != '\0') {
                
                // Handles cases like /foo, /foo/, /foo//////bar
                if (*atFilename == '/') {
                        atFilename += 1;
                        continue;
                }
                
                // Calculate the length of the filename.
                while(*(atFilename + lengthFileName) != '/' &&
                      *(atFilename + lengthFileName) != '\0') {
                        lengthFileName += 1;
                }
                
                // Create a buffer for the filename. 
                // Max filename length is 255 in linux. Name must be 
                // null terminated too.
                char fname[256] = {0};
                memcpy(fname, atFilename, lengthFileName);
                
                // Find the directory entry for that specific filename
                struct dirent foundEntry = {0};
                ret = dir_find(atIno, fname, lengthFileName + 1, &foundEntry);
                
                if (ret != 0) {
                        // Filename not found
                        return -1; 
                } else {
                        atIno = foundEntry.ino;
                        atFilename += lengthFileName;
                        lengthFileName = 0;
                }
        }
        
        // Read the resolved inode.
        ret = readi(atIno, inode);
        if (ret != 0) {
                return -1;
        } else {
                return 0;
        }
}

/* 
 * Make file system
 */
int tfs_mkfs() {

	// Call dev_init() to initialize (Create) Diskfile
	dev_init(diskfile_path);
	SuperBlockInit();
        
        int ret = 0;
        
        // Create the superblock block
        unsigned char onDiskSuperBlock[BLOCK_SIZE] = {0};
        memcpy(onDiskSuperBlock, &SuperBlock, sizeof(struct superblock));
        
        // Write the superblock block to disk
        ret = bio_write(0, onDiskSuperBlock);
        if (ret < 0) {return -1;}
        
        unsigned char flatBlock[BLOCK_SIZE] = {0};
        
        // Write a blank data bitmap
        ret = bio_write(2, flatBlock);
        if (ret < 0) {return -1;}
        
        // update inode bitmap
        // The first two inodes are used by convention.
        // The third inode is for the root directory.
        set_bitmap(flatBlock, 0);
        set_bitmap(flatBlock, 1);
        set_bitmap(flatBlock, 2);
        
        // Write the inode bitmap
        ret = bio_write(1, flatBlock);
        if (ret < 0) {return -1;}

	// update inode for root directory
        struct inode rootInode = {0};
        rootInode.ino      = 2;
        rootInode.valid    = 1;
        rootInode.size  = BLOCK_SIZE;
        rootInode.type  = DIRECTORY;
        rootInode.link  = 2;
        memset(rootInode.direct_ptr, 0, sizeof(int) * 16);       // Initialize the direct pointers to NULL 
        memset(rootInode.indirect_ptr, 0, sizeof(int) * 8);      // Initialize the indirect pointers to NULL
        rootInode.vstat.st_ino     = 2;
        rootInode.vstat.st_mode    = (S_IFDIR | 0755);
        rootInode.vstat.st_nlink   = 2;
        rootInode.vstat.st_uid     = getuid();
        rootInode.vstat.st_gid     = getgid();
        rootInode.vstat.st_size    = BLOCK_SIZE;
        rootInode.vstat.st_blksize = BLOCK_SIZE;
        rootInode.vstat.st_atime   = time(NULL);
        rootInode.vstat.st_mtime   = time(NULL);
        rootInode.vstat.st_ctime   = time(NULL);

        // Write the root inode to disk
        ret = writei(2, &rootInode);
        if (ret != 0) {return -1;}
        
        // Add '.' and '..' to the root inode
        ret = dir_add(rootInode, rootInode.ino, ".", 2);
        if (ret != 0) {return -1;}
        
        // Reread the rootInode since it was update by the previous dir_add.
        ret = readi(2, &rootInode);
        if (ret != 0) {return -1;}
        
        ret = dir_add(rootInode, rootInode.ino, "..", 3);
        if (ret != 0) {return -1;}
        
	return 0;
}


/* 
 * FUSE file operations
 */
static void *tfs_init(struct fuse_conn_info *conn) {

	// Step 1a: If disk file is not found, call mkfs

        int ret = dev_open(diskfile_path);
        
        if (ret != 0) {
                // We must make the file system
                tfs_mkfs();
                
        } else {
                // Step 1b: If disk file is found, just initialize in-memory data structures
                // and read superblock from disk
                // Write the super block to global space.
                ret = bio_read(0, &SuperBlock); 
                if (ret < 0) {
                        exit(EXIT_FAILURE);
                }
                
                // Check to make sure we're using the correct fs
                if (SuperBlock.magic_num != MAGIC_NUM) {
                        exit(EXIT_FAILURE);
                }
        }

        return NULL;
}

static void tfs_destroy(void *userdata) {

	// Step 1: De-allocate in-memory data structures
        // No structures are used.
        
	// Step 2: Close diskfile
        dev_close();
}

static int tfs_getattr(const char *path, struct stat *stbuf) {
	// Step 1: call get_node_by_path() to get inode from path
        
        int ret = 0;
        struct inode getIno = {0};
        
        // We assume that the path is absolute since no root is provided.
        ret = get_node_by_path(path, ROOT_INODE, &getIno);
        if (ret != 0) {return -ENOENT;}

	// Step 2: fill attribute of file into stbuf from inode
        memcpy(stbuf, &(getIno.vstat), sizeof(struct stat));

        /* stbuf->st_mode   = S_IFDIR | 0755;
           stbuf->st_nlink  = 2;
           time(&stbuf->st_mtime);
        */

	return 0;
}

static int tfs_opendir(const char *path, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path
        int ret = 0;
        struct inode getIno = {0};
        
        // We assume that the path is absolute since no root is provided
        ret = get_node_by_path(path, ROOT_INODE, &getIno);

	// Step 2: If not find, return -1
        // It seems that all this function does is return 0 or -1 depending on
        // whether or not the path is valid. 
        return ret;
}

static int tfs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path
        int ret = 0;
        struct inode getIno = {0};
        int directoryEntryCount = BLOCK_SIZE / sizeof(struct dirent);
        
        // We assume that the path is absolute since no root is provided
        ret = get_node_by_path(path, ROOT_INODE, &getIno);
        if (ret != 0) {return -1;}

	// Step 2: Read directory entries from its data blocks, and copy them to filler
        for (int blockIndex = 0; blockIndex < 16; blockIndex++) {
                // Means that the block hasn't been allocated
                if (getIno.direct_ptr[blockIndex] == 0) break;
                
                // Read block of directory entries from the disk
                unsigned char dirBlock[BLOCK_SIZE] = {0};
                ret = bio_read(getIno.direct_ptr[blockIndex], dirBlock);
                if (ret < 0) {return -1;}
                
                // Read all the directory entries in the block.
                for (int i = 0; i < directoryEntryCount; i++) {
                        struct dirent *directoryEntry = (struct dirent *) (dirBlock + (i * sizeof(struct dirent)));
                        
                        // Check that directory entry is valid (wasn't deleted)
                        if (directoryEntry->valid == 1) {
                                ret = filler(buffer, directoryEntry->name, NULL, 0);
                                // Some documentation said to return 0 if filler doesn't return 0.
                                if (ret != 0) {return 0;}
                        }                                
                }
        }
        
        // Finished reading all the directory entries in all the blocks. 
	return 0;
}


static int tfs_mkdir(const char *path, mode_t mode) {
	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name.

        // Create a copy of the path since dirname() and basename() can alter path.
        char dir[4096] = {0};         // Max path size if 4096 in linux
        strncpy(dir, path, 4095);     // Copy at most the first 4095 bytes
        char *dirName = dirname(dir);
        
        char base[4096] = {0};
        strncpy(base, path, 4095);
        char *baseName = basename(base);
        
        // Max length filename is 255 bytes.
        int name_len = strnlen(baseName, 255) + 1;
        
        int ret = 0;
	
        // Step 2: Call get_node_by_path() to get inode of parent directory
        struct inode parentInode = {0};
        ret = get_node_by_path(dirName, ROOT_INODE, &parentInode);
        if (ret != 0) {return -1;}
        
        // Check that the directory doesn't already exist
        struct dirent dummy = {0};
        ret = dir_find(parentInode.ino, baseName, name_len, &dummy);
        if (ret == 0) {return -1;} // Name found
        
	// Step 3: Call get_avail_ino() to get an available inode number
        int availableInode = get_avail_ino();
        if (availableInode == -1) {return -1;}
        
	// Step 4: Call dir_add() to add directory entry of target directory to parent directory
        ret = dir_add(parentInode, availableInode, baseName, name_len);
        if (ret != 0) {return -1;}

	// Step 5: Update inode for target directory
        struct inode newInode = {0};
        newInode.ino   = availableInode;
        newInode.valid = 1;
        newInode.size  = BLOCK_SIZE;
        newInode.type  = DIRECTORY;
        newInode.link  = 2;
        memset(newInode.direct_ptr, 0, sizeof(int) * 16);       // Initialize the direct pointers to NULL 
        memset(newInode.indirect_ptr, 0, sizeof(int) * 8);      // Initialize the indirect pointers to NULL
        newInode.vstat.st_ino     = availableInode;
        newInode.vstat.st_mode    = S_IFDIR | 0755;             // Just use these permissions.
        newInode.vstat.st_nlink   = 2;
        newInode.vstat.st_uid     = getuid();
        newInode.vstat.st_gid     = getgid();
        newInode.vstat.st_size    = BLOCK_SIZE;
        newInode.vstat.st_blksize = BLOCK_SIZE;
        newInode.vstat.st_atime   = time(NULL);
        newInode.vstat.st_mtime   = time(NULL);
        newInode.vstat.st_ctime   = time(NULL);

	// Step 6: Call writei() to write inode to disk
        // Write the new inode to the disk
	ret = writei(availableInode, &newInode);
        if (ret != 0) {return -1;}
        
        // Add '.' and '..' to the new directory
        ret = dir_add(newInode, newInode.ino, ".", 2);
        if (ret != 0) {return -1;}
        
        // Reread the inode since it was updated on disk
        ret = readi(availableInode, &newInode);
        if (ret != 0) {return -1;}
        
        ret = dir_add(newInode, newInode.ino,"..", 3);
        if (ret != 0) {return -1;}
        

	return 0;
}

static int tfs_rmdir(const char *path) {
	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name
        // Create a copy of the path since dirname() and basename() can alter path.
        char dir[4096] = {0};         // Max path size if 4096 in linux
        strncpy(dir, path, 4095);     // Copy at most the first 4095 bytes
        char *dirName = dirname(dir);
        
        char base[4096] = {0};
        strncpy(base, path, 4095);
        char *baseName = basename(base);
        
        // Max length filename is 255 bytes.
        int name_len = strnlen(baseName, 255) + 1;
        
        int ret = 0;

	// Step 2: Call get_node_by_path() to get inode of target directory
        struct inode targetDir = {0};
        ret = get_node_by_path(path, ROOT_INODE, &targetDir);
        if (ret != 0) {return -1;} // If directory can't be reached or doesn't exist.

	// Step 3: Clear data block bitmap of target directory
        
        unsigned char blockBitmap[BLOCK_SIZE] = {0};
        ret = bio_read(SuperBlock.d_bitmap_blk, &blockBitmap);
        if (ret < 0) {return -1;}
        for (int i = 0; i < 16; i++) {
                // Check if block pointer is valid
                if (targetDir.direct_ptr[i] != 0) {
                        // If yes, we delete the block.
                        int blockPointer = targetDir.direct_ptr[i] - SuperBlock.d_start_blk;
                        unset_bitmap(blockBitmap, blockPointer);
                }
        }
        // Commit changes to the data bitmap. Note that we might have anywhere
        // from 0 to 16 changes.
        ret = bio_write(SuperBlock.d_bitmap_blk, &blockBitmap);
        if (ret < 0) {return -1;}
        
	// Step 4: Clear inode bitmap and its data block
        // Note that we don't overwrite the inode data since creating a new inode 
        // always zeroes it out anyway. 
        unsigned char inodeBitmap[BLOCK_SIZE] = {0};
        ret = bio_read(SuperBlock.i_bitmap_blk, &inodeBitmap);
        if (ret < 0) {return -1;}
        unset_bitmap(inodeBitmap, targetDir.ino);
        ret = bio_write(SuperBlock.i_bitmap_blk, &inodeBitmap);
        if (ret < 0) {return -1;}
        
	// Step 5: Call get_node_by_path() to get inode of parent directory
        struct inode parentDir = {0};
        ret = get_node_by_path(dirName, ROOT_INODE, &parentDir);
        if (ret != 0) {return -1;}

	// Step 6: Call dir_remove() to remove directory entry of target directory in its parent directory
        ret = dir_remove(parentDir, baseName, name_len);
        if (ret != 0) {return -1;}

	return 0;
}

static int tfs_releasedir(const char *path, struct fuse_file_info *fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int tfs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
	// Step 1: Use dirname() and basename() to separate parent directory path and target file name
        // Create a copy of the path since dirname() and basename() can alter path.
        char dir[4096] = {0};         // Max path size if 4096 in linux
        strncpy(dir, path, 4095);     // Copy at most the first 4095 bytes
        char *dirName = dirname(dir);
        
        char base[4096] = {0};
        strncpy(base, path, 4095);
        char *baseName = basename(base);
        
        // Max length filename is 255 bytes.
        int name_len = strnlen(baseName, 255) + 1;
        
        int ret = 0;

	// Step 2: Call get_node_by_path() to get inode of parent directory
        struct inode parentInode = {0};
        ret = get_node_by_path(dirName, ROOT_INODE, &parentInode);
        if (ret != 0) {return -1;}

	// Step 3: Call get_avail_ino() to get an available inode number
        int newIno = get_avail_ino();
        if (newIno < 0) {return -1;}

	// Step 4: Call dir_add() to add directory entry of target file to parent directory
        ret = dir_add(parentInode, newIno, baseName, name_len);
        if (ret != 0) {return -1;}

	// Step 5: Update inode for target file
        struct inode newInode = {0};
        newInode.ino   = newIno;
        newInode.valid = 1;
        newInode.size  = 0;
        newInode.type  = FILE;
        newInode.link  = 1;
        memset(newInode.direct_ptr, 0, sizeof(int) * 16);       // Initialize the direct pointers to NULL 
        memset(newInode.indirect_ptr, 0, sizeof(int) * 8);      // Initialize the indirect pointers to NULL
        newInode.vstat.st_ino     = newIno;
        newInode.vstat.st_mode    = S_IFREG | 0600;             // read and write for only the owner
        newInode.vstat.st_nlink   = 1;
        newInode.vstat.st_uid     = getuid();
        newInode.vstat.st_gid     = getgid();
        newInode.vstat.st_size    = 0;
        newInode.vstat.st_blksize = BLOCK_SIZE;
        newInode.vstat.st_atime   = time(NULL);
        newInode.vstat.st_mtime   = time(NULL);
        newInode.vstat.st_ctime   = time(NULL);

	// Step 6: Call writei() to write inode to disk
        ret = writei(newIno, &newInode);
        if (ret != 0) {return -1;}
        
	return 0;
}

static int tfs_open(const char *path, struct fuse_file_info *fi) {
	// Step 1: Call get_node_by_path() to get inode from path
        struct inode getInode = {0};
        int ret = get_node_by_path(path, ROOT_INODE, &getInode);
        
        // Case where file not found.
        if (ret != 0) {return -1;}

	// Step 2: If not find, return -1

	return 0;
}

static int tfs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {
	// Step 1: You could call get_node_by_path() to get inode from path
        struct inode fileInode = {0};
        int ret = get_node_by_path(path, ROOT_INODE, &fileInode);
        if (ret != 0) {return -1;}

	// Step 2: Based on size and offset, read its data blocks from disk
        int fileSize = fileInode.size;
        // If the offset is at or beyond file size, we can't read any bytes.
        if (offset >= fileSize) {return 0;}
        
        // Prevent the function from reading past the end of the file.
        if (size + offset > fileSize) {
                size = fileSize - offset;
        }
        
        int bufferIndex   = 0;    // Keeps track of spot in the buffer.
        int bytesIterated = 0;    // Keeps track of spot in the file.
        
	// Step 3: copy the correct amount of data from offset to buffer
        
        // Iterate over all the direct pointers first.
        for (int i = 0; i < 16; i++) {
                // Check if all bytes read or we're past the end of the file.
                if (size == 0 || offset >= fileSize) {
                        // Return the number of bytes written.
                        return bufferIndex;
                }
                
                // Invalid pointer means that block isn't allocated, which means no more
                // data in the file to read.
                if (fileInode.direct_ptr[i] == 0) {
                        return bufferIndex;
                }
                
                bytesIterated += BLOCK_SIZE;
                
                // Nothing from this block should be read.
                if (bytesIterated <= offset) {
                        continue;
                
                // Blocks needs to be either partially or completely read.
                } else {
                        // Read the block from the disk
                        unsigned char dataBlock[BLOCK_SIZE] = {0};
                        ret = bio_read(fileInode.direct_ptr[i], dataBlock);
                        if (ret < 0) {return -1;}
                        
                        // Read the entire block.
                        if (bytesIterated <= offset + size) {
                                memcpy(buffer + bufferIndex, dataBlock, BLOCK_SIZE);
                                offset      += BLOCK_SIZE;
                                size        -= BLOCK_SIZE;
                                bufferIndex += BLOCK_SIZE;
                        
                        // Partially read the block
                        } else {
                                int startReadingFrom  = offset % BLOCK_SIZE;
                                int readThisManyBytes = size % BLOCK_SIZE;
                                
                                memcpy(buffer + bufferIndex, dataBlock + startReadingFrom, readThisManyBytes);
                                offset      += readThisManyBytes;
                                size        -= readThisManyBytes;
                                bufferIndex += readThisManyBytes;
                        }
                }
        }
        
        // Iterate over all indirect pointers.
        for (int i = 0; i < 8; i++) {
                // Invalid pointer means that block isn't allocated, which means no more
                // data in the file to read.
                if (fileInode.indirect_ptr[i] == 0) {
                        return bufferIndex;
                }
                
                // Read the indirect block from disk
                unsigned char indirectBlock[BLOCK_SIZE] = {0};
                ret = bio_read(fileInode.indirect_ptr[i], indirectBlock);
                if (ret < 0) {return -1;}
                
                int pointerCount = BLOCK_SIZE / sizeof(int);
                
                // Iterate over all block pointers inside the indirect block.
                for (int j = 0; j < pointerCount; j++) {
                        // Ending conditions
                        if (size == 0 || offset >= fileSize) {
                                // Return the number of bytes written.
                                return bufferIndex;
                        }
                        
                        // Cast the indirectBlock into an array of integers.
                        int pointer = ((int *) indirectBlock)[j];
                        
                        // Pointers are allocated in order. If pointer is invalid,
                        // we are at the end of the file.
                        if (pointer == 0) {
                                return bufferIndex;
                        }
                        
                        bytesIterated += BLOCK_SIZE;
                
                        // Nothing from this block should be read.
                        if (bytesIterated <= offset) {
                                continue;
                        
                        // Blocks needs to be either partially or completely read.
                        } else {
                                // Read the block from the disk
                                unsigned char dataBlock[BLOCK_SIZE] = {0};
                                ret = bio_read(pointer, dataBlock);
                                if (ret < 0) {return -1;}
                                
                                // Read the entire block.
                                if (bytesIterated <= offset + size) {
                                        memcpy(buffer + bufferIndex, dataBlock, BLOCK_SIZE);
                                        offset      += BLOCK_SIZE;
                                        size        -= BLOCK_SIZE;
                                        bufferIndex += BLOCK_SIZE;
                                
                                // Partially read the block
                                } else {
                                        int startReadingFrom  = offset % BLOCK_SIZE;
                                        int readThisManyBytes = size % BLOCK_SIZE;
                                        
                                        memcpy(buffer + bufferIndex, dataBlock + startReadingFrom, readThisManyBytes);
                                        offset      += readThisManyBytes;
                                        size        -= readThisManyBytes;
                                        bufferIndex += readThisManyBytes;
                                }
                        }
                }
        }

	// Note: this function should return the amount of bytes you copied to buffer
	return bufferIndex;
}

static int tfs_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {
	// Step 1: You could call get_node_by_path() to get inode from path
        struct inode fileInode = {0};
        int ret = get_node_by_path(path, ROOT_INODE, &fileInode);
        if (ret != 0) {return -1;}

	// Step 2: Based on size and offset, read its data blocks from disk
        int fileSize = fileInode.size;
        int newSize = ((fileSize > (offset + size)) ? fileSize : (offset + size));
        // If the offset is beyond the file size, we would need to write
        // more than size bytes (pad zeroes). We can't do this.
        if (offset > fileSize) {return 0;}
        
        int bufferIndex   = 0;    // Keeps track of spot in the buffer.
        int bytesIterated = 0;    // Keeps track of spot in the file.
        
	// Step 3: Write the correct amount of data from offset to disk
        
        // Iterate over all the direct pointers first.
        for (int i = 0; i < 16; i++) {
                // No more data to write. Update inode and return.
                if (size == 0) {
                        fileInode.size          = newSize;
                        fileInode.vstat.st_size = newSize;
                        fileInode.vstat.st_atime = time(NULL);
                        fileInode.vstat.st_mtime = time(NULL);
                                
                        ret = writei(fileInode.ino, &fileInode);
                        return ((ret != 0) ? -1 : bufferIndex);
                }
                
                // Invalid pointer means that block isn't allocated. 
                // We need to allocate block and write to it.
                if (fileInode.direct_ptr[i] == 0) {
                        int blkno = get_avail_blkno();
                        if (blkno < 0) {return -1;}
                        
                        // Update the inode.
                        fileInode.direct_ptr[i] = SuperBlock.d_start_blk + blkno;
                        
                        // Create a new block.
                        unsigned char dataBlock[BLOCK_SIZE] = {0};
                        
                        // Write an entire block of data.
                        if (size > BLOCK_SIZE) {
                                memcpy(dataBlock, buffer + bufferIndex, BLOCK_SIZE);
                                
                                offset      += BLOCK_SIZE;
                                size        -= BLOCK_SIZE;
                                bufferIndex += BLOCK_SIZE;
                        
                        // Write a partial block of data.
                        } else {
                                int writeThisManyBytes = size;
                                memcpy(dataBlock, buffer + bufferIndex, writeThisManyBytes);
                                
                                offset      += writeThisManyBytes;
                                size        -= writeThisManyBytes;
                                bufferIndex += writeThisManyBytes;
                        }
                        
                        // Write block back to disk.
                        ret = bio_write(fileInode.direct_ptr[i], dataBlock);
                        if (ret < 0) {return -1;}
                        continue;
                        
                }
                
                bytesIterated += BLOCK_SIZE;
                // This block shouldn't be written to.
                if (bytesIterated <= offset) {
                        continue;
                        
                // Blocks needs to be either partially or completely writen.
                } else {
                        // Read the block from the disk
                        unsigned char dataBlock[BLOCK_SIZE] = {0};
                        ret = bio_read(fileInode.direct_ptr[i], dataBlock);
                        if (ret < 0) {return -1;}
                        
                        // Write the entire block.
                        if (bytesIterated <= offset + size) {
                                memcpy(dataBlock, buffer + bufferIndex, BLOCK_SIZE);
                                offset      += BLOCK_SIZE;
                                size        -= BLOCK_SIZE;
                                bufferIndex += BLOCK_SIZE;
                        
                        // Partially write the block
                        } else {
                                int startWritingTo = offset % BLOCK_SIZE;
                                int writeThisManyBytes = size % BLOCK_SIZE;
                                
                                memcpy(dataBlock + startWritingTo, buffer + bufferIndex, writeThisManyBytes);
                                offset      += writeThisManyBytes;
                                size        -= writeThisManyBytes;
                                bufferIndex += writeThisManyBytes;
                        }
                        
                        // Write the block back to disk
                        ret = bio_write(fileInode.direct_ptr[i], dataBlock);
                        if (ret < 0) {return -1;}
                        
                }
                
                
        }

        // Iterate over all the indirect pointers
        for (int i = 0; i < 8; i++) {
                // No more data to write. Break out of loop.
                if (size == 0) {break;}
                
                // Invalid pointer means that block isn't allocated. 
                // We need to allocate block and write to it.
                unsigned char indirectBlock[BLOCK_SIZE] = {0};
                if (fileInode.indirect_ptr[i] == 0) {
                        int blkno = get_avail_blkno();
                        if (blkno < 0) {return -1;}
                        
                        // Update the inode.
                        fileInode.indirect_ptr[i] = SuperBlock.d_start_blk + blkno;
                } else {
                        ret = bio_read(fileInode.indirect_ptr[i], indirectBlock);
                        if (ret < 0) {return -1;}
                }
                
                // Iterate over the direct blocks.
                int pointerCount = BLOCK_SIZE / sizeof(int);
                for (int j = 0; j < pointerCount; j++) {
                        // No more data to write. Break out of loop.
                        if (size == 0) { break; }
                        
                        // Invalid pointer means that block isn't allocated. 
                        // We need to allocate block and write to it.
                        
                        int pointer = ((int *) indirectBlock)[j];
                        
                        // Block not allocated.
                        if (pointer == 0) {
                                int blkno = get_avail_blkno();
                                if (blkno < 0) {return -1;}
                                
                                // Add pointer to indirect block.
                                ((int *) indirectBlock)[j]  = SuperBlock.d_start_blk + blkno;
                                
                                // Create a new block.
                                unsigned char dataBlock[BLOCK_SIZE] = {0};
                                
                                // Write an entire block of data.
                                if (size > BLOCK_SIZE) {
                                        memcpy(dataBlock, buffer + bufferIndex, BLOCK_SIZE);
                                        
                                        offset      += BLOCK_SIZE;
                                        size        -= BLOCK_SIZE;
                                        bufferIndex += BLOCK_SIZE;
                                
                                // Write a partial block of data.
                                } else {
                                        int writeThisManyBytes = size;
                                        memcpy(dataBlock, buffer + bufferIndex, writeThisManyBytes);
                                        
                                        offset      += writeThisManyBytes;
                                        size        -= writeThisManyBytes;
                                        bufferIndex += writeThisManyBytes;
                                }
                                
                                // Write block back to disk and move on to the next block pointer.
                                ret = bio_write(((int *) indirectBlock)[j], dataBlock);
                                if (ret < 0) {return -1;}
                                continue;
                        }
                        
                        bytesIterated += BLOCK_SIZE;
                        // This block shouldn't be written to.
                        if (bytesIterated <= offset) {
                                continue;
                                
                        // Blocks needs to be either partially or completely writen.
                        } else {
                                // Read the block from the disk
                                unsigned char dataBlock[BLOCK_SIZE] = {0};
                                ret = bio_read(fileInode.direct_ptr[i], dataBlock);
                                if (ret < 0) {return -1;}
                                
                                // Write the entire block.
                                if (bytesIterated <= offset + size) {
                                        memcpy(dataBlock, buffer + bufferIndex, BLOCK_SIZE);
                                        offset      += BLOCK_SIZE;
                                        size        -= BLOCK_SIZE;
                                        bufferIndex += BLOCK_SIZE;
                                
                                // Partially write the block
                                } else {
                                        int startWritingTo = offset % BLOCK_SIZE;
                                        int writeThisManyBytes = size % BLOCK_SIZE;
                                        
                                        memcpy(dataBlock + startWritingTo, buffer + bufferIndex, writeThisManyBytes);
                                        offset      += writeThisManyBytes;
                                        size        -= writeThisManyBytes;
                                        bufferIndex += writeThisManyBytes;
                                }
                                
                                // Write the block back to disk
                                ret = bio_write(fileInode.direct_ptr[i], dataBlock);
                                if (ret < 0) {return -1;}
                                
                        }
                        
                }

                // Write indirect block to disk.
                ret = bio_write(fileInode.indirect_ptr[i], indirectBlock);
                if (ret < 0) {return -1;}
                
        }
        
        // Update and write inode to disk.
        // Note: this function should return the amount of bytes you write to disk
        fileInode.size          = newSize;
        fileInode.vstat.st_size = newSize;
        fileInode.vstat.st_atime = time(NULL);
        fileInode.vstat.st_mtime = time(NULL);
        ret = writei(fileInode.ino, &fileInode);
        return ((ret != 0) ? -1 : bufferIndex);
}

static int tfs_unlink(const char *path) {
	// Step 1: Use dirname() and basename() to separate parent directory path and target file name
        // Create a copy of the path since dirname() and basename() can alter path.
        char dir[4096] = {0};         // Max path size if 4096 in linux
        strncpy(dir, path, 4095);     // Copy at most the first 4095 bytes
        char *dirName = dirname(dir);
        
        char base[4096] = {0};
        strncpy(base, path, 4095);
        char *baseName = basename(base);
        
        // Max length filename is 255 bytes.
        int name_len = strnlen(baseName, 255) + 1;
        int ret = 0;
        
	// Step 2: Call get_node_by_path() to get inode of target file
        struct inode targetInode = {0};
        ret = get_node_by_path(path, ROOT_INODE, &targetInode);

	// Step 3: Clear data block bitmap of target file
        
        // Copy the block bitmap from disk.
        unsigned char blockBitmap[BLOCK_SIZE] = {0};
        ret = bio_read(SuperBlock.d_bitmap_blk, blockBitmap);
        if (ret < 0) {return -1;}
        
        // Unset all the direct pointers.
        for (int i = 0; i < 16; i++) {
                if (targetInode.direct_ptr[i] == 0) {break;}
                
                unset_bitmap(blockBitmap, targetInode.direct_ptr[i] - SuperBlock.d_start_blk);
        }
        
        // Unset all the indirect pointers.
        for (int i = 0; i < 8; i++) {
                if (targetInode.indirect_ptr[i] == 0) {break;}
                
                unsigned char indirectBlock[BLOCK_SIZE] = {0};
                ret = bio_read(targetInode.indirect_ptr[i], indirectBlock);
                if (ret < 0) {return -1;}
                
                int pointerCount = BLOCK_SIZE / sizeof(int);
                for (int j = 0; j < pointerCount; j++) {
                        int pointer = ((int *) indirectBlock)[j];
                        
                        if (pointer == 0) {break;}
                        
                        unset_bitmap(blockBitmap, pointer - SuperBlock.d_start_blk);
                }
                
                unset_bitmap(blockBitmap, targetInode.indirect_ptr[i] - SuperBlock.d_start_blk);
        }
        
        // Commit block bitmap back to the disk.
        ret = bio_write(SuperBlock.d_bitmap_blk, blockBitmap);
        if (ret < 0) {return -1;}
        
	// Step 4: Clear inode bitmap and its data block
        unsigned char inodeBitmap[BLOCK_SIZE] = {0};
        ret = bio_read(SuperBlock.i_bitmap_blk, inodeBitmap);
        if (ret < 0) {return -1;}
        unset_bitmap(inodeBitmap, targetInode.ino);
        ret = bio_write(SuperBlock.i_bitmap_blk, inodeBitmap);
        if (ret < 0) {return -1;}

	// Step 5: Call get_node_by_path() to get inode of parent directory
        struct inode parentInode = {0};
        ret = get_node_by_path(dirName, ROOT_INODE, &parentInode);
        if (ret != 0) {return -1;}

	// Step 6: Call dir_remove() to remove directory entry of target file in its parent directory
        ret = dir_remove(parentInode, baseName, name_len);
        if (ret != 0) {return -1;}

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

