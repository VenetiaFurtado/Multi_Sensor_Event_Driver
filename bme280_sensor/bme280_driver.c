/**
 * @file bme280_driver.c
 * @brief Driver code for the BME280 sensor
 *
 * @author Venetia Furtado
 * @date 2026-04-05
 *
 * References:
 * 1. https://github.com/sparkfun/SparkFun_BME280_Arduino_Library/tree/870c17da1f4c76561e14b8ffcc7cdffd63136e10/src
 * 2. https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bme280-ds002.pdf
 * 3. https://github.com/cu-ecen-aeld/assignments-3-and-later-VenetiaFurtado
 * 4. https://developerhelp.microchip.com/xwiki/bin/view/software-tools/linux/apps-i2c/
 * 5. https://chat.deepseek.com/share/w18dko9noz63hm2vq9
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
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/wait.h>
#include <linux/random.h>
#include <linux/poll.h>
#include <linux/compiler.h> // READ_ONCE / WRITE_ONCE
#include "bme280_driver.h"
#include "i2c.h"

#define BME280_SENSOR_DEVICE "bme280_sensor"
#define DEVICE_COUNT 3
#define HIGH_TEMP_THRESHOLD 30
#define LOW_TEMP_THRESHOLD 5

/*******************************************************************************
 *                        Device type setup
 ******************************************************************************/

enum bme280_type
{
    BME_TEMP = 0,
    BME_HUMIDITY = 1,
    BME_PRESSURE = 2,
};

// Shared hardware structure
struct bme280_hw
{
    struct mutex lock;
    int temp;
    int humidity;
    int pressure;
};

// Per-device node
struct bme280_node
{
    struct miscdevice miscdev;
    struct bme280_hw *hw;
    enum bme280_type type;
};

/*******************************************************************************
 *                              Global declarations
 ******************************************************************************/
static struct bme280_hw g_hw;                  // singleton hardware instance
static struct bme280_node nodes[DEVICE_COUNT]; // array of device nodes

static struct i2c_client *client_singleton;
static struct task_struct *sensor_read_kthread; // reads bme280 asynchronously
static struct task_struct *synthetic_data_event_kthread;

static wait_queue_head_t wq_high;
static wait_queue_head_t wq_low;

static BME280_Data sensor_data; // kthread writes into (shared memory between kthread and user_read)
static int current_temp = 20;   // Synthetic temperature
static int high_flag = 0;
static int low_flag = 0;

/*******************************************************************************
 *                            Function forward declarations
 ******************************************************************************/

int user_open(struct inode *inode, struct file *filp);
int user_release(struct inode *inode, struct file *filp);
ssize_t user_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos);
static __poll_t high_temp_poll(struct file *file, poll_table *wait);
ssize_t high_temp_read(struct file *f, char __user *buf, size_t len, loff_t *off);
static __poll_t low_temp_poll(struct file *file, poll_table *wait);
ssize_t low_temp_read(struct file *f, char __user *buf, size_t len, loff_t *off);
static int sensor_read(void *data);
static int synthetic_data_event_thread(void *data);
static int bme280_sensor_probe(struct i2c_client *client, const struct i2c_device_id *id);
static void bme280_sensor_remove(struct i2c_client *client);

/*******************************************************************************
 *                        File operations / device nodes
 ******************************************************************************/
struct file_operations bme280_sensor_fops = {
    .owner = THIS_MODULE,
    .read = user_read,
    .open = user_open,
    .release = user_release,
};

static const struct file_operations fops_high = {
    .owner = THIS_MODULE,
    .read = high_temp_read,
    .poll = high_temp_poll,
};

static struct miscdevice dev_high = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "high_temperature_event",
    .fops = &fops_high,
};

static const struct file_operations fops_low = {
    .owner = THIS_MODULE,
    .read = low_temp_read,
    .poll = low_temp_poll,
};

static struct miscdevice dev_low = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "low_temperature_event",
    .fops = &fops_low,
};

/*******************************************************************************
 *                         Main sensor device operations
 ******************************************************************************/
/**
 * @brief This function is called when a user-space application performs the open()
 * system call on the sensor device file
 *
 * @param inode Pointer to the inode structure representing the device file.
 * @param filp  Pointer to the file structure representing the open file descriptor.
 */
int user_open(struct inode *inode, struct file *filp)
{
    struct miscdevice *mdev = filp->private_data;
    struct bme280_node *node = container_of(mdev, struct bme280_node, miscdev);
    filp->private_data = node;
    return 0;
}

/**
 * @brief Sensor device release/close handler.
 *
 * @param inode Device inode structure
 * @param filp  File structure for the device being closed
 */
int user_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    filp->private_data = NULL;
    return 0;
}

/**
 * @brief  syscall handler for read()
 *
 * @param filp  Pointer to the file structure representing the open file descriptor.
 * @param buf   User-space buffer to receive the sensor data.
 * @param count Number of bytes requested to read.
 * @param f_pos Pointer to the current file position.
 */
ssize_t user_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    ssize_t retval = 0;
    ssize_t num_copy_bytes = 0;
    unsigned long copy_status = 0;
    int32_t val = 0x24;
    BME280_Data local_user_data;
    struct bme280_node *node = filp->private_data;
    struct bme280_hw *hw = node->hw;

    // error checking
    if (filp == NULL || buf == NULL || f_pos == NULL)
    {
        PDEBUG("ERROR: input argument error");
        return -EINVAL;
    }

    mutex_lock(&hw->lock);
    memcpy(&local_user_data, &sensor_data, sizeof(BME280_Data));
    mutex_unlock(&hw->lock);

    switch (node->type)
    {
    case BME_TEMP:
        val = local_user_data.temperature;
        break;
    case BME_HUMIDITY:
        val = local_user_data.humidity;
        break;
    case BME_PRESSURE:
        val = local_user_data.pressure;
        break;
    default:
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

/*******************************************************************************
 *                         High temperature event device
 *******************************************************************************/
/**
 * @brief .poll callback
 */
static __poll_t high_temp_poll(struct file *file, poll_table *wait)
{
    __poll_t mask = 0;

    poll_wait(file, &wq_high, wait);

    if (READ_ONCE(high_flag))
        mask |= POLLIN | POLLRDNORM;

    return mask;
}

/**
 * @brief syscall handler for read() for /dev/high_temperature_event
 */
ssize_t high_temp_read(struct file *f, char __user *buf, size_t len, loff_t *off)
{
    int temp;
    int ret;

    if (len < sizeof(temp))
        return -EINVAL;

    // Non-blocking read support
    if ((f->f_flags & O_NONBLOCK) && !READ_ONCE(high_flag))
        return -EAGAIN;

    // Blocking read: sleep until an event is available
    ret = wait_event_interruptible(wq_high, READ_ONCE(high_flag) != 0);
    if (ret)
        return ret; //-ERESTARTSYS if interrupted by signal

    // Capture the event data before clearing the flag
    temp = READ_ONCE(current_temp);

    if (copy_to_user(buf, &temp, sizeof(temp)))
        return -EFAULT;

    // Consume the event so poll() won't keep firing
    WRITE_ONCE(high_flag, 0);

    return sizeof(temp);

#if 0
    wait_event_interruptible(wq_high, high_flag != 0);

    high_flag = 0;

    return 0;
#endif
}

/*******************************************************************************
 *                         Low temperature event device
 *******************************************************************************/
/**
 * @brief .poll callback
 */
static __poll_t low_temp_poll(struct file *file, poll_table *wait)
{
    __poll_t mask = 0;

    poll_wait(file, &wq_low, wait);

    if (READ_ONCE(low_flag))
        mask |= POLLIN | POLLRDNORM;

    return mask;
}

/**
 * @brief syscall handler for read() for /dev/low_temperature_event
 */
ssize_t low_temp_read(struct file *f, char __user *buf, size_t len, loff_t *off)
{
    int temp;
    int ret;

    if (len < sizeof(temp))
        return -EINVAL;

    // Non-blocking support
    if ((f->f_flags & O_NONBLOCK) && !READ_ONCE(low_flag))
        return -EAGAIN;

    // Blocking wait
    ret = wait_event_interruptible(wq_low, READ_ONCE(low_flag) != 0);
    if (ret)
        return ret;

    // Capture event data
    temp = READ_ONCE(current_temp);

    if (copy_to_user(buf, &temp, sizeof(temp)))
        return -EFAULT;

    // Consume the event
    WRITE_ONCE(low_flag, 0);

    return sizeof(temp);
#if 0
    wait_event_interruptible(wq_low, low_flag != 0);
    low_flag = 0;

    return 0;
#endif
}

/*******************************************************************************
 *                                Threads
 ******************************************************************************/

/**
 * @brief kthread entry function
 */
static int sensor_read(void *data)
{
    struct bme280_hw *hw = &g_hw;
    BME280_Data local_data;
    PDEBUG("sensor_read: running\n");
    while (!kthread_should_stop())
    {
        bme280_read_all(&local_data); // reads from hw
        mutex_lock(&hw->lock);
        memcpy(&sensor_data, &local_data, sizeof(BME280_Data)); // copies to sensor_data(shared memory between kthread and user_read)
        mutex_unlock(&hw->lock);
        msleep(100);
    }
    PDEBUG("sensor_read: stopping\n");

    return 0;
}

/**
 * @brief Thread generating high/low temperature events
 */
static int synthetic_data_event_thread(void *data)
{
    while (!kthread_should_stop())
    {
        ssleep(1); // 1 sec

        // current_temp = get_random_u32() % (HIGH_TEMP_THRESHOLD + 5);
        current_temp++;

        if (current_temp >= HIGH_TEMP_THRESHOLD)
        {
            WRITE_ONCE(high_flag, 1);
            wake_up_interruptible(&wq_high);
            PDEBUG("DEBUG: HIGH TEMP EVENT: %d\n", READ_ONCE(current_temp));
            if (current_temp > 35)
            {
                current_temp = 0;
            }
        }

        if (current_temp <= LOW_TEMP_THRESHOLD)
        {
            WRITE_ONCE(low_flag, 1);
            wake_up_interruptible(&wq_low);
            PDEBUG("DEBUG: LOW TEMP EVENT: %d\n", READ_ONCE(current_temp));
        }
    }
    return 0;
}

/*******************************************************************************
 *                              Probe / remove
 ******************************************************************************/
/**
 * @brief I2C probe function for the BME280 sensor. This function is called when
 * the I2C core detects a device matching the driver's
 * ID table. It verifies the chip ID by reading from the BME280's CHIP_ID register,
 * initializes the sensor, and registers the device.
 *
 * References:
 * 1. https://developerhelp.microchip.com/xwiki/bin/view/software-tools/linux/apps-i2c/
 * 2. https://chat.deepseek.com/share/w18dko9noz63hm2vq9
 *
 * @param client Pointer to the I2C client structure representing the detected device.
 * @param id     Pointer to the I2C device ID entry that matched the detected device.
 */
static int bme280_sensor_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    int status;
    uint8_t who;
    int i;
    const char *names[DEVICE_COUNT];

    client_singleton = client;

    who = i2c_smbus_read_byte_data(client, BME280_REG_CHIP_ID);
    if (who != BME280_CHIP_ID)
    {
        dev_err(&client->dev, "unknown CHIP ID: 0x%02x\n", who);
        return -ENODEV;
    }

    PDEBUG("DEBUG: bme280 sensor found");

    bme280_init(client_singleton);

    mutex_init(&g_hw.lock);

    // Events
    init_waitqueue_head(&wq_high);
    status = misc_register(&dev_high);
    if (status)
    {
        PDEBUG("ERROR: Failed to register high temperature dev node\n");
        return -ENOMEM;
    }

    init_waitqueue_head(&wq_low);
    status = misc_register(&dev_low);
    if (status)
    {
        PDEBUG("ERROR: Failed to register low temperature dev node\n");
        return -ENOMEM;
    }

    sensor_read_kthread = kthread_run(sensor_read, NULL, "sensor_read_kthread");
    if (IS_ERR(sensor_read_kthread))
    {
        PDEBUG("ERROR: Failed to create kthread\n");
        return -ENOMEM;
    }

    synthetic_data_event_kthread = kthread_run(synthetic_data_event_thread, NULL, "synthetic_data_event_kthread");
    if (IS_ERR(synthetic_data_event_kthread))
    {
        PDEBUG("ERROR: Failed to create synthetic_data_event_kthread\n");
        return -ENOMEM;
    }

    names[0] = "bme280_temp";
    names[1] = "bme280_humidity";
    names[2] = "bme280_pressure";

    // register device
    for (i = 0; i < DEVICE_COUNT; i++)
    {
        nodes[i].type = i;
        nodes[i].hw = &g_hw;

        nodes[i].miscdev.minor = MISC_DYNAMIC_MINOR; // device node minor number
        nodes[i].miscdev.name = names[i];            // device node name
        nodes[i].miscdev.fops = &bme280_sensor_fops; // file ops for device node

        status = misc_register(&nodes[i].miscdev);
        if (status)
        {
            PDEBUG("ERROR: Failed to register %s\n", names[i]);
            goto fail;
        }
    }

    PDEBUG("DEBUG: bme280 sensor registered");
    dev_info(&client->dev, "bme280 sensor driver loaded\n");

    return 0;

fail:
    while (--i >= 0)
    {
        misc_deregister(&nodes[i].miscdev);
    }
    return status;
}

/**
 * @brief I2C remove handler for BME280 sensor cleanup
 *
 * @param client I2C client structure for the device being removed
 */
static void bme280_sensor_remove(struct i2c_client *client)
{
    int i;
    for (i = 0; i < DEVICE_COUNT; i++)
    {
        misc_deregister(&nodes[i].miscdev);
    }
    if (sensor_read_kthread)
    {
        kthread_stop(sensor_read_kthread);
    }
    PDEBUG("DEBUG: bme280 sensor removed");
    dev_info(&client->dev, "bme280 sensor unloaded\n");

    // return 0;
}

/*******************************************************************************
 *                               I2C boilerplate
 ******************************************************************************/

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

MODULE_AUTHOR("Venetia Furtado");
MODULE_LICENSE("Dual BSD/GPL");
