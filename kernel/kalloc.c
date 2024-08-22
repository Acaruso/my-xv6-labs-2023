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
void kfree_init(void *pa);

extern char end[];  // first address after kernel.
                    // defined by kernel.ld.

struct run {
    struct run *next;
};

struct {
    struct spinlock lock;
    struct run *freelist;
} kmem;

int page_refs[32731] = {0};

uint64 pa_start = 0;

#define PA2PAGENUM(pa) ((((uint64)pa) - (pa_start)) / (PGSIZE))

int get_page_ref(uint64 pa) {
    return page_refs[PA2PAGENUM(pa)];
}

void set_page_ref(uint64 pa, int new_value) {
    page_refs[PA2PAGENUM(pa)] = new_value;
}

void increment_page_ref(uint64 pa) {
    page_refs[PA2PAGENUM(pa)] += 1;
}

void increment_page_ref_synchronized(uint64 pa) {
    acquire(&kmem.lock);
    page_refs[PA2PAGENUM(pa)] += 1;
    release(&kmem.lock);
}

void decrement_page_ref(uint64 pa) {
    if (page_refs[PA2PAGENUM(pa)] == 0) {
        return;
    }
    page_refs[PA2PAGENUM(pa)] -= 1;
}

void kinit() {
    initlock(&kmem.lock, "kmem");
    pa_start = (uint64)end;
    freerange(end, (void *)PHYSTOP);
}

void freerange(void *pa_start, void *pa_end) {
    char *p;
    p = (char *)PGROUNDUP((uint64)pa_start);
    for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE) {
        kfree_init(p);
    }
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void kfree(void *pa) {
    struct run *r;

    if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP) {
        panic("kfree");
    }

    acquire(&kmem.lock);
    decrement_page_ref((uint64)pa);
    if (get_page_ref((uint64)pa) > 0) {
        release(&kmem.lock);
        return;
    }
    release(&kmem.lock);

    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);

    r = (struct run *)pa;

    acquire(&kmem.lock);
    r->next = kmem.freelist;
    kmem.freelist = r;
    release(&kmem.lock);
}

// `kfree_init` is only called at startup time
// unlike `kfree`, `kfree_init` doesn't decrement the page reference count
void kfree_init(void *pa) {
    struct run *r;

    if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP) {
        panic("kfree");
    }

    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);

    r = (struct run *)pa;

    acquire(&kmem.lock);
    r->next = kmem.freelist;
    kmem.freelist = r;
    release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *kalloc(void) {
    struct run *r;

    acquire(&kmem.lock);
    r = kmem.freelist;
    if (r) {
        kmem.freelist = r->next;
        set_page_ref((uint64)r, 1);
    }
    release(&kmem.lock);

    if (r) {
        memset((char *)r, 5, PGSIZE);  // fill with junk
    }

    return (void *)r;
}

void *kalloc_no_lock(void) {
    struct run *r;

    r = kmem.freelist;
    if (r) {
        kmem.freelist = r->next;
        set_page_ref((uint64)r, 1);
    }

    if (r) {
        memset((char *)r, 5, PGSIZE);  // fill with junk
    }

    return (void *)r;
}

// `pte` is the PTE for a page that we tried to write to and triggered a store page fault on.
// assume that `pte` has PTE_COW set.
int handle_cow_page(pte_t *pte) {
    uint64 old_page = PTE2PA(*pte);

    acquire(&kmem.lock);
    if (get_page_ref((uint64)old_page) > 1) {
        // allocate new page
        char *new_page = kalloc_no_lock();
        if (new_page == 0) {
            release(&kmem.lock);
            return 0;
        }

        memmove(
            new_page,           // dest
            (char *)old_page,   // source
            PGSIZE              // size
        );

        uint flags = PTE_FLAGS(*pte);
        flags = flags & ~PTE_COW;
        flags = flags | PTE_W;
        *pte = PA2PTE(new_page) | flags;

        decrement_page_ref((uint64)old_page);
    } else {
        // if page ref count == 1, don't need to copy the page,
        // just need to update flags
        *pte = *pte & ~PTE_COW;
        *pte = *pte | PTE_W;
    }

    release(&kmem.lock);

    return 1;
}
