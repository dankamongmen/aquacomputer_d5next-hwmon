// SPDX-License-Identifier: GPL-2.0+
/*
 * hwmon driver for Aquacomputer devices (D5 Next, Farbwerk, Farbwerk 360, Octo, Quadro)
 *
 * Aquacomputer devices send HID reports (with ID 0x01) every second to report
 * sensor values.
 *
 * Copyright 2021 Aleksa Savic <savicaleksa83@gmail.com>
 * Copyright 2022 Jack Doan <me@jackdoan.com>
 */

#include <linux/crc16.h>
#include <linux/debugfs.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <asm/unaligned.h>

#define USB_VENDOR_ID_AQUACOMPUTER	0x0c70
#define USB_PRODUCT_ID_FARBWERK 	0xf00a
#define USB_PRODUCT_ID_QUADRO 		0xf00d
#define USB_PRODUCT_ID_D5NEXT		0xf00e
#define USB_PRODUCT_ID_FARBWERK360	0xf010
#define USB_PRODUCT_ID_OCTO		0xf011

enum kinds { d5next, farbwerk, farbwerk360, octo, quadro };

static const char *const aqc_device_names[] = {
	[d5next] = "d5next",
	[farbwerk] = "farbwerk",
	[farbwerk360] = "farbwerk360",
	[octo] = "octo",
	[quadro] = "quadro"
};

#define DRIVER_NAME			"aquacomputer_d5next"

#define STATUS_REPORT_ID		0x01
#define STATUS_UPDATE_INTERVAL		(2 * HZ)	/* In seconds */
#define SERIAL_FIRST_PART		3
#define SERIAL_SECOND_PART		5
#define FIRMWARE_VERSION		13

#define CTRL_REPORT_ID			0x03

/* The HID report that the official software always sends
 * after writing values, currently same for all devices
 */
#define SECONDARY_CTRL_REPORT_ID	0x02
#define SECONDARY_CTRL_REPORT_SIZE	0x0B

static u8 secondary_ctrl_report[] = {
	0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x34, 0xC6
};

/* Register offsets for all Aquacomputer devices */
#define AQC_TEMP_SENSOR_SIZE		0x02
#define AQC_TEMP_SENSOR_DISCONNECTED	0x7FFF
#define AQC_FAN_PERCENT_OFFSET		0x00
#define AQC_FAN_VOLTAGE_OFFSET		0x02
#define AQC_FAN_CURRENT_OFFSET		0x04
#define AQC_FAN_POWER_OFFSET		0x06
#define AQC_FAN_SPEED_OFFSET		0x08

/* Register offsets for fan control*/
#define AQC_FAN_CTRL_PWM_OFFSET		0x01
#define AQC_FAN_CTRL_TEMP_SELECT_OFFSET	0x03

/* Register offsets for the D5 Next pump */
#define D5NEXT_POWER_CYCLES		0x18
#define D5NEXT_COOLANT_TEMP		0x57
#define D5NEXT_NUM_FANS			2
#define D5NEXT_NUM_SENSORS		1
#define D5NEXT_NUM_VIRTUAL_SENSORS	8
#define D5NEXT_VIRTUAL_SENSOR_START	0x3f
#define D5NEXT_PUMP_OFFSET		0x6c
#define D5NEXT_FAN_OFFSET		0x5f
#define D5NEXT_5V_VOLTAGE		0x39
#define D5NEXT_12V_VOLTAGE		0x37
#define D5NEXT_CTRL_REPORT_SIZE		0x329
#define D5NEXT_TEMP_CTRL_OFFSET		0x2D
static u8 d5next_sensor_fan_offsets[] = { D5NEXT_PUMP_OFFSET, D5NEXT_FAN_OFFSET };

/* Pump and fan speed registers in D5 Next control report (from 0-100%) */
static u16 d5next_ctrl_fan_offsets[] = { 0x96, 0x41 };

/* Register offsets for the Farbwerk RGB controller */
#define FARBWERK_NUM_SENSORS		4
#define FARBWERK_SENSOR_START		0x2f

/* Register offsets for the Farbwerk 360 RGB controller */
#define FARBWERK360_NUM_SENSORS			4
#define FARBWERK360_SENSOR_START		0x32
#define FARBWERK360_NUM_VIRTUAL_SENSORS		16
#define FARBWERK360_VIRTUAL_SENSORS_START	0x3a
#define FARBWERK360_CTRL_REPORT_SIZE	0x682
#define FARBWERK360_TEMP_CTRL_OFFSET	0x8

/* Register offsets for the Octo fan controller */
#define OCTO_POWER_CYCLES		0x18
#define OCTO_NUM_FANS			8
#define OCTO_NUM_SENSORS		4
#define OCTO_SENSOR_START		0x3D
#define OCTO_NUM_VIRTUAL_SENSORS	16
#define OCTO_VIRTUAL_SENSOR_START	0x45
#define OCTO_CTRL_REPORT_SIZE		0x65F
#define OCTO_TEMP_CTRL_OFFSET		0xa
static u8 octo_sensor_fan_offsets[] = { 0x7D, 0x8A, 0x97, 0xA4, 0xB1, 0xBE, 0xCB, 0xD8 };

/* Fan speed registers in Octo control report (from 0-100%) */
static u16 octo_ctrl_fan_offsets[] = { 0x5A, 0xAF, 0x104, 0x159, 0x1AE, 0x203, 0x258, 0x2AD };

/* Register offsets for the Quadro fan controller */
#define QUADRO_POWER_CYCLES		0x18
#define QUADRO_NUM_FANS			4
#define QUADRO_NUM_SENSORS		4
#define QUADRO_SENSOR_START		0x34
#define QUADRO_NUM_VIRTUAL_SENSORS	16
#define QUADRO_VIRTUAL_SENSORS_START	0x3c
#define QUADRO_CTRL_REPORT_SIZE		0x3c1
#define QUADRO_FLOW_SENSOR_OFFSET	0x6e
#define QUADRO_TEMP_CTRL_OFFSET		0xa
static u8 quadro_sensor_fan_offsets[] = { 0x70, 0x7D, 0x8A, 0x97 };

/* Fan speed registers in Quadro control report (from 0-100%) */
static u16 quadro_ctrl_fan_offsets[] = { 0x36, 0x8b, 0xe0, 0x135 };

/* Labels for D5 Next */
static const char *const label_d5next_temp[] = {
	"Coolant temp"
};

static const char *const label_d5next_speeds[] = {
	"Pump speed",
	"Fan speed"
};

static const char *const label_d5next_power[] = {
	"Pump power",
	"Fan power"
};

static const char *const label_d5next_voltages[] = {
	"Pump voltage",
	"Fan voltage",
	"+5V voltage",
	"+12V voltage"
};

static const char *const label_d5next_current[] = {
	"Pump current",
	"Fan current"
};

/* Labels for Farbwerk, Farbwerk 360, Octo and Quadro temperature sensors */
static const char *const label_temp_sensors[] = {
	"Sensor 1",
	"Sensor 2",
	"Sensor 3",
	"Sensor 4"
};

static const char *const label_virtual_temp_sensors[] = {
	"Virtual sensor 1",
	"Virtual sensor 2",
	"Virtual sensor 3",
	"Virtual sensor 4",
	"Virtual sensor 5",
	"Virtual sensor 6",
	"Virtual sensor 7",
	"Virtual sensor 8",
	"Virtual sensor 9",
	"Virtual sensor 10",
	"Virtual sensor 11",
	"Virtual sensor 12",
	"Virtual sensor 13",
	"Virtual sensor 14",
	"Virtual sensor 15",
	"Virtual sensor 16",
};

/* Labels for Octo and Quadro (except speed) */
static const char *const label_fan_speed[] = {
	"Fan 1 speed",
	"Fan 2 speed",
	"Fan 3 speed",
	"Fan 4 speed",
	"Fan 5 speed",
	"Fan 6 speed",
	"Fan 7 speed",
	"Fan 8 speed"
};

static const char *const label_fan_power[] = {
	"Fan 1 power",
	"Fan 2 power",
	"Fan 3 power",
	"Fan 4 power",
	"Fan 5 power",
	"Fan 6 power",
	"Fan 7 power",
	"Fan 8 power"
};

static const char *const label_fan_voltage[] = {
	"Fan 1 voltage",
	"Fan 2 voltage",
	"Fan 3 voltage",
	"Fan 4 voltage",
	"Fan 5 voltage",
	"Fan 6 voltage",
	"Fan 7 voltage",
	"Fan 8 voltage"
};

static const char *const label_fan_current[] = {
	"Fan 1 current",
	"Fan 2 current",
	"Fan 3 current",
	"Fan 4 current",
	"Fan 5 current",
	"Fan 6 current",
	"Fan 7 current",
	"Fan 8 current"
};

/* Labels for Quadro fan speeds */
static const char *const label_quadro_speeds[] = {
	"Fan 1 speed",
	"Fan 2 speed",
	"Fan 3 speed",
	"Fan 4 speed",
	"Flow speed [dL/h]"
};

struct aqc_data {
	struct hid_device *hdev;
	struct device *hwmon_dev;
	struct dentry *debugfs;
	struct mutex mutex;	/* Used for locking access when reading and writing PWM values */
	enum kinds kind;
	const char *name;

	int buffer_size;
	u8 *buffer;
	int checksum_start;
	int checksum_length;
	int checksum_offset;

	int num_fans;
	u8 *fan_sensor_offsets;
	u16 *fan_ctrl_offsets;
	int num_temp_sensors;
	int temp_sensor_start_offset;
	int num_virtual_temp_sensors;
	int virtual_temp_sensor_start_offset;
	u16 temp_ctrl_offset;
	u16 power_cycle_count_offset;
	u8 flow_sensor_offset;

	/* General info, same across all devices */
	u32 serial_number[2];
	u16 firmware_version;

	/* How many times the device was powered on */
	u32 power_cycles;

	/* Sensor values */
	s32 temp_input[20]; /* Max 4 normal and 16 virtual */
	u16 speed_input[8];
	u32 power_input[8];
	u16 voltage_input[8];
	u16 current_input[8];

	/* Label values */
	const char *const *temp_label;
	const char *const *virtual_temp_label;
	const char *const *speed_label;
	const char *const *power_label;
	const char *const *voltage_label;
	const char *const *current_label;

	unsigned long updated;
};

/* Converts from centi-percent */
static int aqc_percent_to_pwm(u16 val)
{
	return DIV_ROUND_CLOSEST(val * 255, 100 * 100);
}

/* Converts to centi-percent */
static int aqc_pwm_to_percent(long val)
{
	if (val < 0 || val > 255)
		return -EINVAL;

	return DIV_ROUND_CLOSEST(val * 100 * 100, 255);
}

/* Expects the mutex to be locked */
static int aqc_get_ctrl_data(struct aqc_data *priv)
{
	int ret;

	memset(priv->buffer, 0x00, priv->buffer_size);
	ret = hid_hw_raw_request(priv->hdev, CTRL_REPORT_ID, priv->buffer, priv->buffer_size,
				 HID_FEATURE_REPORT, HID_REQ_GET_REPORT);
	if (ret < 0)
		ret = -ENODATA;

	return ret;
}

/* Expects the mutex to be locked */
static int aqc_send_ctrl_data(struct aqc_data *priv)
{
	int ret;
	u16 checksum;

	/* Init and xorout value for CRC-16/USB is 0xffff */
	checksum = crc16(0xffff, priv->buffer + priv->checksum_start, priv->checksum_length);
	checksum ^= 0xffff;

	/* Place the new checksum at the end of the report */
	put_unaligned_be16(checksum, priv->buffer + priv->checksum_offset);

	/* Send the patched up report back to the device */
	ret = hid_hw_raw_request(priv->hdev, CTRL_REPORT_ID, priv->buffer, priv->buffer_size,
				 HID_FEATURE_REPORT, HID_REQ_SET_REPORT);
	if (ret < 0)
		return ret;

	/* The official software sends this report after every change, so do it here as well */
	ret = hid_hw_raw_request(priv->hdev, SECONDARY_CTRL_REPORT_ID, secondary_ctrl_report,
				 SECONDARY_CTRL_REPORT_SIZE, HID_FEATURE_REPORT,
				 HID_REQ_SET_REPORT);
	return ret;
}

/* Refreshes the control buffer and stores value at offset in val */
static int aqc_get_ctrl_val(struct aqc_data *priv, int offset, long *val, size_t size)
{
	int ret;

	mutex_lock(&priv->mutex);

	ret = aqc_get_ctrl_data(priv);
	if (ret < 0)
		goto unlock_and_return;

	switch (size) {
	case 16:
		*val = (s16) get_unaligned_be16(priv->buffer + offset);
		break;
	case 8:
		*val = priv->buffer[offset];
		break;
	default:
		ret = -EINVAL;
		break;
	}

unlock_and_return:
	mutex_unlock(&priv->mutex);
	return ret;
}

/* Refreshes the control buffer, updates value at offset and writes buffer to device */
static int aqc_set_ctrl_val(struct aqc_data *priv, int offset, long val, size_t size)
{
	int ret;

	mutex_lock(&priv->mutex);

	ret = aqc_get_ctrl_data(priv);
	if (ret < 0)
		goto unlock_and_return;

	switch (size) {
	case 16:
		put_unaligned_be16((u16) val, priv->buffer + offset);
		break;
	case 8:
		priv->buffer[offset] = (u8) val;
		break;
	default:
		ret = -EINVAL;
		goto unlock_and_return;
	}

	ret = aqc_send_ctrl_data(priv);

unlock_and_return:
	mutex_unlock(&priv->mutex);
	return ret;
}

static umode_t aqc_is_visible(const void *data, enum hwmon_sensor_types type, u32 attr, int channel)
{
	const struct aqc_data *priv = data;

	switch (type) {
	case hwmon_temp:
		if (channel < priv->num_temp_sensors) {
			switch (attr) {
			case hwmon_temp_label:
			case hwmon_temp_input:
				return 0444;
			case hwmon_temp_offset:
				if (priv->temp_ctrl_offset != 0)
					return 0644;
			default:
				break;
			}
		}

		if (channel < priv->num_temp_sensors + priv->num_virtual_temp_sensors)
			return 0444;
		break;
	case hwmon_pwm:
		if (priv->fan_ctrl_offsets && channel < priv->num_fans) {
			switch (attr) {
			case hwmon_pwm_enable:
				return 0644;
			case hwmon_pwm_input:
				return 0644;
			case hwmon_pwm_auto_channels_temp:
				return 0644;
			default:
				break;
			}
		}
		break;
	case hwmon_fan:
		switch (priv->kind) {
		case quadro:
			/* Special case to support flow sensor */
			if (channel < priv->num_fans + 1)
				return 0444;
			break;
		default:
			if (channel < priv->num_fans)
				return 0444;
			break;
		}
		break;
	case hwmon_power:
	case hwmon_curr:
		if (channel < priv->num_fans)
			return 0444;
		break;
	case hwmon_in:
		switch (priv->kind) {
		case d5next:
			/* Special case to support +5V and +12V voltage sensors */
			if (channel < priv->num_fans + 2)
				return 0444;
			break;
		default:
			if (channel < priv->num_fans)
				return 0444;
			break;
		}
		break;
	default:
		break;
	}

	return 0;
}

static int aqc_read(struct device *dev, enum hwmon_sensor_types type, u32 attr,
		    int channel, long *val)
{
	int ret;
	struct aqc_data *priv = dev_get_drvdata(dev);

	if (time_after(jiffies, priv->updated + STATUS_UPDATE_INTERVAL))
		return -ENODATA;

	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
			if (priv->temp_input[channel] == -ENODATA)
				return -ENODATA;

			*val = priv->temp_input[channel];
			break;
		case hwmon_temp_offset:
			ret = aqc_get_ctrl_val(priv, priv->temp_ctrl_offset + channel * AQC_TEMP_SENSOR_SIZE, val, 16);
			if (ret < 0)
				return ret;
			*val *= 10;
		default:
			break;
		}
		break;
	case hwmon_fan:
		*val = priv->speed_input[channel];
		break;
	case hwmon_power:
		*val = priv->power_input[channel];
		break;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_enable:
			ret = aqc_get_ctrl_val(priv, priv->fan_ctrl_offsets[channel], val, 8);
			if (ret < 0)
				return ret;

			*val = *val + 1;	/* Incrementing to satisfy hwmon rules */
			break;
		case hwmon_pwm_input:
			ret =
			    aqc_get_ctrl_val(priv,
					     priv->fan_ctrl_offsets[channel] +
					     AQC_FAN_CTRL_PWM_OFFSET, val, 16);
			if (ret < 0)
				return ret;

			*val = aqc_percent_to_pwm(*val);
			break;
		case hwmon_pwm_auto_channels_temp:
			ret =
			    aqc_get_ctrl_val(priv,
					     priv->fan_ctrl_offsets[channel] +
					     AQC_FAN_CTRL_TEMP_SELECT_OFFSET, val, 16);
			if (ret < 0)
				return ret;

			*val = 1 << *val;
		default:
			break;
		}
		break;
	case hwmon_in:
		*val = priv->voltage_input[channel];
		break;
	case hwmon_curr:
		*val = priv->current_input[channel];
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int aqc_read_string(struct device *dev, enum hwmon_sensor_types type, u32 attr,
			   int channel, const char **str)
{
	struct aqc_data *priv = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_temp:
		if (channel < priv->num_temp_sensors)
			*str = priv->temp_label[channel];
		else
			*str = priv->virtual_temp_label[channel - priv->num_temp_sensors];
		break;
	case hwmon_fan:
		*str = priv->speed_label[channel];
		break;
	case hwmon_power:
		*str = priv->power_label[channel];
		break;
	case hwmon_in:
		*str = priv->voltage_label[channel];
		break;
	case hwmon_curr:
		*str = priv->current_label[channel];
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int aqc_write(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel,
		     long val)
{
	int ret, pwm_value, temp_sensor;
	long ctrl_mode;
	struct aqc_data *priv = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_offset:
			/* Limit temp offset to +/- 15K as in the official software */
			val = clamp_val(val, -15000, 15000) / 10;
			ret = aqc_set_ctrl_val(priv, priv->temp_ctrl_offset + channel * AQC_TEMP_SENSOR_SIZE,
								val, 16);
			if (ret < 0)
				return ret;
			break;
		default:
			return -EOPNOTSUPP;
		}
		break;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_enable:
			switch (priv->kind) {
			case d5next:
				if (val < 0 || val > 3)
					return -EINVAL;
				break;
			case octo:
			case quadro:
				if (val < 0 || val > priv->num_fans + 3)
					return -EINVAL;

				/* Fan can't follow itself */
				if (val == channel + 4)
					return -EINVAL;

				/* Check if fan we want to follow is following another one
				 * currently. Following the official software, this is not
				 * supported.
				 */
				if (val > 3) {
					ret =
					    aqc_get_ctrl_val(priv, priv->fan_ctrl_offsets[val - 4],
							     &ctrl_mode, 8);
					if (ret < 0)
						return ret;

					/* The fan is indeed following another one */
					if (ctrl_mode > 2)
						return -EINVAL;
				}
				break;
			default:
				return -EOPNOTSUPP;
			}

			/* Decrement to convert from hwmon to aqc */
			ret = aqc_set_ctrl_val(priv, priv->fan_ctrl_offsets[channel], val - 1, 8);
			if (ret < 0)
				return ret;
			break;
		case hwmon_pwm_input:
			pwm_value = aqc_pwm_to_percent(val);
			if (pwm_value < 0)
				return pwm_value;

			ret =
			    aqc_set_ctrl_val(priv,
					     priv->fan_ctrl_offsets[channel] +
					     AQC_FAN_CTRL_PWM_OFFSET, pwm_value, 16);
			if (ret < 0)
				return ret;
			break;
		case hwmon_pwm_auto_channels_temp:
			switch (val) {
			case 1:
				temp_sensor = 0;
				break;
			case 2:
				temp_sensor = 1;
				break;
			case 4:
				temp_sensor = 2;
				break;
			case 8:
				temp_sensor = 3;
				break;
			default:
				return -EINVAL;
			}

			if (temp_sensor >= priv->num_temp_sensors)
				return -EINVAL;

			ret =
			    aqc_set_ctrl_val(priv,
					     priv->fan_ctrl_offsets[channel] +
					     AQC_FAN_CTRL_TEMP_SELECT_OFFSET, temp_sensor, 16);
			if (ret < 0)
				return ret;
		default:
			break;
		}
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static const struct hwmon_ops aqc_hwmon_ops = {
	.is_visible = aqc_is_visible,
	.read = aqc_read,
	.read_string = aqc_read_string,
	.write = aqc_write
};

static const struct hwmon_channel_info *aqc_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_OFFSET,
			   HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_OFFSET,
			   HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_OFFSET,
			   HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_OFFSET,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(power,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL,
			   HWMON_P_INPUT | HWMON_P_LABEL),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_AUTO_CHANNELS_TEMP,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_AUTO_CHANNELS_TEMP,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_AUTO_CHANNELS_TEMP,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_AUTO_CHANNELS_TEMP,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_AUTO_CHANNELS_TEMP,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_AUTO_CHANNELS_TEMP,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_AUTO_CHANNELS_TEMP,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE | HWMON_PWM_AUTO_CHANNELS_TEMP),
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(curr,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL),
	NULL
};

static const struct hwmon_chip_info aqc_chip_info = {
	.ops = &aqc_hwmon_ops,
	.info = aqc_info,
};

static int aqc_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data, int size)
{
	int i, j;
	s16 sensor_value;
	struct aqc_data *priv;

	if (report->id != STATUS_REPORT_ID)
		return 0;

	priv = hid_get_drvdata(hdev);

	/* Info provided with every report */
	priv->serial_number[0] = get_unaligned_be16(data + SERIAL_FIRST_PART);
	priv->serial_number[1] = get_unaligned_be16(data + SERIAL_SECOND_PART);
	priv->firmware_version = get_unaligned_be16(data + FIRMWARE_VERSION);

	/* Normal temperature sensor readings */
	for (i = 0; i < priv->num_temp_sensors; i++) {
		sensor_value = get_unaligned_be16(data +
						  priv->temp_sensor_start_offset +
						  i * AQC_TEMP_SENSOR_SIZE);
		if (sensor_value == AQC_TEMP_SENSOR_DISCONNECTED)
			priv->temp_input[i] = -ENODATA;
		else
			priv->temp_input[i] = sensor_value * 10;
	}

	/* Virtual temperature sensor readings */
	for (j = 0; j< priv->num_virtual_temp_sensors; j++) {
		sensor_value = get_unaligned_be16(data +
						  priv->virtual_temp_sensor_start_offset +
						  j * AQC_TEMP_SENSOR_SIZE);
		if (sensor_value == AQC_TEMP_SENSOR_DISCONNECTED)
			priv->temp_input[i] = -ENODATA;
		else
			priv->temp_input[i] = sensor_value * 10;
		i++;
	}

	/* Fan speed and related readings */
	for (i = 0; i < priv->num_fans; i++) {
		priv->speed_input[i] =
		    get_unaligned_be16(data + priv->fan_sensor_offsets[i] + AQC_FAN_SPEED_OFFSET);
		priv->power_input[i] =
		    get_unaligned_be16(data + priv->fan_sensor_offsets[i] +
				       AQC_FAN_POWER_OFFSET) * 10000;
		priv->voltage_input[i] =
		    get_unaligned_be16(data + priv->fan_sensor_offsets[i] +
				       AQC_FAN_VOLTAGE_OFFSET) * 10;
		priv->current_input[i] =
		    get_unaligned_be16(data + priv->fan_sensor_offsets[i] + AQC_FAN_CURRENT_OFFSET);
	}

	if (priv->power_cycle_count_offset != 0)
		priv->power_cycles = get_unaligned_be32(data + priv->power_cycle_count_offset);

	/* Special-case sensor readings */
	switch (priv->kind) {
	case d5next:
		priv->voltage_input[2] = get_unaligned_be16(data + D5NEXT_5V_VOLTAGE) * 10;
		priv->voltage_input[3] = get_unaligned_be16(data + D5NEXT_12V_VOLTAGE) * 10;
		break;
	case quadro:
		priv->speed_input[4] = get_unaligned_be16(data + priv->flow_sensor_offset);
		break;
	default:
		break;
	}

	priv->updated = jiffies;

	return 0;
}

#ifdef CONFIG_DEBUG_FS

static int serial_number_show(struct seq_file *seqf, void *unused)
{
	struct aqc_data *priv = seqf->private;

	seq_printf(seqf, "%05u-%05u\n", priv->serial_number[0], priv->serial_number[1]);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(serial_number);

static int firmware_version_show(struct seq_file *seqf, void *unused)
{
	struct aqc_data *priv = seqf->private;

	seq_printf(seqf, "%u\n", priv->firmware_version);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(firmware_version);

static int power_cycles_show(struct seq_file *seqf, void *unused)
{
	struct aqc_data *priv = seqf->private;

	seq_printf(seqf, "%u\n", priv->power_cycles);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(power_cycles);

static void aqc_debugfs_init(struct aqc_data *priv)
{
	char name[64];

	scnprintf(name, sizeof(name), "%s_%s-%s", "aquacomputer", priv->name,
		  dev_name(&priv->hdev->dev));

	priv->debugfs = debugfs_create_dir(name, NULL);
	debugfs_create_file("serial_number", 0444, priv->debugfs, priv, &serial_number_fops);
	debugfs_create_file("firmware_version", 0444, priv->debugfs, priv, &firmware_version_fops);

	if (priv->power_cycle_count_offset != 0)
		debugfs_create_file("power_cycles", 0444, priv->debugfs, priv, &power_cycles_fops);
}

#else

static void aqc_debugfs_init(struct aqc_data *priv)
{
}

#endif

static int aqc_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct aqc_data *priv;
	int ret;

	priv = devm_kzalloc(&hdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->hdev = hdev;
	hid_set_drvdata(hdev, priv);

	priv->updated = jiffies - STATUS_UPDATE_INTERVAL;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret)
		return ret;

	ret = hid_hw_open(hdev);
	if (ret)
		goto fail_and_stop;

	switch (hdev->product) {
	case USB_PRODUCT_ID_D5NEXT:
		priv->kind = d5next;

		priv->num_fans = D5NEXT_NUM_FANS;
		priv->fan_sensor_offsets = d5next_sensor_fan_offsets;
		priv->fan_ctrl_offsets = d5next_ctrl_fan_offsets;

		priv->num_temp_sensors = D5NEXT_NUM_SENSORS;
		priv->temp_sensor_start_offset = D5NEXT_COOLANT_TEMP;
		priv->num_virtual_temp_sensors = D5NEXT_NUM_VIRTUAL_SENSORS;
		priv->virtual_temp_sensor_start_offset = D5NEXT_VIRTUAL_SENSOR_START;

		priv->power_cycle_count_offset = D5NEXT_POWER_CYCLES;
		priv->buffer_size = D5NEXT_CTRL_REPORT_SIZE;
		priv->temp_ctrl_offset = D5NEXT_TEMP_CTRL_OFFSET;

		priv->temp_label = label_d5next_temp;
		priv->virtual_temp_label = label_virtual_temp_sensors;
		priv->speed_label = label_d5next_speeds;
		priv->power_label = label_d5next_power;
		priv->voltage_label = label_d5next_voltages;
		priv->current_label = label_d5next_current;
		break;
	case USB_PRODUCT_ID_FARBWERK:
		priv->kind = farbwerk;

		priv->num_fans = 0;

		priv->num_temp_sensors = FARBWERK_NUM_SENSORS;
		priv->temp_sensor_start_offset = FARBWERK_SENSOR_START;
		priv->temp_ctrl_offset = 0;

		priv->temp_label = label_temp_sensors;
		break;
	case USB_PRODUCT_ID_FARBWERK360:
		priv->kind = farbwerk360;

		priv->num_fans = 0;

		priv->num_temp_sensors = FARBWERK360_NUM_SENSORS;
		priv->temp_sensor_start_offset = FARBWERK360_SENSOR_START;

		priv->num_virtual_temp_sensors = FARBWERK360_NUM_VIRTUAL_SENSORS;
		priv->virtual_temp_sensor_start_offset = FARBWERK360_VIRTUAL_SENSORS_START;
		priv->buffer_size = FARBWERK360_CTRL_REPORT_SIZE;
		priv->temp_ctrl_offset = FARBWERK360_TEMP_CTRL_OFFSET;

		priv->temp_label = label_temp_sensors;
		priv->virtual_temp_label = label_virtual_temp_sensors;
		break;
	case USB_PRODUCT_ID_OCTO:
		priv->kind = octo;

		priv->num_fans = OCTO_NUM_FANS;
		priv->fan_sensor_offsets = octo_sensor_fan_offsets;
		priv->fan_ctrl_offsets = octo_ctrl_fan_offsets;

		priv->num_temp_sensors = OCTO_NUM_SENSORS;
		priv->temp_sensor_start_offset = OCTO_SENSOR_START;
		priv->num_virtual_temp_sensors = OCTO_NUM_VIRTUAL_SENSORS;
		priv->virtual_temp_sensor_start_offset = OCTO_VIRTUAL_SENSOR_START;

		priv->power_cycle_count_offset = OCTO_POWER_CYCLES;
		priv->buffer_size = OCTO_CTRL_REPORT_SIZE;
		priv->temp_ctrl_offset = OCTO_TEMP_CTRL_OFFSET;

		priv->temp_label = label_temp_sensors;
		priv->virtual_temp_label = label_virtual_temp_sensors;
		priv->speed_label = label_fan_speed;
		priv->power_label = label_fan_power;
		priv->voltage_label = label_fan_voltage;
		priv->current_label = label_fan_current;
		break;
	case USB_PRODUCT_ID_QUADRO:
		priv->kind = quadro;

		priv->num_fans = QUADRO_NUM_FANS;
		priv->fan_sensor_offsets = quadro_sensor_fan_offsets;
		priv->fan_ctrl_offsets = quadro_ctrl_fan_offsets;

		priv->num_temp_sensors = QUADRO_NUM_SENSORS;
		priv->temp_sensor_start_offset = QUADRO_SENSOR_START;
		priv->num_virtual_temp_sensors = QUADRO_NUM_VIRTUAL_SENSORS;
		priv->virtual_temp_sensor_start_offset = QUADRO_VIRTUAL_SENSORS_START;

		priv->power_cycle_count_offset = QUADRO_POWER_CYCLES;
		priv->buffer_size = QUADRO_CTRL_REPORT_SIZE;
		priv->flow_sensor_offset = QUADRO_FLOW_SENSOR_OFFSET;
		priv->temp_ctrl_offset = QUADRO_TEMP_CTRL_OFFSET;

		priv->temp_label = label_temp_sensors;
		priv->virtual_temp_label = label_virtual_temp_sensors;
		priv->speed_label = label_quadro_speeds;
		priv->power_label = label_fan_power;
		priv->voltage_label = label_fan_voltage;
		priv->current_label = label_fan_current;
		break;
	default:
		break;
	}

	if (priv->buffer_size != 0) {
		priv->checksum_start = 0x01;
		priv->checksum_length = priv->buffer_size - 3;
		priv->checksum_offset = priv->buffer_size - 2;
	}

	priv->name = aqc_device_names[priv->kind];

	priv->buffer = devm_kzalloc(&hdev->dev, priv->buffer_size, GFP_KERNEL);
	if (!priv->buffer) {
		ret = -ENOMEM;
		goto fail_and_close;
	}

	mutex_init(&priv->mutex);

	priv->hwmon_dev = hwmon_device_register_with_info(&hdev->dev, priv->name, priv,
							  &aqc_chip_info, NULL);

	if (IS_ERR(priv->hwmon_dev)) {
		ret = (int)PTR_ERR(priv->hwmon_dev);
		goto fail_and_close;
	}

	aqc_debugfs_init(priv);

	return 0;

fail_and_close:
	hid_hw_close(hdev);
fail_and_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void aqc_remove(struct hid_device *hdev)
{
	struct aqc_data *priv = hid_get_drvdata(hdev);

	debugfs_remove_recursive(priv->debugfs);
	hwmon_device_unregister(priv->hwmon_dev);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id aqc_table[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_AQUACOMPUTER, USB_PRODUCT_ID_D5NEXT) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_AQUACOMPUTER, USB_PRODUCT_ID_FARBWERK) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_AQUACOMPUTER, USB_PRODUCT_ID_FARBWERK360) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_AQUACOMPUTER, USB_PRODUCT_ID_OCTO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_AQUACOMPUTER, USB_PRODUCT_ID_QUADRO) },
	{ }
};

MODULE_DEVICE_TABLE(hid, aqc_table);

static struct hid_driver aqc_driver = {
	.name = DRIVER_NAME,
	.id_table = aqc_table,
	.probe = aqc_probe,
	.remove = aqc_remove,
	.raw_event = aqc_raw_event,
};

static int __init aqc_init(void)
{
	return hid_register_driver(&aqc_driver);
}

static void __exit aqc_exit(void)
{
	hid_unregister_driver(&aqc_driver);
}

/* Request to initialize after the HID bus to ensure it's not being loaded before */
late_initcall(aqc_init);
module_exit(aqc_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Aleksa Savic <savicaleksa83@gmail.com>");
MODULE_AUTHOR("Jack Doan <me@jackdoan.com>");
MODULE_DESCRIPTION("Hwmon driver for Aquacomputer devices");
