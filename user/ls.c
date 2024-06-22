#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

void ls(char *path);
char *fmtname(char *path);

int main(int argc, char *argv[]) {
    int i;

    if (argc < 2) {
        ls(".");
        exit(0);
    }

    for (i = 1; i < argc; i++) {
        ls(argv[i]);
    }

    exit(0);
}

void ls(char *path) {
    char buf[512], *p;
    int fd;
    struct dirent de;
    struct stat st;

    // if `path` refers to a directory, `fd` will refer to that directory.
    // we can then do `read(fd, ...)` to read one entry in the directory.
    if ((fd = open(path, O_RDONLY)) < 0) {
        fprintf(2, "ls: cannot open %s\n", path);
        return;
    }

    if (fstat(fd, &st) < 0) {
        fprintf(2, "ls: cannot stat %s\n", path);
        close(fd);
        return;
    }

    switch (st.type) {
        case T_DEVICE:
        case T_FILE:
            printf("%s %d %d %l\n", fmtname(path), st.type, st.ino, st.size);
            break;

        case T_DIR: {
            if (strlen(path) + 1 + DIRSIZ + 1 > sizeof buf) {
                printf("ls: path too long\n");
                break;
            }

            strcpy(buf, path);
            p = buf + strlen(buf);
            *p++ = '/';

            // read one entry of directory into `de`
            while (read(fd, &de, sizeof(de)) == sizeof(de)) {
                // `de.inum` is the inode number
                // if `de.inum == 0`, the directory is unused
                if (de.inum == 0) {
                    continue;
                }

                // `DIRSIZ` is the maximum size of a directory name
                memmove(p, de.name, DIRSIZ);

                p[DIRSIZ] = '\0';

                if (stat(buf, &st) < 0) {
                    printf("ls: cannot stat %s\n", buf);
                    continue;
                }

                printf("%s %d %d %d\n", fmtname(buf), st.type, st.ino, st.size);
            }

            break;
        }
    }

    close(fd);
}

char *fmtname(char *path) {
    static char buf[DIRSIZ + 1];
    char *p;

    // Find first character after last slash.
    for (p = path + strlen(path); p >= path && *p != '/'; p--) {
        // do nothing
    }
    p++;

    if (strlen(p) >= DIRSIZ) {
        return p;
    }

    // Return blank-padded name.
    memmove(buf, p, strlen(p));
    memset(buf + strlen(p), ' ', DIRSIZ - strlen(p));

    return buf;
}
