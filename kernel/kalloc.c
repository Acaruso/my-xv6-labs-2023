// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);
struct kmem* find_free_memory(int cpu_id);

extern char end[];  // first address after kernel.
                    // defined by kernel.ld.

struct run {
    struct run *next;
};

struct kmem {
    struct spinlock lock;
    struct run *freelist;
};

struct kmem kmem_arr[NCPU];

void kinit() {
    for (int i = 0; i < NCPU; i++) {
        initlock(&kmem_arr[i].lock, "kmem");
    }
    freerange(end, (void *)PHYSTOP);
}

void freerange(void *pa_start, void *pa_end) {
    char *p;
    p = (char *)PGROUNDUP((uint64)pa_start);
    for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE) kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void kfree(void *pa) {
    if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP) {
        panic("kfree");
    }

    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);

    struct run *r = (struct run *)pa;

    push_off();
    int cpu_id = cpuid();
    pop_off();
    struct kmem *kmem = &kmem_arr[cpu_id];

    acquire(&kmem->lock);
    r->next = kmem->freelist;
    kmem->freelist = r;
    release(&kmem->lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *kalloc(void) {
    struct run *r;

    push_off();
    int cpu_id = cpuid();
    pop_off();
    struct kmem *kmem = &kmem_arr[cpu_id];

    acquire(&kmem->lock);

    if (kmem->freelist == 0) {
        release(&kmem->lock);
        kmem = find_free_memory(cpu_id);
        if (kmem == 0) {
            // system is out of memory
            return 0;
        }
        // else if kmem != 0, then free memory was found
        // kmem is returned with lock held
        r = kmem->freelist;
        kmem->freelist = r->next;
        release(&kmem->lock);
        memset((char *)r, 5, PGSIZE);
        return (void *)r;
    } else {
        r = kmem->freelist;
        kmem->freelist = r->next;
        release(&kmem->lock);
        memset((char *)r, 5, PGSIZE);
        return (void *)r;
    }
}

struct kmem* find_free_memory(int cpu_id) {
    struct kmem *kmem = 0;

    for (int i = 0; i < NCPU; i++) {
        if (i == cpu_id) {
            continue;
        }
        kmem = &kmem_arr[i];

        acquire(&kmem->lock);
        if (kmem->freelist != 0) {
            return kmem;
        }
        release(&kmem->lock);
    }

    return 0;
}
