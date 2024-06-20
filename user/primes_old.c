#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

int READ = 0;
int WRITE = 1;
int CHILD = 0;

void f(int pipe_[]);

int main(int argc, char *argv[]) {
    // printf("main\n");

    int pipe_[2];
    pipe(pipe_);

    int child_pid = fork();
    if (child_pid == 0) {
        f(pipe_);
        exit(0);
    }

    for (int i = 2; i <= 35; i++) {
        // printf("main write: %d\n", i);
        write(pipe_[WRITE], &i, sizeof(int));
    }

    close(pipe_[READ]);
    close(pipe_[WRITE]);

    wait(&child_pid);
}

void f(int pipe_[]) {
    int p = 0;
    read(pipe_[READ], &p, sizeof(int));
    int n = p;
    printf("pid: %d, p: %d\n", getpid(), p);

    int new_pipe_write = -1;
    int new_pipe_read = -1;
    int child_pid = -1;

    while (n != 0) {
        // if ((n % p) != 0) {
        //     if (new_pipe_write == -1) {
        //         int new_pipe[2];
        //         pipe(new_pipe);
        //         new_pipe_write = new_pipe[WRITE];
        //         new_pipe_read  = new_pipe[READ];

        //         child_pid = fork();
        //         if (child_pid == 0) {
        //             f(new_pipe);
        //             exit(0);
        //         }
        //     }
        //     write(new_pipe_write, &n, sizeof(int));
        // }
        read(pipe_[READ], &n, sizeof(int));
        printf("child read: %d\n", n);
    }

    if (new_pipe_write != -1) {
        close(new_pipe_write);
    }
    if (new_pipe_read != -1) {
        close(new_pipe_read);
    }
    if (child_pid != -1) {
        wait(&child_pid);
    }
}
