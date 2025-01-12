//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int argfd(int n, int *pfd, struct file **pf) {
    int fd;
    struct file *f;

    argint(n, &fd);
    if (fd < 0 || fd >= NOFILE || (f = myproc()->ofile[fd]) == 0) return -1;
    if (pfd) *pfd = fd;
    if (pf) *pf = f;
    return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int fdalloc(struct file *f) {
    int fd;
    struct proc *p = myproc();

    for (fd = 0; fd < NOFILE; fd++) {
        if (p->ofile[fd] == 0) {
            p->ofile[fd] = f;
            return fd;
        }
    }
    return -1;
}

uint64 sys_dup(void) {
    struct file *f;

    int rc = argfd(0, 0, &f);
    if (rc < 0) {
        return -1;
    }

    int fd = fdalloc(f);
    if (fd < 0) {
        return -1;
    }

    filedup(f);

    return fd;
}

uint64 sys_read(void) {
    struct file *f;
    int n;
    uint64 p;

    argaddr(1, &p);
    argint(2, &n);
    if (argfd(0, 0, &f) < 0) return -1;
    return fileread(f, p, n);
}

uint64 sys_write(void) {
    struct file *f;
    int n;
    uint64 p;

    argaddr(1, &p);
    argint(2, &n);
    if (argfd(0, 0, &f) < 0) return -1;

    return filewrite(f, p, n);
}

uint64 sys_close(void) {
    int fd;
    struct file *f;

    if (argfd(0, &fd, &f) < 0) return -1;
    myproc()->ofile[fd] = 0;
    fileclose(f);
    return 0;
}

uint64 sys_fstat(void) {
    struct file *f;
    uint64 st;  // user pointer to struct stat

    argaddr(1, &st);
    if (argfd(0, 0, &f) < 0) return -1;
    return filestat(f, st);
}

// Create the path `new_path` as a link to the same inode as `old_path`
uint64 sys_link(void) {
    char old_path[MAXPATH];
    char new_path[MAXPATH];

    if (argstr(0, old_path, MAXPATH) < 0 || argstr(1, new_path, MAXPATH) < 0) {
        return -1;
    }

    begin_op();

    struct inode *old_inode = namei(old_path);
    if (old_inode == 0) {
        end_op();
        return -1;
    }

    ilock(old_inode);

    if (old_inode->type == T_DIR) {
        iunlockput(old_inode);
        end_op();
        return -1;
    }

    old_inode->nlink++;

    iupdate(old_inode);

    iunlock(old_inode);

    // return the parent inode of `new_path`, and copy the last path element
    // into `new_path_filename`
    char new_path_filename[DIRSIZ];
    struct inode *new_inode_parent = nameiparent(new_path, new_path_filename);
    if (new_inode_parent == 0) {
        goto bad;
    }

    ilock(new_inode_parent);

    // create a new dirent in `new_inode_parent` with name `new_path_filename` and inum `old_inode->inum`
    int rc = dirlink(new_inode_parent, new_path_filename, old_inode->inum);
    if (new_inode_parent->dev != old_inode->dev || rc < 0) {
        iunlockput(new_inode_parent);
        goto bad;
    }

    iunlockput(new_inode_parent);

    iput(old_inode);

    end_op();

    return 0;

bad:
    ilock(old_inode);
    old_inode->nlink--;
    iupdate(old_inode);
    iunlockput(old_inode);
    end_op();
    return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int isdirempty(struct inode *dp) {
    int off;
    struct dirent de;

    for (off = 2 * sizeof(de); off < dp->size; off += sizeof(de)) {
        if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de)) panic("isdirempty: readi");
        if (de.inum != 0) return 0;
    }
    return 1;
}

uint64 sys_unlink(void) {
    char path[MAXPATH];
    if (argstr(0, path, MAXPATH) < 0) {
        return -1;
    }

    begin_op();

    // return parent inode of `path` and copy last path element into `name`
    char name[DIRSIZ];
    struct inode *parent_inode = nameiparent(path, name);
    if (parent_inode == 0) {
        end_op();
        return -1;
    }

    ilock(parent_inode);

    // Cannot unlink "." or "..".
    if (namecmp(name, ".") == 0 || namecmp(name, "..") == 0) {
        goto bad;
    }

    // return inode for `name` by looking up its dirent in `parent_inode`s data blocks
    // write the offset of that dirent into `off`
    uint off;
    struct inode *child_inode = dirlookup(parent_inode, name, &off);
    if (child_inode == 0) {
        goto bad;
    }

    ilock(child_inode);

    if (child_inode->nlink < 1) {
        panic("unlink: nlink < 1");
    }

    if (child_inode->type == T_DIR && !isdirempty(child_inode)) {
        iunlockput(child_inode);
        goto bad;
    }

    // zero out dirent
    struct dirent dirent;
    memset(&dirent, 0, sizeof(dirent));
    int rc = writei(
        parent_inode,       // inode
        0,                  // user_src
        (uint64)&dirent,    // src
        off,                // offset
        sizeof(dirent)      // n
    );
    if (rc != sizeof(dirent)) {
        panic("unlink: writei");
    }

    if (child_inode->type == T_DIR) {
        parent_inode->nlink--;
        iupdate(parent_inode);
    }

    iunlockput(parent_inode);

    child_inode->nlink--;
    iupdate(child_inode);
    iunlockput(child_inode);

    end_op();

    return 0;

bad:
    iunlockput(parent_inode);
    end_op();
    return -1;
}

static struct inode *create(char *path, short type, short major, short minor) {
    char name[DIRSIZ];
    int rc = 0;

    // return inode at second to last path element
    // write last path element into `name`
    // example:
    //   path = /a/b/c.txt
    //   return inode at `b`, write `"c.txt"` into `name`
    struct inode *parent_inode = nameiparent(path, name);
    if (parent_inode == 0) {
        return 0;
    }

    ilock(parent_inode);

    struct inode *child_inode = dirlookup(parent_inode, name, 0);
    if (child_inode != 0) {         // child already exists
        iunlockput(parent_inode);
        ilock(child_inode);
        if (type == T_FILE && (child_inode->type == T_FILE || child_inode->type == T_DEVICE)) {
            return child_inode;
        }
        iunlockput(child_inode);
        return 0;
    }

    child_inode = ialloc(parent_inode->dev, type);
    if (child_inode == 0) {
        iunlockput(parent_inode);
        return 0;
    }

    ilock(child_inode);
    child_inode->major = major;
    child_inode->minor = minor;
    child_inode->nlink = 1;
    iupdate(child_inode);

    if (type == T_DIR) {  // create "." and ".." entries
        // don't do `ip->nlink++` for "." to avoid cyclic ref count
        rc = dirlink(child_inode, ".", child_inode->inum);
        if (rc < 0) {
            goto fail;
        }

        rc = dirlink(child_inode, "..", parent_inode->inum);
        if (rc < 0) {
            goto fail;
        }
    }

    rc = dirlink(parent_inode, name, child_inode->inum);
    if (rc < 0) {
        goto fail;
    }

    if (type == T_DIR) {
        // now that success is guaranteed:
        parent_inode->nlink++;  // for ".."
        iupdate(parent_inode);
    }

    iunlockput(parent_inode);

    return child_inode;

fail:
    // something went wrong. de-allocate ip.
    child_inode->nlink = 0;
    iupdate(child_inode);
    iunlockput(child_inode);
    iunlockput(parent_inode);
    return 0;
}

struct inode *follow_symlink(struct inode *inode);

uint64 sys_open(void) {
    char path[MAXPATH];
    if (argstr(0, path, MAXPATH) < 0) {
        return -1;
    }

    int omode;
    argint(1, &omode);

    begin_op();

    struct inode *inode;
    if (omode & O_CREATE) {
        inode = create(path, T_FILE, 0, 0);     // returns inode with lock held
        if (inode == 0) {
            end_op();
            return -1;
        }
    } else {
        inode = namei(path);
        if (inode == 0) {
            end_op();
            return -1;
        }
        ilock(inode);
        if (inode->type == T_DIR && omode != O_RDONLY) {
            iunlockput(inode);
            end_op();
            return -1;
        }
    }

    if (
        inode->type == T_DEVICE
        && (inode->major < 0 || inode->major >= NDEV)
    ) {
        iunlockput(inode);
        end_op();
        return -1;
    }

    if (inode->type == T_SYMLINK && (omode & O_NOFOLLOW) == 0) {
        inode = follow_symlink(inode);  // return inode with lock held
        // if `follow_symlink` failed, it also unlocked `inode`, so we don't need to unlock it
        if (inode == 0) {
            end_op();
            return -1;
        }
    }

    struct file *file = filealloc();
    int fd = fdalloc(file);
    if (file == 0 || fd < 0) {
        if (file) {
            fileclose(file);
        }
        iunlockput(inode);
        end_op();
        return -1;
    }

    if (inode->type == T_DEVICE) {
        file->type = FD_DEVICE;
        file->major = inode->major;
    } else {
        file->type = FD_INODE;
        file->off = 0;
    }

    file->ip = inode;
    file->readable = !(omode & O_WRONLY);
    file->writable = (omode & O_WRONLY) || (omode & O_RDWR);

    if ((omode & O_TRUNC) && inode->type == T_FILE) {
        itrunc(inode);
    }

    iunlock(inode);
    end_op();

    return fd;
}

// return inode with lock held
struct inode *follow_symlink(struct inode *inode) {
    char path[MAXPATH];
    int num_iterations = 0;

    while (1) {
        int rc = readi(
            inode,          // inode
            0,              // user_dst
            (uint64)path,   // dst
            0,              // offset
            MAXPATH         // n
        );
        if (rc == 0) {
            iunlockput(inode);
            return 0;
        }

        iunlockput(inode);

        inode = namei(path);  // returns inode without lock held
        if (inode == 0) {
            return 0;
        }

        ilock(inode);

        if (inode->type != T_SYMLINK) {
            return inode;
        }

        num_iterations++;
        if (num_iterations > 10) {
            iunlockput(inode);
            break;
        }
    }

    return 0;
}

uint64 sys_mkdir(void) {
    char path[MAXPATH];

    begin_op();

    if (argstr(0, path, MAXPATH) < 0) {
        end_op();
        return -1;
    }

    struct inode *ip = create(path, T_DIR, 0, 0);
    if (ip == 0) {
        end_op();
        return -1;
    }

    iunlockput(ip);
    end_op();
    return 0;
}

uint64 sys_mknod(void) {
    int major;
    int minor;
    char path[MAXPATH];

    begin_op();

    argint(1, &major);
    argint(2, &minor);

    if ((argstr(0, path, MAXPATH)) < 0) {
        end_op();
        return -1;
    }

    struct inode *ip = create(path, T_DEVICE, major, minor);
    if (ip == 0) {
        end_op();
        return -1;
    }

    iunlockput(ip);
    end_op();
    return 0;
}

uint64 sys_chdir(void) {
    char path[MAXPATH];
    struct inode *ip;
    struct proc *p = myproc();

    begin_op();
    if (argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0) {
        end_op();
        return -1;
    }
    ilock(ip);
    if (ip->type != T_DIR) {
        iunlockput(ip);
        end_op();
        return -1;
    }
    iunlock(ip);
    iput(p->cwd);
    end_op();
    p->cwd = ip;
    return 0;
}

uint64 sys_exec(void) {
    char path[MAXPATH], *argv[MAXARG];
    int i;
    uint64 uargv, uarg;

    argaddr(1, &uargv);
    if (argstr(0, path, MAXPATH) < 0) {
        return -1;
    }
    memset(argv, 0, sizeof(argv));
    for (i = 0;; i++) {
        if (i >= NELEM(argv)) {
            goto bad;
        }
        if (fetchaddr(uargv + sizeof(uint64) * i, (uint64 *)&uarg) < 0) {
            goto bad;
        }
        if (uarg == 0) {
            argv[i] = 0;
            break;
        }
        argv[i] = kalloc();
        if (argv[i] == 0) goto bad;
        if (fetchstr(uarg, argv[i], PGSIZE) < 0) goto bad;
    }

    int ret = exec(path, argv);

    for (i = 0; i < NELEM(argv) && argv[i] != 0; i++) kfree(argv[i]);

    return ret;

bad:
    for (i = 0; i < NELEM(argv) && argv[i] != 0; i++) kfree(argv[i]);
    return -1;
}

uint64 sys_pipe(void) {
    uint64 fdarray;         // user pointer to array of two integers
    argaddr(0, &fdarray);

    struct file *rf;
    struct file *wf;
    int rc = pipealloc(&rf, &wf);
    if (rc < 0) {
        return -1;
    }

    struct proc *p = myproc();
    int fd0 = fdalloc(rf);
    int fd1 = fdalloc(wf);
    if (fd0 < 0 || fd1 < 0) {
        if (fd0 >= 0) {
            p->ofile[fd0] = 0;
        }
        fileclose(rf);
        fileclose(wf);
        return -1;
    }

    int rc1 = copyout(
        p->pagetable,               // pagetable
        fdarray,                    // dstva
        (char *)&fd0,               // src
        sizeof(fd0)                 // length
    );
    int rc2 = copyout(
        p->pagetable,               // pagetable
        fdarray + sizeof(fd0),      // dstva
        (char *)&fd1,               // src
        sizeof(fd1)                 // length
    );
    if (rc1 < 0 || rc2 < 0) {
        p->ofile[fd0] = 0;
        p->ofile[fd1] = 0;
        fileclose(rf);
        fileclose(wf);
        return -1;
    }

    return 0;
}

// symlink(target, path)
// creates a new symlink at location `path` that points to the file at `target`
int sys_symlink(void) {
    char target[MAXPATH];
    if (argstr(0, target, MAXPATH) < 0) {
        return -1;
    }

    char path[MAXPATH];
    if (argstr(1, path, MAXPATH) < 0) {
        return -1;
    }

    begin_op();

    // create new inode to represent the symlink
    struct inode *new_inode = create(path, T_SYMLINK, 0, 0);    // returns inode with lock held
    if (new_inode == 0) {
        end_op();
        return -1;
    }

    // write `target` to `new_inode`s data block at offset `0`
    int rc = writei(
        new_inode,         // inode
        0,                 // user_src
        (uint64)target,    // src
        0,                 // offset
        MAXPATH            // n
    );
    if (rc != MAXPATH) {
        end_op();
        return -1;
    }

    iunlockput(new_inode);
    end_op();

    return 0;
}

#ifdef LAB_NET
int sys_connect(void) {
    struct file *f;
    int fd;
    uint32 raddr;
    uint32 rport;
    uint32 lport;

    argint(0, (int *)&raddr);
    argint(1, (int *)&lport);
    argint(2, (int *)&rport);

    if (sockalloc(&f, raddr, lport, rport) < 0) return -1;
    if ((fd = fdalloc(f)) < 0) {
        fileclose(f);
        return -1;
    }

    return fd;
}
#endif
