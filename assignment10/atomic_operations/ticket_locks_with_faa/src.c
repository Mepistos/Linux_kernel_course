#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/delay.h>

struct lock {
	int ticket;
	int turn;
};

void init_lock(struct lock *lock_v) {
	lock_v->ticket = 0;
	lock_v->turn = 0;
}

void acquire(struct lock *lock_v) {
	int myturn = __sync_fetch_and_add(&lock_v->ticket, 1);
	while(lock_v->turn != myturn);
}

void release(struct lock *lock_v) {
	lock_v->turn = lock_v->turn +1;
}

int counter;
struct lock counter_lock;
struct task_struct *thread1, *thread2, *thread3, *thread4;

static int work_fn(void *data)
{
	int original;

	while(!kthread_should_stop()) {
		// critical section
		acquire(&counter_lock);
		counter++;
		original = counter;
		release(&counter_lock);
		// end of the critical section

		printk(KERN_INFO "pid[%u] %s: counter: %d\n", current->pid, __func__, original);
		msleep(500);
	}

	do_exit(0);
}

static int __init my_mod_init(void)
{
	printk("%s: Entering Ticket locks Module!\n", __func__);
	counter = 0;
	init_lock(&counter_lock);
	thread1 = kthread_run(work_fn, NULL, "ticket_locks_function");
	thread2 = kthread_run(work_fn, NULL, "ticket_locks_function");
	thread3 = kthread_run(work_fn, NULL, "ticket_locks_function");
	thread4 = kthread_run(work_fn, NULL, "ticket_locks_function");
	return 0;
}

static void __exit my_mod_exit(void)
{
	kthread_stop(thread1);
	kthread_stop(thread2);
	kthread_stop(thread3);
	kthread_stop(thread4);
	printk("%s: Exiting Ticket locks Module!\n", __func__);
}
module_init(my_mod_init);
module_exit(my_mod_exit);

MODULE_LICENSE("GPL");
