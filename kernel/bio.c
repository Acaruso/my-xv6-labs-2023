// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define BUF_TABLE_SIZE 13

struct bcache {
    struct buf buf_arr[NBUF];
    struct buf buf_table[BUF_TABLE_SIZE];
    struct spinlock buf_table_locks[BUF_TABLE_SIZE];
    struct spinlock scan_lock;
};

struct bcache bcache;

void binit(void) {
    // printf("binit\n");
    for (int i = 0; i < BUF_TABLE_SIZE; i++) {
        bcache.buf_table[i].prev = 0;
        bcache.buf_table[i].next = 0;
        initlock(&bcache.buf_table_locks[i], "bcache");
    }

    for (int i = 0; i < NBUF; i++) {
        bcache.buf_arr[i].prev = 0;
        bcache.buf_arr[i].next = 0;
        initsleeplock(&bcache.buf_arr[i].lock, "buffer");
    }

    initlock(&bcache.scan_lock, "bcache_scan");
}

// static struct buf *bget(uint dev, uint blockno) {
//     printf("bget 1\n");
//     uint table_idx = blockno % BUF_TABLE_SIZE;

//     acquire(&bcache.buf_table_locks[table_idx]);
//     printf("bget 2\n");

//     // search for data in buf_table
//     struct buf *b = &bcache.buf_table[table_idx];

//     if (b->next != 0) {     // if there are more bufs besides just the head
//         printf("bget 3\n");

//         b = b->next;
//         while (b != 0) {
//             printf("bget 3.5\n");
//             if (b->dev == dev && b->blockno == blockno) {
//                 b->refcnt++;
//                 release(&bcache.buf_table_locks[table_idx]);
//                 acquiresleep(&b->lock);
//                 return b;
//             }
//             b = b->next;
//         }
//     }

//     // we couldn't find data in `bcache.buf_table`
//     // loop over `bcache.buf_arr` looking for a free buffer

//     release(&bcache.buf_table_locks[table_idx]);

//     printf("bget 4\n");

//     for (int i = 0; i < NBUF; i++) {
//         b = &bcache.buf_arr[i];

//         acquiresleep(&b->lock);
//         printf("bget 5\n");

//         if (b->refcnt == 0) {
//             b->dev = dev;
//             b->blockno = blockno;
//             b->valid = 0;
//             b->refcnt = 1;

//             releasesleep(&b->lock);
//             printf("bget 6\n");

//             acquire(&bcache.buf_table_locks[table_idx]);
//             printf("bget 7\n");

//             // add b to buf_table
//             struct buf *cur = &bcache.buf_table[table_idx];
//             while (cur->next != 0) {
//                 printf("bget 7.5\n");

//                 cur = cur->next;
//             }
//             cur->next = b;
//             b->prev = cur;
//             b->next = 0;

//             release(&bcache.buf_table_locks[table_idx]);
//             printf("bget 8\n");

//             acquiresleep(&b->lock);
//             printf("bget 9\n");

//             return b;
//         }

//         releasesleep(&b->lock);
//         printf("bget 10\n");
//     }

//     panic("bget: no buffers");
// }

static struct buf *bget(uint dev, uint blockno) {
    // printf("bget 1\n");
    uint table_idx = blockno % BUF_TABLE_SIZE;

    acquire(&bcache.buf_table_locks[table_idx]);
    // printf("bget 2\n");

    // search for data in buf_table
    struct buf *b = &bcache.buf_table[table_idx];

    // recall that b is the dummy head. so we must advance by one
    b = b->next;

    while (b != 0) {
        // printf("bget 3.5\n");
        if (b->dev == dev && b->blockno == blockno) {
            // printf("found\n");
            b->refcnt++;
            release(&bcache.buf_table_locks[table_idx]);
            acquiresleep(&b->lock);
            return b;
        }
        b = b->next;
    }

    // printf("didn't find\n");

    // we couldn't find data in `bcache.buf_table`
    // loop over `bcache.buf_arr` looking for a free buffer

    release(&bcache.buf_table_locks[table_idx]);

    acquire(&bcache.scan_lock);

    for (int i = 0; i < NBUF; i++) {
        b = &bcache.buf_arr[i];

        if (b->refcnt == 0) {
            b->dev = dev;
            b->blockno = blockno;
            b->valid = 0;
            b->refcnt = 1;

            release(&bcache.scan_lock);

            acquire(&bcache.buf_table_locks[table_idx]);

            // add b to buf_table
            struct buf *cur = &bcache.buf_table[table_idx];
            while (cur->next != 0) {
                cur = cur->next;
            }
            cur->next = b;
            b->prev = cur;
            b->next = 0;

            release(&bcache.buf_table_locks[table_idx]);

            acquiresleep(&b->lock);

            return b;
        }
    }

    panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf *bread(uint dev, uint blockno) {
    struct buf *b;

    b = bget(dev, blockno);
    if (!b->valid) {
        virtio_disk_rw(b, 0);
        b->valid = 1;
    }
    return b;
}

// Write b's contents to disk.  Must be locked.
void bwrite(struct buf *b) {
    if (!holdingsleep(&b->lock)) panic("bwrite");
    virtio_disk_rw(b, 1);
}

// Release a locked buffer
// void brelse(struct buf *b) {
//     // printf("brelse\n");

//     if (!holdingsleep(&b->lock)) {
//         panic("brelse");
//     }

//     uint table_idx = b->blockno % BUF_TABLE_SIZE;

//     releasesleep(&b->lock);

//     acquire(&bcache.buf_table_locks[table_idx]);

//     b->refcnt--;

//     if (b->refcnt == 0) {
//         struct buf *l = b->prev;
//         struct buf *r = b->next;
//         if (l != 0) {
//             l->next = r;
//         }
//         if (r != 0) {
//             r->prev = l;
//         }
//         b->prev = 0;
//         b->next = 0;
//     }

//     release(&bcache.buf_table_locks[table_idx]);
// }

void brelse(struct buf *b) {
    // printf("brelse 1\n");

    if (!holdingsleep(&b->lock)) {
        panic("brelse");
    }

    uint table_idx = b->blockno % BUF_TABLE_SIZE;

    releasesleep(&b->lock);
    // printf("brelse 2\n");

    acquire(&bcache.buf_table_locks[table_idx]);
    // printf("brelse 3\n");

    // use b->refcnt as a pseudo-lock
    // if b->refcnt is about to become 0, do all other operations first, and then do b->refcnt--
    // note that we are not holding b->lock at this point
    if (b->refcnt == 1) {
        // printf("brelse 4\n");

        struct buf *l = b->prev;
        struct buf *r = b->next;
        if (l != 0) {
            l->next = r;
        }
        if (r != 0) {
            r->prev = l;
        }
        b->prev = 0;
        b->next = 0;

        __sync_synchronize();

        b->refcnt--;
    } else {
        b->refcnt--;
        // printf("brelse 5\n");
    }

    release(&bcache.buf_table_locks[table_idx]);

    // printf("brelse 6\n");
}

void bpin(struct buf *b) {
    uint table_idx = b->blockno % BUF_TABLE_SIZE;
    acquire(&bcache.buf_table_locks[table_idx]);
    b->refcnt++;
    release(&bcache.buf_table_locks[table_idx]);
}

void bunpin(struct buf *b) {
    uint table_idx = b->blockno % BUF_TABLE_SIZE;
    acquire(&bcache.buf_table_locks[table_idx]);
    b->refcnt--;
    release(&bcache.buf_table_locks[table_idx]);
}
