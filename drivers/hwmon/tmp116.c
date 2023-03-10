/* Texas Instruments TMP116 SMBus temperature sensor driver
 * based on the existing TMP102 driver by Steven King.
 *
 * Copyright (C) 2010 Steven King <sfking@fdwdc.com>
 * Copyright (c) 2018 Mats Karrman <mats.karrman@flir.se>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/thermal.h>
#include <linux/of.h>

#define	DRIVER_NAME "tmp116"

#define	TMP116_TEMP_REG			0x00
#define	TMP116_CONF_REG			0x01
#define		TMP116_CONF_DA_ALERT	0x0000
#define		TMP116_CONF_DA_DATARDY	0x0004
#define		TMP116_CONF_POL_ACTLOW	0x0000
#define		TMP116_CONF_POL_ACTHIGH	0x0008
#define		TMP116_CONF_TNA_ALERT	0x0000
#define		TMP116_CONF_TNA_THERM	0x0010
#define		TMP116_CONF_AVG_1	0x0000
#define		TMP116_CONF_AVG_8	0x0020
#define		TMP116_CONF_AVG_32	0x0040
#define		TMP116_CONF_AVG_64	0x0060
#define		TMP116_CONF_CONV(n)	(((n) & 0x7) << 7)
#define		TMP116_CONF_MOD_CC	0x0000
#define		TMP116_CONF_MOD_SD	0x0400
#define		TMP116_CONF_MOD_CCRB0	0x0800
#define		TMP116_CONF_MOD_OS	0x0c00
#define		TMP116_CONF_MOD_MASK	0x0c00
#define		TMP116_CONF_EEPROM_BSY	0x1000
#define		TMP116_CONF_DATARDY	0x2000
#define		TMP116_CONF_LOW_ALERT	0x4000
#define		TMP116_CONF_HIGH_ALERT	0x8000
#define		TMP116_CONF_RDWR_MASK	0x0ffc
#define	TMP116_TLOW_REG			0x02
#define	TMP116_THIGH_REG		0x03

/* Used configuration */
#define TMP116_CONFIG	(TMP116_CONF_DA_ALERT |   \
			 TMP116_CONF_POL_ACTLOW | \
			 TMP116_CONF_TNA_ALERT |  \
			 TMP116_CONF_AVG_8 |      \
			 TMP116_CONF_CONV(4) |    \
			 TMP116_CONF_MOD_CC)

struct tmp116 {
	struct device *hwmon_dev;
	struct thermal_zone_device *tz;
	struct mutex lock;  /* Serialize access to device registers */
	u16 config_orig;
	unsigned long last_update;
	int temp[3];
};

/* convert TMP116 register value to milliCelsius */
static inline int tmp116_reg_to_mC(s16 val)
{
	return ((val & ~0x01) * 1000) / 128;
}

/* convert milliCelsius to 16-bit TMP116 register value */
static inline u16 tmp116_mC_to_reg(int val)
{
	return (val * 128) / 1000;
}

static const u8 tmp116_reg[] = {
	TMP116_TEMP_REG,
	TMP116_TLOW_REG,
	TMP116_THIGH_REG,
};

static struct tmp116 *tmp116_update_device(struct i2c_client *client)
{
	struct tmp116 *tmp116 = i2c_get_clientdata(client);

	mutex_lock(&tmp116->lock);
	if (time_after(jiffies, tmp116->last_update + HZ / 3)) {
		int i;

		for (i = 0; i < ARRAY_SIZE(tmp116->temp); ++i) {
			int status = i2c_smbus_read_word_swapped(client,
								 tmp116_reg[i]);
			if (status > -1)
				tmp116->temp[i] = tmp116_reg_to_mC(status);
		}
		tmp116->last_update = jiffies;
	}
	mutex_unlock(&tmp116->lock);
	return tmp116;
}

static int tmp116_read_temp(void *dev, int *temp)
{
	struct tmp116 *tmp116 = tmp116_update_device(to_i2c_client(dev));

	*temp = tmp116->temp[0];

	return 0;
}

static ssize_t tmp116_show_temp(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct sensor_device_attribute *sda = to_sensor_dev_attr(attr);
	struct tmp116 *tmp116 = tmp116_update_device(to_i2c_client(dev));

	return sprintf(buf, "%d\n", tmp116->temp[sda->index]);
}

static ssize_t tmp116_set_temp(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct sensor_device_attribute *sda = to_sensor_dev_attr(attr);
	struct i2c_client *client = to_i2c_client(dev);
	struct tmp116 *tmp116 = i2c_get_clientdata(client);
	long val;
	int status;

	if (kstrtol(buf, 10, &val) < 0)
		return -EINVAL;
	val = clamp_val(val, -256000, 255000);

	mutex_lock(&tmp116->lock);
	tmp116->temp[sda->index] = val;
	status = i2c_smbus_write_word_swapped(client, tmp116_reg[sda->index],
					      tmp116_mC_to_reg(val));
	mutex_unlock(&tmp116->lock);
	return status ? : count;
}

static ssize_t tmp116_show_update_int(struct device *dev,
				      struct device_attribute *attr,
				      char *buf)
{
	(void)attr;
	(void)dev;
	/* Update interval is hard set by config, write not implemented */
	return sprintf(buf, "1000\n");
}

static SENSOR_DEVICE_ATTR(temp1_input, 0444, tmp116_show_temp, NULL, 0);

static SENSOR_DEVICE_ATTR(temp1_max_hyst, 0644, tmp116_show_temp,
			  tmp116_set_temp, 1);

static SENSOR_DEVICE_ATTR(temp1_max, 0644, tmp116_show_temp,
			  tmp116_set_temp, 2);

static SENSOR_DEVICE_ATTR(update_interval, 0444, tmp116_show_update_int,
			  NULL, 0);

static struct attribute *tmp116_attributes[] = {
	&sensor_dev_attr_temp1_input.dev_attr.attr,
	&sensor_dev_attr_temp1_max_hyst.dev_attr.attr,
	&sensor_dev_attr_temp1_max.dev_attr.attr,
	&sensor_dev_attr_update_interval.dev_attr.attr,
	NULL
};

static const struct attribute_group tmp116_attr_group = {
	.attrs = tmp116_attributes,
};

static int tmp116_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct tmp116 *tmp116;
	int status;
	struct thermal_zone_of_device_ops tzodo = { tmp116_read_temp, NULL, NULL, NULL, NULL };

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_WORD_DATA)) {
		dev_err(&client->dev,
			"adapter doesn't support SMBus word transactions\n");
		return -ENODEV;
	}

	tmp116 = devm_kzalloc(&client->dev, sizeof(*tmp116), GFP_KERNEL);
	if (!tmp116)
		return -ENOMEM;

	i2c_set_clientdata(client, tmp116);

	status = i2c_smbus_read_word_swapped(client, TMP116_CONF_REG);
	if (status < 0) {
		dev_err(&client->dev, "error reading config register\n");
		return status;
	}
	tmp116->config_orig = status;
	status = i2c_smbus_write_word_swapped(client, TMP116_CONF_REG,
					      TMP116_CONFIG);
	if (status < 0) {
		dev_err(&client->dev, "error writing config register\n");
		goto fail_restore_config;
	}
	status = i2c_smbus_read_word_swapped(client, TMP116_CONF_REG);
	if (status < 0) {
		dev_err(&client->dev, "error reading config register\n");
		goto fail_restore_config;
	}
	status &= TMP116_CONF_RDWR_MASK;
	if (status != TMP116_CONFIG) {
		dev_err(&client->dev, "config settings did not stick\n");
		status = -ENODEV;
		goto fail_restore_config;
	}
	tmp116->last_update = jiffies - HZ;
	mutex_init(&tmp116->lock);

	status = sysfs_create_group(&client->dev.kobj, &tmp116_attr_group);
	if (status) {
		dev_dbg(&client->dev, "could not create sysfs files\n");
		goto fail_restore_config;
	}
	tmp116->hwmon_dev = hwmon_device_register(&client->dev);
	if (IS_ERR(tmp116->hwmon_dev)) {
		dev_dbg(&client->dev, "unable to register hwmon device\n");
		status = PTR_ERR(tmp116->hwmon_dev);
		goto fail_remove_sysfs;
	}

	tmp116->tz = thermal_zone_of_sensor_register(&client->dev, 0,
						     &client->dev,
						     &tzodo);
	if (IS_ERR(tmp116->tz))
		tmp116->tz = NULL;

	dev_info(&client->dev, "initialized\n");

	return 0;

fail_remove_sysfs:
	sysfs_remove_group(&client->dev.kobj, &tmp116_attr_group);
fail_restore_config:
	i2c_smbus_write_word_swapped(client, TMP116_CONF_REG,
				     tmp116->config_orig);
	return status;
}

static int tmp116_remove(struct i2c_client *client)
{
	struct tmp116 *tmp116 = i2c_get_clientdata(client);

	thermal_zone_of_sensor_unregister(&client->dev, tmp116->tz);
	hwmon_device_unregister(tmp116->hwmon_dev);
	sysfs_remove_group(&client->dev.kobj, &tmp116_attr_group);

	/* Stop monitoring if device was stopped originally */
	if ((tmp116->config_orig & TMP116_CONF_MOD_MASK) ==
	    TMP116_CONF_MOD_SD) {
		int config;

		config = i2c_smbus_read_word_swapped(client, TMP116_CONF_REG);
		if (config >= 0) {
			config = (config & ~TMP116_CONF_MOD_MASK) |
				 TMP116_CONF_MOD_SD;
			i2c_smbus_write_word_swapped(client,
						     TMP116_CONF_REG, config);
		}
	}

	return 0;
}

#ifdef CONFIG_PM
static int tmp116_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	int config;

	config = i2c_smbus_read_word_swapped(client, TMP116_CONF_REG);
	if (config < 0)
		return config;

	config = (config & ~TMP116_CONF_MOD_MASK) | TMP116_CONF_MOD_SD;
	return i2c_smbus_write_word_swapped(client, TMP116_CONF_REG, config);
}

static int tmp116_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	int config;

	config = i2c_smbus_read_word_swapped(client, TMP116_CONF_REG);
	if (config < 0)
		return config;

	config = (config & ~TMP116_CONF_MOD_MASK) | TMP116_CONF_MOD_CC;
	return i2c_smbus_write_word_swapped(client, TMP116_CONF_REG, config);
}

static const struct dev_pm_ops tmp116_dev_pm_ops = {
	.suspend	= tmp116_suspend,
	.resume		= tmp116_resume,
};

#define TMP116_DEV_PM_OPS (&tmp116_dev_pm_ops)
#else
#define	TMP116_DEV_PM_OPS NULL
#endif /* CONFIG_PM */

static const struct i2c_device_id tmp116_id[] = {
	{ "tmp116", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tmp116_id);

#if defined(CONFIG_OF)
static const struct of_device_id tmp116_of_match[] = {
	{ .compatible = "ti,tmp116" },
	{},
};
MODULE_DEVICE_TABLE(of, tmp116_of_match);

#define TMP116_OF_MATCH_PTR of_match_ptr(tmp116_of_match)
#else
#define TMP116_OF_MATCH_PTR NULL
#endif /* CONFIG_OF */

static struct i2c_driver tmp116_driver = {
	.driver = {
		.name	= DRIVER_NAME,
		.of_match_table = TMP116_OF_MATCH_PTR,
		.pm	= TMP116_DEV_PM_OPS,
	},
	.probe		= tmp116_probe,
	.remove		= tmp116_remove,
	.id_table	= tmp116_id,
};

module_i2c_driver(tmp116_driver);

MODULE_AUTHOR("Mats Karrman <mats.karrman@flir.se>");
MODULE_DESCRIPTION("Texas Instruments TMP116 temperature sensor driver");
MODULE_LICENSE("GPL");
