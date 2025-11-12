// magician.c
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/version.h>
#include <linux/errno.h>
#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/printk.h>
#include <linux/uaccess.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hud Miller");
MODULE_DESCRIPTION("/dev/magician: Reads a variable from the file /proc/magician, and outputs an infinite supply of that variable.");
MODULE_VERSION("1.0");

// Usually, I try to be more direct with names, but I couldn't come up with anything that made sense and was one word.
// it's a "magician" because it operates like the classic card trick setup. you're picking a card from the deck with /proc/magician,
// and /dev/magician goes "is this your card?" when it prints that value.

// PROTOTYPES
static int device_open(struct inode *, struct file *);
static int device_release(struct inode *, struct file *);
static ssize_t device_read(struct file *, char __user *, size_t, loff_t *);
static ssize_t device_write(struct file *, const char __user *, size_t, loff_t *);

static ssize_t procfile_read(struct file *, char __user *, size_t, loff_t *);
static ssize_t procfile_write(struct file *, const char __user *, size_t , loff_t *);

//DIRECTIVES 

// the guide I read used something like this to ensure compatibility with different kernel versions.
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
#define HAVE_PROC_OPS
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
#define NEW_CLASS_CREATE
#endif

// names as they appear in execution (/proc/magician and /dev/magician) 
#define PROC_NAME "magician"
#define DEV_NAME "magician"

// max size of the buffer for procfs
#define MAX_SIZE 1024

// STATIC VALUES

static int major; // major number assigned to the device driver

// track usage to prevent multiple access.
enum {
    DEVICE_NOT_USED,
    DEVICE_EXCLUSIVE_OPEN
};
static atomic_t already_open = ATOMIC_INIT(DEVICE_NOT_USED);

static struct proc_dir_entry *our_proc_file;

// "card" is the saved value we want to repeat endlessly.
static char card[MAX_SIZE];
static unsigned long card_size = 0;

static struct class *cls;
static struct device *dev;


//file operations structs for our device and proc

static struct file_operations device_fops = {
    .read = device_read,
    .write = device_write,
    .open = device_open, 
    .release = device_release
};

//creates the correct proc_file_fops based on if we have the right struct available
#ifdef HAVE_PROC_OPS
static const struct proc_ops proc_file_fops = {
    .proc_read = procfile_read,
    .proc_write = procfile_write,
};
#else
static const struct file_operations proc_file_fops = {
    .read = procfile_read,
    .write = procfile_write,
};
#endif

// DEV

// called when a process tries to open the device file.
// this should mark the device as in use.
static int device_open(struct inode *inode, struct file *file) {
    if(atomic_cmpxchg(&already_open, DEVICE_NOT_USED, DEVICE_EXCLUSIVE_OPEN)) {
        return -EBUSY;
    }
    return 0;
}

// called when a process closes the device file.
// this should just mark the device as Not In Use.
static int device_release(struct inode *inode, struct file *file) {
    // mark the device as open again
    atomic_set(&already_open, DEVICE_NOT_USED);
    return 0;
}

// called when a process attempts to read from the device.
// returns an infinite source of the value written to the /proc file,
// similar to the behavior of /dev/zero.
static ssize_t device_read(struct file *filp, char __user *buffer, size_t length, loff_t *offset) {

    if(card_size == 0) {
        return 0; // There is nothing to read.
    }

    size_t i;

    for(i = 0; i < length; i++) {
        if(put_user(card[i % card_size], buffer++)) {
            return -EFAULT;
        }
    }
    *offset += length;
    return length;

}

//called when a process writes to the dev file.
//we don't really allow writing to /dev/magician,
// so this should just be basic error stuff.
static ssize_t device_write(struct file *filp, const char __user *buffer, size_t length, loff_t *offset) {
    pr_alert("This operation is not supported. Did you mean to write to /proc/%s ?\n", PROC_NAME);
    return length; //returning length is still the expected behavior
}

// PROC

// READ /proc/magician
// expected behavior: Just read the value.
static ssize_t procfile_read(struct file *filp, char __user *buffer, size_t length, loff_t *offset) {
    
    if(*offset > 0 || card_size == 0) {
        return 0; // end of file.
    }
    
    if(copy_to_user(buffer, card, card_size)) {
        pr_info("copy_to_user failed\n");
        return -EFAULT;
    }
    
    *offset += card_size;
    return card_size;
}

// WRITE /proc/magician
// expected behavior: saves to msg.
static ssize_t procfile_write(struct file *filp, const char __user *buffer, size_t length, loff_t *offset) {
    card_size = length;
    if(card_size >= MAX_SIZE) {
        card_size = MAX_SIZE - 1;
    }

    if(copy_from_user(card, buffer, card_size)) {
        return -EFAULT;
    }

    card[card_size] = '\0';
    *offset += card_size;

    pr_info("procfile write %s\n", card);

    return card_size;
}

// should create /proc/magician and /dev/magician
static int __init magician_init(void) {
    int err;

    our_proc_file = proc_create(PROC_NAME, 0644, NULL, &proc_file_fops);
    if(NULL == our_proc_file) {
        pr_alert("Error: Could not initialize /proc/%s\n", PROC_NAME);
        return -ENOMEM;
    }
    pr_info("/proc/%s created\n", PROC_NAME);
    
    major = register_chrdev(0, DEV_NAME, &device_fops);
    if(major < 0) {
        pr_alert("Registering char device failed with %d\n", major);
        err = major;
        goto remove_proc; //cleanup earlier code
    }
    pr_info("Magician assigned to major number %d.\n", major);


#ifdef NEW_CLASS_CREATE
    cls = class_create(DEV_NAME);
#else
    cls = class_create(THIS_MODULE, DEV_NAME);
#endif
    if(IS_ERR(cls)) {
        err = PTR_ERR(cls);
        goto unregister_chrdev;
    }

    dev = device_create(cls, NULL, MKDEV(major, 0), NULL, DEV_NAME);
    if(IS_ERR(dev)) {
        err = PTR_ERR(cls);
        goto destroy_class;
    }
    pr_info("Device created on /dev/%s\n", DEV_NAME);

    memset(card, 0, sizeof(card));
    return 0;

destroy_class:
    class_destroy(cls);
unregister_chrdev:
    unregister_chrdev(major, DEV_NAME);
remove_proc:
    proc_remove(our_proc_file);
    return err;
}

// cleanup of /proc/magician and /dev/magician
static void __exit magician_exit(void) {
    proc_remove(our_proc_file);
    pr_info("/proc/%s removed\n", PROC_NAME);

    device_destroy(cls, MKDEV(major, 0));
    class_destroy(cls);

    // unregister the device
    unregister_chrdev(major, DEV_NAME);
    pr_info("/dev/%s removed\n", DEV_NAME);
}

module_init(magician_init);
module_exit(magician_exit);