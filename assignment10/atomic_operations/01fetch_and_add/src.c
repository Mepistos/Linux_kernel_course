#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/delay.h>

// global var
int counter;
struct task_struct *thread1, *thread2, *thread3, *thread4;

static int work_fn(void *data)
{
	int original;

	while(!kthread_should_stop()) {
		// critical section
		original = __sync_fetch_and_add(&counter, 1);
		// end of the critical section

		printk(KERN_INFO "pid[%u] %s: counter: %d\n", current->pid, __func__, original);
		msleep(500);
	}

	do_exit(0);
}

static int __init my_mod_init(void)
{
	printk("%s: Entering Fetch and Add Module!\n", __func__);
	counter = 0;
	thread1 = kthread_run(work_fn, NULL, "fetch_and_add_function");
	thread2 = kthread_run(work_fn, NULL, "fetch_and_add_function");
	thread3 = kthread_run(work_fn, NULL, "fetch_and_add_function");
	thread4 = kthread_run(work_fn, NULL, "fetch_and_add_function");
	return 0;
}

static void __exit my_mod_exit(void)
{
	kthread_stop(thread1);
	kthread_stop(thread2);
	kthread_stop(thread3);
	kthread_stop(thread4);
	printk("%s: Exiting Fetch and Add Module!\n", __func__);
}
module_init(my_mod_init);
module_exit(my_mod_exit);

MODULE_LICENSE("GPL");
