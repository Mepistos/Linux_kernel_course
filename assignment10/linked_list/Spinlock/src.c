#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/ktime.h>

//lock header
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include "../calclock.h"
#include "../calclock.c"

// define spin lock
spinlock_t s_lock;

// init linked list
struct my_node {
	struct list_head list;
	int data;
};
LIST_HEAD(my_list);

int counter;

int add_counter;
int srch_counter;
int del_counter;

KTDEF(add_to_list);
KTDEF(search_list);
KTDEF(delete_from_list);

void *add_to_list(int thread_id, int range_bound[])
{
	printk(KERN_INFO "thread #%d: range: %d ~ %d\n", thread_id, range_bound[0], range_bound[1]);
	
	// put code
	spin_lock(&s_lock);

	int i;
	for(i=range_bound[0];i<=range_bound[1];i++) {
		struct my_node *new = kmalloc(sizeof(struct my_node), GFP_KERNEL);
		new->data = i;
		list_add(&new->list, &my_list);
	}

	spin_unlock(&s_lock);
	add_counter++;

	return &my_list;
}

int search_list(int thread_id, int range_bound[])
{
	printk(KERN_INFO "thread #%d: search range: %d ~ %d\n", thread_id, range_bound[0], range_bound[1]);
	struct my_node *cur, *tmp;

	// put code
	spin_lock(&s_lock);

	list_for_each_entry(cur, &my_list, list) {

	}

	spin_unlock(&s_lock);
	srch_counter++;

	return 0;
}

int delete_from_list(int thread_id, int range_bound[])
{
	printk(KERN_INFO "thread #%d: delete range: %d ~ %d\n", thread_id, range_bound[0], range_bound[1]);
	struct my_node *cur, *tmp;

	// put code
	spin_lock(&s_lock);

	list_for_each_entry_safe(cur, tmp, &my_list, list) {
		if(range_bound[0] <= cur->data && cur->data <= range_bound[1]) {
			list_del(&cur->list);
			kfree(cur);
		}
	}

	spin_unlock(&s_lock);
	del_counter++;

	return 0;
}

static int control_func(void *data)
{
	int thread_id = counter++;
	int bound[2] = { 250000*(thread_id-1), 249999+250000*(thread_id-1) };

	int ret;
	ktime_t stopwatch[2];

	// add
	ktget(&stopwatch[0]);
	ret = add_to_list(thread_id, bound);
	ktget(&stopwatch[1]);
	ktput(stopwatch, add_to_list);

	// search
	ktget(&stopwatch[0]);
	ret = search_list(thread_id, bound);
	ktget(&stopwatch[1]);
	ktput(stopwatch, search_list);

	// delete
	ktget(&stopwatch[0]);
	ret = delete_from_list(thread_id, bound);
	ktget(&stopwatch[1]);
	ktput(stopwatch, delete_from_list);

	while(!kthread_should_stop()){
		msleep(500);
	}
	printk(KERN_INFO "thread #%d stopped\n", thread_id);

	return 0;
}

struct task_struct *thread1, *thread2, *thread3, *thread4;

static int __init my_mod_init(void)
{
	printk("%s, Entering module(spinlock)\n", __func__);
	counter = 1;
	add_counter = srch_counter = del_counter = 0;

	INIT_LIST_HEAD(&my_list);

	spin_lock_init(&s_lock);

	thread1 = kthread_run(control_func, NULL, "thread1");
	thread2 = kthread_run(control_func, NULL, "thread2");
	thread3 = kthread_run(control_func, NULL, "thread3");
	thread4 = kthread_run(control_func, NULL, "thread4");

	return 0;
}

KTDEC(add_to_list);
KTDEC(search_list);
KTDEC(delete_from_list);

static void __exit my_mod_exit(void)
{
	ktprint(1, add_to_list);
	ktprint(1, search_list);
	ktprint(1, delete_from_list);

	kthread_stop(thread1);
	kthread_stop(thread2);
	kthread_stop(thread3);
	kthread_stop(thread4);

	list_del_init(&my_list);

	printk("%s, Exiting module(spinlock)\n", __func__);
}

module_init(my_mod_init);
module_exit(my_mod_exit);

MODULE_LICENSE("GPL");
