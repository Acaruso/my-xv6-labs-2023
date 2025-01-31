#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fcntl.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void trapinit(void) { initlock(&tickslock, "time"); }

// set up to take exceptions and traps while in the kernel.
void trapinithart(void) { w_stvec((uint64)kernelvec); }

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//

int is_page_fault(uint64 scause);
int is_mmap_page_fault(uint64 scause, uint64 va, struct vma* vma);
int handle_mmap_page_fault(uint64 va, struct vma* vma);

void usertrap(void) {
    int which_dev = 0;

    if ((r_sstatus() & SSTATUS_SPP) != 0) panic("usertrap: not from user mode");

    // send interrupts and exceptions to kerneltrap(),
    // since we're now in the kernel.
    w_stvec((uint64)kernelvec);

    struct proc *p = myproc();

    // save user program counter.
    p->trapframe->epc = r_sepc();

    if (r_scause() == 8) {
        // system call

        if (killed(p)) exit(-1);

        // sepc points to the ecall instruction,
        // but we want to return to the next instruction.
        p->trapframe->epc += 4;

        // an interrupt will change sepc, scause, and sstatus,
        // so enable only now that we're done with those registers.
        intr_on();

        syscall();
    } else if ((which_dev = devintr()) != 0) {
        // ok
    } else {
        uint64 scause = r_scause();
        int should_kill = 1;

        if (is_page_fault(scause)) {
            uint64 va = r_stval();
            struct vma* vma = get_vma(va);
            if (vma != 0) {
                if (is_mmap_page_fault(scause, va, vma)) {
                    should_kill = 0;
                    int rc = handle_mmap_page_fault(va, vma);
                    if (rc != 0) {
                        should_kill = 1;
                    }
                }
            }
        }

        if (should_kill == 1) {
            printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
            printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
            setkilled(p);
        }
    }

    if (killed(p)) exit(-1);

    // give up the CPU if this is a timer interrupt.
    if (which_dev == 2) yield();

    usertrapret();
}

int is_page_fault(uint64 scause) {
    if (
        scause    == SCAUSE_PAGE_FAULT_INSTR
        || scause == SCAUSE_PAGE_FAULT_LOAD
        || scause == SCAUSE_PAGE_FAULT_STORE
    ) {
        return 1;
    } else {
        return 0;
    }
}

int is_mmap_page_fault(uint64 scause, uint64 va, struct vma* vma) {
    if (
        scause == SCAUSE_PAGE_FAULT_INSTR
        && (vma->prot & PROT_EXEC)
    ) {
        return 1;
    } else if (
        scause == SCAUSE_PAGE_FAULT_LOAD
        && (vma->prot & PROT_READ)
    ) {
        return 1;
    } else if (
        scause == SCAUSE_PAGE_FAULT_STORE
        && (vma->prot & PROT_WRITE)
    ) {
        return 1;
    }

    return 0;
}

int handle_mmap_page_fault(uint64 va, struct vma* vma) {
    char *pa = kalloc();
    if (pa == 0) {
        return -1;
    }
    memset(pa, 0, PGSIZE);

    begin_op();
    struct inode *inode = vma->file->ip;
    ilock(inode);

    uint file_offset = PGROUNDDOWN(va) - vma->addr;
    int rc = 0;

    if (file_offset < inode->size) {
        uint n = (file_offset + PGSIZE <= inode->size) ? PGSIZE : inode->size % PGSIZE;

        // read `n` bytes from `inode` into newly allocated physical page `pa`
        rc = readi(
            inode,          // inode
            0,              // user_dst
            (uint64)pa,     // dst
            file_offset,    // offset
            n               // n
        );
        if (rc != n) {
            iunlock(inode);
            end_op();
            return -1;
        }
    }

    // PTE bits are `UXWRV` and VMA bits are `XWR`, so left shift VMA bits by 1
    int perm = vma->prot << 1;
    perm = perm | PTE_U;            // don't forget to set PTE_U

    rc = mappages(
        myproc()->pagetable,
        (uint64)PGROUNDDOWN(va),
        (uint64)PGSIZE,
        (uint64)pa,
        perm
    );
    if (rc != 0) {
        iunlock(inode);
        end_op();
        return -1;
    }

    iunlock(inode);
    end_op();

    return 0;
}

//
// return to user space
//
void usertrapret(void) {
    struct proc *p = myproc();

    // we're about to switch the destination of traps from
    // kerneltrap() to usertrap(), so turn off interrupts until
    // we're back in user space, where usertrap() is correct.
    intr_off();

    // send syscalls, interrupts, and exceptions to uservec in trampoline.S
    uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
    w_stvec(trampoline_uservec);

    // set up trapframe values that uservec will need when
    // the process next traps into the kernel.
    p->trapframe->kernel_satp = r_satp();          // kernel page table
    p->trapframe->kernel_sp = p->kstack + PGSIZE;  // process's kernel stack
    p->trapframe->kernel_trap = (uint64)usertrap;
    p->trapframe->kernel_hartid = r_tp();  // hartid for cpuid()

    // set up the registers that trampoline.S's sret will use
    // to get to user space.

    // set S Previous Privilege mode to User.
    unsigned long x = r_sstatus();
    x &= ~SSTATUS_SPP;  // clear SPP to 0 for user mode
    x |= SSTATUS_SPIE;  // enable interrupts in user mode
    w_sstatus(x);

    // set S Exception Program Counter to the saved user pc.
    w_sepc(p->trapframe->epc);

    // tell trampoline.S the user page table to switch to.
    uint64 satp = MAKE_SATP(p->pagetable);

    // jump to userret in trampoline.S at the top of memory, which
    // switches to the user page table, restores user registers,
    // and switches to user mode with sret.
    uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
    ((void (*)(uint64))trampoline_userret)(satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void kerneltrap() {
    int which_dev = 0;
    uint64 sepc = r_sepc();
    uint64 sstatus = r_sstatus();
    uint64 scause = r_scause();

    if ((sstatus & SSTATUS_SPP) == 0) panic("kerneltrap: not from supervisor mode");
    if (intr_get() != 0) panic("kerneltrap: interrupts enabled");

    if ((which_dev = devintr()) == 0) {
        printf("scause %p\n", scause);
        printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
        panic("kerneltrap");
    }

    // give up the CPU if this is a timer interrupt.
    if (which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING) yield();

    // the yield() may have caused some traps to occur,
    // so restore trap registers for use by kernelvec.S's sepc instruction.
    w_sepc(sepc);
    w_sstatus(sstatus);
}

void clockintr() {
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks);
    release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int devintr() {
    uint64 scause = r_scause();

    if ((scause & 0x8000000000000000L) && (scause & 0xff) == 9) {
        // this is a supervisor external interrupt, via PLIC.

        // irq indicates which device interrupted.
        int irq = plic_claim();

        if (irq == UART0_IRQ) {
            uartintr();
        } else if (irq == VIRTIO0_IRQ) {
            virtio_disk_intr();
        } else if (irq) {
            printf("unexpected interrupt irq=%d\n", irq);
        }

        // the PLIC allows each device to raise at most one
        // interrupt at a time; tell the PLIC the device is
        // now allowed to interrupt again.
        if (irq) plic_complete(irq);

        return 1;
    } else if (scause == 0x8000000000000001L) {
        // software interrupt from a machine-mode timer interrupt,
        // forwarded by timervec in kernelvec.S.

        if (cpuid() == 0) {
            clockintr();
        }

        // acknowledge the software interrupt by clearing
        // the SSIP bit in sip.
        w_sip(r_sip() & ~2);

        return 2;
    } else {
        return 0;
    }
}
