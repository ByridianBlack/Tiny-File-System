#include <stdio.h>
#include <stdlib.h>

typedef unsigned char* bitmap_t;
char get_bitmap(bitmap_t b, int i) {
    return b[i / 8] & (1 << (i & 7)) ? 1 : 0;
}

int main(int argc, char const *argv[])
{

    bitmap_t data[2] = {'0', '1'};
    printf("%d\n", get_bitmap(data, 10));
    return 0;
}
