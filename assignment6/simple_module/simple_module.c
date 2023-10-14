#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>

int __init simple_module_init(void)
{
	printk("assignment6 module in\n");
	return 0;
}

void __exit simple_module_cleanup(void)
{
	printk("assignment6 module out\n");
}

module_init(simple_module_init);
module_exit(simple_module_cleanup);
MODULE_LICENSE("GPL");
