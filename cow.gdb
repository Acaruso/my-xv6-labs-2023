target remote localhost:26000

# file user/_sh
# b 159
# b 162
# c

# file user/_init
# b 37
# b 41
# c

# file kernel/kernel
# b exit
#c

# file kernel/kernel
# b uvmcopy
# c

# file user/_usertests
# b countfree

file kernel/kernel
b kernel_trap_page_fault
