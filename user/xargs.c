#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/param.h"

int STDIN = 0;

void run_command(char* command, char* argv[]);
void make_new_argv(int argc, char *argv[], char* new_argv[]);
void push_to_argv(char* new_item, char* argv[]);

int main(int argc, char *argv[]) {
    char* command = argv[1];
    char* new_argv[MAXARG];
    make_new_argv(argc, argv, new_argv);

    if (fork() == 0) {
        run_command(command, new_argv);
        exit(0);
    } else {
        int status;
        wait(&status);
    }

    exit(0);
}

void run_command(char* command, char* argv[]) {
    char buf[512];
    int bytes_read = read(STDIN, buf, 512);
    while (bytes_read != 0) {
        int start_idx = 0;
        for (int i = 0; i < bytes_read; i++) {
            if (buf[i] == '\n') {
                buf[i] = '\0';

                if (fork() == 0) {
                    push_to_argv(buf + start_idx, argv);
                    exec(command, argv);
                } else {
                    int status = 0;
                    wait(&status);
                }

                start_idx = i + 1;
            }
        }
        bytes_read = read(STDIN, buf, 512);
    }
}

void make_new_argv(int argc, char *argv[], char* new_argv[]) {
    int old_idx = 1;
    int new_idx = 0;
    while (old_idx < argc) {
        new_argv[new_idx++] = argv[old_idx++];
    }
    new_argv[new_idx] = 0;  // argv is null-terminated
}

void push_to_argv(char* new_item, char* argv[]) {
    int i = 0;
    while (argv[i] != 0) {
        i++;
    }
    argv[i++] = new_item;
    argv[i] = 0;
}
