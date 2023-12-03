#include "../calclock.h"

#include <linux/list.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/delay.h>


// define your spinlock here
struct rw_semaphore counter_rwse;

// initialize your list here
struct my_node {
	struct list_head node_list;
	int data;
};

struct list_head my_list;

void *add_to_list(int thread_id, int range_bound[])
{
	void *first = NULL;
	int i = 0;

	// put your code here
		// lock acquire
	down_write(&counter_rwse);
		// add
	printk("%d: %d ~ %d\n", thread_id, range_bound[0], range_bound[1]);
	for (i=1;i<=10;i++){
		struct my_node *new = kmalloc(sizeof(struct my_node), GFP_KERNEL);
		new->data = range_bound[0]*i;
		list_add(&new->node_list, &my_list);
		first = new;
	}
#if 0
	for (i=range_bound[0];i<=range_bound[1];i++){
		struct my_node *new = kmalloc(sizeof(struct my_node), GFP_KERNEL);
		new->data = i;
		list_add(&new->node_list, &my_list);
		if(i==range_bound[0])
			first = &new;
	}
#endif
		// lock release
	up_write(&counter_rwse);
	
	return first;
}

int search_list(int thread_id, void *data, int range_bound[])
{
	//struct timespec localclock[2];

	struct my_node *cur = (struct my_node *) data, *tmp;

	// put your code here
		// lock acquire
	down_read(&counter_rwse);
		// search
	list_for_each_entry(tmp, &my_list, node_list) {
		if(cur->data == tmp->data) {
			printk("found\n");
			break;
		}
		printk("current value: %d\n", tmp->data);
	}
		// lock release
	up_read(&counter_rwse);
	
	return 0;
}

int delete_from_list(int thread_id, int range_bound[])
{
	struct my_node *cur, *tmp;
	//struct timespec localclock[2];
	// put your code here
		// lock acquire
	down_write(&counter_rwse);
		// delete
	list_for_each_entry_safe(cur, tmp, &my_list, node_list) {
		list_del(&cur->node_list);
		kfree(cur);
	}
		// lock release
	up_write(&counter_rwse);
	
	return 0;
}

// thread
struct task_struct *thread1, *thread2, *thread3, *thread4;

static int work_fn(void *data)
{
	int thread_id = *(int *)data;
	int range_bound[2];

	// set iter range
	range_bound[0] = 250000 * (thread_id-1);
	range_bound[1] = 250000 * (thread_id);

	printk("thread #%d range: %d ~ %d\n", thread_id, range_bound[0], range_bound[1]);
	void *ret = add_to_list(thread_id, range_bound);

	//printk("thread #%d searched range: %d ~ %d\n", thread_id, range_bound[0], range_bound[1]);
	//search_list(thread_id, ret, range_bound);

	//printk("thread #%d deleted range: %d ~ %d\n", thread_id, range_bound[0], range_bound[1]);
	//delete_from_list(thread_id, range_bound);

	while(!kthread_should_stop()){
		msleep(500);
	}
	do_exit(0);
}

// module define
static int __init my_mod_init(void)
{
	int *value = kmalloc(sizeof(int)*4, GFP_KERNEL);

	INIT_LIST_HEAD(&my_list);
	list_entry(&my_list, struct my_node, node_list);

	printk("~_module_init: Entering ~ Module!\n");

	value[0] = 1;
	value[1] = 2;
	value[2] = 3;
	value[3] = 4;
	
	thread1 = kthread_run(work_fn, &(value[0]), "linked_list");
	thread2 = kthread_run(work_fn, &(value[1]), "linked_list");
	thread3 = kthread_run(work_fn, &(value[2]), "linked_list");
	thread4 = kthread_run(work_fn, &(value[3]), "linked_list");

	return 0;
}
static void __exit my_mod_exit(void)
{
	struct my_node *current_node = NULL;
	list_for_each_entry_reverse(current_node, &my_list, node_list) {
		printk("cur : %d\n", current_node->data);
	}
	kthread_stop(thread1);
	kthread_stop(thread2);
	kthread_stop(thread3);
	kthread_stop(thread4);
	printk("~_module_cleanup: Exiting ~ Module!\n");
}
module_init(my_mod_init);
module_exit(my_mod_exit);

MODULE_LICENSE("GPL");
