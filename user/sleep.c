#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(2, "usage: sleep [ticks]\n");
        exit(1);
    }
    char* ticks_str = argv[1];
    int ticks = atoi(ticks_str);
    sleep(ticks);
    exit(0);
}
