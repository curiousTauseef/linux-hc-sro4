/* Precise measurements of time delta between sending a trigger signal
   to the HC-SRO4 distance sensor and receiving the echo signal from
   the sensor back. This has to be precise in the us range. We (for now)
   do this with local_irq_disable and polling (with timeout). 

   DO NOT attach your HC-SRO4's echo pin directly to the raspberry, since
   it runs with 5V while raspberry expects 3V on the GPIO inputs.

*/

/* TODO: check this for measure / delete races ! fix locking */
/* TODO: destroy */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/timekeeping.h>
#include <linux/gpio/consumer.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/interrupt.h>
#include <linux/kdev_t.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/gpio/driver.h>
#include <linux/delay.h>
#include <linux/sched.h>

struct hc_sro4 {
	int gpio_echo;
	int gpio_trig;
	struct gpio_desc *echo_desc;
	struct gpio_desc *trig_desc;
	struct timeval time_triggered;
	struct timeval time_echoed;
	struct mutex measurement_mutex;
	wait_queue_head_t wait_for_echo;
	unsigned long timeout;
	struct list_head list;
};

static LIST_HEAD(hc_sro4_devices);
static DEFINE_MUTEX(devices_mutex);

static struct hc_sro4* create_hc_sro4(int echo, int trig, unsigned long timeout)
{
	struct hc_sro4 *new;
	int err;

	new = kmalloc(sizeof(*new), GFP_KERNEL);
	if (new == NULL) {
		return ERR_PTR(-ENOMEM);
	}

	new->gpio_echo = echo;
	new->gpio_trig = trig;
	new->echo_desc = gpio_to_desc(echo);
	if (new->echo_desc == NULL) {
		kfree(new);
		return ERR_PTR(-EINVAL);
	}
	new->trig_desc = gpio_to_desc(trig);
	if (new->trig_desc == NULL) {
		kfree(new);
		return ERR_PTR(-EINVAL);
	}

	err = gpiod_direction_input(new->echo_desc);
	if (err < 0) {
		kfree(new);
		return ERR_PTR(err);
	}
	err = gpiod_direction_output(new->trig_desc, 0);
	if (err < 0) {
		kfree(new);
		return ERR_PTR(err);
	}

	mutex_init(&new->measurement_mutex);
	init_waitqueue_head(&new->wait_for_echo);
	new->timeout = timeout;

	mutex_lock(&devices_mutex);
	list_add_tail(&new->list, &hc_sro4_devices);
	mutex_unlock(&devices_mutex);

	return new;
}

static int destroy_hc_sro4(struct hc_sro4 *device)
{
	mutex_lock(&devices_mutex);
	mutex_lock(&device->measurement_mutex);
	list_del(&device->list);
	mutex_unlock(&devices_mutex);
	kfree(device);

	return 0;
}

static irqreturn_t echo_received_irq(int irq, void *data)
{
	struct hc_sro4 *device = (struct hc_sro4*) data;
	
	do_gettimeofday(&device->time_echoed);	
	wake_up_interruptible(&device->wait_for_echo);
	
	return IRQ_HANDLED;
}

/* devices_mutex must be held by caller, so nobody deletes the device
   before we lock it. */

static int do_measurement(struct hc_sro4 *device, unsigned long long *usecs_elapsed)
{
	unsigned long timeout;
	int irq;
	int ret;

	if (!mutex_trylock(&device->measurement_mutex)) {
		mutex_unlock(&devices_mutex);
		return -EBUSY;
	}
	mutex_unlock(&devices_mutex);

/* TODO: if ACTIVE_LOW */

        ret = request_any_context_irq(irq, echo_received_irq, IRQF_SHARED | IRQF_TRIGGER_FALLING, "hc_sro4", device);
	if (ret < 0) 
		goto out_mutex;

        ret = gpiochip_lock_as_irq(gpiod_to_chip(device->echo_desc), device->gpio_echo);
	if (ret < 0) 
		goto out_irq;

	gpiod_set_value(device->trig_desc, 1);
	udelay(10);
	gpiod_set_value(device->trig_desc, 0);	 /* TODO: ?? or leave it until echo reveiced.. */

	do_gettimeofday(&device->time_triggered);

	timeout = wait_event_interruptible_timeout(device->wait_for_echo, 1, device->timeout);
	if (timeout == 0)
		ret = -ETIMEDOUT;
	else if (timeout < 0)
		ret = timeout;
	else
		*usecs_elapsed = 
			(device->time_echoed.tv_sec - device->time_triggered.tv_sec) * 1000000 + 
			(device->time_echoed.tv_usec - device->time_triggered.tv_usec);

out_irq:
	free_irq(irq, device);
out_mutex:
	mutex_unlock(&device->measurement_mutex);

	return ret;
}

static ssize_t sysfs_do_measurement(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct hc_sro4 *sensor = dev_get_drvdata(dev);
	unsigned long long usecs_elapsed;
	int status;

	mutex_lock(&devices_mutex);
	status = do_measurement(sensor, &usecs_elapsed);
/*	mutex_unlock(&devices_mutex);  TODO: will be unlocked inside, this is not good. */

	if (status < 0) 
		return status;

	return sprintf(buf, "%lld\n", usecs_elapsed);
}

DEVICE_ATTR(measure, 0444, sysfs_do_measurement, NULL);


static struct attribute *sensor_attrs[] = {
	&dev_attr_measure.attr,
	NULL,
};

static const struct attribute_group sensor_group = {
	.attrs = sensor_attrs
};

static const struct attribute_group *sensor_groups[] = {
	&sensor_group, 
	NULL
};

static ssize_t sysfs_configure_store(struct class *class,
				struct class_attribute *attr,
				const char *buf, size_t len)
{
	int add = buf[0] != '-';
	const char *s = buf;
	int trig, echo, timeout;
	struct hc_sro4 *new_sensor;

	if (buf[0] == '-' || buf[0] == '+') s++;
	if (add) {
		if (sscanf(s, "%d %d %d", &trig, &echo, &timeout) != 3)
			return -EINVAL;

		new_sensor = create_hc_sro4(trig, echo, timeout);
		if (IS_ERR(new_sensor))
			return PTR_ERR(new_sensor);

		device_create_with_groups(class, NULL, MKDEV(0, 0), new_sensor, sensor_groups, "distance_%d_%d", trig, echo);
	} else {
		return -ENOSYS;
	}
	return len;
}

static struct class_attribute hc_sro4_class_attrs[] = {
	__ATTR(configure, 0200, NULL, sysfs_configure_store),
	__ATTR_NULL,
};

static struct class hc_sro4_class = {
	.name = "distance-sensor",
	.owner = THIS_MODULE,
	.class_attrs = hc_sro4_class_attrs
};

static int __init init_hc_sro4(void)
{
	printk(KERN_INFO "Hello world\n");

        return class_register(&hc_sro4_class);
}

module_init(init_hc_sro4);
	
MODULE_AUTHOR("Johannes Thoma");
MODULE_DESCRIPTION("Distance measurement for the HC-SRO4 ultrasonic distance sensor for the raspberry pi");
MODULE_LICENSE("GPL");

