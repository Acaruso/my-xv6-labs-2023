#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

int READ = 0;
int WRITE = 1;

char buf;

int main(int argc, char *argv[]) {
    int parent_to_child[2];
    pipe(parent_to_child);

    int child_to_parent[2];
    pipe(child_to_parent);

    int rc = fork();
    if (rc == 0) {
        int child_pid = getpid();

        read(parent_to_child[READ], &buf, 1);

        printf("%d: received ping\n", child_pid);

        write(child_to_parent[WRITE], "2", 1);

        exit(0);
    } else {
        int parent_pid = getpid();

        write(parent_to_child[WRITE], "1", 1);

        read(child_to_parent[READ], &buf, 1);

        printf("%d: received pong\n", parent_pid);

        close(parent_to_child[READ]);
        close(parent_to_child[WRITE]);
        close(child_to_parent[READ]);
        close(child_to_parent[WRITE]);

        exit(0);
    }
}
