// find all files in a directory tree with some name
// syntax:
//   find <starting-directory> <file-to-find>

// also see `ls.c`

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

int find(char* path, char* file_to_find);
int compare(char* path, char* file_to_find);
int should_skip_dir(struct dirent *de);
void remove_last_part_of_path(char* path);

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(2, "usage: find <starting-directory> <file-to-find>");
        return 1;
    }

    char path[512];
    strcpy(path, argv[1]);
    char* file_to_find = argv[2];

    return find(path, file_to_find);
}

int find(char* path, char* file_to_find) {
    int fd;
    struct dirent de;
    struct stat st;

    // open `fd` to `path`
    if ((fd = open(path, O_RDONLY)) < 0) {
        fprintf(2, "cannot open %s\n", path);
        return 1;
    }

    // load data into `st`
    if (fstat(fd, &st) < 0) {
        fprintf(2, "cannot stat %s\n", path);
        close(fd);
        return 1;
    }

    // found a match
    if (compare(path, file_to_find) == 0) {
        printf("%s\n", path);
    }

    if (st.type != T_DIR) {
        close(fd);
        return 0;
    }

    // read each entry of directory into `de`
    while (read(fd, &de, sizeof(de)) == sizeof(de)) {
        if (should_skip_dir(&de)) {
            continue;
        }

        // replace null terminator with '\'
        int path_len = strlen(path);
        char* p = path + path_len;
        *p++ = '/';

        // copy `de.name` into `path`
        int dir_name_len = strlen(de.name);
        memmove(p, de.name, dir_name_len);
        p[dir_name_len] = '\0';

        find(path, file_to_find);

        remove_last_part_of_path(path);
    }

    close(fd);

    return 0;
}

int compare(char* path, char* file_to_find) {
    // find first character after last slash
    char* p;
    for (p = path + strlen(path); p >= path && *p != '/'; p--) {
        // do nothing
    }
    p++;
    return strcmp(p, file_to_find);
}

int should_skip_dir(struct dirent *de) {
    if (
        de->inum == 0
        || (strcmp(de->name, ".") == 0)
        || (strcmp(de->name, "..") == 0)
    ) {
        return 1;
    } else {
        return 0;
    }
}

void remove_last_part_of_path(char* path) {
    // find last slash
    char* p;
    for (p = path + strlen(path); p >= path && *p != '/'; p--) {
        // do nothing
    }

    while (*p != '\0') {
        *p++ = '\0';
    }
}
