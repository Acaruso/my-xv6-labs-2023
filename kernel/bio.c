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
};

struct bcache bcache;

void move_to_bucket(int table_idx, struct buf *b);

void binit(void) {
    for (int i = 0; i < BUF_TABLE_SIZE; i++) {
        bcache.buf_table[i].prev = 0;
        bcache.buf_table[i].next = 0;
        initlock(&bcache.buf_table_locks[i], "buf_table_lock");
    }

    for (int i = 0; i < NBUF; i++) {
        bcache.buf_arr[i].prev = 0;
        bcache.buf_arr[i].next = 0;
        bcache.buf_arr[i].refcnt = 0;
        bcache.buf_arr[i].valid = 0;
        bcache.buf_arr[i].dev = 0;
        bcache.buf_arr[i].blockno = 0;
        bcache.buf_arr[i].disk = 0;
        initsleeplock(&bcache.buf_arr[i].lock, "buffer");
        move_to_bucket(0, &bcache.buf_arr[i]);
    }
}

static struct buf *bget(uint dev, uint blockno) {
    uint table_idx = blockno % BUF_TABLE_SIZE;

    acquire(&bcache.buf_table_locks[table_idx]);

    // search the cache for the data
    struct buf *b = bcache.buf_table[table_idx].next;
    while (b != 0) {
        if (b->dev == dev && b->blockno == blockno) {
            b->refcnt++;
            release(&bcache.buf_table_locks[table_idx]);
            acquiresleep(&b->lock);
            return b;
        }
        b = b->next;
    }

    // if we got to this point, we didn't find the data in the cache.
    // now we need to find and return a free buf.
    // search the current bucket for a free buf.
    b = bcache.buf_table[table_idx].next;
    while (b != 0) {
        if (b->refcnt == 0) {
            b->dev = dev;
            b->blockno = blockno;
            b->valid = 0;
            b->refcnt = 1;
            release(&bcache.buf_table_locks[table_idx]);
            acquiresleep(&b->lock);
            return b;
        }
        b = b->next;
    }

    // if we got to this point, we didn't find a free buf in the current bucket.
    // search the other buckets for a free buf and move it into the current bucket.
    release(&bcache.buf_table_locks[table_idx]);

    for (int i = 1; i < BUF_TABLE_SIZE; i++) {
        uint victim_table_idx = (blockno + i) % BUF_TABLE_SIZE;

        uint smaller_idx = victim_table_idx < table_idx ? victim_table_idx : table_idx;
        uint larger_idx  = victim_table_idx > table_idx ? victim_table_idx : table_idx;

        acquire(&bcache.buf_table_locks[smaller_idx]);
        acquire(&bcache.buf_table_locks[larger_idx]);

        // re-check table_idx to see if a duplicate block was added to `bcache.buf_table_locks[table_idx]`
        // in the time between `release(&bcache.buf_table_locks[table_idx])`
        // and `acquire(&bcache.buf_table_locks[table_idx])`.
        for (b = bcache.buf_table[table_idx].next; b != 0; b = b->next) {
            if (b->dev == dev && b->blockno == blockno) {
                b->refcnt++;
                release(&bcache.buf_table_locks[larger_idx]);
                release(&bcache.buf_table_locks[smaller_idx]);
                acquiresleep(&b->lock);
                return b;
            }
        }

        for (b = bcache.buf_table[victim_table_idx].next; b != 0; b = b->next) {
            if (b->refcnt == 0) {
                b->dev = dev;
                b->blockno = blockno;
                b->valid = 0;
                b->refcnt = 1;
                move_to_bucket(table_idx, b);
                release(&bcache.buf_table_locks[larger_idx]);
                release(&bcache.buf_table_locks[smaller_idx]);
                acquiresleep(&b->lock);
                return b;
            }
        }

        release(&bcache.buf_table_locks[larger_idx]);
        release(&bcache.buf_table_locks[smaller_idx]);
    }

    panic("bget: no buffers");
}

// move `b` from its current bucket into `bcache.buf_table[bucket_idx]`.
// caller must hold locks for both old bucket and new bucket.
void move_to_bucket(int bucket_idx, struct buf *b) {
    struct buf *old_l = b->prev;
    struct buf *old_r = b->next;
    if (old_l != 0) {
        old_l->next = old_r;
    }
    if (old_r != 0) {
        old_r->prev = old_l;
    }

    struct buf *new_l = &bcache.buf_table[bucket_idx];
    struct buf *new_r = new_l->next;

    new_l->next = b;
    b->prev = new_l;
    b->next = new_r;
    if (new_r != 0) {
        new_r->prev = b;
    }
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
    if (!holdingsleep(&b->lock)) {
        panic("bwrite");
    }
    virtio_disk_rw(b, 1);
}

void brelse(struct buf *b) {
    if (!holdingsleep(&b->lock)) {
        panic("brelse");
    }

    releasesleep(&b->lock);

    uint table_idx = b->blockno % BUF_TABLE_SIZE;

    acquire(&bcache.buf_table_locks[table_idx]);
    b->refcnt--;
    release(&bcache.buf_table_locks[table_idx]);
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
