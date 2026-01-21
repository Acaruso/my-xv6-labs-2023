//
// network system calls.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "net.h"

struct sock {
    struct sock *next;     // the next socket in the list
    uint32 raddr;          // the remote IPv4 address
    uint16 lport;          // the local UDP port number
    uint16 rport;          // the remote UDP port number
    struct spinlock lock;  // protects the rxq
    struct mbufq rxq;      // a queue of packets waiting to be received
};

static struct spinlock lock;
static struct sock *sockets;

void sockinit(void) {
    initlock(&lock, "socktbl");
}

// raddr - dest ip address, lport - source port, rport - dest port
int sockalloc(struct file **file, uint32 raddr, uint16 lport, uint16 rport) {
    struct sock *sock = 0;

    *file = 0;
    *file = filealloc();
    if (*file == 0) {
        goto bad;
    }

    sock = (struct sock *)kalloc();
    if (sock == 0) {
        goto bad;
    }

    sock->raddr = raddr;
    sock->lport = lport;
    sock->rport = rport;
    initlock(&sock->lock, "sock");
    mbufq_init(&sock->rxq);           // do `sock->rxq.head = 0`
                                      // recall that `head` is the
                                      // current start position of the buffer
    (*file)->type = FD_SOCK;
    (*file)->readable = 1;
    (*file)->writable = 1;
    (*file)->sock = sock;

    // add the socket to the list
    acquire(&lock);

    // first check that we're not adding a duplicate socket to the list
    struct sock *cur_sock = sockets;
    while (cur_sock) {
        if (cur_sock->raddr == raddr && cur_sock->lport == lport && cur_sock->rport == rport) {
            release(&lock);
            goto bad;
        }
        cur_sock = cur_sock->next;
    }

    // add the socket to the list
    sock->next = sockets;
    sockets = sock;

    release(&lock);

    return 0;

bad:
    if (sock) kfree((char *)sock);
    if (*file) fileclose(*file);
    return -1;
}

void sockclose(struct sock *si) {
    struct sock **pos;
    struct mbuf *m;

    // remove from list of sockets
    acquire(&lock);
    pos = &sockets;
    while (*pos) {
        if (*pos == si) {
            *pos = si->next;
            break;
        }
        pos = &(*pos)->next;
    }
    release(&lock);

    // free any pending mbufs
    while (!mbufq_empty(&si->rxq)) {
        m = mbufq_pophead(&si->rxq);
        mbuffree(m);
    }

    kfree((char *)si);
}

int sockread(struct sock *sock, uint64 addr, int n) {
    struct proc *p = myproc();

    acquire(&sock->lock);

    while (mbufq_empty(&sock->rxq) && !p->killed) {
        sleep(&sock->rxq, &sock->lock);
    }

    if (p->killed) {
        release(&sock->lock);
        return -1;
    }

    struct mbuf *m = mbufq_pophead(&sock->rxq);

    release(&sock->lock);

    int len = m->len;
    if (len > n) {
        len = n;
    }

    int rc = copyout(
        p->pagetable,       // pagetable
        addr,               // dest_va
        m->head,            // source
        len                 // len
    );
    if (rc == -1) {
        mbuffree(m);
        return -1;
    }

    mbuffree(m);

    return len;
}

int sockwrite(struct sock *sock, uint64 addr, int n) {
    struct proc *p = myproc();

    struct mbuf *m = mbufalloc(MBUF_DEFAULT_HEADROOM);
    if (!m) {
        return -1;
    }

    int rc = copyin(
        p->pagetable,       // pagetable
        mbufput(m, n),      // dest
        addr,               // source_va
        n                   // len
    );
    if (rc == -1) {
        mbuffree(m);
        return -1;
    }

    net_tx_udp(m, sock->raddr, sock->lport, sock->rport);

    return n;
}

// Find the socket that handles this mbuf and deliver it, waking
// any sleeping reader. Free the mbuf if there are no sockets
// registered to handle it.
void sockrecvudp(struct mbuf *m, uint32 raddr, uint16 lport, uint16 rport) {
    acquire(&lock);

    struct sock *s = sockets;
    while (s) {
        if (
            s->raddr == raddr
            && s->lport == lport
            && s->rport == rport
        ) {
            goto found;
        }
        s = s->next;
    }

    release(&lock);

    mbuffree(m);

    return;

found:
    acquire(&s->lock);
    mbufq_pushtail(&s->rxq, m);
    wakeup(&s->rxq);
    release(&s->lock);
    release(&lock);
}
