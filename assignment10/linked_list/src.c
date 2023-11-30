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

// initialize your list here
struct list_head {
	struct list_head *next, *prev;
};
typedef struct my_node {
	struct list_head node_list;
	int data;
}

struct list_head my_list;
INIT_LIST_HEAD(&my_list);

void *add_to_list(int thread_id, int range_bound[])
{
	// ...

	printk(KERN_INFO "thread #%d range: %d ~ %d\n", thread_id, range_bound[0], range_bound[1]);

	// put your code here
		// lock acquire
		for (int i=range_bound[0];i<=ragne_bound[1];i++){
			struct my_node *new = kmalloc(sizeof(struct my_node), GFP_KERNEL);
			new->data = i;
			list_add(&new->entry
		}
		// lock release

	return first;
}

int search_list(int thread_id, void *data, int range_bound[])
{
	struct timespec localclock[2];

	struct my_node *cur = (struct my_node *) data, *tmp;

	// put your code here
	
	return 0;
}

int delete_from_list(int thread_id, int range_bound[])
{
	struct my_node *cur, *tmp;
	struct timespec localclock[2];
	// put your code here
	
	return 0;
}

// thread
struct task_strct *thread1, *thread2, *thread3, *thread4;

// module define
static int __init my_mod_init(void)
{
	printk("~_module_init: Entering ~ Module!\n");



	return 0;
}
static void __exit my_mod_exit(void)
{
	printk("~_module_cleanup: Exiting ~ Module!\n");
}
module_init(my_mod_init);
module_exit(my_mod_exit);

MODULE_LICENSE("GPL");
