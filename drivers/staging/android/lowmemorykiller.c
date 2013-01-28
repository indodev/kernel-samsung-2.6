/* drivers/misc/lowmemorykiller.c
 *
 * The lowmemorykiller driver lets user-space specify a set of memory thresholds
 * where processes with a range of oom_adj values will get killed. Specify the
 * minimum oom_adj values in /sys/module/lowmemorykiller/parameters/adj and the
 * number of free pages in /sys/module/lowmemorykiller/parameters/minfree. Both
 * files take a comma separated list of numbers in ascending order.
 *
 * For example, write "0,8" to /sys/module/lowmemorykiller/parameters/adj and
 * "1024,4096" to /sys/module/lowmemorykiller/parameters/minfree to kill processes
 * with a oom_adj value of 8 or higher when the free memory drops below 4096 pages
 * and kill processes with a oom_adj value of 0 or higher when the free memory
 * drops below 1024 pages.
 *
 * The driver considers memory used for caches to be free, but if a large
 * percentage of the cached memory is locked this can be very inaccurate
 * and processes may not get killed until the normal oom killer is triggered.
 *
 * Copyright (C) 2007-2008 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/oom.h>
#include <linux/sched.h>
#include <linux/notifier.h>

#ifdef CONFIG_SWAP
#include <linux/fs.h>
#include <linux/swap.h>
#endif

#ifdef CONFIG_VOKULMK
#include <linux/earlysuspend.h>
#endif

#define DEBUG_LEVEL_DEATHPENDING 6

#ifdef CONFIG_SWAP
#include <linux/fs.h>
#include <linux/swap.h>
#endif

static uint32_t lowmem_debug_level = 0;
static int lowmem_adj[6] = {
	0,
	1,
	2,
	4,
	6,
	15,
};

static int lowmem_adj_size = 6;
static int lowmem_minfree[6] = {
	3 * 512,	/* 6MB */
	2 * 1024,	/* 8MB */
	3 * 1024, 	/* 12MB */
	4 * 1024,	/* 16MB */
	5 * 1024, 	/* 20MB */
	6 * 1024, 	/* 25MB */
};

#ifdef CONFIG_VOKULMK
static int lowmem_minfree_screen_on[6] = {
	3 * 512,	/* 6MB */
	2 * 1024,	/* 8MB */
	3 * 1024, 	/* 12MB */
	4 * 1024,	/* 16MB */
	5 * 1024, 	/* 20MB */
	6 * 1024, 	/* 25MB */
};
static int lowmem_minfree_screen_off[6] = {
	3 * 512,	/* 6MB */
	2 * 1024,	/* 8MB */
	3 * 1024, 	/* 12MB */
	4 * 1024,	/* 16MB */
	5 * 1024, 	/* 20MB */
	6 * 1024, 	/* 25MB */
};
#endif

static int lowmem_minfree_size = 6;
static size_t lowmem_minfile[6] = {
	3 * 512,	/* 6MB */
	2 * 1024,	/* 8MB */
	4 * 1024,	/* 16MB */
	8 * 1024, 	/* 32MB */
 	12 * 1024, 	/* 45MB */
	16 * 1024, 	/* 65MB */
};
static int lowmem_minfile_size = 6;

static unsigned long lowmem_deathpending_timeout;
static uint32_t lowmem_check_filepages = 0;
#ifdef CONFIG_SWAP
static int fudgeswap = 512;
#endif

#define lowmem_print(level, x...)			\
	do {						\
		if (lowmem_debug_level >= (level))   {  \
			printk("lowmem: ");    		\
			printk(x);			\
		}					\
	} while (0)

static int lowmem_shrink(struct shrinker *s, int nr_to_scan, gfp_t gfp_mask)
{
	struct task_struct *tsk;
	struct task_struct *selected = NULL;
	int rem = 0;
	int tasksize;
	int i;
	int min_adj = OOM_ADJUST_MAX + 1;
	int selected_tasksize = 0;
	int selected_oom_adj;
	int array_size = ARRAY_SIZE(lowmem_adj);
	int other_free = global_page_state(NR_FREE_PAGES);
	int other_file = global_page_state(NR_FILE_PAGES) - 
				global_page_state(NR_SHMEM);
	int lru_file = global_page_state(NR_ACTIVE_FILE) + 
				global_page_state(NR_INACTIVE_FILE);

#ifdef CONFIG_SWAP
	if(fudgeswap != 0){
		struct sysinfo si;
		si_swapinfo(&si);

		if(si.freeswap > 0){
			if(fudgeswap > si.freeswap)
				other_file += si.freeswap;
			else
				other_file += fudgeswap;
		}
	}
#endif

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;
	if (lowmem_minfree_size < array_size)
		array_size = lowmem_minfree_size;
	for (i = 0; i < array_size; i++) {
		if (other_free < lowmem_minfree[i]) {
			if (other_file < lowmem_minfree[i] ||
				(lowmem_check_filepages &&
				(lru_file < lowmem_minfile[i]))) {

				min_adj = lowmem_adj[i];
				break;
			}
		}
	}

	if (min_adj == OOM_ADJUST_MAX + 1)
		return 0;

	if (nr_to_scan > 0)
		lowmem_print(3, "lowmem_shrink %d, %x, ofree %d %d, ma %d\n",
			     nr_to_scan, gfp_mask, other_free, other_file,
			     min_adj);
	rem = global_page_state(NR_ACTIVE_ANON) +
		global_page_state(NR_ACTIVE_FILE) +
		global_page_state(NR_INACTIVE_ANON) +
		global_page_state(NR_INACTIVE_FILE);
	if (nr_to_scan <= 0) {
		lowmem_print(5, "lowmem_shrink %d, %x, return %d\n",
			     nr_to_scan, gfp_mask, rem);
		return rem;
	}
	selected_oom_adj = min_adj;

	rcu_read_lock();
	for_each_process(tsk) {
		struct task_struct *p;
		int oom_adj;

		if (tsk->flags & PF_KTHREAD)
			continue;

		p = find_lock_task_mm(tsk);
		if (!p)
			continue;
	
		if (test_tsk_thread_flag(p, TIF_MEMDIE) &&
			  time_before_eq(jiffies, lowmem_deathpending_timeout)) {
			task_unlock(p);
			rcu_read_unlock();
			return 0;
		}

		oom_adj = p->signal->oom_adj;
		if (oom_adj < min_adj) {
			task_unlock(p);
			continue;
		}
		tasksize = get_mm_rss(p->mm);
		task_unlock(p);
		if (tasksize <= 0)
			continue;
		if (selected) {
			if (oom_adj < selected_oom_adj)
				continue;
			if (oom_adj == selected_oom_adj &&
			    tasksize <= selected_tasksize)
				continue;
		}
		selected = p;
		selected_tasksize = tasksize;
		selected_oom_adj = oom_adj;
		lowmem_print(2, "select %d (%s), adj %d, size %d, to kill\n",
			     p->pid, p->comm, oom_adj, tasksize);
	}

	if (selected) {
		lowmem_print(1, "send sigkill to %d (%s), adj %d, size %d\n",
			     selected->pid, selected->comm,
			     selected_oom_adj, selected_tasksize);
		send_sig(SIGKILL, selected, 0);
		set_tsk_thread_flag(selected, TIF_MEMDIE);
		rem -= selected_tasksize;
	} else
		rem = -1;
	lowmem_print(4, "lowmem_shrink %d, %x, return %d\n",
		     nr_to_scan, gfp_mask, rem);
	rcu_read_unlock();
	return rem;
}

static struct shrinker lowmem_shrinker = {
	.shrink = lowmem_shrink,
	.seeks = DEFAULT_SEEKS * 16
};

#ifdef CONFIG_VOKULMK
static void low_mem_early_suspend(struct early_suspend *handler)
{
	memcpy(lowmem_minfree_screen_on, lowmem_minfree, sizeof(lowmem_minfree));
	memcpy(lowmem_minfree, lowmem_minfree_screen_off, sizeof(lowmem_minfree_screen_off));
}

static void low_mem_late_resume(struct early_suspend *handler)
{
	memcpy(lowmem_minfree, lowmem_minfree_screen_on, sizeof(lowmem_minfree_screen_on));
}

static struct early_suspend low_mem_suspend = {
	.suspend = low_mem_early_suspend,
	.resume = low_mem_late_resume,
};
#endif



static int __init lowmem_init(void)
{
	#ifdef CONFIG_VOKULMK
	register_early_suspend(&low_mem_suspend);
	#endif
	register_shrinker(&lowmem_shrinker);
	return 0;
}

static void __exit lowmem_exit(void)
{
	unregister_shrinker(&lowmem_shrinker);
}

module_param_named(cost, lowmem_shrinker.seeks, int, S_IRUGO | S_IWUSR);
module_param_array_named(adj, lowmem_adj, int, &lowmem_adj_size,
			 S_IRUGO | S_IWUSR);
module_param_array_named(minfree, lowmem_minfree, uint, &lowmem_minfree_size,
			 S_IRUGO | S_IWUSR);
#ifdef CONFIG_VOKULMK
module_param_array_named(minfree_screen_off, lowmem_minfree_screen_off, uint, &lowmem_minfree_size,
			 S_IRUGO | S_IWUSR);
#endif
module_param_named(debug_level, lowmem_debug_level, uint, S_IRUGO | S_IWUSR);
module_param_named(check_filepages , lowmem_check_filepages, uint,
		   S_IRUGO | S_IWUSR);
module_param_array_named(minfile, lowmem_minfile, uint, &lowmem_minfile_size,
			 S_IRUGO | S_IWUSR);
#ifdef CONFIG_SWAP
module_param_named(fudgeswap, fudgeswap, int, S_IRUGO | S_IWUSR);
#endif

module_init(lowmem_init);
module_exit(lowmem_exit);

MODULE_LICENSE("GPL");