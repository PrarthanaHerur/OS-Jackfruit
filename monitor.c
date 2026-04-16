#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/signal.h>
#include "monitor_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("student");
MODULE_DESCRIPTION("Container Memory Monitor");

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_MS 1000

struct container_entry {
    struct list_head list;
    pid_t            pid;
    char             id[MONITOR_NAME_LEN];
    long             soft_limit_bytes;
    long             hard_limit_bytes;
    int              soft_warned;
};

static LIST_HEAD(container_list);
static DEFINE_MUTEX(list_lock);
static dev_t              dev_num;
static struct cdev        c_dev;
static struct class      *cl;
static struct timer_list  monitor_timer;

static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    long rss = -1;
    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task && task->mm)
        rss = get_mm_rss(task->mm) << PAGE_SHIFT;
    rcu_read_unlock();
    return rss;
}

static void check_memory(struct timer_list *t)
{
    struct container_entry *entry, *tmp;
    mutex_lock(&list_lock);
    list_for_each_entry_safe(entry, tmp, &container_list, list) {
        long rss = get_rss_bytes(entry->pid);
        if (rss < 0) {
            pr_info("[container_monitor] pid %d (%s) gone, removing\n",
                    entry->pid, entry->id);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }
        if (rss > entry->hard_limit_bytes) {
            pr_warn("[container_monitor] HARD LIMIT: '%s' pid=%d rss=%ldMB > hard=%ldMB KILLING\n",
                    entry->id, entry->pid,
                    rss/(1024*1024),
                    entry->hard_limit_bytes/(1024*1024));
            kill_pid(find_vpid(entry->pid), SIGKILL, 1);
            list_del(&entry->list);
            kfree(entry);
        } else if (rss > entry->soft_limit_bytes && !entry->soft_warned) {
            pr_warn("[container_monitor] SOFT LIMIT: '%s' pid=%d rss=%ldMB > soft=%ldMB\n",
                    entry->id, entry->pid,
                    rss/(1024*1024),
                    entry->soft_limit_bytes/(1024*1024));
            entry->soft_warned = 1;
        }
    }
    mutex_unlock(&list_lock);
    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(CHECK_INTERVAL_MS));
}

static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;
    struct container_entry *entry, *tmp;

    switch (cmd) {
    case MONITOR_REGISTER:
        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;
        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry) return -ENOMEM;
        entry->pid              = req.pid;
        entry->soft_limit_bytes = (long)req.soft_limit_bytes;
        entry->hard_limit_bytes = (long)req.hard_limit_bytes;
        entry->soft_warned      = 0;
        strncpy(entry->id, req.container_id, MONITOR_NAME_LEN-1);
        entry->id[MONITOR_NAME_LEN-1] = '\0';
        mutex_lock(&list_lock);
        list_add(&entry->list, &container_list);
        mutex_unlock(&list_lock);
        pr_info("[container_monitor] registered '%s' pid=%d soft=%luMB hard=%luMB\n",
                entry->id, entry->pid,
                req.soft_limit_bytes/(1024*1024),
                req.hard_limit_bytes/(1024*1024));
        return 0;

    case MONITOR_UNREGISTER:
        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;
        mutex_lock(&list_lock);
        list_for_each_entry_safe(entry, tmp, &container_list, list) {
            if (entry->pid == req.pid) {
                list_del(&entry->list);
                kfree(entry);
                break;
            }
        }
        mutex_unlock(&list_lock);
        return 0;

    default:
        return -EINVAL;
    }
}

static const struct file_operations fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

static int __init monitor_init(void)
{
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0)
        return -1;
    cl = class_create(DEVICE_NAME);
    if (IS_ERR(cl)) {
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }
    if (!device_create(cl, NULL, dev_num, NULL, DEVICE_NAME)) {
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }
    cdev_init(&c_dev, &fops);
    if (cdev_add(&c_dev, dev_num, 1) < 0) {
        device_destroy(cl, dev_num);
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }
    timer_setup(&monitor_timer, check_memory, 0);
    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(CHECK_INTERVAL_MS));
    pr_info("[container_monitor] Module loaded. Device: /dev/%s\n", DEVICE_NAME);
    return 0;
}

static void __exit monitor_exit(void)
{
    struct container_entry *entry, *tmp;
    timer_delete_sync(&monitor_timer);
    mutex_lock(&list_lock);
    list_for_each_entry_safe(entry, tmp, &container_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&list_lock);
    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);
    pr_info("[container_monitor] Module unloaded.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
