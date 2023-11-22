#include <../calclock.h>

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/semaphore.h>

// define your spinlock here



// global var
int counter;
struct rw_semaphore counter_rwse;
struct task_struct *thread1, *thread2, *thread3, *thread4;
struct my_args {
	int thread_id;
	int range_bound[2];
	struct my_nod *data;
};

// initialize your list here
struct list_head{
        struct list_head *next, *prev;
};
typedef struct linked_list {
        struct list_head l_list;
        int data;
} my_node;


// linked list function
void *add_to_list(void *tdata)
{
	// local var
	struct my_args *args = (struct my_args *)tdata;
	int thread_id = args->thread_id;
	int *range_bound = args->range_bound;

	printk(KERN_INFO "thread #%d range: %d ~ %d\n",
			thread_id, range_bound[], range_bound[1]);
	
	// put your code here
	down_write(&counter_rwse);
	counter++;
	up_write(&counter_rwse);
	
	return first;
}

int search_list(void *tdata)
{
	// local var
        struct my_args *args = (struct my_args *)tdata;
        int thread_id = args->thread_id;
        int *range_bound = args->range_bound;
	struct my_node *data = args->data;

	struct timespec localclock[2];
	/* This will point on the actual data structures during the iteration */
	struct my_node *cur = (struct my_node *) data, *tmp;

	// put your code here
	
	return 0;
}

int delete_from_list(void *tdata)
{
	// local var
        struct my_args *args = (struct my_args *)data;
        int thread_id = args->thread_id;
        int *range_bound = args->range_bound;

	struct my_node *cur, *tmp;
	struct timespec localclock[2];
	// put your code here
	
	return 0;
}


// module
static int __init my_mod_init(void)
{
	printk("%s: Entering module\n", __func__);
	counter = 0;
	init_rwsem(&counter_rwse);

	// args
	struct my_args args1 = {
		.thread_id = thread1->pid,
		.range_bound = {0, 249999},
		.data = NULL,
	};
	struct my_args args2 = {
                .thread_id = thread2->pid,
                .range_bound = {250000, 499999},
                .data = NULL,
        };
	struct my_args args3 = {
                .thread_id = thread3->pid,
                .range_bound = {500000, 749999},
                .data = NULL,
        };
	struct my_args args4 = {
                .thread_id = thread4->pid,
                .range_bound = {750000, 999999},
                .data = NULL,
        };

	// add
	thread1 = kthread_run(add_to_list, &args1, "add_to_list");
	thread2 = kthread_run(add_to_list, &args2, "add_to_list");
	thread3 = kthread_run(add_to_list, &args3, "add_to_list");
	thread4 = kthread_run(add_to_list, &args4, "add_to_list");

	// search
	thread1 = kthread_run(search_list, &args1, "search_list");
	thread2 = kthread_run(search_list, &args2, "search_list");
	thread3 = kthread_run(search_list, &args3, "search_list");
	thread4 = kthread_run(search_list, &args4, "search_list");

	// delete
	thread1 = kthread_run(delete_from_list, &args1, "delete_from_list");
	thread2 = kthread_run(delete_from_list, &args2, "delete_from_list");
	thread3 = kthread_run(delete_from_list, &args3, "delete_from_list");
	thread4 = kthread_run(delete_from_list, &args4, "delete_from_list");

	return 0;
}
static int __exit my_mod_exit(void)
{
	kthread_stop(thread1);
	kthread_stop(thread2);
	kthread_stop(thread3);
	kthread_stop(thread4);

	printk("%s: Exiting RW Module!\n", __func__);
}
module_init(my_mod_init);
module_exit(my_mod_exit);

MODULE_LICENSE("GPL");
