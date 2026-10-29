// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Linux driver for WMI platform features on MSI notebooks.
 *
 * Copyright (C) 2024 Armin Wolf <W_Armin@gmx.de>
 *                 patch: fan duty/temp curve sysfs (MS-1591 family) — see
 *                 FAN_CURVE_MAPPING.md (mcontrolcenter-analysis) for the
 *                 validated WMAM <-> EC RAM mapping.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/device/driver.h>
#include <linux/dmi.h>
#include <linux/errno.h>
#include <linux/hwmon.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/rwsem.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/wmi.h>

#include <linux/unaligned.h>

#define DRIVER_NAME	"msi-wmi-platform"

#define MSI_PLATFORM_GUID	"ABBC0F6E-8EA1-11D1-00A0-C90629100000"

#define MSI_WMI_PLATFORM_INTERFACE_VERSION	2

#define MSI_PLATFORM_WMI_MAJOR_OFFSET	1
#define MSI_PLATFORM_WMI_MINOR_OFFSET	2

#define MSI_PLATFORM_EC_FLAGS_OFFSET	1
#define MSI_PLATFORM_EC_MINOR_MASK	GENMASK(3, 0)
#define MSI_PLATFORM_EC_MAJOR_MASK	GENMASK(5, 4)
#define MSI_PLATFORM_EC_CHANGED_PAGE	BIT(6)
#define MSI_PLATFORM_EC_IS_TIGERLAKE	BIT(7)
#define MSI_PLATFORM_EC_VERSION_OFFSET	2

#define MSI_PLATFORM_BUFFER_SIZE	32

static bool force;
module_param_unsafe(force, bool, 0);
MODULE_PARM_DESC(force, "Force loading without checking for supported WMI interface versions");

enum msi_wmi_platform_method {
	MSI_PLATFORM_GET_PACKAGE	= 0x01,
	MSI_PLATFORM_SET_PACKAGE	= 0x02,
	MSI_PLATFORM_GET_EC		= 0x03,
	MSI_PLATFORM_SET_EC		= 0x04,
	MSI_PLATFORM_GET_BIOS		= 0x05,
	MSI_PLATFORM_SET_BIOS		= 0x06,
	MSI_PLATFORM_GET_SMBUS		= 0x07,
	MSI_PLATFORM_SET_SMBUS		= 0x08,
	MSI_PLATFORM_GET_MASTER_BATTERY = 0x09,
	MSI_PLATFORM_SET_MASTER_BATTERY	= 0x0a,
	MSI_PLATFORM_GET_SLAVE_BATTERY	= 0x0b,
	MSI_PLATFORM_SET_SLAVE_BATTERY	= 0x0c,
	MSI_PLATFORM_GET_TEMPERATURE	= 0x0d,
	MSI_PLATFORM_SET_TEMPERATURE	= 0x0e,
	MSI_PLATFORM_GET_THERMAL	= 0x0f,
	MSI_PLATFORM_SET_THERMAL	= 0x10,
	MSI_PLATFORM_GET_FAN		= 0x11,
	MSI_PLATFORM_SET_FAN		= 0x12,
	MSI_PLATFORM_GET_DEVICE		= 0x13,
	MSI_PLATFORM_SET_DEVICE		= 0x14,
	MSI_PLATFORM_GET_POWER		= 0x15,
	MSI_PLATFORM_SET_POWER		= 0x16,
	MSI_PLATFORM_GET_DEBUG		= 0x17,
	MSI_PLATFORM_SET_DEBUG		= 0x18,
	MSI_PLATFORM_GET_AP		= 0x19,
	MSI_PLATFORM_SET_AP		= 0x1a,
	MSI_PLATFORM_GET_DATA		= 0x1b,
	MSI_PLATFORM_SET_DATA		= 0x1c,
	MSI_PLATFORM_GET_WMI		= 0x1d,
};

struct msi_wmi_platform_data {
	struct wmi_device *wdev;
	struct mutex wmi_lock;	/* Necessary when calling WMI methods */
};

struct msi_wmi_platform_debugfs_data {
	struct msi_wmi_platform_data *data;
	enum msi_wmi_platform_method method;
	struct rw_semaphore buffer_lock;	/* Protects debugfs buffer */
	size_t length;
	u8 buffer[32];
};

static const char * const msi_wmi_platform_debugfs_names[] = {
	"get_package",
	"set_package",
	"get_ec",
	"set_ec",
	"get_bios",
	"set_bios",
	"get_smbus",
	"set_smbus",
	"get_master_battery",
	"set_master_battery",
	"get_slave_battery",
	"set_slave_battery",
	"get_temperature",
	"set_temperature",
	"get_thermal",
	"set_thermal",
	"get_fan",
	"set_fan",
	"get_device",
	"set_device",
	"get_power",
	"set_power",
	"get_debug",
	"set_debug",
	"get_ap",
	"set_ap",
	"get_data",
	"set_data",
	"get_wmi"
};

static int msi_wmi_platform_parse_buffer(union acpi_object *obj, u8 *output, size_t length)
{
	if (obj->type != ACPI_TYPE_BUFFER)
		return -ENOMSG;

	if (obj->buffer.length != length)
		return -EPROTO;

	if (!obj->buffer.pointer[0])
		return -EIO;

	memcpy(output, obj->buffer.pointer, obj->buffer.length);

	return 0;
}

static int msi_wmi_platform_query(struct msi_wmi_platform_data *data,
				  enum msi_wmi_platform_method method, u8 *input,
				  size_t input_length, u8 *output, size_t output_length)
{
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	struct acpi_buffer in = {
		.length = input_length,
		.pointer = input
	};
	union acpi_object *obj;
	acpi_status status;
	int ret;

	if (!input_length || !output_length)
		return -EINVAL;

	/*
	 * The ACPI control method responsible for handling the WMI method calls
	 * is not thread-safe. Because of this we have to do the locking ourself.
	 */
	scoped_guard(mutex, &data->wmi_lock) {
		status = wmidev_evaluate_method(data->wdev, 0x0, method, &in, &out);
		if (ACPI_FAILURE(status))
			return -EIO;
	}

	obj = out.pointer;
	if (!obj)
		return -ENODATA;

	ret = msi_wmi_platform_parse_buffer(obj, output, output_length);
	kfree(obj);

	return ret;
}

static umode_t msi_wmi_platform_is_visible(const void *drvdata, enum hwmon_sensor_types type,
					   u32 attr, int channel)
{
	return 0444;
}

static int msi_wmi_platform_read(struct device *dev, enum hwmon_sensor_types type, u32 attr,
				 int channel, long *val)
{
	struct msi_wmi_platform_data *data = dev_get_drvdata(dev);
	u8 input[32] = { 0 };
	u8 output[32];
	u16 value;
	int ret;

	ret = msi_wmi_platform_query(data, MSI_PLATFORM_GET_FAN, input, sizeof(input), output,
				     sizeof(output));
	if (ret < 0)
		return ret;

	value = get_unaligned_be16(&output[channel * 2 + 1]);
	if (!value)
		*val = 0;
	else
		*val = 480000 / value;

	return 0;
}

static const struct hwmon_ops msi_wmi_platform_ops = {
	.is_visible = msi_wmi_platform_is_visible,
	.read = msi_wmi_platform_read,
};

static const struct hwmon_channel_info * const msi_wmi_platform_info[] = {
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT
			   ),
	NULL
};

static const struct hwmon_chip_info msi_wmi_platform_chip_info = {
	.ops = &msi_wmi_platform_ops,
	.info = msi_wmi_platform_info,
};

/* --- Fan curve interface (MS-1591 family, validated) ---------------------- *
 *
 * WMAM GET_FAN (0x11) / SET_FAN (0x12), 32-byte buffers:
 *   IPAR 1: EC RAM 0x71-0x78 (fan1 = CPU):
 *     buffer[1] = EC[0x71] live duty (EC-written), buffer[2..8] = EC[0x72-0x78]
 *     = 7 duty curve points (observed default: 45 55 60 70 90 110 130).
 *   IPAR 2: EC RAM 0x89-0x90 (fan2 = GPU), same layout.
 *
 * WMAM GET_TEMPERATURE (0x0D), non-sequential temperature windows:
 *   IPAR 1: EC 0x69, 0x6F, 0x70, 0x6A-0x6E  (in this output order)
 *   IPAR 2: EC 0x81, 0x87, 0x88, 0x82-0x86
 * EC-ascending order: enable, 5 curve temperature points, 2 alarm thresholds.
 * Curve temperature writes go through SET_TEMPERATURE (0x0E) with the same
 * windows — deliberately NOT exposed by this patch (the firmware only
 * updates them through its own defaults; the Windows UI keeps them read-only
 * as well). Expose them read-only so tools can monitor coherence.
 *
 * SAFETY:
 *   - Duty writes are disabled unless the module parameter
 *     `allow_curve_writes=1` is passed at load time (explicit opt-in).
 *   - Writes always preserve the live-duty byte (buffer[1]) taken from a
 *     fresh GET, write the full 8-byte window in one WC-set WMAM command
 *     (never per-byte EC writes), and verify with a readback.
 *   - Fan mode (advanced) is out of scope: it is handled by the msi-ec
 *     driver through its validated sysfs interface (`fan_mode`).
 */

static bool allow_curve_writes;
module_param_unsafe(allow_curve_writes, bool, 0644);
MODULE_PARM_DESC(allow_curve_writes, "Allow fan duty curve writes via WMAM SET_FAN (experimental, requires explicit user consent)");

static const struct dmi_system_id msi_wmi_platform_curve_whitelist[] = {
	{
		.ident = "MSI Summit E16 Flip A11UCT (MS-1591)",
		.matches = {
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "MS-1591"),
		},
	},
	{ }
};

static u8 msi_wmi_platform_fan_ipar(bool fan2)
{
	return fan2 ? 2 : 1;
}

static int msi_wmi_platform_curve_get_fan_block(struct msi_wmi_platform_data *data, bool fan2,
						u8 *output)
{
	u8 input[32] = { };

	input[0] = msi_wmi_platform_fan_ipar(fan2);

	return msi_wmi_platform_query(data, MSI_PLATFORM_GET_FAN, input, sizeof(input), output,
				      MSI_PLATFORM_BUFFER_SIZE);
}

static int msi_wmi_platform_curve_parse_duty(const char *buf, u8 duty[7])
{
	int vals[7];
	int i, n;

	n = sscanf(buf, "%i %i %i %i %i %i %i",
		   &vals[0], &vals[1], &vals[2], &vals[3], &vals[4], &vals[5], &vals[6]);
	if (n != 7)
		return -EINVAL;

	for (i = 0; i < 7; i++) {
		if (vals[i] < 0 || vals[i] > 255)
			return -EINVAL;

		duty[i] = vals[i];
	}

	return 0;
}

static ssize_t msi_wmi_platform_duty_curve_show(struct device *dev, char *buf, bool fan2)
{
	struct msi_wmi_platform_data *data = dev_get_drvdata(dev);
	u8 output[32];
	int ret;

	ret = msi_wmi_platform_curve_get_fan_block(data, fan2, output);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%u %u %u %u %u %u %u\n",
			  output[2], output[3], output[4], output[5], output[6], output[7],
			  output[8]);
}

static ssize_t msi_wmi_platform_duty_curve_store(struct device *dev, const char *buf,
						 size_t count, bool fan2)
{
	struct msi_wmi_platform_data *data = dev_get_drvdata(dev);
	u8 input[32] = { };
	u8 scratch[32];
	u8 readback[32];
	u8 duty[7];
	const char *fan_name = fan2 ? "fan2" : "fan1";
	int ret;

	if (!allow_curve_writes)
		return -EPERM;

	ret = msi_wmi_platform_curve_parse_duty(buf, duty);
	if (ret < 0) {
		dev_warn(&data->wdev->dev, "%s: invalid duty curve (need 7 values 0-255)\n",
			 fan_name);
		return ret;
	}

	/* Fresh snapshot of the window so the live-duty byte is preserved. */
	ret = msi_wmi_platform_curve_get_fan_block(data, fan2, scratch);
	if (ret < 0)
		return ret;

	input[0] = msi_wmi_platform_fan_ipar(fan2);
	input[1] = scratch[1];
	memcpy(&input[2], duty, 7);

	ret = msi_wmi_platform_query(data, MSI_PLATFORM_SET_FAN, input, sizeof(input), scratch,
				     sizeof(scratch));
	if (ret < 0)
		dev_warn(&data->wdev->dev,
			 "%s: SET_FAN error 0x%x (continuing with readback check)\n", fan_name, ret);

	/* The result is decided by the readback, not the SET status. */
	ret = msi_wmi_platform_curve_get_fan_block(data, fan2, readback);
	if (ret < 0)
		return ret;

	if (memcmp(&readback[2], duty, 7)) {
		dev_warn(&data->wdev->dev, "%s: duty curve readback mismatch, write lost\n",
			 fan_name);
		return -EIO;
	}

	dev_dbg(&data->wdev->dev, "%s: duty curve updated\n", fan_name);

	return count;
}

static ssize_t fan1_duty_curve_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return msi_wmi_platform_duty_curve_show(dev, buf, false);
}

static ssize_t fan1_duty_curve_store(struct device *dev, struct device_attribute *attr,
				     const char *buf, size_t count)
{
	return msi_wmi_platform_duty_curve_store(dev, buf, count, false);
}

static ssize_t fan2_duty_curve_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return msi_wmi_platform_duty_curve_show(dev, buf, true);
}

static ssize_t fan2_duty_curve_store(struct device *dev, struct device_attribute *attr,
				     const char *buf, size_t count)
{
	return msi_wmi_platform_duty_curve_store(dev, buf, count, true);
}

static ssize_t msi_wmi_platform_live_duty_show(struct device *dev, char *buf, bool fan2)
{
	struct msi_wmi_platform_data *data = dev_get_drvdata(dev);
	u8 output[32];
	int ret;

	ret = msi_wmi_platform_curve_get_fan_block(data, fan2, output);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%u\n", output[1]);
}

static ssize_t fan1_live_duty_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return msi_wmi_platform_live_duty_show(dev, buf, false);
}

static ssize_t fan2_live_duty_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return msi_wmi_platform_live_duty_show(dev, buf, true);
}

static ssize_t msi_wmi_platform_temp_curve_show(struct device *dev, char *buf, bool fan2)
{
	struct msi_wmi_platform_data *data = dev_get_drvdata(dev);
	u8 input[32] = { };
	u8 output[32];
	int ret;

	input[0] = msi_wmi_platform_fan_ipar(fan2);

	ret = msi_wmi_platform_query(data, MSI_PLATFORM_GET_TEMPERATURE, input, sizeof(input),
				     output, sizeof(output));
	if (ret < 0)
		return ret;

	/* De-shuffle to EC-ascending order: enable, 5 curve points, 2 alarms. */
	return sysfs_emit(buf, "%u %u %u %u %u %u %u %u\n",
			  output[1], output[4], output[5], output[6], output[7], output[8],
			  output[2], output[3]);
}

static ssize_t fan1_temp_curve_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return msi_wmi_platform_temp_curve_show(dev, buf, false);
}

static ssize_t fan2_temp_curve_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return msi_wmi_platform_temp_curve_show(dev, buf, true);
}

static DEVICE_ATTR_RW(fan1_duty_curve);
static DEVICE_ATTR_RW(fan2_duty_curve);
static DEVICE_ATTR_RO(fan1_live_duty);
static DEVICE_ATTR_RO(fan2_live_duty);
static DEVICE_ATTR_RO(fan1_temp_curve);
static DEVICE_ATTR_RO(fan2_temp_curve);

static struct attribute *msi_wmi_platform_curve_attrs[] = {
	&dev_attr_fan1_duty_curve.attr,
	&dev_attr_fan2_duty_curve.attr,
	&dev_attr_fan1_live_duty.attr,
	&dev_attr_fan2_live_duty.attr,
	&dev_attr_fan1_temp_curve.attr,
	&dev_attr_fan2_temp_curve.attr,
	NULL
};

static const struct attribute_group msi_wmi_platform_curve_group = {
	.attrs = msi_wmi_platform_curve_attrs,
};

static void msi_wmi_platform_curve_remove(void *priv)
{
	struct device *dev = priv;

	sysfs_remove_group(&dev->kobj, &msi_wmi_platform_curve_group);
}

static int msi_wmi_platform_curve_init(struct msi_wmi_platform_data *data)
{
	int ret;

	if (!dmi_check_system(msi_wmi_platform_curve_whitelist))
		return 0;	/* No curve hardware known — stay read-only. */

	ret = sysfs_create_group(&data->wdev->dev.kobj, &msi_wmi_platform_curve_group);
	if (ret < 0)
		return ret;

	return devm_add_action_or_reset(&data->wdev->dev, msi_wmi_platform_curve_remove,
					&data->wdev->dev);
}

/* --- debugfs -------------------------------------------------------------- */

static ssize_t msi_wmi_platform_write(struct file *fp, const char __user *input, size_t length,
				      loff_t *offset)
{
	struct seq_file *seq = fp->private_data;
	struct msi_wmi_platform_debugfs_data *data = seq->private;
	u8 payload[32] = { };
	ssize_t ret;

	/* Do not allow partial writes */
	if (*offset != 0)
		return -EINVAL;

	/* Do not allow incomplete command buffers */
	if (length != data->length)
		return -EINVAL;

	ret = simple_write_to_buffer(payload, sizeof(payload), offset, input, length);
	if (ret < 0)
		return ret;

	down_write(&data->buffer_lock);
	ret = msi_wmi_platform_query(data->data, data->method, payload, data->length, data->buffer,
				     data->length);
	up_write(&data->buffer_lock);

	if (ret < 0)
		return ret;

	return length;
}

static int msi_wmi_platform_show(struct seq_file *seq, void *p)
{
	struct msi_wmi_platform_debugfs_data *data = seq->private;
	int ret;

	down_read(&data->buffer_lock);
	ret = seq_write(seq, data->buffer, data->length);
	up_read(&data->buffer_lock);

	return ret;
}

static int msi_wmi_platform_open(struct inode *inode, struct file *fp)
{
	struct msi_wmi_platform_debugfs_data *data = inode->i_private;

	/* The seq_file uses the last byte of the buffer for detecting buffer overflows */
	return single_open_size(fp, msi_wmi_platform_show, data, data->length + 1);
}

static const struct file_operations msi_wmi_platform_debugfs_fops = {
	.owner = THIS_MODULE,
	.open = msi_wmi_platform_open,
	.read = seq_read,
	.write = msi_wmi_platform_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static void msi_wmi_platform_debugfs_remove(void *data)
{
	struct dentry *dir = data;

	debugfs_remove_recursive(dir);
}

static void msi_wmi_platform_debugfs_add(struct msi_wmi_platform_data *drvdata, struct dentry *dir,
					 const char *name, enum msi_wmi_platform_method method)
{
	struct msi_wmi_platform_debugfs_data *data;
	struct dentry *entry;

	data = devm_kzalloc(&drvdata->wdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return;

	data->data = drvdata;
	data->method = method;
	init_rwsem(&data->buffer_lock);

	/* The ACPI firmware for now always requires a 32 byte input buffer due to
	 * a peculiarity in how Windows handles the CreateByteField() ACPI operator.
	 */
	data->length = 32;

	entry = debugfs_create_file(name, 0600, dir, data, &msi_wmi_platform_debugfs_fops);
	if (IS_ERR(entry))
		devm_kfree(&drvdata->wdev->dev, data);
}

static void msi_wmi_platform_debugfs_init(struct msi_wmi_platform_data *data)
{
	struct dentry *dir;
	char dir_name[64];
	int ret, method;

	scnprintf(dir_name, ARRAY_SIZE(dir_name), "%s-%s", DRIVER_NAME, dev_name(&data->wdev->dev));

	dir = debugfs_create_dir(dir_name, NULL);
	if (IS_ERR(dir))
		return;

	ret = devm_add_action_or_reset(&data->wdev->dev, msi_wmi_platform_debugfs_remove, dir);
	if (ret < 0)
		return;

	for (method = MSI_PLATFORM_GET_PACKAGE; method <= MSI_PLATFORM_GET_WMI; method++)
		msi_wmi_platform_debugfs_add(data, dir, msi_wmi_platform_debugfs_names[method - 1],
					     method);
}

static int msi_wmi_platform_hwmon_init(struct msi_wmi_platform_data *data)
{
	struct device *hdev;

	hdev = devm_hwmon_device_register_with_info(&data->wdev->dev, "msi_wmi_platform", data,
						    &msi_wmi_platform_chip_info, NULL);

	return PTR_ERR_OR_ZERO(hdev);
}

static int msi_wmi_platform_ec_init(struct msi_wmi_platform_data *data)
{
	u8 input[32] = { 0 };
	u8 output[32];
	u8 flags;
	int ret;

	ret = msi_wmi_platform_query(data, MSI_PLATFORM_GET_EC, input, sizeof(input), output,
				     sizeof(output));
	if (ret < 0)
		return ret;

	flags = output[MSI_PLATFORM_EC_FLAGS_OFFSET];

	dev_dbg(&data->wdev->dev, "EC RAM version %lu.%lu\n",
		FIELD_GET(MSI_PLATFORM_EC_MAJOR_MASK, flags),
		FIELD_GET(MSI_PLATFORM_EC_MINOR_MASK, flags));
	dev_dbg(&data->wdev->dev, "EC firmware version %.28s\n",
		&output[MSI_PLATFORM_EC_VERSION_OFFSET]);

	if (!(flags & MSI_PLATFORM_EC_IS_TIGERLAKE)) {
		if (!force)
			return -ENODEV;

		dev_warn(&data->wdev->dev, "Loading on a non-Tigerlake platform\n");
	}

	return 0;
}

static int msi_wmi_platform_init(struct msi_wmi_platform_data *data)
{
	u8 input[32] = { 0 };
	u8 output[32];
	int ret;

	ret = msi_wmi_platform_query(data, MSI_PLATFORM_GET_WMI, input, sizeof(input), output,
				     sizeof(output));
	if (ret < 0)
		return ret;

	dev_dbg(&data->wdev->dev, "WMI interface version %u.%u\n",
		output[MSI_PLATFORM_WMI_MAJOR_OFFSET],
		output[MSI_PLATFORM_WMI_MINOR_OFFSET]);

	if (output[MSI_PLATFORM_WMI_MAJOR_OFFSET] != MSI_WMI_PLATFORM_INTERFACE_VERSION) {
		if (!force)
			return -ENODEV;

		dev_warn(&data->wdev->dev,
			 "Loading despite unsupported WMI interface version (%u.%u)\n",
			 output[MSI_PLATFORM_WMI_MAJOR_OFFSET],
			 output[MSI_PLATFORM_WMI_MINOR_OFFSET]);
	}

	return 0;
}

static int msi_wmi_platform_probe(struct wmi_device *wdev, const void *context)
{
	struct msi_wmi_platform_data *data;
	int ret;

	data = devm_kzalloc(&wdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->wdev = wdev;
	dev_set_drvdata(&wdev->dev, data);

	ret = devm_mutex_init(&wdev->dev, &data->wmi_lock);
	if (ret < 0)
		return ret;

	ret = msi_wmi_platform_init(data);
	if (ret < 0)
		return ret;

	ret = msi_wmi_platform_ec_init(data);
	if (ret < 0)
		return ret;

	ret = msi_wmi_platform_curve_init(data);
	if (ret < 0)
		return ret;

	msi_wmi_platform_debugfs_init(data);

	return msi_wmi_platform_hwmon_init(data);
}

static const struct wmi_device_id msi_wmi_platform_id_table[] = {
	{ MSI_PLATFORM_GUID, NULL },
	{ }
};
MODULE_DEVICE_TABLE(wmi, msi_wmi_platform_id_table);

static struct wmi_driver msi_wmi_platform_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.id_table = msi_wmi_platform_id_table,
	.probe = msi_wmi_platform_probe,
	.no_singleton = true,
};

/*
 * MSI reused the WMI GUID from the WMI-ACPI sample code provided by Microsoft,
 * so other manufacturers might use it as well for their WMI-ACPI implementations.
 */
static const struct dmi_system_id msi_wmi_platform_whitelist[] __initconst = {
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "MICRO-STAR INT"),
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Micro-Star International"),
		},
	},
	{ }
};

static int __init msi_wmi_platform_module_init(void)
{
	if (!dmi_check_system(msi_wmi_platform_whitelist)) {
		if (!force)
			return -ENODEV;

		pr_warn("Ignoring DMI whitelist\n");
	}

	return wmi_driver_register(&msi_wmi_platform_driver);
}

static void __exit msi_wmi_platform_module_exit(void)
{
	wmi_driver_unregister(&msi_wmi_platform_driver);
}

module_init(msi_wmi_platform_module_init);
module_exit(msi_wmi_platform_module_exit);


MODULE_AUTHOR("Armin Wolf <W_Armin@gmx.de>");
MODULE_DESCRIPTION("MSI WMI platform features");
MODULE_LICENSE("GPL");
