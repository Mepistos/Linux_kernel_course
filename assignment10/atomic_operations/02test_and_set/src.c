#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/delay.h>

// test-and-set lock
struct lock {
	int held;
};
void init_lock(struct lock *lock_v){
	lock_v->held = 0;
}
void acquire(struct lock *lock_v){
	while(__sync_lock_test_and_set(&lock_v->held, 1));
}
void release(struct lock *lock_v){
	__sync_lock_test_and_set(&lock_v->held, 0);
}

// global var
int counter;
struct lock c_lock;
struct task_struct *thread1, *thread2, *thread3, *thread4;

static int work_fn(void *data)
{
	int original;

	while(!kthread_should_stop()) {
		// critical section
		acquire(&c_lock);
		original = ++counter;
		release(&c_lock);
		// end of the critical section

		printk(KERN_INFO "pid[%u] %s: counter: %d\n", current->pid, __func__, original);
		msleep(500);
	}

	do_exit(0);
}

static int __init my_mod_init(void)
{
	printk("%s: Entering Test and Set Module!\n", __func__);
	counter = 0;
	init_lock(&c_lock);
	thread1 = kthread_run(work_fn, NULL, "test_and_set_function");
	thread2 = kthread_run(work_fn, NULL, "test_and_set_function");
	thread3 = kthread_run(work_fn, NULL, "test_and_set_function");
	thread4 = kthread_run(work_fn, NULL, "test_and_set_function");
	return 0;
}

static void __exit my_mod_exit(void)
{
	kthread_stop(thread1);
	kthread_stop(thread2);
	kthread_stop(thread3);
	kthread_stop(thread4);
	printk("%s: Exiting Test and Set Module!\n", __func__);
}
module_init(my_mod_init);
module_exit(my_mod_exit);

MODULE_LICENSE("GPL");
