/**
 * @file bme280_sensor.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/miscdevice.h>
#include <linux/i2c.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/slab.h> // required for kmalloc
#include <linux/fs.h>   // file_operations
#include "bme280_sensor.h"

#define BME280_SENSOR_DEVICE "bme280_sensor"
#define BME280_CHIP_ID_REGISTER 0xD0
#define BME280_CHIP_ID_VALUE 0x60

int sensor_major = 0; // use dynamic major
int sensor_minor = 0;

MODULE_AUTHOR("Venetia Furtado");
MODULE_LICENSE("Dual BSD/GPL");

struct sensor_dev sensor_device;

int sensor_open(struct inode *inode, struct file *filp)
{
    struct sensor_dev *dev;
    PDEBUG("open");
    dev = container_of(inode->i_cdev, struct sensor_dev, cdev);
    filp->private_data = dev;
    return 0;
}

int sensor_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    filp->private_data = NULL;
    return 0;
}

ssize_t sensor_read(struct file *filp, char __user *buf, size_t count,
                    loff_t *f_pos)
{
    ssize_t retval = 0;
    ssize_t num_copy_bytes = 0;
    unsigned long copy_status = 0;
    int val = 0x24;

    // error checking
    if (filp == NULL || buf == NULL || f_pos == NULL)
    {
        PDEBUG("ERROR: input argument error");
        return -EINVAL;
    }

    // copy data
    num_copy_bytes = 4;

    copy_status = copy_to_user(buf, &val, num_copy_bytes);
    if (copy_status == 0)
    {
        // advance file position by number of bytes copied
        *f_pos += num_copy_bytes;
        retval = num_copy_bytes;
    }
    else
    {
        PDEBUG("DEBUG: copy to user failed");
        retval = -EFAULT;
        return 0;
    }

    return retval;
}

#if 0
static int sensor_setup_cdev(struct sensor_dev *dev)
{
    int err, devno = MKDEV(sensor_major, sensor_minor);

    cdev_init(&dev->cdev, &sensor_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &sensor_fops;
    err = cdev_add(&dev->cdev, devno, 1);
    if (err)
    {
        printk(KERN_ERR "Error %d adding bme280 sensor cdev", err);
    }
    return err;
}

int sensor_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, sensor_minor, 1,
                                 "bme280_sensor");
    sensor_major = MAJOR(dev);
    if (result < 0)
    {
        printk(KERN_WARNING "Can't get major %d\n", sensor_major);
        return result;
    }
    memset(&sensor_device, 0, sizeof(struct sensor_dev));

    result = sensor_setup_cdev(&sensor_device);

    if (result)
    {
        unregister_chrdev_region(dev, 1);
    }
    return result;
}

void sensor_cleanup_module(void)
{

    dev_t devno = MKDEV(sensor_major, sensor_minor);

    cdev_del(&sensor_device.cdev);

    unregister_chrdev_region(devno, 1);
}
#endif

struct file_operations bme280_sensor_fops = {
    .owner = THIS_MODULE,
    .read = sensor_read,
    .open = sensor_open,
    .release = sensor_release,
};

static struct miscdevice bme280_sensor_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = BME280_SENSOR_DEVICE,
    .fops = &bme280_sensor_fops,
};

static int bme280_sensor_probe(struct i2c_client *client,
                               const struct i2c_device_id *id)
{
    int status;
    uint8_t who;

    who = i2c_smbus_read_byte_data(client, BME280_CHIP_ID_REGISTER);
    if (who != BME280_CHIP_ID_VALUE)
    {
        dev_err(&client->dev,
                "unknown CHIP ID: 0x%02x\n", who);
        return -ENODEV;
    }

    PDEBUG("DEBUG: bme280 sensor found");

    /* register device */
    status = misc_register(&bme280_sensor_device);
    if (status != 0)
    {
        dev_err(&client->dev,
                "misc_register failed: %d\n", status);
        return status;
    }

    PDEBUG("DEBUG: bme280 sensor registered");

    dev_info(&client->dev, "bme280 sensor driver loaded\n");
    return 0;

    /*
    TODO: deregister if register and we have error
err_misc:
    misc_deregister(&bme280_sensor_device);
    return ret;
    */
}

static int bme280_sensor_remove(struct i2c_client *client)
{
    misc_deregister(&bme280_sensor_device);
    PDEBUG("DEBUG: bme280 sensor removed");
    dev_info(&client->dev, "bme280 sensor unloaded\n");
    return 0;
}

static const struct i2c_device_id bme280_sensor_id[] = {
    {BME280_SENSOR_DEVICE, 0},
    {}};
MODULE_DEVICE_TABLE(i2c, bme280_sensor_id);

static struct i2c_driver bme280_sensor_driver = {
    .driver = {
        .name = BME280_SENSOR_DEVICE,
    },
    .probe = bme280_sensor_probe,
    .remove = bme280_sensor_remove,
    .id_table = bme280_sensor_id,
};

module_i2c_driver(bme280_sensor_driver);

// module_init(sensor_init_module);
// module_exit(sensor_cleanup_module);