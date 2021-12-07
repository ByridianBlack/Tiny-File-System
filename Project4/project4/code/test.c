#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "tfs.h"

int main(int argc, char const *argv[]) {
        
        printf("Size of inode: %ld\n", sizeof(struct inode));
        
        printf("Size of dirent: %ld\n", sizeof(struct dirent)); 
        

}
