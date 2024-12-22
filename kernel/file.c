//
// Support functions for system calls that involve file descriptors.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "stat.h"
#include "proc.h"

struct devsw devsw[NDEV];
struct {
    struct spinlock lock;
    struct file file[NFILE];
} ftable;

void fileinit(void) { initlock(&ftable.lock, "ftable"); }

// Allocate a file structure.
struct file *filealloc(void) {
    acquire(&ftable.lock);

    for (struct file *f = ftable.file; f < ftable.file + NFILE; f++) {
        if (f->ref == 0) {
            f->ref = 1;
            release(&ftable.lock);
            return f;
        }
    }

    release(&ftable.lock);

    return 0;
}

// Increment ref count for file f.
struct file *filedup(struct file *f) {
    acquire(&ftable.lock);
    if (f->ref < 1) {
        panic("filedup");
    }
    f->ref++;
    release(&ftable.lock);
    return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void fileclose(struct file *file_ptr) {
    struct file file;

    acquire(&ftable.lock);

    if (file_ptr->ref < 1) {
        panic("fileclose");
    }

    file_ptr->ref--;
    if (file_ptr->ref > 0) {
        release(&ftable.lock);
        return;
    }

    file = *file_ptr;
    file_ptr->ref = 0;
    file_ptr->type = FD_NONE;

    release(&ftable.lock);

    if (file.type == FD_PIPE) {
        pipeclose(file.pipe, file.writable);
    } else if (file.type == FD_INODE || file.type == FD_DEVICE) {
        begin_op();
        iput(file.ip);
        end_op();
    }
#ifdef LAB_NET
    else if (ff.type == FD_SOCK) {
        sockclose(ff.sock);
    }
#endif
}

// Get metadata about file f.
// addr is a user virtual address, pointing to a struct stat.
int filestat(struct file *f, uint64 addr) {
    struct proc *p = myproc();
    struct stat st;

    if (f->type == FD_INODE || f->type == FD_DEVICE) {
        ilock(f->ip);
        stati(f->ip, &st);
        iunlock(f->ip);

        int rc = copyout(p->pagetable, addr, (char *)&st, sizeof(st));
        if (rc < 0) {
            return -1;
        }

        return 0;
    }

    return -1;
}

// Read from file f.
// addr is a user virtual address.
int fileread(struct file *f, uint64 addr, int n) {
    int r = 0;

    if (f->readable == 0) {
        return -1;
    }

    if (f->type == FD_PIPE) {
        r = piperead(f->pipe, addr, n);
    } else if (f->type == FD_DEVICE) {
        if (f->major < 0 || f->major >= NDEV || !devsw[f->major].read) {
            return -1;
        }
        r = devsw[f->major].read(1, addr, n);
    } else if (f->type == FD_INODE) {
        ilock(f->ip);
        r = readi(
            f->ip,      // inode
            1,          // user_dst
            addr,       // dst
            f->off,     // offset
            n           // n
        );
        if (r > 0) {
            f->off += r;
        }
        iunlock(f->ip);
    }
#ifdef LAB_NET
    else if (f->type == FD_SOCK) {
        r = sockread(f->sock, addr, n);
    }
#endif
    else {
        panic("fileread");
    }

    return r;
}

// Write to file f.
// addr is a user virtual address.
int filewrite(struct file *f, uint64 addr, int n) {
    int ret = 0;

    if (f->writable == 0) {
        return -1;
    }

    if (f->type == FD_PIPE) {
        ret = pipewrite(f->pipe, addr, n);
    } else if (f->type == FD_DEVICE) {
        if (f->major < 0 || f->major >= NDEV || !devsw[f->major].write) {
            return -1;
        }
        ret = devsw[f->major].write(1, addr, n);
    } else if (f->type == FD_INODE) {
        // write a few blocks at a time to avoid exceeding
        // the maximum log transaction size, including
        // i-node, indirect block, allocation blocks,
        // and 2 blocks of slop for non-aligned writes.
        // this really belongs lower down, since writei()
        // might be writing a device like the console.
        int max = ((MAXOPBLOCKS - 1 - 1 - 2) / 2) * BSIZE;
        int total_bytes_written = 0;
        while (total_bytes_written < n) {
            int num_bytes_to_write = n - total_bytes_written;
            if (num_bytes_to_write > max) {
                num_bytes_to_write = max;
            }

            begin_op();

            ilock(f->ip);

            int bytes_written = writei(
                f->ip,                          // inode
                1,                              // user_src
                addr + total_bytes_written,     // src
                f->off,                         // offset
                num_bytes_to_write              // n
            );
            if (bytes_written > 0) {
                f->off += bytes_written;
            }

            iunlock(f->ip);

            end_op();

            if (bytes_written != num_bytes_to_write) {
                // error from writei
                break;
            }

            total_bytes_written += bytes_written;
        }

        ret = (total_bytes_written == n ? n : -1);
    }
#ifdef LAB_NET
    else if (f->type == FD_SOCK) {
        ret = sockwrite(f->sock, addr, n);
    }
#endif
    else {
        panic("filewrite");
    }

    return ret;
}
