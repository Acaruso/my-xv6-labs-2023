// #include "kernel/types.h"
// #include "kernel/stat.h"
// #include "user/user.h"
// #include "kernel/fs.h"
// #include "kernel/fcntl.h"

// const int STDIN = 0;
// const int STDOUT = 1;

// void find(
//     char* path,
//     char* file_to_find,
//     int fd,
//     struct dirent de,
//     struct stat st
// );

// // find all files in a directory tree with some name
// // syntax:
// //   find <starting-directory> <file-to-find>

// int main(int argc, char *argv[]) {
//     if (argc < 2) {
//         fprintf(2, "usage: find <starting-directory> <file-to-find>");
//         return 1;
//     }

//     char path_buf[2048];
//     char out_buf[2048];
//     char* path = argv[1];
//     char* file_to_find = argv[2];
//     int fd;
//     struct dirent de;
//     struct stat st;

//     if ((fd = open(path, O_RDONLY)) < 0) {
//         fprintf(2, "cannot open %s\n", path);
//         return 1;
//     }

//     if (fstat(fd, &st) < 0) {
//         fprintf(2, "cannot stat %s\n", path);
//         close(fd);
//         return 1;
//     }

//     // if (st.type != T_DIR) {
//     //     fprintf(2, "path is not a directory %s\n", path);
//     //     close(fd);
//     //     return 1;
//     // }

//     strcpy(path_buf, path);

//     find(
//         path,
//         file_to_find,
//         fd,
//         de,
//         st
//     );

//     close(fd);
// }

// void find(
//     char* path,
//     char* file_to_find,
//     int fd,
//     struct dirent de,
//     struct stat st
// ) {
//     // read one entry of directory into `de`
//     while (read(fd, &de, sizeof(de)) == sizeof(de)) {
//         if (de.inum == 0) {
//             continue;
//         }
//         printf("got entry: %s\n", de.name);
//     }
// }



// find all files in a directory tree with some name
// syntax:
//   find <starting-directory> <file-to-find>

// also see `ls.c`

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

const int STDIN = 0;
const int STDOUT = 1;

int find(char* path_buf, char* file_to_find);
int compare(char* path_buf, char* file_to_find);
int should_skip_dir(struct dirent *de);
int is_directory(char* path_buf);
void remove_last_part_of_path(char* path_buf);

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(2, "usage: find <starting-directory> <file-to-find>");
        return 1;
    }

    char path_buf[512];
    char* path = argv[1];
    char* file_to_find = argv[2];

    // copy path into path_buf
    strcpy(path_buf, path);

    return find(path, file_to_find);
}

int find(char* path_buf, char* file_to_find) {
    printf("find(%s, %s)\n", path_buf, file_to_find);

    // if (compare(path_buf, file_to_find) == 0) {
    //     printf("found %s", path_buf);
    // }

    int fd;
    struct dirent de;
    struct stat st;

    // open `fd` to `path_buf`
    if ((fd = open(path_buf, O_RDONLY)) < 0) {
        fprintf(2, "cannot open %s\n", path_buf);
        return 1;
    }

    // load data into `st`
    if (fstat(fd, &st) < 0) {
        fprintf(2, "cannot stat %s\n", path_buf);
        close(fd);
        return 1;
    }

    // if `st` is not a directory, return
    if (st.type != T_DIR) {
        close(fd);
        return 0;
    }

    // read one entry of directory into `de`
    while (read(fd, &de, sizeof(de)) == sizeof(de)) {
        if (should_skip_dir(&de)) {
            continue;
        }

        printf("got entry: %s\n", de.name);

        // replace null terminator with '\'
        int path_buf_len = strlen(path_buf);
        char* p = path_buf + path_buf_len;
        *p++ = '/';

        // copy `de.name` into `path_buf`
        int dir_name_len = strlen(de.name);
        memmove(p, de.name, dir_name_len);
        p[dir_name_len] = '\0';

        if (compare(path_buf, file_to_find) == 0) {
            printf("found %s\n", path_buf);
        }

        // if `path_buf` doesn't refer to a directory, don't
        // navigate into it
        if (is_directory(path_buf) == 0) {
            remove_last_part_of_path(path_buf);
            continue;
        }

        find(path_buf, file_to_find);

        remove_last_part_of_path(path_buf);
    }

    close(fd);

    return 0;
}

int compare(char* path_buf, char* file_to_find) {
    // find first character after last slash
    char* p;
    for (p = path_buf + strlen(path_buf); p >= path_buf && *p != '/'; p--) {
        // do nothing
    }
    p++;
    printf("compare: %s %s\n", p, file_to_find);
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

int is_directory(char* path_buf) {
    int fd;
    struct stat st;

    // open `fd` to `path_buf`
    if ((fd = open(path_buf, O_RDONLY)) < 0) {
        fprintf(2, "cannot open %s\n", path_buf);
        return 1;
    }

    // load data into `st`
    if (fstat(fd, &st) < 0) {
        fprintf(2, "cannot stat %s\n", path_buf);
        close(fd);
        return 1;
    }

    close(fd);

    // return (st.type == T_DIR);
    if (st.type == T_DIR) {
        return 1;
    } else {
        return 0;
    }
}

void remove_last_part_of_path(char* path_buf) {
    // find last slash
    char* p;
    for (p = path_buf + strlen(path_buf); p >= path_buf && *p != '/'; p--) {
        // do nothing
    }

    while (*p != '\0') {
        *p++ = '\0';
    }
}
