/*  Semaphore Auto Brightness driver
 *  for Samsung Galaxy S I9000
 *  
 *   Copyright (c) 2011-2012 stratosk@semaphore.gr
 *   
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/workqueue.h>	
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/miscdevice.h>
#include <linux/device.h>
#include <linux/earlysuspend.h>


#define AUTOBR_WORK_QUEUE_NAME "kautobr"

#define DEF_MIN_BRIGHTNESS		(15)
#define DEF_MAX_BRIGHTNESS		(255)
#define DEF_INSTANT_UPD_THRESHOLD	(30)
#define DEF_MAX_LUX			(2900)
#define DEF_EFFECT_DELAY_MS		(0)
#define DEF_BLOCK_FW			(1)	
	
#define SAMPLE_PERIOD 400000 /* Check every 400000us */

#define DRV_MAX_BRIGHTNESS		(255)
#define DRV_MIN_BRIGHTNESS		(1)
#define DRV_MAX_LUX			(3000)
#define DRV_MAX_UPD_THRESHOLD		(100)
#define DRV_MAX_EFFECT_DELAY		(10)
	
/* min_brightness: the minimun brightness that will be used 
 * max_brightness: the maximum brightness that will be used 
 * instant_upd_threshold: the difference threshold that we have to update 
 * instantly 
 * max_lux: max value from the light sensor 
 * effect_delay_ms: delay between step for the fade effect 
 * block_fw: block framework brighness updates 
 */
static struct sema_ab_tuners {
	unsigned int min_brightness;
	unsigned int max_brightness;
	unsigned int instant_upd_threshold;
	unsigned int max_lux;
	int effect_delay_ms;
	unsigned int block_fw;
} sa_tuners = {
	.min_brightness = DEF_MIN_BRIGHTNESS,
	.max_brightness = DEF_MAX_BRIGHTNESS,
	.instant_upd_threshold = DEF_INSTANT_UPD_THRESHOLD,
	.max_lux = DEF_MAX_LUX,
	.effect_delay_ms = DEF_EFFECT_DELAY_MS,
	.block_fw = DEF_BLOCK_FW,
};

extern int ls_get_adcvalue(void);
extern int bl_update_brightness(int bl);
extern void block_bl_update(void);
extern void unblock_bl_update(void);
extern void block_ls_update(void);
extern void unblock_ls_update(void);

static void autobr_handler(struct work_struct *w);
static struct workqueue_struct *wq = 0;
static DECLARE_DELAYED_WORK(autobr_wq, autobr_handler);

static struct sema_ab_info {
	unsigned int current_br;	/* holds the current brightness */
	unsigned int update_br;	/* the brightness value that we have to reach */
	unsigned int sum_update_br;	/* the sum of samples */
	unsigned int cnt;
	unsigned int delay;
} sa_info;

/************************** sysfs interface ************************/

static ssize_t show_current_br(struct device *dev, 
				struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", sa_info.current_br);
}

static ssize_t show_min_brightness(struct device *dev, 
				   struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", sa_tuners.min_brightness);
}

static ssize_t store_min_brightness(struct device *dev, 
		struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned int input;
	int ret;
	
	ret = sscanf(buf, "%u", &input);
	if (ret != 1 || input < 1 || input > DRV_MAX_BRIGHTNESS || 
		input > sa_tuners.max_brightness) 
		return -EINVAL;
	
	sa_tuners.min_brightness = input;

	return size;
}
 
static ssize_t show_max_brightness(struct device *dev, 
				   struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", sa_tuners.max_brightness);
}

static ssize_t store_max_brightness(struct device *dev, 
		struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned int input;
	int ret;
	
	ret = sscanf(buf, "%u", &input);
	if (ret != 1 || input < 1 || input > DRV_MAX_BRIGHTNESS || 
		input < sa_tuners.min_brightness) 
		return -EINVAL;
	
	sa_tuners.max_brightness = input;

	return size;
}

static ssize_t show_instant_upd_threshold(struct device *dev, 
				struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", sa_tuners.instant_upd_threshold);
}

static ssize_t store_instant_upd_threshold(struct device *dev, 
		struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned int input;
	int ret;
	
	ret = sscanf(buf, "%u", &input);
	if (ret != 1 || input < 1 || input > DRV_MAX_UPD_THRESHOLD) 
		return -EINVAL;
	
	sa_tuners.instant_upd_threshold = input;

	return size;
}

static ssize_t show_max_lux(struct device *dev, struct device_attribute *attr, 
								char *buf)
{
	return sprintf(buf, "%u\n", sa_tuners.max_lux);
}

static ssize_t store_max_lux(struct device *dev, struct device_attribute *attr, 
						const char *buf, size_t size)
{
	unsigned int input;
	int ret;
	
	ret = sscanf(buf, "%u", &input);
	if (ret != 1 || input < 1 || input > DRV_MAX_LUX)
		return -EINVAL;
	
	sa_tuners.max_lux = input;

	return size;
}

static ssize_t show_effect_delay_ms(struct device *dev, 
				    struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%i\n", sa_tuners.effect_delay_ms);
}

static ssize_t store_effect_delay_ms(struct device *dev, 
		struct device_attribute *attr, const char *buf, size_t size)
{
	int input;
	int ret;
	
	ret = sscanf(buf, "%i", &input);
	if (ret != 1 || input < -1 || input > DRV_MAX_EFFECT_DELAY)
		return -EINVAL;
	
	sa_tuners.effect_delay_ms = input;

	return size;
}

static ssize_t show_block_fw(struct device *dev, struct device_attribute *attr, 
								char *buf)
{
	return sprintf(buf, "%u\n", sa_tuners.block_fw);
}

static ssize_t store_block_fw(struct device *dev, struct device_attribute *attr,
						const char *buf, size_t size)
{
	unsigned int input;
	int ret;
	
	ret = sscanf(buf, "%u", &input);
	if (ret != 1 || input > 1) 
		return -EINVAL;
	
	sa_tuners.block_fw = input;

	if (sa_tuners.block_fw) 
		block_bl_update();
	else
		unblock_bl_update();

	return size;
}

static DEVICE_ATTR(current_br, S_IRUGO, show_current_br, NULL);
static DEVICE_ATTR(min_brightness, S_IRUGO | S_IWUGO , show_min_brightness, 
							store_min_brightness);
static DEVICE_ATTR(max_brightness, S_IRUGO | S_IWUGO , show_max_brightness, 
							store_max_brightness);
static DEVICE_ATTR(instant_upd_threshold, S_IRUGO | S_IWUGO , 
			show_instant_upd_threshold, store_instant_upd_threshold);
static DEVICE_ATTR(max_lux, S_IRUGO | S_IWUGO , show_max_lux, store_max_lux);
static DEVICE_ATTR(effect_delay_ms, S_IRUGO | S_IWUGO , 
			show_effect_delay_ms, store_effect_delay_ms);
static DEVICE_ATTR(block_fw, S_IRUGO | S_IWUGO , show_block_fw, store_block_fw);
 
static struct attribute *sema_autobr_attributes[] = {
	&dev_attr_current_br.attr,
	&dev_attr_min_brightness.attr,
	&dev_attr_max_brightness.attr,
	&dev_attr_instant_upd_threshold.attr,
	&dev_attr_max_lux.attr,
	&dev_attr_effect_delay_ms.attr,
	&dev_attr_block_fw.attr,
	NULL
};

static struct attribute_group sema_autobr_group = {
	.attrs  = sema_autobr_attributes,
};

static struct miscdevice sema_autobr_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "sema_autobr",
};

/************************** sysfs end ************************/

static void step_update(void)
{
	int ret;
	
	(sa_info.current_br < sa_info.update_br) ? 
		++sa_info.current_br : --sa_info.current_br; 
	
	ret = bl_update_brightness(sa_info.current_br);
}

static void instant_update(void)
{
	int ret;
	
	if (sa_info.current_br < sa_info.update_br) {
		if (sa_tuners.effect_delay_ms >= 0) {
			while (sa_info.current_br++ < sa_info.update_br) {	
			/* fade in */
				ret = bl_update_brightness(sa_info.current_br);
				msleep(sa_tuners.effect_delay_ms);
		    }
		} else {
			sa_info.current_br = sa_info.update_br;
			ret = bl_update_brightness(sa_info.current_br);
		}
	} else {
		if (sa_tuners.effect_delay_ms >= 0) {
			while (sa_info.current_br-- > sa_info.update_br) {	
			/* fade out */
				ret = bl_update_brightness(sa_info.current_br);
				msleep(sa_tuners.effect_delay_ms);
		    }			  
		} else {
			sa_info.current_br = sa_info.update_br;
			ret = bl_update_brightness(sa_info.current_br);
		}
	}		
}

static void autobr_handler(struct work_struct *w)
{
	int diff;
	
	/* Get the adc value from light sensor and 
	   normalize it to 0 - max_brightness scale */
	sa_info.sum_update_br += ls_get_adcvalue() * sa_tuners.max_brightness / 
							sa_tuners.max_lux;
	
	if (++sa_info.cnt < 5)
		goto NEXT_ITER;
	
	/* Get the average after 5 samples and only then adjust the brightness*/
	sa_info.update_br = sa_info.sum_update_br / 5;
	
	/* cap the update brightness within the limits */
	if (sa_info.update_br < sa_tuners.min_brightness)
		sa_info.update_br = sa_tuners.min_brightness;
	if (sa_info.update_br > sa_tuners.max_brightness)
		sa_info.update_br = sa_tuners.max_brightness;
	
	/* the difference between current and update brightness */
	diff = abs(sa_info.current_br - sa_info.update_br);
	
	if (diff > 1 && diff <= sa_tuners.instant_upd_threshold) {
		/* update one step every SAMPLE_PERIOD * 5 */
		step_update();
		
	} else if (diff > sa_tuners.instant_upd_threshold) {
		/* instantly update */		
		instant_update();
	} else
		sa_info.current_br = sa_info.update_br;
	
	/* reset counters */
	sa_info.sum_update_br = 0;
	sa_info.cnt = 0;

NEXT_ITER:	
	if (wq)
		queue_delayed_work(wq, &autobr_wq, sa_info.delay);
}

static void powersave_early_suspend(struct early_suspend *handler)
{
	if (wq) {
		cancel_delayed_work(&autobr_wq);	
		flush_workqueue(wq);
	}
}

static void powersave_late_resume(struct early_suspend *handler)
{
	if (wq)
		queue_delayed_work(wq, &autobr_wq, sa_info.delay);
}

static struct early_suspend _powersave_early_suspend = {
	.suspend = powersave_early_suspend,
	.resume = powersave_late_resume,
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN,
};

static int autobr_init(void)
{
	
	misc_register(&sema_autobr_device);
	if (sysfs_create_group(&sema_autobr_device.this_device->kobj, 
							&sema_autobr_group) < 0)
	{
		printk("%s sysfs_create_group fail\n", __FUNCTION__);
		pr_err("Failed to create sysfs group for device (%s)!\n", 
						sema_autobr_device.name);
	}  
  
	/* initial values */
	sa_info.current_br = 120;
	sa_info.cnt = 0;
	sa_info.delay = usecs_to_jiffies(SAMPLE_PERIOD);
	
	if (!wq)
                wq = create_workqueue(AUTOBR_WORK_QUEUE_NAME);
	
	if (wq)
		queue_delayed_work(wq, &autobr_wq, sa_info.delay);

	block_bl_update();

	register_early_suspend(&_powersave_early_suspend);

	printk(KERN_INFO "Semaphore Auto Brightness enabled\n");
	
	return 0;
}

static void autobr_exit(void)
{
	misc_deregister(&sema_autobr_device);
	
	if (wq) {
		cancel_delayed_work(&autobr_wq);	
		flush_workqueue(wq);
		destroy_workqueue(wq);
	}

	unblock_bl_update();

	unregister_early_suspend(&_powersave_early_suspend);

	printk(KERN_INFO "Semaphore Auto Brightness disabled\n");
}

module_init(autobr_init);
module_exit(autobr_exit);

MODULE_AUTHOR("stratosk@semaphore.gr");
MODULE_DESCRIPTION("Semaphore Auto Brightness driver");
MODULE_LICENSE("GPL");