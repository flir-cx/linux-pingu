/*
 * Copyright (c) 2014, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/pm.h>
#include <linux/jiffies.h>
#include <linux/interrupt.h>
#include <linux/unistd.h>

#define LC709203F_THERMISTOR_B		0x06
#define LC709203F_INITIAL_RSOC		0x07
#define LC709203F_TEMPERATURE		0x08
#define LC709203F_VOLTAGE		0x09

#define LC709203F_ADJUSTMENT_PACK_APPLI	0x0B
#define LC709203F_ADJUSTMENT_PACK_THERM	0x0C
#define LC709203F_RSOC			0x0D
#define LC709203F_INDICATOR_TO_EMPTY	0x0F

#define LC709203F_IC_VERSION		0x11
#define LC709203F_CHANGE_OF_THE_PARAM	0x12
#define LC709203F_ALARM_LOW_CELL_RSOC	0x13
#define LC709203F_ALARM_LOW_CELL_VOLT	0x14
#define LC709203F_IC_POWER_MODE		0x15
#define LC709203F_STATUS_BIT		0x16
#define LC709203F_NUM_OF_THE_PARAM	0x1A

#define LC709203F_DELAY			(30*HZ)
#define LC709203F_MAX_REGS		0x1A

#define LC709203F_BATTERY_LOW		15
#define LC709203F_BATTERY_FULL		100

#define LC709203F 0x1
#define LC709204F 0x2


enum battery_charger_status {
	BATTERY_DISCHARGING,
	BATTERY_CHARGING,
	BATTERY_CHARGING_DONE,
};

struct lc709203f_platform_data {
	const char *tz_name;
	u32 initial_rsoc;
	u32 appli_adjustment;
	u32 thermistor_beta;
	u32 therm_adjustment;
	u32 threshold_soc;
	u32 maximum_soc;
	u32 alert_low_rsoc;
	u32 alert_low_voltage;
    u32 battery_param;
	bool support_battery_current;
};

struct lc709203f_chip {
	struct i2c_client		*client;
	struct delayed_work		work;
	struct power_supply		*battery;
    struct power_supply_desc battery_desc;
	struct lc709203f_platform_data	*pdata;

	/* battery voltage */
	int vcell;
	/* battery capacity */
	int soc;
	/* State Of Charge */
	int status;
	/* battery health */
	int health;
	/* battery capacity */
	int capacity_level;

	int temperature;

	int lasttime_soc;
	int lasttime_status;
	int shutdown_complete;
	int charge_complete;
	struct mutex mutex;
	int read_failed;
};

int lc709203f_get_adjusted_soc(int min_soc, int max_soc, int actual_soc_semi)
{
	int min_soc_semi = min_soc * 100;
	int max_soc_semi = max_soc * 100;

	if (actual_soc_semi >= max_soc_semi)
		return 100;

	if (actual_soc_semi <= min_soc_semi)
		return 0;

	return (actual_soc_semi - min_soc_semi + 50) / (max_soc - min_soc);
}

static int lc709203f_read_word(struct i2c_client *client, u8 reg)
{
	int ret;

	ret = i2c_smbus_read_word_data(client, reg);
	if (ret < 0)
		dev_err(&client->dev, "err reading reg: 0x%x, %d\n", reg, ret);
	return ret;
}

static int lc709203f_write_word(struct i2c_client *client, u8 reg, u16 value)
{
	int ret;

	ret = i2c_smbus_write_word_data(client, reg, value);
	if (ret < 0)
		dev_err(&client->dev, "err writing 0x%0x, %d\n" , reg, ret);

	return ret;
}

static int lc709203f_get_battery_soc(struct lc709203f_chip *chip)
{
	int val;

	val = lc709203f_read_word(chip->client, LC709203F_RSOC);
	if (val < 0)
		dev_err(&chip->client->dev, "%s: err %d\n", __func__, val);
	else
		val =  lc709203f_get_adjusted_soc(
				chip->pdata->threshold_soc,
				chip->pdata->maximum_soc, val * 100);

	return val;
}

static int lc709203f_wakeup(struct i2c_client *client)
{
    int ret;

    u8 reg = LC709203F_IC_POWER_MODE;
    u16 value = 0x0001;

    ret = i2c_smbus_write_word_data(client, reg, value);

    msleep(1); // Wakeup retention time

    ret = i2c_smbus_write_word_data(client, reg, value);
    if (ret < 0)
		dev_err(&client->dev, "err writing 0x%0x, %d\n" , reg, ret);

    return ret;
}

static int lc709203f_update_soc_voltage(struct lc709203f_chip *chip)
{
	int val;

	val = lc709203f_read_word(chip->client, LC709203F_VOLTAGE);
	if (val < 0)
		dev_err(&chip->client->dev, "%s: err %d\n", __func__, val);
	else
		chip->vcell = val;

	val = lc709203f_read_word(chip->client, LC709203F_RSOC);
	if (val < 0)
		dev_err(&chip->client->dev, "%s: err %d\n", __func__, val);
	else
		chip->soc = lc709203f_get_adjusted_soc(
				chip->pdata->threshold_soc,
				chip->pdata->maximum_soc, val * 100);

	if (chip->soc == LC709203F_BATTERY_FULL && chip->charge_complete) {
		chip->status = POWER_SUPPLY_STATUS_FULL;
		chip->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
		chip->health = POWER_SUPPLY_HEALTH_GOOD;
	} else if (chip->soc < LC709203F_BATTERY_LOW) {
		chip->status = chip->lasttime_status;
		chip->health = POWER_SUPPLY_HEALTH_DEAD;
		chip->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
	} else {
		chip->charge_complete = 0;
		chip->status = chip->lasttime_status;
		chip->health = POWER_SUPPLY_HEALTH_GOOD;
		chip->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
	}

	return 0;
}

static void lc709203f_work(struct work_struct *work)
{
	struct lc709203f_chip *chip;
	
	chip = container_of(to_delayed_work(work), struct lc709203f_chip, work);

	mutex_lock(&chip->mutex);
	if (chip->shutdown_complete) {
		mutex_unlock(&chip->mutex);
		return;
	}

	lc709203f_update_soc_voltage(chip);

	if (chip->soc != chip->lasttime_soc ||
		chip->status != chip->lasttime_status) {
		chip->lasttime_soc = chip->soc;
		power_supply_changed(chip->battery);
	}

	mutex_unlock(&chip->mutex);
	schedule_delayed_work(&chip->work, LC709203F_DELAY);
}

static int lc709203f_get_temperature(struct lc709203f_chip *chip)
{
	int val;
	int retries = 2;
	int i;

	if (chip->shutdown_complete)
		return chip->temperature;

	for (i = 0; i < retries; i++) {
		val = lc709203f_read_word(chip->client, LC709203F_TEMPERATURE);
		if (val < 0)
			continue;
	}

	if (val < 0) {
		chip->read_failed++;
		dev_err(&chip->client->dev, "%s: err %d\n", __func__, val);
		if (chip->read_failed > 50)
			return val;
		return chip->temperature;
	}
	chip->read_failed = 0;;
	chip->temperature = val;
	return val;
}

static enum power_supply_property lc709203f_battery_props[] = {
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TECHNOLOGY,
//	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
};

static int lc709203f_get_property(struct power_supply *psy,
			    enum power_supply_property psp,
			    union power_supply_propval *val)
{
	struct lc709203f_chip *chip = power_supply_get_drvdata(psy);
	int temperature;
	int ret = 0;

	mutex_lock(&chip->mutex);

	switch (psp) {
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
        lc709203f_update_soc_voltage(chip);
		val->intval = 1000 * chip->vcell;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = lc709203f_get_battery_soc(chip);
		if (chip->soc == 15)
			dev_warn(&chip->client->dev,
			"System Running low on battery - 15 percent\n");
		if (chip->soc == 10)
			dev_warn(&chip->client->dev,
			"System Running low on battery - 10 percent\n");
		if (chip->soc == 5)
			dev_warn(&chip->client->dev,
			"System Running low on battery - 5 percent\n");
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = chip->health;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		val->intval = chip->capacity_level;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		temperature = lc709203f_get_temperature(chip);
		/*
		   Temp ready by device is deci-kelvin
		   C = K -273.2
		   Report temp in dec-celcius.
		*/
		val->intval = temperature - 2732;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	mutex_unlock(&chip->mutex);
	return ret;
}

static irqreturn_t lc709203f_irq(int id, void *dev)
{
	struct lc709203f_chip *chip = dev;
	struct i2c_client *client = chip->client;

	dev_info(&client->dev, "%s(): STATUS_VL\n", __func__);
	/* Forced set SOC 0 to power off */
	chip->soc = 0;
	chip->lasttime_soc = chip->soc;
	chip->status = chip->lasttime_status;
	chip->health = POWER_SUPPLY_HEALTH_DEAD;
	chip->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
	power_supply_changed(chip->battery);

	return IRQ_HANDLED;
}

static void of_lc709203f_parse_platform_data(struct i2c_client *client,
				struct lc709203f_platform_data *pdata)
{
	char const *pstr;
	struct device_node *np = client->dev.of_node;
	u32 pval;
	int ret;

	ret = of_property_read_u32(np, "initial-rsoc", &pval);
	if (!ret)
		pdata->initial_rsoc = pval;

	ret = of_property_read_u32(np, "appli-adjustment", &pval);
	if (!ret)
		pdata->appli_adjustment = pval;

	pdata->tz_name = NULL;
	ret = of_property_read_string(np, "tz-name", &pstr);
	if (!ret)
		pdata->tz_name = pstr;

	ret = of_property_read_u32(np, "thermistor-beta", &pval);
	if (!ret) {
		pdata->thermistor_beta = pval;
	} else {
		if (!pdata->tz_name)
			dev_warn(&client->dev,
				"Thermistor beta not provided\n");
	}

	ret = of_property_read_u32(np, "thermistor-adjustment", &pval);
	if (!ret)
		pdata->therm_adjustment = pval;

	ret = of_property_read_u32(np, "kernel-threshold-soc", &pval);
	if (!ret)
		pdata->threshold_soc = pval;

	ret = of_property_read_u32(np, "kernel-maximum-soc", &pval);
	if (!ret)
		pdata->maximum_soc = pval;
	else
		pdata->maximum_soc = 100;

	ret = of_property_read_u32(np, "alert-low-rsoc", &pval);
	if (!ret)
		pdata->alert_low_rsoc = pval;

	ret = of_property_read_u32(np, "alert-low-voltage", &pval);
	if (!ret)
		pdata->alert_low_voltage = pval;

    ret = of_property_read_u32(np, "battery-param", &pval);
	if (!ret)
		pdata->battery_param = pval;

	pdata->support_battery_current = of_property_read_bool(np,
						"io-channel-names");
}

#ifdef CONFIG_DEBUG_FS

#include <linux/debugfs.h>
#include <linux/seq_file.h>

static struct dentry *debugfs_root;
static u8 valid_command[] = {0x6, 0x7, 0x8, 0x9, 0xA, 0xB, 0xD, 0xF,
			0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x1A};
static int dbg_lc709203f_show(struct seq_file *s, void *data)
{
	struct i2c_client *client = s->private;
	int ret;
	int i;

	seq_puts(s, "Register-->Value(16bit)\n");
	for (i = 0; i < ARRAY_SIZE(valid_command); ++i) {
		ret = lc709203f_read_word(client, valid_command[i]);
		if (ret < 0)
			seq_printf(s, "0x%02x: ERROR\n", valid_command[i]);
		else
			seq_printf(s, "0x%02x: 0x%04x\n",
						valid_command[i], ret);
	}
	return 0;
}

static int dbg_lc709203f_open(struct inode *inode, struct file *file)
{
	return single_open(file, dbg_lc709203f_show, inode->i_private);
}

static const struct file_operations lc709203f_debug_fops = {
	.open		= dbg_lc709203f_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int lc709203f_debugfs_init(struct i2c_client *client)
{
	debugfs_root = debugfs_create_dir("lc709203f", NULL);
	if (!debugfs_root)
		pr_warn("lc709203f: Failed to create debugfs directory\n");

	(void) debugfs_create_file("registers", S_IRUGO,
			debugfs_root, (void *)client, &lc709203f_debug_fops);
	return 0;
}
#else
static int lc709203f_debugfs_init(struct i2c_client *client)
{
	return 0;
}
#endif

static int lc709203f_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
{
	struct lc709203f_chip *chip;
	int ret;
	u32 type;
	u32 param;
	u32 param_val;

	/* Required PEC functionality */
	client->flags = client->flags | I2C_CLIENT_PEC;    

    ret = lc709203f_wakeup(client);
	if (ret < 0) {
		dev_err(&client->dev, "device is not responding, %d\n", ret);
		return ret;
	}

	dev_info(&client->dev, "Device Params 0x%04x\n", ret);

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->client = client;
	if (client->dev.of_node) {
		chip->pdata = devm_kzalloc(&client->dev,
					sizeof(*chip->pdata), GFP_KERNEL);
		if (!chip->pdata)
			return -ENOMEM;
		of_lc709203f_parse_platform_data(client, chip->pdata);
	} else {
		chip->pdata = client->dev.platform_data;
	}

	if (!chip->pdata)
		return -ENODATA;

	mutex_init(&chip->mutex);
	chip->shutdown_complete = 0;
	i2c_set_clientdata(client, chip);

	if (chip->pdata->initial_rsoc) {
		ret = lc709203f_write_word(chip->client,
			LC709203F_INITIAL_RSOC, chip->pdata->initial_rsoc);
		if (ret < 0) {
			dev_err(&client->dev,
				"INITIAL_RSOC write failed: %d\n", ret);
			return ret;
		}
		dev_info(&client->dev, "initial-rsoc: 0x%04x\n",
			chip->pdata->initial_rsoc);
	}

	ret = lc709203f_write_word(chip->client,
		LC709203F_ALARM_LOW_CELL_RSOC, chip->pdata->alert_low_rsoc);
	if (ret < 0) {
		dev_err(&client->dev, "LOW_RSOC write failed: %d\n", ret);
		return ret;
	}

	ret = lc709203f_write_word(chip->client,
		LC709203F_ALARM_LOW_CELL_VOLT, chip->pdata->alert_low_voltage);
	if (ret < 0) {
		dev_err(&client->dev, "LOW_VOLT write failed: %d\n", ret);
		return ret;
	}

	if (chip->pdata->appli_adjustment) {
		ret = lc709203f_write_word(chip->client,
			LC709203F_ADJUSTMENT_PACK_APPLI,
			chip->pdata->appli_adjustment);
		if (ret < 0) {
			dev_err(&client->dev,
				"ADJUSTMENT_APPLI write failed: %d\n", ret);
			return ret;
		}
	}

	if (chip->pdata->tz_name || !chip->pdata->thermistor_beta)
		goto skip_thermistor_config;

	if (chip->pdata->therm_adjustment) {
		ret = lc709203f_write_word(chip->client,
			LC709203F_ADJUSTMENT_PACK_THERM,
			chip->pdata->therm_adjustment);
		if (ret < 0) {
			dev_err(&client->dev,
				"ADJUSTMENT_THERM write failed: %d\n", ret);
			return ret;
		}
	}

	ret = lc709203f_write_word(chip->client,
		LC709203F_THERMISTOR_B, chip->pdata->thermistor_beta);
	if (ret < 0) {
		dev_err(&client->dev, "THERMISTOR_B write failed: %d\n", ret);
		return ret;
	}

	ret = lc709203f_write_word(chip->client, LC709203F_STATUS_BIT, 0x1);
	if (ret < 0) {
		dev_err(&client->dev, "STATUS_BIT write failed: %d\n", ret);
		return ret;
	}

	type = lc709203f_read_word(chip->client, LC709203F_NUM_OF_THE_PARAM) == 0x0301? LC709203F:LC709204F;
	dev_err(&client->dev, "Fuelguage %s detected\n", type==LC709203F?"LC709203f":"LC709204f");

	param = lc709203f_read_word(chip->client, LC709203F_CHANGE_OF_THE_PARAM);

	// Battery param should be 1 for lc709203f and 0 for lc709204f (FLIR ec201 settings)
	param_val = type==LC709203F?1:0;

	if(param != param_val)
	{
		ret = lc709203f_write_word(chip->client, LC709203F_CHANGE_OF_THE_PARAM, param_val);
		if (ret < 0) {
			dev_err(&client->dev, "STATUS_BIT write failed: %d\n", ret);
			return ret;
		}
	}


skip_thermistor_config:
	lc709203f_update_soc_voltage(chip);

	chip->battery_desc.name		= "battery";
	chip->battery_desc.type		= POWER_SUPPLY_TYPE_BATTERY;
	chip->battery_desc.get_property	= lc709203f_get_property;
	chip->battery_desc.properties	= lc709203f_battery_props;
	chip->battery_desc.num_properties	= ARRAY_SIZE(lc709203f_battery_props);
	chip->status			= POWER_SUPPLY_STATUS_DISCHARGING;
	chip->lasttime_status		= POWER_SUPPLY_STATUS_DISCHARGING;
	chip->charge_complete		= 0;

	if (chip->pdata->tz_name)
		lc709203f_battery_props[0] = POWER_SUPPLY_PROP_TEMP;

	/* Remove current property if it is not supported */
	if (!chip->pdata->support_battery_current)
		chip->battery_desc.num_properties--;

	chip->battery = power_supply_register(&client->dev, &chip->battery_desc, NULL);
	
    if (!chip->battery) {
		dev_err(&client->dev, "failed: power supply register\n");
		goto error;
	}

    chip->battery->drv_data = chip;

	INIT_DELAYED_WORK(&chip->work, lc709203f_work);
	schedule_delayed_work(&chip->work, 0);

	lc709203f_debugfs_init(client);

	if (client->irq) {
		ret = devm_request_threaded_irq(&client->dev, client->irq,
			NULL, lc709203f_irq,
			IRQF_ONESHOT | IRQF_TRIGGER_FALLING,
			dev_name(&client->dev), chip);
		if (ret < 0) {
			dev_err(&client->dev,
				"%s: request IRQ %d fail, err = %d\n",
				__func__, client->irq, ret);
			client->irq = 0;
			goto irq_reg_error;
		}
	}
	device_set_wakeup_capable(&client->dev, 1);

	dev_info(&client->dev, "Battery Voltage %dmV and SoC %d%%\n",
			chip->vcell, chip->soc);

	return 0;
irq_reg_error:
	cancel_delayed_work_sync(&chip->work);
error:
	mutex_destroy(&chip->mutex);

	return ret;
}

static int lc709203f_remove(struct i2c_client *client)
{
	struct lc709203f_chip *chip = i2c_get_clientdata(client);

	power_supply_unregister(chip->battery);
	cancel_delayed_work_sync(&chip->work);
	mutex_destroy(&chip->mutex);

	return 0;
}

static void lc709203f_shutdown(struct i2c_client *client)
{
	struct lc709203f_chip *chip = i2c_get_clientdata(client);

	mutex_lock(&chip->mutex);
	chip->shutdown_complete = 1;
	mutex_unlock(&chip->mutex);

	cancel_delayed_work_sync(&chip->work);
	dev_info(&chip->client->dev, "At shutdown Voltage %dmV and SoC %d%%\n",
			chip->vcell, chip->soc);
}

#ifdef CONFIG_PM_SLEEP
static int lc709203f_suspend(struct device *dev)
{
	struct lc709203f_chip *chip = dev_get_drvdata(dev);
	cancel_delayed_work_sync(&chip->work);

	if (device_may_wakeup(&chip->client->dev))
		enable_irq_wake(chip->client->irq);

	dev_info(&chip->client->dev, "At suspend Voltage %dmV and SoC %d%%\n",
			chip->vcell, chip->soc);

	return 0;
}

static int lc709203f_resume(struct device *dev)
{
	struct lc709203f_chip *chip = dev_get_drvdata(dev);

	if (device_may_wakeup(&chip->client->dev))
		disable_irq_wake(chip->client->irq);

	mutex_lock(&chip->mutex);
	lc709203f_update_soc_voltage(chip);
	power_supply_changed(chip->battery);
	mutex_unlock(&chip->mutex);

	dev_info(&chip->client->dev, "At resume Voltage %dmV and SoC %d%%\n",
			chip->vcell, chip->soc);

	schedule_delayed_work(&chip->work, LC709203F_DELAY);
	return 0;
}
#endif /* CONFIG_PM_SLEEP */

static SIMPLE_DEV_PM_OPS(lc709203f_pm_ops, lc709203f_suspend, lc709203f_resume);

static const struct i2c_device_id lc709203f_id[] = {
	{ "lc709203f", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, lc709203f_id);

static const struct of_device_id lc709203f_of_match [] = {
    { .compatible = "onsemi,lc709203f" },
    { .compatible = "onsemi,lc709204f" },
    { }
};
MODULE_DEVICE_TABLE(of, lc709203f_of_match);

static struct i2c_driver lc709203f_i2c_driver = {
	.driver	= {
		.name	= "lc709203f",
		.pm = &lc709203f_pm_ops,
	},
	.probe		= lc709203f_probe,
	.remove		= lc709203f_remove,
	.id_table	= lc709203f_id,
	.shutdown	= lc709203f_shutdown,
};

static int __init lc709203f_init(void)
{
	return i2c_add_driver(&lc709203f_i2c_driver);
}
fs_initcall_sync(lc709203f_init);

static void __exit lc709203f_exit(void)
{
	i2c_del_driver(&lc709203f_i2c_driver);
}
module_exit(lc709203f_exit);

MODULE_AUTHOR("Chaitanya Bandi <bandik@nvidia.com>");
MODULE_DESCRIPTION("OnSemi LC709203F Fuel Gauge");
MODULE_LICENSE("GPL v2");