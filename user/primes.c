// #include "kernel/types.h"
// #include "kernel/stat.h"
// #include "kernel/fcntl.h"
// #include "user/user.h"

// int READ = 0;
// int WRITE = 1;

// void f(int pipe_read);

// int main(int argc, char *argv[]) {
//     int pipe_[2];
//     pipe(pipe_);

//     int child_pid = fork();
//     if (child_pid == 0) {
//         close(pipe_[WRITE]);
//         f(pipe_[READ]);
//         printf("child exiting\n");
//         exit(0);
//     }

//     for (int i = 2; i <= 35; i++) {
//         write(pipe_[WRITE], &i, sizeof(int));
//     }

//     close(pipe_[READ]);
//     close(pipe_[WRITE]);

//     printf("waiting for child\n");
//     wait(&child_pid);
// }

// void f(int pipe_read) {
//     int p = 0;
//     int rc = 0;
//     rc = read(pipe_read, &p, sizeof(int));
//     printf("pid: %d, p: %d\n", getpid(), p);

//     while (rc != 0) {
//         rc = read(pipe_read, &p, sizeof(int));
//         printf("child read: %d\n", p);
//     }
// }





// #include "kernel/types.h"
// #include "kernel/stat.h"
// #include "kernel/fcntl.h"
// #include "user/user.h"

// int READ = 0;
// int WRITE = 1;

// void f(int pipe_[]);

// int main(int argc, char *argv[]) {
//     int pipe_[2];
//     pipe(pipe_);

//     int child_pid = fork();
//     if (child_pid == 0) {
//         close(pipe_[WRITE]);
//         f(pipe_);
//         close(pipe_[READ]);
//         exit(0);
//     }

//     close(pipe_[READ]);

//     for (int i = 2; i <= 35; i++) {
//         write(pipe_[WRITE], &i, sizeof(int));
//     }

//     close(pipe_[WRITE]);

//     wait((int *)0);
// }

// void f(int pipe_[]) {
//     int p = 0;
//     int rc = 0;
//     rc = read(pipe_[READ], &p, sizeof(int));
//     if (rc == 0) {
//         return;
//     }
//     int n = p;
//     printf("pid: %d, p: %d\n", getpid(), p);

//     int new_pipe_write = -1;
//     int new_pipe_read = -1;
//     int child_pid = -1;

//     while (rc != 0) {
//         if ((n % p) != 0) {
//             if (new_pipe_write == -1) {
//                 int new_pipe[2];
//                 pipe(new_pipe);
//                 new_pipe_write = new_pipe[WRITE];
//                 new_pipe_read  = new_pipe[READ];

//                 child_pid = fork();
//                 if (child_pid == 0) {
//                     f(new_pipe);
//                     exit(0);
//                 }

//                 close(new_pipe[READ]);
//             }
//             write(new_pipe_write, &n, sizeof(int));
//         }
//         rc = read(pipe_[READ], &n, sizeof(int));
//         // printf("child read: %d\n", n);
//     }

//     if (new_pipe_write != -1) {
//         close(new_pipe_write);
//     }
//     if (new_pipe_read != -1) {
//         close(new_pipe_read);
//     }
//     if (child_pid != -1) {
//         wait(&child_pid);
//     }
// }






// #include "kernel/types.h"
// #include "kernel/stat.h"
// #include "kernel/fcntl.h"
// #include "user/user.h"

// int READ = 0;
// int WRITE = 1;

// void f(int pipe_read) {
//     int p;
//     if (read(pipe_read, &p, sizeof(int)) == 0) {
//         close(pipe_read);
//         return; // No data read, close and return.
//     }
//     printf("prime: %d\n", p); // Print the first prime number.

//     int new_pipe[2];
//     pipe(new_pipe);

//     int child_pid = fork();
//     if (child_pid == 0) {
//         close(new_pipe[WRITE]);
//         f(new_pipe[READ]);
//         exit(0);
//     }

//     close(new_pipe[READ]); // Parent does not read from the new pipe.

//     int num;
//     while (read(pipe_read, &num, sizeof(int)) > 0) {
//         if (num % p != 0) { // Check if not a multiple of p
//             write(new_pipe[WRITE], &num, sizeof(int));
//         }
//     }

//     close(pipe_read); // Close the original read end.
//     close(new_pipe[WRITE]); // Close the write end after done writing.

//     wait((int *)0); // Wait for the child to finish.
// }

// int main(int argc, char *argv[]) {
//     int pipe_[2];
//     pipe(pipe_);

//     int child_pid = fork();
//     if (child_pid == 0) {
//         close(pipe_[WRITE]);
//         f(pipe_[READ]);
//         exit(0);
//     }

//     close(pipe_[READ]); // Parent does not read from this pipe.
//     for (int i = 2; i <= 35; i++) {
//         write(pipe_[WRITE], &i, sizeof(int));
//     }
//     close(pipe_[WRITE]); // Close the write end to signal EOF to the child.

//     wait((int *)0); // Wait for the child to finish.
//     return 0;
// }









// #include "kernel/types.h"
// #include "kernel/stat.h"
// #include "kernel/fcntl.h"
// #include "user/user.h"

// void source();
// void cull(int p);
// void redirect(int k, int pd[2]);
// void sink();

// void log(char* str);

// int main() {
//     log("main");

//     int pd[2];
//     pipe(pd);

//     if (fork()) {
//         redirect(0, pd);
//         sink();
//     } else {
//         redirect(1, pd);
//         source();
//     }
// }

// void source() {
//     for (int i = 2; i <= 35; i++) {
//         write(1, &i, sizeof(i));
//     }
// }

// void sink() {
//     int pd[2];
//     int p;

//     while (1) {
//         read(0, &p, sizeof(p));
//         printf("p: %d\n", p);
//         pipe(pd);
//         if (fork()) {
//             redirect(0, pd);
//             continue;
//         } else {
//             redirect(1, pd);
//             cull(p);
//         }
//     }
// }

// void cull(int p) {
//     int n;
//     while (1) {
//         read(0, &n, sizeof(n));
//         if (n % p != 0) {
//             write(1, &n, sizeof(n));
//         }
//     }
// }

// // redirect `stdin` or `stdout` to the pipe
// // if fd == 0, redirect `stdin` to `p[0]`
// // if fd == 1, redirect `stdout` to `p[1]`
// // in either case, close `pd[0]` and `pd[1]`
// void redirect(int fd, int pd[2]) {
//     close(fd);
//     dup(pd[fd]);
//     close(pd[0]);
//     close(pd[1]);
// }

// // logging stuff /////////////////////////////////////////

// void get_filename(int pid, char* buf);
// char digit_to_char(int digit);

// void log(char* str) {
//     char filename[16];
//     int pid = getpid();
//     get_filename(pid, filename);
//     int fd = open(filename, O_WRONLY | O_CREATE);
//     fprintf(fd, str);
//     close(fd);
// }

// void get_filename(int pid, char* buf) {
//     int i = 0;
//     while (pid > 0) {
//         int digit = pid % 10;
//         buf[i++] = digit_to_char(digit);
//         pid = pid / 10;
//     }
//     buf[i++] = '.';
//     buf[i++] = 't';
//     buf[i++] = 'x';
//     buf[i++] = 't';
//     buf[i++] = '\0';
// }

// char digit_to_char(int digit) {
//     return '0' + digit;
// }








// #include "kernel/types.h"
// #include "kernel/stat.h"
// #include "kernel/fcntl.h"
// #include "user/user.h"

// void source();
// void cull(int p);
// void redirect(int k, int pd[2]);
// void sink();

// void log(char* str);

// int main() {
//     log("main");

//     int pd[2];
//     pipe(pd);

//     if (fork()) {
//         redirect(0, pd);            // read
//         sink();
//     } else {
//         redirect(1, pd);            // write
//         source();
//     }
// }

// void source() {
//     log("source");
//     for (int i = 2; i <= 35; i++) {
//         write(1, &i, sizeof(i));
//     }
// }

// void sink() {
//     log("sink");
//     int p;

//     while (read(0, &p, sizeof(p))) {
//         printf("p: %d\n", p);

//         int pd[2];
//         pipe(pd);

//         if (fork()) {
//             redirect(0, pd);        // read
//             continue;
//         } else {
//             redirect(1, pd);        // write
//             cull(p);
//         }
//     }
// }

// void cull(int p) {
//     log("cull");
//     int n;
//     while (read(0, &n, sizeof(n))) {
//         if (n % p != 0) {
//             write(1, &n, sizeof(n));
//         }
//     }
// }

// // redirect `stdin` or `stdout` to the pipe
// // if fd == 0, redirect `stdin` to `p[0]`
// // if fd == 1, redirect `stdout` to `p[1]`
// // in either case, close `pd[0]` and `pd[1]`
// void redirect(int fd, int pd[2]) {
//     close(fd);
//     dup(pd[fd]);
//     close(pd[0]);
//     close(pd[1]);
// }

// // logging stuff /////////////////////////////////////////

// void get_filename(int pid, char* buf);
// char digit_to_char(int digit);

// void log(char* str) {
//     char filename[16];
//     int pid = getpid();
//     get_filename(pid, filename);
//     int fd = open(filename, O_WRONLY | O_CREATE);
//     fprintf(fd, str);
//     fprintf(fd, "\n");
//     close(fd);
// }

// void get_filename(int pid, char* buf) {
//     int num_digits = pid / 10;
//     int i = num_digits;
//     while (pid > 0 && i >= 0) {
//         int digit = pid % 10;
//         buf[i--] = digit_to_char(digit);
//         pid = pid / 10;
//     }
//     i = num_digits + 1;
//     buf[i++] = '.';
//     buf[i++] = 't';
//     buf[i++] = 'x';
//     buf[i++] = 't';
//     buf[i++] = '\0';
// }

// char digit_to_char(int digit) {
//     return '0' + digit;
// }










#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

void source();
void cull(int p);
void connect(int pd[2], int fd);
void sink();

const int STDIN = 0;
const int STDOUT = 1;

int main() {
    int pd[2];
    pipe(pd);

    if (fork() == 0) {
        connect(pd, STDOUT);
        source();
    } else {
        connect(pd, STDIN);
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
            connect(new_pipe, STDOUT);
            cull(p);
        } else {
            connect(new_pipe, STDIN);
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

void connect(int pd[2], int fd) {
    close(fd);
    dup(pd[fd]);
    close(pd[0]);
    close(pd[1]);
}
