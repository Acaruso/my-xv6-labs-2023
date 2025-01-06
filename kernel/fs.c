// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb;

// Read the super block.
static void readsb(int dev, struct superblock *sb) {
    struct buf *bp;

    bp = bread(dev, 1);
    memmove(sb, bp->data, sizeof(*sb));
    brelse(bp);
}

// Init fs
void fsinit(int dev) {
    readsb(dev, &sb);
    if (sb.magic != FSMAGIC) panic("invalid file system");
    initlog(dev, &sb);
}

// Zero a block.
static void bzero(int dev, int bno) {
    struct buf *bp;

    bp = bread(dev, bno);
    memset(bp->data, 0, BSIZE);
    log_write(bp);
    brelse(bp);
}

// Blocks.

// Allocate a zeroed disk block.
// returns 0 if out of disk space.
static uint balloc(uint dev) {
    struct buf *buf = 0;

    for (int i = 0; i < sb.size; i += BPB) {
        buf = bread(dev, BBLOCK(i, sb));
        for (int k = 0; k < BPB && i + k < sb.size; k++) {
            int m = 1 << (k % 8);
            if ((buf->data[k / 8] & m) == 0) {  // Is block free?
                buf->data[k / 8] |= m;          // Mark block in use.
                log_write(buf);
                brelse(buf);
                bzero(dev, i + k);
                return i + k;
            }
        }
        brelse(buf);
    }
    printf("balloc: out of blocks\n");
    return 0;
}

// Free a disk block.
static void bfree(int dev, uint b) {
    struct buf *buf;
    buf = bread(dev, BBLOCK(b, sb));

    int i = b % BPB;
    int m = 1 << (i % 8);

    if ((buf->data[i / 8] & m) == 0) {
        panic("freeing free block");
    }

    buf->data[i / 8] &= ~m;
    log_write(buf);
    brelse(buf);
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at block
// sb.inodestart. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a table of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The in-memory
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in table: an entry in the inode table
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a table entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   table entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays in the table and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The itable.lock spin-lock protects the allocation of itable
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold itable.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

struct {
    struct spinlock lock;
    struct inode inode[NINODE];
} itable;

void iinit() {
    int i = 0;

    initlock(&itable.lock, "itable");
    for (i = 0; i < NINODE; i++) {
        initsleeplock(&itable.inode[i].lock, "inode");
    }
}

static struct inode *iget(uint dev, uint inum);

// Allocate an inode on device dev.
// Mark it as allocated by  giving it type type.
// Returns an unlocked but allocated and referenced inode,
// or NULL if there is no free inode.
struct inode *ialloc(uint dev, short type) {
    for (int inum = 1; inum < sb.ninodes; inum++) {
        struct buf *buf = bread(dev, IBLOCK(inum, sb));
        struct dinode *dinode = (struct dinode *)buf->data + (inum % IPB);

        if (dinode->type == 0) {
            memset(dinode, 0, sizeof(*dinode));
            dinode->type = type;
            log_write(buf);
            brelse(buf);
            return iget(dev, inum);
        }

        brelse(buf);
    }

    printf("ialloc: no inodes\n");
    return 0;
}

// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk.
// Caller must hold ip->lock.
void iupdate(struct inode *inode) {
    struct buf *buf = bread(inode->dev, IBLOCK(inode->inum, sb));
    struct dinode *dinode = (struct dinode *)buf->data + (inode->inum % IPB);

    dinode->type = inode->type;
    dinode->major = inode->major;
    dinode->minor = inode->minor;
    dinode->nlink = inode->nlink;
    dinode->size = inode->size;
    memmove(dinode->addrs, inode->addrs, sizeof(inode->addrs));
    log_write(buf);
    brelse(buf);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
static struct inode *iget(uint dev, uint inum) {
    struct inode *inode = 0;
    struct inode *empty = 0;

    acquire(&itable.lock);

    // is the inode already in the table?
    for (inode = &itable.inode[0]; inode < &itable.inode[NINODE]; inode++) {
        if (inode->ref > 0 && inode->dev == dev && inode->inum == inum) {
            inode->ref++;
            release(&itable.lock);
            return inode;
        }
        if (empty == 0 && inode->ref == 0) {   // remember empty slot
            empty = inode;
        }
    }

    // recycle an inode entry
    if (empty == 0) panic("iget: no inodes");

    inode = empty;
    inode->dev = dev;
    inode->inum = inum;
    inode->ref = 1;
    inode->valid = 0;
    release(&itable.lock);

    return inode;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode *idup(struct inode *ip) {
    acquire(&itable.lock);
    ip->ref++;
    release(&itable.lock);
    return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void ilock(struct inode *inode) {
    if (inode == 0 || inode->ref < 1) panic("ilock");

    acquiresleep(&inode->lock);

    if (inode->valid == 0) {
        struct buf *buf = bread(inode->dev, IBLOCK(inode->inum, sb));
        struct dinode *dinode = (struct dinode *)buf->data + (inode->inum % IPB);

        inode->type  = dinode->type;
        inode->major = dinode->major;
        inode->minor = dinode->minor;
        inode->nlink = dinode->nlink;
        inode->size  = dinode->size;

        memmove(inode->addrs, dinode->addrs, sizeof(inode->addrs));

        brelse(buf);

        inode->valid = 1;

        if (inode->type == 0) panic("ilock: no type");
    }
}

// Unlock the given inode.
void iunlock(struct inode *inode) {
    if (inode == 0 || !holdingsleep(&inode->lock) || inode->ref < 1) panic("iunlock");

    releasesleep(&inode->lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode table entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
void iput(struct inode *inode) {
    acquire(&itable.lock);

    if (inode->ref == 1 && inode->valid && inode->nlink == 0) {
        // inode has no links and no other references: truncate and free.

        // ip->ref == 1 means no other process can have ip locked,
        // so this acquiresleep() won't block (or deadlock).
        acquiresleep(&inode->lock);

        release(&itable.lock);

        itrunc(inode);
        inode->type = 0;
        iupdate(inode);
        inode->valid = 0;

        releasesleep(&inode->lock);

        acquire(&itable.lock);
    }

    inode->ref--;
    release(&itable.lock);
}

// Common idiom: unlock, then put.
void iunlockput(struct inode *ip) {
    iunlock(ip);
    iput(ip);
}

// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
// returns 0 if out of disk space.

static uint bmap_direct(struct inode *inode, uint n);
static uint bmap_singly_indirect(struct inode *inode, uint n);
static uint bmap_doubly_indirect(struct inode *inode, uint n);

static uint bmap(struct inode *inode, uint n) {
    if (n < NDIRECT) {
        return bmap_direct(inode, n);
    }

    n -= NDIRECT;

    if (n < NINDIRECT) {
        if (n < N_SINGLY_INDIRECT) {
            return bmap_singly_indirect(inode, n);
        } else {
            n -= (BSIZE / sizeof(uint));
            return bmap_doubly_indirect(inode, n);
        }
    }

    panic("bmap: out of range");
}

static uint bmap_direct(struct inode *inode, uint n) {
    uint nth_block_addr = inode->addrs[n];
    if (nth_block_addr == 0) {
        nth_block_addr = balloc(inode->dev);
        if (nth_block_addr == 0) {
            return 0;
        }
        inode->addrs[n] = nth_block_addr;
    }

    return nth_block_addr;
}

static uint bmap_singly_indirect(struct inode *inode, uint n) {
    uint indirect_block_addr = inode->addrs[SINGLY_INDIRECT_IDX];
    if (indirect_block_addr == 0) {
        indirect_block_addr = balloc(inode->dev);
        if (indirect_block_addr == 0) {
            return 0;
        }
        inode->addrs[SINGLY_INDIRECT_IDX] = indirect_block_addr;
    }

    struct buf *indirect_block_buf = bread(inode->dev, indirect_block_addr);
    uint *indirect_block_data = (uint *)indirect_block_buf->data;

    uint nth_block_addr = indirect_block_data[n];
    if (nth_block_addr == 0) {
        nth_block_addr = balloc(inode->dev);
        if (nth_block_addr == 0) {
            brelse(indirect_block_buf);
            return 0;
        }
        indirect_block_data[n] = nth_block_addr;
        log_write(indirect_block_buf);
    }

    brelse(indirect_block_buf);

    return nth_block_addr;
}

static uint bmap_doubly_indirect(struct inode *inode, uint n) {
    uint level_1_blockno = inode->addrs[DOUBLY_INDIRECT_IDX];
    if (level_1_blockno == 0) {
        level_1_blockno = balloc(inode->dev);
        if (level_1_blockno == 0) {
            return 0;
        }
        inode->addrs[DOUBLY_INDIRECT_IDX] = level_1_blockno;
    }

    struct buf *level_1_buf = bread(inode->dev, level_1_blockno);
    uint *level_1_data = (uint *)level_1_buf->data;

    int level_1_idx = n / 256;

    uint level_2_blockno = level_1_data[level_1_idx];
    if (level_2_blockno == 0) {
        level_2_blockno = balloc(inode->dev);
        if (level_2_blockno == 0) {
            brelse(level_1_buf);
            return 0;
        }
        level_1_data[level_1_idx] = level_2_blockno;
        log_write(level_1_buf);
    }

    struct buf *level_2_buf = bread(inode->dev, level_2_blockno);
    uint *level_2_data = (uint *)level_2_buf->data;

    int level_2_idx = n % 256;

    uint data_block_blockno = level_2_data[level_2_idx];
    if (data_block_blockno == 0) {
        data_block_blockno = balloc(inode->dev);
        if (data_block_blockno == 0) {
            brelse(level_2_buf);
            brelse(level_1_buf);
            return 0;
        }
        level_2_data[level_2_idx] = data_block_blockno;
        log_write(level_2_buf);
    }

    brelse(level_2_buf);
    brelse(level_1_buf);

    return data_block_blockno;
}

// Truncate inode (discard contents).
// Caller must hold ip->lock.
void itrunc(struct inode *inode) {
    for (int i = 0; i < NDIRECT; i++) {
        if (inode->addrs[i] != 0) {
            bfree(inode->dev, inode->addrs[i]);
            inode->addrs[i] = 0;
        }
    }

    // handle singly indirect block
    if (inode->addrs[SINGLY_INDIRECT_IDX] != 0) {
        struct buf *buf = bread(inode->dev, inode->addrs[SINGLY_INDIRECT_IDX]);
        uint *buf_data = (uint *)buf->data;
        for (int i = 0; i < N_SINGLY_INDIRECT; i++) {
            if (buf_data[i] != 0) {
                bfree(inode->dev, buf_data[i]);
            }
        }
        brelse(buf);
        bfree(inode->dev, inode->addrs[SINGLY_INDIRECT_IDX]);
        inode->addrs[SINGLY_INDIRECT_IDX] = 0;
    }

    // handle doubly indirect block
    if (inode->addrs[DOUBLY_INDIRECT_IDX] != 0) {
        struct buf *level_1_buf = bread(inode->dev, inode->addrs[DOUBLY_INDIRECT_IDX]);
        uint *level_1_data = (uint *)level_1_buf->data;

        for (int i = 0; i < BLOCKNOS_PER_BLOCK; i++) {
            if (level_1_data[i] != 0) {
                struct buf *level_2_buf = bread(inode->dev, level_1_data[i]);
                uint *level_2_data = (uint *)level_2_buf->data;

                for (int k = 0; k < BLOCKNOS_PER_BLOCK; k++) {
                    if (level_2_data[k] != 0) {
                        bfree(inode->dev, level_2_data[k]);
                    }
                }

                brelse(level_2_buf);
                bfree(inode->dev, level_1_data[i]);
            }
        }

        brelse(level_1_buf);
        bfree(inode->dev, inode->addrs[DOUBLY_INDIRECT_IDX]);
        inode->addrs[DOUBLY_INDIRECT_IDX] = 0;
    }

    inode->size = 0;
    iupdate(inode);
}

// Copy stat information from inode.
// Caller must hold ip->lock.
void stati(struct inode *inode, struct stat *stat) {
    stat->dev   = inode->dev;
    stat->ino   = inode->inum;
    stat->type  = inode->type;
    stat->nlink = inode->nlink;
    stat->size  = inode->size;
}

// Read data from inode.
// Caller must hold ip->lock.
// If user_dst == 1, then dst is a user virtual address;
// otherwise, dst is a kernel address.
int readi(struct inode *inode, int user_dst, uint64 dst, uint offset, uint n) {
    if (offset > inode->size || offset + n < offset) {
        return 0;
    }

    if (offset + n > inode->size) {
        n = inode->size - offset;
    }

    uint total;
    uint to_read;
    for (
        total = 0, to_read = 0;
        total < n;
        total += to_read, offset += to_read, dst += to_read
    ) {
        // get the blockno of the block that offset is inside
        uint block_no = bmap(inode, offset / BSIZE);
        if (block_no == 0) {
            break;
        }

        struct buf *buf = bread(inode->dev, block_no);

        to_read = min(n - total, BSIZE - (offset % BSIZE));

        int rc = either_copyout(user_dst, dst, buf->data + (offset % BSIZE), to_read);
        if (rc == -1) {
            brelse(buf);
            total = -1;
            break;
        }

        brelse(buf);
    }

    return total;
}

// Write data to inode.
// Caller must hold ip->lock.
// If user_src==1, then src is a user virtual address;
// otherwise, src is a kernel address.
// Returns the number of bytes successfully written.
// If the return value is less than the requested n,
// there was an error of some kind.
int writei(struct inode *inode, int user_src, uint64 src, uint offset, uint n) {
    if (offset > inode->size || offset + n < offset) {
        return -1;
    }

    if (offset + n > MAXFILE * BSIZE) {
        return -1;
    }

    uint total, m;
    for (
        total = 0;
        total < n;
        total += m, offset += m, src += m
    ) {
        uint block_no = bmap(inode, offset / BSIZE);
        if (block_no == 0) {
            break;
        }
        struct buf *buf = bread(inode->dev, block_no);

        m = min(n - total, BSIZE - offset % BSIZE);

        if (either_copyin(buf->data + (offset % BSIZE), user_src, src, m) == -1) {
            brelse(buf);
            break;
        }

        log_write(buf);
        brelse(buf);
    }

    if (offset > inode->size) {
        inode->size = offset;
    }

    // write the i-node back to disk even if the size didn't change
    // because the loop above might have called bmap() and added a new
    // block to ip->addrs[].
    iupdate(inode);

    return total;
}

// Directories

int namecmp(const char *s, const char *t) { return strncmp(s, t, DIRSIZ); }

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode *dirlookup(struct inode *dir, char *name, uint *poff) {
    if (dir->type != T_DIR) {
        panic("dirlookup not DIR");
    }

    struct dirent dirent;

    for (uint offset = 0; offset < dir->size; offset += sizeof(dirent)) {
        int rc = readi(
            dir,                // inode
            0,                  // user_dst
            (uint64)&dirent,    // dst
            offset,             // offset
            sizeof(dirent)      // n
        );
        if (rc != sizeof(dirent)) {
            panic("dirlookup read");
        }

        if (dirent.inum == 0) {
            continue;
        }

        if (namecmp(name, dirent.name) == 0) {
            // entry matches path element
            if (poff) {
                *poff = offset;
            }
            uint inum = dirent.inum;
            return iget(dir->dev, inum);
        }
    }

    return 0;
}

// Write a new directory entry (name, inum) into the directory `dir`.
// Returns 0 on success, -1 on failure (e.g. out of disk blocks).
int dirlink(struct inode *dir, char *name, uint inum) {
    struct dirent dirent;
    struct inode *ip;
    int rc;

    // check that `name` is not present
    ip = dirlookup(dir, name, 0);
    if (ip != 0) {
        iput(ip);
        return -1;
    }

    // find an empty dirent in `dir`s data blocks
    int offset;
    for (offset = 0; offset < dir->size; offset += sizeof(dirent)) {
        rc = readi(
            dir,                // inode
            0,                  // user_dst
            (uint64)&dirent,    // dst
            offset,             // offset
            sizeof(dirent)      // n
        );
        if (rc != sizeof(dirent)) {
            panic("dirlink read");
        }
        if (dirent.inum == 0) {
            break;
        }
    }

    // copy `name` and `inum` into the dirent
    strncpy(dirent.name, name, DIRSIZ);
    dirent.inum = inum;

    // write the dirent back into `dir`s data block
    rc = writei(
        dir,                // inode
        0,                  // user_src
        (uint64)&dirent,    // src
        offset,             // offset
        sizeof(dirent)      // n
    );
    if (rc != sizeof(dirent)) {
        return -1;
    }

    return 0;
}

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char *skipelem(char *path, char *name) {
    char *s;
    int len;

    while (*path == '/') {
        path++;
    }

    if (*path == 0) {
        return 0;
    }

    s = path;

    while (*path != '/' && *path != 0) {
        path++;
    }

    len = path - s;

    if (len >= DIRSIZ) {
        memmove(
            name,       // dest
            s,          // src
            DIRSIZ      // n
        );
    } else {
        memmove(
            name,       // dest
            s,          // src
            len         // n
        );
        name[len] = 0;
    }

    while (*path == '/') {
        path++;
    }

    return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode *namex(char *path, int nameiparent, char *name) {
    struct inode *ip;
    if (*path == '/') {
        ip = iget(ROOTDEV, ROOTINO);
    } else {
        ip = idup(myproc()->cwd);
    }

    struct inode *next;
    path = skipelem(path, name);
    while (path != 0) {
        ilock(ip);

        if (ip->type != T_DIR) {
            iunlockput(ip);
            return 0;
        }

        if (nameiparent && *path == '\0') {
            // Stop one level early.
            iunlock(ip);
            return ip;
        }

        next = dirlookup(ip, name, 0);
        if (next == 0) {
            iunlockput(ip);
            return 0;
        }

        iunlockput(ip);

        ip = next;
        path = skipelem(path, name);
    }

    if (nameiparent) {
        iput(ip);
        return 0;
    }

    return ip;
}

struct inode *namei(char *path) {
    char name[DIRSIZ];
    return namex(path, 0, name);
}

struct inode *nameiparent(char *path, char *name) {
    return namex(path, 1, name);
}
