#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <linux/wait.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/kdev_t.h>
#include <linux/mutex.h>

#include <asm/io.h>
#include <asm/delay.h>
#include <mach/platform.h>

#define MAX_SENSORS		2

#define TRIGGER1_PIN	17
#define ECHO1_PIN		4
#define TRIGGER2_PIN	22
#define ECHO2_PIN		27

// description for /proc/interrupt
#define ECHO_PIN_DESC		"HC-SR04 echo interrupt"
#define TRIGGER_PIN_DESC	"HC-SR04 trigger pin"
#define DEVICE_DESC			"HC-SR04 sonar"

// see linux/Documentation/devices.txt (240-254 char = LOCAL/EXPERIMENTAL USE)
// use 0 for dynamic
#define DEV_MAJOR		240

#define PERROR(ERRSTR, ERRNO)	printk(KERN_ERR "HC-SR04: %s %d\n", ERRSTR, ERRNO);

#define DEV_ID			NULL

struct raspberry_gpio {
	uint32_t GPFSEL[6];
	uint32_t Reserved1;
	uint32_t GPSET[2];
	uint32_t Reserved2;
	uint32_t GPCLR[2];
};
static struct raspberry_gpio * const gpio_b = (struct raspberry_gpio *) __io_address(GPIO_BASE);
// file operations prototypes
static int hc_sr04_open(struct inode *inode, struct file *file);
static int hc_sr04_release(struct inode *inode, struct file *file);
static ssize_t hc_sr04_read(struct file *filp, char *buffer, size_t length, loff_t * offset);

static short int irq1id, irq2id;
static int opened[MAX_SENSORS];
static int data1_ready, data2_ready;
static ktime_t end1, end2, last_call;
DECLARE_WAIT_QUEUE_HEAD(int1_wait_queue); // type wait_queue_head_t
DECLARE_WAIT_QUEUE_HEAD(int2_wait_queue);
DEFINE_MUTEX(triggered_mutex);
#if DEV_MAJOR == 0
static u8 dev_major;
#endif

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.read = hc_sr04_read,
	.open = hc_sr04_open,
	.release = hc_sr04_release
};

static irqreturn_t hc_sr04_irq1_handler(int irq, void *dev_id, struct pt_regs *regs) {
	end1 = ktime_get();
	data1_ready = 1;
	wake_up(&int1_wait_queue);
	return IRQ_HANDLED;
}

static irqreturn_t hc_sr04_irq2_handler(int irq, void *dev_id, struct pt_regs *regs) {
	end2 = ktime_get();
	data2_ready = 1;
	wake_up(&int2_wait_queue);
	return IRQ_HANDLED;
}

static int __init hc_sr04_init(void) {
	int err;
	
	if ((err = gpio_request(ECHO1_PIN, ECHO_PIN_DESC))) {
		PERROR("gpio_request", err);
		return -1;
	}
	if ((err = gpio_request(ECHO2_PIN, ECHO_PIN_DESC))) {
		PERROR("gpio_request", err);
		return -1;
	}
	
	if ((err = gpio_request(TRIGGER1_PIN, TRIGGER_PIN_DESC))) {
		PERROR("gpio_request", err);
		return -1;
	}
	if ((err = gpio_request(TRIGGER2_PIN, TRIGGER_PIN_DESC))) {
		PERROR("gpio_request", err);
		return -1;
	}
	
	if (((irq1id = gpio_to_irq(ECHO1_PIN)) < 0)) {
		PERROR("gpio_request", err);
		return -1;
	}
	if (((irq2id = gpio_to_irq(ECHO2_PIN)) < 0)) {
		PERROR("gpio_request", err);
		return -1;
	}
	
	// IRQF_DISABLED indicates a fast interrupt handler (won't be interrupted)
	if ((err = request_irq(irq1id, (irq_handler_t) hc_sr04_irq1_handler, IRQF_TRIGGER_FALLING,
	DEVICE_DESC, DEV_ID))) {
		PERROR("request_irq", err);
		return -1;
	}
	if ((err = request_irq(irq2id, (irq_handler_t) hc_sr04_irq2_handler, IRQF_TRIGGER_FALLING,
	DEVICE_DESC, DEV_ID))) {
		PERROR("request_irq", err);
		return -1;
	}
	
	if ((err = register_chrdev(DEV_MAJOR, DEVICE_DESC, &fops)) < 0) {
		PERROR("register_chrdev", err);
		return -1;
	}
	
#if DEV_MAJOR == 0
	dev_major = err;
	printk(KERN_INFO "HC-SR04: Device major: %hhu\n", dev_major);
#endif
	
	gpio_direction_output(TRIGGER1_PIN, 0);
	gpio_direction_output(TRIGGER2_PIN, 0);
	
	printk(KERN_INFO "HC-SR04: Driver initialized\n");
	
	return 0;
}

static void __exit hc_sr04_exit(void) {
#if DEV_MAJOR == 0
	unregister_chrdev(dev_major, DEVICE_DESC);
#else
	unregister_chrdev(DEV_MAJOR, DEVICE_DESC);
#endif
	free_irq(irq1id, DEV_ID);
	free_irq(irq2id, DEV_ID);
	gpio_free(ECHO1_PIN);
	gpio_free(ECHO2_PIN);
	gpio_free(TRIGGER1_PIN);
	gpio_free(TRIGGER2_PIN);
	printk(KERN_INFO "HC-SR04: Driver unloaded\n");
}

static int hc_sr04_open(struct inode *inode, struct file *filp) {
	if (MINOR(inode->i_rdev) >= MAX_SENSORS) {
		printk("HC-SR04: Invalid minor dev number\n");
		return -EINVAL;
	}
	if (opened[MINOR(inode->i_rdev)]) {
		printk(KERN_DEBUG "HC-SR04: Already opened\n");
		return -EBUSY;
	}
	filp->private_data = ((void *) MINOR(inode->i_rdev));
	opened[MINOR(inode->i_rdev)] = 1;
	return 0;
}

static int hc_sr04_release(struct inode *inode, struct file *filp) {
	if (MINOR(inode->i_rdev) >= MAX_SENSORS)
		return -EINVAL;
	opened[MINOR(inode->i_rdev)] = 0;
	return 0;
}

#define RELEASE_TRIGGERED_MUTEX_RETURN(r)	\
	do {mutex_unlock(&triggered_mutex); return r;} while (0)
static ssize_t hc_sr04_read(struct file *filp, char *buffer, size_t length, loff_t * offset) {
	ktime_t start;
	u64 distance;
	u64 *buf64 = (u64 *) buffer;
	int err;
	
	if (mutex_lock_interruptible(&triggered_mutex))
		return -ERESTARTSYS;
	
	if (length != sizeof(distance)) {
		printk(KERN_DEBUG "HC-SR04: Invalid size\n");
		RELEASE_TRIGGERED_MUTEX_RETURN(-EINVAL);
	}
	
	if (ktime_to_ns(ktime_sub(ktime_get(), last_call)) < 600 * 1000) {
		printk(KERN_DEBUG "HC-SR04: Consecutive calls\n");
		RELEASE_TRIGGERED_MUTEX_RETURN(-EBUSY);
	}
	
	switch (((int) filp->private_data)) {
		case 0:
		data1_ready = 0;
		gpio_set_value(TRIGGER1_PIN, 1);
		udelay(10);
		gpio_set_value(TRIGGER1_PIN, 0);
		start = ktime_get();
		// https://www.kernel.org/doc/htmldocs/device-drivers/API-wait-event-timeout.html
		wait_event_timeout(int1_wait_queue, data1_ready, HZ / 16);
		if (!data1_ready) {
			printk(KERN_ERR "HC-SR04: 0 timed out\n");
			RELEASE_TRIGGERED_MUTEX_RETURN(-EIO);
		}
		distance = ktime_to_ns(ktime_sub(end1, start));
		/*if (distance > 1000 * 1000 * 100) {
			printk(KERN_ERR "HC-SR04: 0 I/O error\n");
			return -EIO;
		}*/
		break;
		
		case 1:
		data2_ready = 0;
		gpio_set_value(TRIGGER2_PIN, 1);
		udelay(10);
		#warning delay é bigolas
		gpio_set_value(TRIGGER2_PIN, 0);
		start = ktime_get();
		// https://www.kernel.org/doc/htmldocs/device-drivers/API-wait-event-timeout.html
		wait_event_timeout(int2_wait_queue, data2_ready, HZ / 16);
		if (!data2_ready) {
			printk(KERN_ERR "HC-SR04: 1 timed out\n");
			RELEASE_TRIGGERED_MUTEX_RETURN(-EIO);
		}
		distance = ktime_to_ns(ktime_sub(end2, start));
		/*if (distance > 1000 * 1000 * 100) {
			printk(KERN_ERR "HC-SR04: 1 I/O error\n");
			return -EIO;
		}*/
		break;
		
		default:
		printk(KERN_DEBUG "HC-SR04: Invalid device minor %d\n", (int) filp->private_data);
		RELEASE_TRIGGERED_MUTEX_RETURN(-EINVAL);
	}
	
	if ((err = put_user(distance, buf64)) < 0) {
		PERROR("put_user", err);
		RELEASE_TRIGGERED_MUTEX_RETURN(-EACCES);
	}
	
	//printk(KERN_DEBUG "HC-SR04: success!\n");
	last_call = ktime_get();
	RELEASE_TRIGGERED_MUTEX_RETURN(sizeof(distance));
}

module_init(hc_sr04_init);
module_exit(hc_sr04_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tiago Koji Castro Shibata <tiago.shibata@thunderatz.org> <tishi@linux.com>");
MODULE_DESCRIPTION("HC-SR04 ultrassonic sensor driver");
