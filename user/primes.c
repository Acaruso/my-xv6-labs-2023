#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

void source();
void sink();
void cull(int p);
void connect(int fd, int pd[2]);

const int STDIN = 0;
const int STDOUT = 1;

int main() {
    int pd[2];
    pipe(pd);

    if (fork() == 0) {
        connect(STDOUT, pd);
        source();
    } else {
        connect(STDIN, pd);
        sink();
    }
}

void source() {
    for (int i = 2; i <= 35; i++) {
        write(STDOUT, &i, sizeof(i));
    }
}

void sink() {
    int p;

    while (read(STDIN, &p, sizeof(p))) {
        printf("p: %d\n", p);

        int new_pipe[2];
        pipe(new_pipe);

        if (fork() == 0) {
            connect(STDOUT, new_pipe);
            cull(p);
        } else {
            connect(STDIN, new_pipe);
            continue;
        }
    }
}

void cull(int p) {
    int n;
    while (read(STDIN, &n, sizeof(n))) {
        if (n % p != 0) {
            write(STDOUT, &n, sizeof(n));
        }
    }
}

void connect(int fd, int pd[2]) {
    close(fd);
    dup(pd[fd]);
    close(pd[0]);
    close(pd[1]);
}
