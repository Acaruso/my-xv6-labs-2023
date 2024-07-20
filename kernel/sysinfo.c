#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "sysinfo.h"

uint64 sys_sysinfo(void) {
    uint64 dest = 0;
    argaddr(0, &dest);

    struct sysinfo sysinfo_;
    sysinfo_.nproc = num_procs_in_use();
    sysinfo_.freemem = get_free_mem_amount();

    return copyout(
        myproc()->pagetable,
        dest,
        (char *)&sysinfo_,
        sizeof(sysinfo_)
    );
}
