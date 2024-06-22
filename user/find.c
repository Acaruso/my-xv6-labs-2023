#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

const int STDIN = 0;
const int STDOUT = 1;

void find(
    char* starting_dir,
    char* file_to_find,
    int fd,
    struct dirent de,
    struct stat st
);

// find all files in a directory tree with some name
// syntax:
//   find <starting-directory> <file-to-find>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(2, "usage: find <starting-directory> <file-to-find>");
        return 1;
    }

    char* starting_dir = argv[1];
    char* file_to_find = argv[2];
    int fd;
    struct dirent de;
    struct stat st;

    if ((fd = open(starting_dir, O_RDONLY)) < 0) {
        fprintf(2, "cannot open %s\n", starting_dir);
        return 1;
    }

    if (fstat(fd, &st) < 0) {
        fprintf(2, "cannot stat %s\n", starting_dir);
        close(fd);
        return 1;
    }

    if (st.type != T_DIR) {
        fprintf(2, "path is not a directory %s\n", starting_dir);
        close(fd);
        return 1;
    }

    find(
        starting_dir,
        file_to_find,
        fd,
        de,
        st
    );

    close(fd);
}

void find(
    char* starting_dir,
    char* file_to_find,
    int fd,
    struct dirent de,
    struct stat st
) {
    // read one entry of directory into `de`
    while (read(fd, &de, sizeof(de)) == sizeof(de)) {
        if (de.inum == 0) {
            continue;
        }
        printf("got entry: %s\n", de.name);
    }
}
