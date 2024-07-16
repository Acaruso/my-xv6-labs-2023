#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

static int loadseg(pde_t *, uint64, struct inode *, uint, uint);

int flags2perm(int flags);

int exec(char *path, char **argv) {
    uint64 sz;
    uint64 sz1;
    int rc = 0;
    pagetable_t pagetable = 0;
    struct proc *p = myproc();

    begin_op();

    // `namei` gets the inode for a path
    struct inode *ip = namei(path);
    if (ip == 0) {
        end_op();
        return -1;
    }
    ilock(ip);

    // read data from file into `elfhdr` and check that file is a valid ELF file
    struct elfhdr elf;
    // `readi` reads data from an inode
    // we use `readi` instead of `read` because `read` is for user-level code and is a lot more
    // complicated under the hood
    rc = readi(
        ip,                 // inode
        0,                  // user_dst -- if `user_dst == 0`, then dest is a kernel address
        (uint64)&elf,       // dst -- read data into this
        0,                  // offset
        sizeof(elf)         // n
    );
    if (rc != sizeof(elf)) {
        goto bad;
    }
    if (elf.magic != ELF_MAGIC) {
        goto bad;
    }

    // Create a user page table for process `p` with no user memory,
    // but with trampoline and trapframe pages.
    pagetable = proc_pagetable(p);
    if (pagetable == 0) {
        goto bad;
    }

    // load program into memory
    // int off;
    struct proghdr ph;
    sz = 0;
    for (int i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
        // read data into `ph`
        if (readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph)) {
            goto bad;
        }

        // check that `ph` actually contains a program header
        if (ph.type != ELF_PROG_LOAD) {
            continue;
        }
        if (ph.memsz < ph.filesz) {
            goto bad;
        }
        if (ph.vaddr + ph.memsz < ph.vaddr) {
            goto bad;
        }
        if (ph.vaddr % PGSIZE != 0) {
            goto bad;
        }

        // grow the process's memory from `sz` to `ph.vaddr + ph.memsz`
        sz1 = uvmalloc(
            pagetable,              // pagetable
            sz,                     // oldsz
            ph.vaddr + ph.memsz,    // newsz
            flags2perm(ph.flags)    // xperm
        );
        if (sz1 == 0) {
            goto bad;
        }
        sz = sz1;

        // load a segment from inode `ip` into `pagetable` at `va`
        rc = loadseg(
            pagetable,      // pagetable
            ph.vaddr,       // va
            ip,             // inode
            ph.off,         // offset
            ph.filesz       // size
        );
        if (rc < 0) {
            goto bad;
        }
    }

    iunlockput(ip);
    end_op();
    ip = 0;

    p = myproc();
    uint64 oldsz = p->sz;

    // allocate two pages for the user stack
    // one for the stack itself and one guard page
    sz = PGROUNDUP(sz);
    sz1 = uvmalloc(pagetable, sz, sz + (2 * PGSIZE), PTE_W);
    if (sz1 == 0) {
        goto bad;
    }

    // create the guard page
    sz = sz1;
    // mark the PTE as invalid by unsetting PTE_U
    uvmclear(pagetable, sz - (2 * PGSIZE));
    uint64 sp = sz;
    uint64 stackbase = sp - PGSIZE;

    // copy argument strings to user stack,
    // store pointers in `ustack` -- this will become `argv`
    uint64 argc;
    uint64 ustack[MAXARG];
    for (argc = 0; argv[argc] != 0; argc++) {
        if (argc >= MAXARG) {
            goto bad;
        }

        sp -= strlen(argv[argc]) + 1;
        sp -= sp % 16;                  // riscv sp must be 16-byte aligned
        if (sp < stackbase) {
            goto bad;
        }

        rc = copyout(
            pagetable,                  // pagetable
            sp,                         // dest va
            argv[argc],                 // source
            strlen(argv[argc]) + 1      // length
        );
        if (rc < 0) {
            goto bad;
        }

        ustack[argc] = sp;
    }
    ustack[argc] = 0;                   // null-terminate argv

    // copy `ustack` to user stack -- this is `argv`
    // recall that `argv` is a null-terminated array of char pointers
    sp -= (argc + 1) * sizeof(uint64);
    sp -= sp % 16;
    if (sp < stackbase) {
        goto bad;
    }

    rc = copyout(
        pagetable,                      // pagetable
        sp,                             // dest va
        (char *)ustack,                 // source
        (argc + 1) * sizeof(uint64)     // length
    );
    if (rc < 0) {
        goto bad;
    }

    // arguments to user main(argc, argv)
    // argc is returned via the system call return
    // value, which goes in a0.
    p->trapframe->a1 = sp;

    // Save program name for debugging.
    char *s;
    char *last;

    // find first character after the final '/' character, and copy the
    // associated string into `p->name`
    for (last = s = path; *s != 0; s++) {
        if (*s == '/') {
            last = s + 1;
        }
    }
    safestrcpy(p->name, last, sizeof(p->name));

    // Commit to the user image.
    pagetable_t oldpagetable = p->pagetable;
    p->pagetable = pagetable;
    p->sz = sz;
    p->trapframe->epc = elf.entry;              // initial program counter = main
    p->trapframe->sp = sp;                      // initial stack pointer
    proc_freepagetable(oldpagetable, oldsz);

    return argc;  // this ends up in a0, the first argument to main(argc, argv)

bad:
    if (pagetable) proc_freepagetable(pagetable, sz);
    if (ip) {
        iunlockput(ip);
        end_op();
    }
    return -1;
}

int flags2perm(int flags) {
    int perm = 0;
    if (flags & 0x1) perm = PTE_X;
    if (flags & 0x2) perm |= PTE_W;
    return perm;
}

// Load a program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages in range `va : va + sz` must already be mapped.
// Returns 0 on success, -1 on failure.
static int loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz) {
    uint n;

    for (uint i = 0; i < sz; i += PGSIZE) {
        uint64 pa = walkaddr(pagetable, va + i);
        if (pa == 0) {
            panic("loadseg: address should exist");
        }

        if (sz - i < PGSIZE) {
            n = sz - i;
        } else {
            n = PGSIZE;
        }

        if (readi(ip, 0, (uint64)pa, offset + i, n) != n) {
            return -1;
        }
    }

    return 0;
}
