/*
 * linux/drivers/power/wave_charger.c
 *
 * Battery charger driver for Samsung Wave phone
 * This driver does implement externally modifable measurement params.
 *
 * based on s5pc110_battery.c
 *
 * Copyright (C) 2009 Samsung Electronics
 * Copyright (C) 2012 Dominik Marszk <dmarszk@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#define DEBUG

#include <asm/mach-types.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mfd/max8998.h>
#include <linux/mfd/max8998-private.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regulator/driver.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wakelock.h>
#include <linux/workqueue.h>
#include <mach/battery.h>

#ifdef CONFIG_MACH_HERRING
#include <mach/gpio-herring.h>
#endif

#ifdef CONFIG_MACH_ARIES
#include <mach/gpio-aries.h>
#endif

#ifdef CONFIG_MACH_WAVE
#include <mach/gpio-wave.h>
#endif

#include <mach/hardware.h>
#include <mach/map.h>
#include <mach/regs-clock.h>
#include <mach/regs-gpio.h>
#include <plat/gpio-cfg.h>
#include <linux/android_alarm.h>
#include <linux/mfd/max8998.h>

#define DRIVER_NAME	"wave_charger"

enum {
    CHARGING_MODE_BOOTING,
	CHARGING_STATUS,
    BATT_TEMP_CHECK,
    BATT_FULL_CHECK,
	BATT_PERCENTAGE,
	BATT_TEMP,
};

#define TOTAL_CHARGING_TIME	(6*60*60)	/* 6 hours */
#define TOTAL_RECHARGING_TIME	  (90*60)	/* 1.5 hours */


#define RECHARGE_COND_VOLTAGE		4130000
#define RECHARGE_COND_TIME		(30*1000)	/* 30 seconds */


#define FAST_POLL			(1 * 60)
#define SLOW_POLL			(10 * 60)

#define DISCONNECT_BAT_FULL		0x1
#define DISCONNECT_TEMP_OVERHEAT	0x2
#define DISCONNECT_TEMP_FREEZE		0x4
#define DISCONNECT_OVER_TIME		0x8

#define ATTACH_USB	1
#define ATTACH_TA	2

#if defined (CONFIG_SAMSUNG_GALAXYS) || defined (CONFIG_SAMSUNG_GALAXYSB) || defined(CONFIG_SAMSUNG_CAPTIVATE)
  #define HIGH_BLOCK_TEMP               630
  #define HIGH_RECOVER_TEMP             580
  #define LOW_BLOCK_TEMP               (-40)
  #define LOW_RECOVER_TEMP               10
#else
  #define HIGH_BLOCK_TEMP		500
  #define HIGH_RECOVER_TEMP		420
  #define LOW_BLOCK_TEMP		  0
  #define LOW_RECOVER_TEMP		 20
#endif

struct battery_info {
	u32 batt_temp;		/* Battery Temperature (C) from ADC */
	u32 batt_health;	/* Battery Health (Authority) */
	u32 dis_reason;
	u32 batt_percentage;
	u32 charging_status;
	bool batt_is_full;      /* 0 : Not full 1: Full */
};


struct chg_data {
	struct device		*dev;
	struct max8998_dev	*iodev;
	struct work_struct	bat_work;
	struct max8998_charger_data *pdata;

	struct power_supply	psy_bat;
	struct power_supply	psy_usb;
	struct power_supply	psy_ac;
	struct alarm		alarm;
	struct workqueue_struct *monitor_wqueue;
	struct wake_lock	vbus_wake_lock;
	struct wake_lock	work_wake_lock;
	struct battery_info	bat_info;
	struct mutex		mutex;

	enum cable_type_t	cable_status;
	bool			charging;
	bool			set_charge_timeout;
	int			present;
	int			timestamp;
	int			set_batt_full;
	unsigned long		discharging_time;
	int                     slow_poll;
	ktime_t                 last_poll;
	struct max8998_charger_callbacks callbacks;
};

static bool lpm_charging_mode;

static char *supply_list[] = {
	"battery",
};

static enum power_supply_property max8998_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TECHNOLOGY,
};

static enum power_supply_property s3c_power_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static ssize_t s3c_bat_show_attrs(struct device *dev,
				  struct device_attribute *attr, char *buf);

static ssize_t s3c_bat_store_attrs(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t count);

#define SEC_BATTERY_ATTR(_name)						\
{									\
	.attr = {.name = #_name, .mode = 0664 },	\
	.show = s3c_bat_show_attrs,					\
	.store = s3c_bat_store_attrs,					\
}

static struct device_attribute s3c_battery_attrs[] = {
	SEC_BATTERY_ATTR(charging_mode_booting),
	SEC_BATTERY_ATTR(batt_full_check),
};

static int s3c_cable_status_update(struct chg_data *chg);

static bool max8998_check_vdcin(struct chg_data *chg)
{
	u8 data = 0;
	int ret;

	ret = max8998_read_reg(chg->iodev->i2c, MAX8998_REG_STATUS2, &data);

	if (ret < 0) {
		pr_err("max8998_read_reg error\n");
		return ret;
	}

	return data & MAX8998_MASK_VDCIN;
}

static void max8998_set_cable(struct max8998_charger_callbacks *ptr,
	enum cable_type_t status)
{
	struct chg_data *chg = container_of(ptr, struct chg_data, callbacks);	
	mutex_lock(&chg->mutex);
	chg->cable_status = status;

	if (lpm_charging_mode &&
	    (max8998_check_vdcin(chg) != 1) &&
	    pm_power_off)
		pm_power_off();

	pr_info("%s : status(%d)\n", __func__, status);
	s3c_cable_status_update(chg);
	power_supply_changed(&chg->psy_ac);
	power_supply_changed(&chg->psy_usb);
	wake_lock(&chg->work_wake_lock);
	queue_work(chg->monitor_wqueue, &chg->bat_work);
	mutex_unlock(&chg->mutex);
}

static void check_lpm_charging_mode(struct chg_data *chg)
{
	if (readl(S5P_INFORM5)) {
		lpm_charging_mode = 1;
		if (max8998_check_vdcin(chg) != 1)
			if (pm_power_off)
				pm_power_off();
	} else
		lpm_charging_mode = 0;

	pr_info("%s : lpm_charging_mode(%d)\n", __func__, lpm_charging_mode);
}

bool charging_mode_get(void)
{
	return lpm_charging_mode;
}
EXPORT_SYMBOL(charging_mode_get);

static int s3c_bat_get_property(struct power_supply *bat_ps,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct chg_data *chg = container_of(bat_ps,
				struct chg_data, psy_bat);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = chg->bat_info.charging_status;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = chg->bat_info.batt_health;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = chg->present;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = chg->bat_info.batt_temp;
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		/* battery is always online */
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		/*if (chg->pdata && chg->pdata->psy_fuelgauge &&
			 chg->pdata->psy_fuelgauge->get_property &&
			 chg->pdata->psy_fuelgauge->get_property(
				chg->pdata->psy_fuelgauge, psp, val) < 0)
			return -EINVAL;*/
		val->intval = chg->bat_info.batt_percentage;
		
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int s3c_bat_set_property(struct power_supply *bat_ps,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct chg_data *chg = container_of(bat_ps,
				struct chg_data, psy_bat);

	mutex_lock(&chg->mutex);
	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		chg->bat_info.charging_status = val->intval;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		chg->bat_info.batt_health = val->intval;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		chg->present = val->intval;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		chg->bat_info.batt_temp = val->intval;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		chg->bat_info.batt_percentage = val->intval;		
		break;
	default:	
		mutex_unlock(&chg->mutex);
		return -EINVAL;
	}
	wake_lock(&chg->work_wake_lock);
	queue_work(chg->monitor_wqueue, &chg->bat_work);
	mutex_unlock(&chg->mutex);
	return 0;
}

static int s3c_bat_property_is_writeable(struct power_supply *bat_ps,
				enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
	case POWER_SUPPLY_PROP_HEALTH:
	case POWER_SUPPLY_PROP_PRESENT:
	case POWER_SUPPLY_PROP_TEMP:
	case POWER_SUPPLY_PROP_CAPACITY:
		return true;	
		break;
	default:	
		return false;
	}
}

static int s3c_usb_get_property(struct power_supply *ps,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct chg_data *chg = container_of(ps, struct chg_data, psy_usb);

	if (psp != POWER_SUPPLY_PROP_ONLINE)
		return -EINVAL;

	/* Set enable=1 only if the USB charger is connected */
	val->intval = ((chg->cable_status == CABLE_TYPE_USB) &&
			max8998_check_vdcin(chg));

	return 0;
}

static int s3c_ac_get_property(struct power_supply *ps,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct chg_data *chg = container_of(ps, struct chg_data, psy_ac);

	if (psp != POWER_SUPPLY_PROP_ONLINE)
		return -EINVAL;

	/* Set enable=1 only if the AC charger is connected */
	val->intval = ((chg->cable_status == CABLE_TYPE_AC) &&
			max8998_check_vdcin(chg));

	return 0;
}

static void s3c_bat_discharge_reason(struct chg_data *chg)
{
	int discharge_reason;
	ktime_t ktime;
	struct timespec cur_time;

	discharge_reason = chg->bat_info.dis_reason & 0xf;
	if(chg->bat_info.batt_percentage >= 100)
	{
		chg->set_batt_full = 1;
		chg->bat_info.batt_is_full = true;
	}
	if (discharge_reason & DISCONNECT_BAT_FULL &&
			/*chg->bat_info.batt_vcell < RECHARGE_COND_VOLTAGE*/			
			chg->bat_info.batt_percentage < 100)
		chg->bat_info.dis_reason &= ~DISCONNECT_BAT_FULL;

	if (discharge_reason & DISCONNECT_TEMP_OVERHEAT &&
			chg->bat_info.batt_temp <=
			HIGH_RECOVER_TEMP)
		chg->bat_info.dis_reason &= ~DISCONNECT_TEMP_OVERHEAT;

	if (discharge_reason & DISCONNECT_TEMP_FREEZE &&
			chg->bat_info.batt_temp >=
			LOW_RECOVER_TEMP)
		chg->bat_info.dis_reason &= ~DISCONNECT_TEMP_FREEZE;

	if (discharge_reason & DISCONNECT_OVER_TIME &&
			/*chg->bat_info.batt_vcell < RECHARGE_COND_VOLTAGE*/
			chg->bat_info.batt_percentage < 100)
		chg->bat_info.dis_reason &= ~DISCONNECT_OVER_TIME;

	if (chg->set_batt_full)
		chg->bat_info.dis_reason |= DISCONNECT_BAT_FULL;

	if (chg->bat_info.batt_health != POWER_SUPPLY_HEALTH_GOOD)
		chg->bat_info.dis_reason |= chg->bat_info.batt_health ==
			POWER_SUPPLY_HEALTH_OVERHEAT ?
			DISCONNECT_TEMP_OVERHEAT : DISCONNECT_TEMP_FREEZE;

	ktime = alarm_get_elapsed_realtime();
	cur_time = ktime_to_timespec(ktime);

	if (chg->discharging_time &&
			cur_time.tv_sec > chg->discharging_time) {
		chg->set_charge_timeout = true;
		chg->bat_info.dis_reason |= DISCONNECT_OVER_TIME;
	}

	pr_debug("%s : Current charge level : %d%%\n\
Current time : %ld  discharging_time : %ld\n\
discharging reason : %d\n",\
		__func__, chg->bat_info.batt_percentage, cur_time.tv_sec,
		chg->discharging_time, chg->bat_info.dis_reason);
}

static int max8998_charging_control(struct chg_data *chg)
{
	int ret;
	struct i2c_client *i2c = chg->iodev->i2c;

	if (!chg->charging) {
		/* disable charging */
		ret = max8998_update_reg(i2c, MAX8998_REG_CHGR2,
			(1 << MAX8998_SHIFT_CHGEN), MAX8998_MASK_CHGEN);
		if (ret < 0)
			goto err;

		pr_debug("%s : charging disabled\n", __func__);
	} else {
		/* enable charging */
		if (chg->cable_status == CABLE_TYPE_AC) {
			/* ac */
			ret = max8998_write_reg(i2c, MAX8998_REG_CHGR1,
						(2 << MAX8998_SHIFT_TOPOFF) |
						(3 << MAX8998_SHIFT_RSTR) |
						(5 << MAX8998_SHIFT_ICHG));
			if (ret < 0)
				goto err;

			pr_debug("%s : TA charging enabled\n", __func__);
		} else {
			/* usb */
			ret = max8998_write_reg(i2c, MAX8998_REG_CHGR1,
						(6 << MAX8998_SHIFT_TOPOFF) |
						(3 << MAX8998_SHIFT_RSTR) |
						(2 << MAX8998_SHIFT_ICHG));
			if (ret < 0)
				goto err;

			pr_debug("%s : USB charging enabled\n", __func__);
		}

		ret = max8998_write_reg(i2c, MAX8998_REG_CHGR2,
					(2 << MAX8998_SHIFT_ESAFEOUT) |
					(2 << MAX8998_SHIFT_FT) |
					(0 << MAX8998_SHIFT_CHGEN));
		if (ret < 0)
			goto err;
	}

	return 0;
err:
	pr_err("max8998_read_reg error\n");
	return ret;
}

static int s3c_cable_status_update(struct chg_data *chg)
{
	int ret;
	bool vdc_status;
	ktime_t ktime;
	struct timespec cur_time;

	/* if max8998 has detected vdcin */
	if (max8998_check_vdcin(chg)) {
		vdc_status = 1;
		if (chg->bat_info.dis_reason) {
			pr_info("%s : battery status discharging : %d\n",
				__func__, chg->bat_info.dis_reason);
			/* have vdcin, but cannot charge */
			chg->charging = false;
			ret = max8998_charging_control(chg);
			if (ret < 0)
				goto err;
			chg->bat_info.charging_status =
				chg->bat_info.batt_is_full ?
				POWER_SUPPLY_STATUS_FULL :
				POWER_SUPPLY_STATUS_NOT_CHARGING;
			chg->discharging_time = 0;
			chg->set_batt_full = 0;
			goto update;
		} else if (chg->discharging_time == 0) {
			ktime = alarm_get_elapsed_realtime();
			cur_time = ktime_to_timespec(ktime);
			chg->discharging_time =
				chg->bat_info.batt_is_full ||
				chg->set_charge_timeout ?
				cur_time.tv_sec + TOTAL_RECHARGING_TIME :
				cur_time.tv_sec + TOTAL_CHARGING_TIME;
		}

		/* able to charge */
		chg->charging = true;
		ret = max8998_charging_control(chg);
		if (ret < 0)
			goto err;

		chg->bat_info.charging_status = chg->bat_info.batt_is_full ?
			POWER_SUPPLY_STATUS_FULL : POWER_SUPPLY_STATUS_CHARGING;

	} else {
		/* no vdc in, not able to charge */
		vdc_status = 0;
		chg->charging = false;
		ret = max8998_charging_control(chg);
		if (ret < 0)
			goto err;

		chg->bat_info.charging_status = POWER_SUPPLY_STATUS_DISCHARGING;

		chg->bat_info.batt_is_full = false;
		chg->set_charge_timeout = false;
		chg->set_batt_full = 0;
		chg->bat_info.dis_reason = 0;
		chg->discharging_time = 0;

		if (lpm_charging_mode && pm_power_off)
			pm_power_off();
	}

update:
	if ((chg->cable_status == CABLE_TYPE_USB) && vdc_status)
		wake_lock(&chg->vbus_wake_lock);
	else
		wake_lock_timeout(&chg->vbus_wake_lock, HZ / 2);

	return 0;
err:
	return ret;
}

static void s3c_program_alarm(struct chg_data *chg, int seconds)
{
	ktime_t low_interval = ktime_set(seconds - 10, 0);
	ktime_t slack = ktime_set(20, 0);
	ktime_t next;

	next = ktime_add(chg->last_poll, low_interval);
	alarm_start_range(&chg->alarm, next, ktime_add(next, slack));
}

static void s3c_bat_work(struct work_struct *work)
{
	struct chg_data *chg =
		container_of(work, struct chg_data, bat_work);
	int ret;
	struct timespec ts;
	unsigned long flags;
	mutex_lock(&chg->mutex);

	s3c_bat_discharge_reason(chg);

	ret = s3c_cable_status_update(chg);
	if (ret < 0)
		goto err;

	mutex_unlock(&chg->mutex);

	power_supply_changed(&chg->psy_bat);

	chg->last_poll = alarm_get_elapsed_realtime();
	ts = ktime_to_timespec(chg->last_poll);
	chg->timestamp = ts.tv_sec;

	/* prevent suspend before starting the alarm */
	local_irq_save(flags);
	wake_unlock(&chg->work_wake_lock);
	s3c_program_alarm(chg, FAST_POLL);
	local_irq_restore(flags);
	return;
err:
	mutex_unlock(&chg->mutex);
	wake_unlock(&chg->work_wake_lock);
	pr_err("battery workqueue fail\n");
}

static void s3c_battery_alarm(struct alarm *alarm)
{
	struct chg_data *chg =
			container_of(alarm, struct chg_data, alarm);

	wake_lock(&chg->work_wake_lock);
	queue_work(chg->monitor_wqueue, &chg->bat_work);
}

static ssize_t s3c_bat_show_attrs(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct chg_data *chg = container_of(psy, struct chg_data, psy_bat);
	int i = 0;
	const ptrdiff_t off = attr - s3c_battery_attrs;

	switch (off) {
	case CHARGING_MODE_BOOTING:
		i += scnprintf(buf + i, PAGE_SIZE - i, "%d\n", lpm_charging_mode);
		break;
	case BATT_FULL_CHECK:
		i += scnprintf(buf + i, PAGE_SIZE - i, "%d\n", chg->bat_info.batt_is_full);
		break;
	default:
		i = -EINVAL;
	}

	return i;
}

static ssize_t s3c_bat_store_attrs(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct chg_data *chg = container_of(psy, struct chg_data, psy_bat);
	int x = 0;
	int ret = 0;
	const ptrdiff_t off = attr - s3c_battery_attrs;
	mutex_lock(&chg->mutex);

	switch (off) {
	case CHARGING_MODE_BOOTING:
		if (sscanf(buf, "%d\n", &x) == 1) {
			lpm_charging_mode = x;
			ret = count;
		}
		break;
	case BATT_FULL_CHECK:
		if (sscanf(buf, "%d\n", &x) == 1) {
			chg->bat_info.batt_is_full = x;
			ret = count;
		}
		break;
	default:
		ret = -EINVAL;
	}
	wake_lock(&chg->work_wake_lock);
	queue_work(chg->monitor_wqueue, &chg->bat_work);

	mutex_unlock(&chg->mutex);

	return ret;
}

static int s3c_bat_create_attrs(struct device *dev)
{
	int i, rc;

	for (i = 0; i < ARRAY_SIZE(s3c_battery_attrs); i++) {
		rc = device_create_file(dev, &s3c_battery_attrs[i]);
		if (rc)
			goto s3c_attrs_failed;
	}
	goto succeed;

s3c_attrs_failed:
	while (i--)
		device_remove_file(dev, &s3c_battery_attrs[i]);
succeed:
	return rc;
}

static irqreturn_t max8998_int_work_func(int irq, void *max8998_chg)
{
	int ret;
	u8 data = 0;
	struct chg_data *chg;
	struct i2c_client *i2c;

	chg = max8998_chg;
	i2c = chg->iodev->i2c;

	ret = max8998_read_reg(i2c, MAX8998_REG_IRQ1, &data);
	if (ret < 0)
		goto err;

	ret = max8998_read_reg(i2c, MAX8998_REG_IRQ3, &data);
	if (ret < 0)
		goto err;

	if ((data & 0x4) || (ret != 0)) {
		pr_info("%s : pmic battery full interrupt\n", __func__);
		chg->set_batt_full = 1;
		chg->bat_info.batt_is_full = true;
	}

	wake_lock(&chg->work_wake_lock);
	queue_work(chg->monitor_wqueue, &chg->bat_work);

	return IRQ_HANDLED;
err:
	pr_err("%s : pmic read error\n", __func__);
	return IRQ_HANDLED;
}

static __devinit int max8998_charger_probe(struct platform_device *pdev)
{
	struct max8998_dev *iodev = dev_get_drvdata(pdev->dev.parent);
	struct max8998_platform_data *pdata = dev_get_platdata(iodev->dev);
	struct chg_data *chg;
	struct i2c_client *i2c = iodev->i2c;
	int ret = 0;

	pr_info("%s : MAX8998 Charger Driver Loading\n", __func__);

	chg = kzalloc(sizeof(*chg), GFP_KERNEL);
	if (!chg)
		return -ENOMEM;

	chg->iodev = iodev;
	chg->pdata = pdata->charger;

	if (!chg->pdata || !chg->pdata->adc_table) {
		pr_err("%s : No platform data & adc_table supplied\n", __func__);
		ret = -EINVAL;
		goto err_bat_table;
	}

	chg->psy_bat.name = "battery",
	chg->psy_bat.type = POWER_SUPPLY_TYPE_BATTERY,
	chg->psy_bat.properties = max8998_battery_props,
	chg->psy_bat.num_properties = ARRAY_SIZE(max8998_battery_props),
	chg->psy_bat.get_property = s3c_bat_get_property,
	chg->psy_bat.property_is_writeable = s3c_bat_property_is_writeable,
	chg->psy_bat.set_property = s3c_bat_set_property,

	chg->psy_usb.name = "usb",
	chg->psy_usb.type = POWER_SUPPLY_TYPE_USB,
	chg->psy_usb.supplied_to = supply_list,
	chg->psy_usb.num_supplicants = ARRAY_SIZE(supply_list),
	chg->psy_usb.properties = s3c_power_properties,
	chg->psy_usb.num_properties = ARRAY_SIZE(s3c_power_properties),
	chg->psy_usb.get_property = s3c_usb_get_property,

	chg->psy_ac.name = "ac",
	chg->psy_ac.type = POWER_SUPPLY_TYPE_MAINS,
	chg->psy_ac.supplied_to = supply_list,
	chg->psy_ac.num_supplicants = ARRAY_SIZE(supply_list),
	chg->psy_ac.properties = s3c_power_properties,
	chg->psy_ac.num_properties = ARRAY_SIZE(s3c_power_properties),
	chg->psy_ac.get_property = s3c_ac_get_property,

	chg->present = 1;
	chg->bat_info.batt_health = POWER_SUPPLY_HEALTH_GOOD;
	chg->bat_info.batt_is_full = false;
	chg->bat_info.batt_temp = 100; //fake
	chg->bat_info.batt_percentage = 50; //fake, modem will bootup soon and update it... hopefully
	chg->set_charge_timeout = false;

	chg->cable_status = CABLE_TYPE_NONE;

	mutex_init(&chg->mutex);

	platform_set_drvdata(pdev, chg);

	ret = max8998_update_reg(i2c, MAX8998_REG_CHGR1, /* disable */
		(0x3 << MAX8998_SHIFT_RSTR), MAX8998_MASK_RSTR);
	if (ret < 0)
		goto err_kfree;

	ret = max8998_update_reg(i2c, MAX8998_REG_CHGR2, /* 6 Hr */
		(0x2 << MAX8998_SHIFT_FT), MAX8998_MASK_FT);
	if (ret < 0)
		goto err_kfree;

	ret = max8998_update_reg(i2c, MAX8998_REG_CHGR2, /* 4.2V */
		(0x0 << MAX8998_SHIFT_BATTSL), MAX8998_MASK_BATTSL);
	if (ret < 0)
		goto err_kfree;

	ret = max8998_update_reg(i2c, MAX8998_REG_CHGR2, /* 105c */
		(0x0 << MAX8998_SHIFT_TMP), MAX8998_MASK_TMP);
	if (ret < 0)
		goto err_kfree;

	pr_info("%s : pmic interrupt registered\n", __func__);
	ret = max8998_write_reg(i2c, MAX8998_REG_IRQM1,
		~(MAX8998_MASK_DCINR | MAX8998_MASK_DCINF));
	if (ret < 0)
		goto err_kfree;

	ret = max8998_write_reg(i2c, MAX8998_REG_IRQM2, 0xFF);
	if (ret < 0)
		goto err_kfree;

	ret = max8998_write_reg(i2c, MAX8998_REG_IRQM3, ~MAX8998_IRQ_CHGRSTF_MASK);
	if (ret < 0)
		goto err_kfree;

	ret = max8998_write_reg(i2c, MAX8998_REG_IRQM4, 0xFF);
	if (ret < 0)
		goto err_kfree;

	wake_lock_init(&chg->vbus_wake_lock, WAKE_LOCK_SUSPEND,
		"vbus_present");
	wake_lock_init(&chg->work_wake_lock, WAKE_LOCK_SUSPEND,
		"max8998-charger");

	INIT_WORK(&chg->bat_work, s3c_bat_work);

	chg->monitor_wqueue =
		create_freezable_workqueue(dev_name(&pdev->dev));
	if (!chg->monitor_wqueue) {
		pr_err("Failed to create freezeable workqueue\n");
		ret = -ENOMEM;
		goto err_wake_lock;
	}

	chg->last_poll = alarm_get_elapsed_realtime();
	alarm_init(&chg->alarm, ANDROID_ALARM_ELAPSED_REALTIME_WAKEUP,
		s3c_battery_alarm);

	check_lpm_charging_mode(chg);

	/* init power supplier framework */
	ret = power_supply_register(&pdev->dev, &chg->psy_bat);
	if (ret) {
		pr_err("Failed to register power supply psy_bat\n");
		goto err_wqueue;
	}

	ret = power_supply_register(&pdev->dev, &chg->psy_usb);
	if (ret) {
		pr_err("Failed to register power supply psy_usb\n");
		goto err_supply_unreg_bat;
	}

	ret = power_supply_register(&pdev->dev, &chg->psy_ac);
	if (ret) {
		pr_err("Failed to register power supply psy_ac\n");
		goto err_supply_unreg_usb;
	}

	ret = request_threaded_irq(iodev->i2c->irq, NULL,
			max8998_int_work_func,
			IRQF_TRIGGER_FALLING, "max8998-charger", chg);
	if (ret) {
		pr_err("%s : Failed to request pmic irq\n", __func__);
		goto err_supply_unreg_ac;
	}

	ret = enable_irq_wake(iodev->i2c->irq);
	if (ret) {
		pr_err("Failed to enable pmic irq wake\n");
		goto err_irq;
	}

	ret = s3c_bat_create_attrs(chg->psy_bat.dev);
	if (ret) {
		pr_err("%s : Failed to create_attrs\n", __func__);
		goto err_irq;
	}

	chg->callbacks.set_cable = max8998_set_cable;
	if (chg->pdata->register_callbacks)
		chg->pdata->register_callbacks(&chg->callbacks);

	wake_lock(&chg->work_wake_lock);
	queue_work(chg->monitor_wqueue, &chg->bat_work);

	return 0;

err_irq:
	free_irq(iodev->i2c->irq, NULL);
err_supply_unreg_ac:
	power_supply_unregister(&chg->psy_ac);
err_supply_unreg_usb:
	power_supply_unregister(&chg->psy_usb);
err_supply_unreg_bat:
	power_supply_unregister(&chg->psy_bat);
err_wqueue:
	destroy_workqueue(chg->monitor_wqueue);
	cancel_work_sync(&chg->bat_work);
	alarm_cancel(&chg->alarm);
err_wake_lock:
	wake_lock_destroy(&chg->work_wake_lock);
	wake_lock_destroy(&chg->vbus_wake_lock);
err_kfree:
	mutex_destroy(&chg->mutex);
err_bat_table:
	kfree(chg);
	return ret;
}

static int __devexit max8998_charger_remove(struct platform_device *pdev)
{
	struct chg_data *chg = platform_get_drvdata(pdev);

	alarm_cancel(&chg->alarm);
	free_irq(chg->iodev->i2c->irq, NULL);
	flush_workqueue(chg->monitor_wqueue);
	destroy_workqueue(chg->monitor_wqueue);
	power_supply_unregister(&chg->psy_bat);
	power_supply_unregister(&chg->psy_usb);
	power_supply_unregister(&chg->psy_ac);

	wake_lock_destroy(&chg->vbus_wake_lock);
	mutex_destroy(&chg->mutex);
	kfree(chg);

	return 0;
}

static int max8998_charger_suspend(struct device *dev)
{

	struct chg_data *chg = dev_get_drvdata(dev);
	if (!chg->charging) {
		s3c_program_alarm(chg, SLOW_POLL);
		chg->slow_poll = 1;
	}

	return 0;
}

static void max8998_charger_resume(struct device *dev)
{

	struct chg_data *chg = dev_get_drvdata(dev);
	/* We might be on a slow sample cycle.  If we're
	 * resuming we should resample the battery state
	 * if it's been over a minute since we last did
	 * so, and move back to sampling every minute until
	 * we suspend again.
	 */
	if (chg->slow_poll) {
		s3c_program_alarm(chg, FAST_POLL);
		chg->slow_poll = 0;
	}
}

static const struct dev_pm_ops max8998_charger_pm_ops = {
	.prepare        = max8998_charger_suspend,
	.complete       = max8998_charger_resume,
};

static struct platform_driver max8998_charger_driver = {
	.driver = {
		.name = "max8998-charger",
		.owner = THIS_MODULE,
		.pm = &max8998_charger_pm_ops,
	},
	.probe = max8998_charger_probe,
	.remove = __devexit_p(max8998_charger_remove),
};

static int __init max8998_charger_init(void)
{
	return platform_driver_register(&max8998_charger_driver);
}

static void __exit max8998_charger_exit(void)
{
	platform_driver_register(&max8998_charger_driver);
}

late_initcall(max8998_charger_init);
module_exit(max8998_charger_exit);

MODULE_AUTHOR("Minsung Kim <ms925.kim@samsung.com>");
MODULE_DESCRIPTION("Wave MAX8998 charger driver");
MODULE_LICENSE("GPL");
