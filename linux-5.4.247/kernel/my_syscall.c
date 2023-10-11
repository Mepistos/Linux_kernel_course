#include <linux/unistd.h>
#include <linux/syscalls.h>

SYSCALL_DEFINE0(mycall)
{
	printk("System Call Example!\n");

	return 0;
}
