/**
 * @file bme280_sensor.c
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
#include "bme280_sensor.h"

#define BME280_SENSOR_DEVICE "bme280_sensor"
// BME280 Register Addresses
#define BME280_REG_CHIP_ID 0xD0
#define BME280_REG_RESET 0xE0
#define BME280_REG_CTRL_HUM 0xF2
#define BME280_REG_STATUS 0xF3
#define BME280_REG_CTRL_MEAS 0xF4
#define BME280_REG_CONFIG 0xF5
#define BME280_REG_PRESS_MSB 0xF7
#define BME280_REG_TEMP_MSB 0xFA
#define BME280_REG_HUM_MSB 0xFD
// Calibration data registers
#define BME280_REG_CALIB_00 0x88
#define BME280_REG_CALIB_26 0xE1
#define BME280_CHIP_ID 0x60 // Expected chip ID
#define DEVICE_COUNT 3

#define HIGH_TEMP_THRESHOLD 30

MODULE_AUTHOR("Venetia Furtado");
MODULE_LICENSE("Dual BSD/GPL");

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

static struct bme280_hw g_hw;                  // singleton hardware instance
static struct bme280_node nodes[DEVICE_COUNT]; // array of device nodes
static struct i2c_client *client_singleton;
static struct task_struct *sensor_read_kthread; // reads bme280 asynchronously
static struct task_struct *synthetic_data_event_kthread;
static wait_queue_head_t wq_high;

// static struct sensor_dev sensor_device;
static BME280_Data sensor_data; // kthread writes into (shared memory between kthread and user_read)

BME280_CalibData calib; // Global calibration data
int32_t t_fine;         // Used for temperature compensation

static int current_temp = 20; // Synthetic temperature
static int high_flag = 0;

/**
 * @brief Writes a single byte to a BME280 register.
 *
 * Sends a command to write a value to the given register address
 * using I2C.
 *
 * @param reg   Register address to write to.
 * @param value Byte value to write into the register.
 */
void bme280_write_reg(const uint8_t reg, const uint8_t value)
{
    i2c_smbus_write_byte_data(client_singleton, reg, value);
}

/**
 * @brief Reads a single byte from a BME280 register.
 *
 * Retrieves a value from the specified register using I2C.
 *
 * @param reg Register address to read from.
 * @return uint8_t Value read from the register.
 */
uint8_t bme280_read_reg(const uint8_t reg)
{
    uint8_t read_val;
    read_val = i2c_smbus_read_byte_data(client_singleton, reg);
    return read_val;
}

/**
 * @brief Reads multiple consecutive BME280 registers.
 *
 * Repeatedly reads from the starting register address and stores
 * each value into the provided buffer.
 *
 * @param reg     Starting register address.
 * @param buffer  Pointer to buffer to store read values.
 * @param len     Number of bytes to read.
 */
void bme280_read_regs(uint8_t reg, uint8_t *buffer, uint8_t len)
{
    uint8_t i = 0;
    for (i = 0; i < len; i++)
    {
        buffer[i] = bme280_read_reg(reg++);
    }
}

// ========== BME280 Initialization ==========
/**
 * @brief Initializes the BME280 sensor.
 *
 * Performs a soft reset, verifies the chip ID, loads factory
 * calibration coefficients, and configures oversampling settings for
 * temperature, pressure, and humidity. Also sets the operating mode.
 *
 * @return uint8_t Returns 1 on successful initialization, 0 if the
 *                 chip ID does not match the expected value.
 */
uint8_t bme280_init(void)
{
    uint8_t chip_id;
    uint8_t calib_data[32];
    volatile int i = 0;
    volatile int j = 0;

    // Check chip ID
    chip_id = bme280_read_reg(BME280_REG_CHIP_ID);
    if (chip_id != BME280_CHIP_ID)
    {
        PDEBUG("chip id incorrect %x", chip_id);
        return 0; // Wrong chip ID
    }
    PDEBUG("Correct BME280 CHIP ID = %x", chip_id);

    // Soft reset
    bme280_write_reg(BME280_REG_RESET, 0xB6);

    // Wait for reset to complete
    for (i = 0; i < 100000; i++)
    {
        for (j = 0; i < 100000; i++)
            ;
    }

    // Read calibration data (Temperature & Pressure)
    bme280_read_regs(BME280_REG_CALIB_00, calib_data, 26);

    calib.dig_T1 = (calib_data[1] << 8) | calib_data[0];
    calib.dig_T2 = (calib_data[3] << 8) | calib_data[2];
    calib.dig_T3 = (calib_data[5] << 8) | calib_data[4];

    PDEBUG("dig_T1=%d,T2=%d,T3=%d", calib.dig_T1, calib.dig_T2, calib.dig_T3);

    calib.dig_P1 = (calib_data[7] << 8) | calib_data[6];
    calib.dig_P2 = (calib_data[9] << 8) | calib_data[8];
    calib.dig_P3 = (calib_data[11] << 8) | calib_data[10];
    calib.dig_P4 = (calib_data[13] << 8) | calib_data[12];
    calib.dig_P5 = (calib_data[15] << 8) | calib_data[14];
    calib.dig_P6 = (calib_data[17] << 8) | calib_data[16];
    calib.dig_P7 = (calib_data[19] << 8) | calib_data[18];
    calib.dig_P8 = (calib_data[21] << 8) | calib_data[20];
    calib.dig_P9 = (calib_data[23] << 8) | calib_data[22];

    calib.dig_H1 = calib_data[25];

    // Read calibration data (Humidity)
    bme280_read_regs(BME280_REG_CALIB_26, calib_data, 7);

    calib.dig_H2 = (calib_data[1] << 8) | calib_data[0];
    calib.dig_H3 = calib_data[2];
    calib.dig_H4 = (calib_data[3] << 4) | (calib_data[4] & 0x0F);
    calib.dig_H5 = (calib_data[5] << 4) | (calib_data[4] >> 4);
    calib.dig_H6 = calib_data[6];

    // Configure sensor
    // Humidity oversampling x1
    bme280_write_reg(BME280_REG_CTRL_HUM, 0x01);

    // Temperature oversampling x1, Pressure oversampling x1, Normal mode
    bme280_write_reg(BME280_REG_CTRL_MEAS, 0x27);

    // Standby time 0.5ms, filter off
    bme280_write_reg(BME280_REG_CONFIG, 0x00);

    return 1; // Success
}

// ========== BME280 Compensation Functions ==========
/**
 * @brief Compensates raw temperature ADC value.
 *
 * @param adc_T Raw temperature ADC value (20-bit).
 * @return int32_t Temperature in hundredths of a degree Celsius (°C × 100).
 */
int32_t bme280_compensate_temp(int32_t adc_T)
{
    int32_t var1, var2, T;

    var1 = ((((adc_T >> 3) - ((int32_t)calib.dig_T1 << 1))) *
            ((int32_t)calib.dig_T2)) >>
           11;
    var2 = (((((adc_T >> 4) - ((int32_t)calib.dig_T1)) *
              ((adc_T >> 4) - ((int32_t)calib.dig_T1))) >>
             12) *
            ((int32_t)calib.dig_T3)) >>
           14;

    t_fine = var1 + var2;
    T = (t_fine * 5 + 128) >> 8;

    return T; // Temperature in 0.01°C
}

/**
 * @brief Compensates raw pressure ADC value.
 *
 * @param adc_P Raw pressure ADC value (20-bit).
 * @return uint32_t Compensated pressure in Pa/256.
 */
uint32_t bme280_compensate_pressure(int32_t adc_P)
{
    int64_t var1, var2, p;

    var1 = ((int64_t)t_fine) - 128000;
    var2 = var1 * var1 * (int64_t)calib.dig_P6;
    var2 = var2 + ((var1 * (int64_t)calib.dig_P5) << 17);
    var2 = var2 + (((int64_t)calib.dig_P4) << 35);
    var1 = ((var1 * var1 * (int64_t)calib.dig_P3) >> 8) +
           ((var1 * (int64_t)calib.dig_P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)calib.dig_P1) >> 33;

    if (var1 == 0)
    {
        return 0; // Avoid division by zero
    }

    p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)calib.dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)calib.dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)calib.dig_P7) << 4);

    return (uint32_t)p; // Pressure in Pa/256
}

/**
 * @brief Compensates raw humidity ADC value.
 *
 * @param adc_H Raw humidity ADC value (16-bit).
 * @return uint32_t Relative humidity in %RH × 1024.
 */
uint32_t bme280_compensate_humidity(int32_t adc_H)
{
    int32_t v_x1_u32r;

    v_x1_u32r = (t_fine - ((int32_t)76800));
    v_x1_u32r = (((((adc_H << 14) - (((int32_t)calib.dig_H4) << 20) -
                    (((int32_t)calib.dig_H5) * v_x1_u32r)) +
                   ((int32_t)16384)) >>
                  15) *
                 (((((((v_x1_u32r * ((int32_t)calib.dig_H6)) >> 10) *
                      (((v_x1_u32r * ((int32_t)calib.dig_H3)) >> 11) + ((int32_t)32768))) >>
                     10) +
                    ((int32_t)2097152)) *
                       ((int32_t)calib.dig_H2) +
                   8192) >>
                  14));

    v_x1_u32r = (v_x1_u32r - (((((v_x1_u32r >> 15) * (v_x1_u32r >> 15)) >> 7) *
                               ((int32_t)calib.dig_H1)) >>
                              4));

    v_x1_u32r = (v_x1_u32r < 0 ? 0 : v_x1_u32r);
    v_x1_u32r = (v_x1_u32r > 419430400 ? 419430400 : v_x1_u32r);

    return (uint32_t)(v_x1_u32r >> 12); // Humidity in %/1024
}

// ========== BME280 Read All Measurements ==========
/**
 * @brief Reads all environmental measurements from the BME280.
 *
 * Reads temperature, pressure, and humidity raw ADC values from the
 * sensor, applies compensation formulas, and stores the results in
 * floating-point format within the provided data structure.
 *
 * @param data Pointer to a BME280_Data structure where the
 *             compensated values will be stored. Fields returned:
 *             - temperature (°C)
 *             - pressure (hPa)
 *             - humidity (%RH)
 */
void bme280_read_all(BME280_Data *data)
{
    uint8_t raw_data[8];
    int32_t adc_T, adc_P, adc_H;

    // Read all sensor data (0xF7 to 0xFE)
    bme280_read_regs(BME280_REG_PRESS_MSB, raw_data, 8);

    // Parse raw data
    adc_P = ((uint32_t)raw_data[0] << 12) | ((uint32_t)raw_data[1] << 4) |
            ((uint32_t)raw_data[2] >> 4);
    adc_T = ((uint32_t)raw_data[3] << 12) | ((uint32_t)raw_data[4] << 4) |
            ((uint32_t)raw_data[5] >> 4);
    adc_H = ((uint32_t)raw_data[6] << 8) | (uint32_t)raw_data[7];

    // Compensate and convert to float
    data->temperature = bme280_compensate_temp(adc_T); // °C
    PDEBUG("ADC-T=%d;  Read Temperature = %d\n", adc_T, data->temperature);
    data->pressure = bme280_compensate_pressure(adc_P); // hPa
    PDEBUG("ADC-P=%d;  Read Pressure = %d\n", adc_P, data->pressure);
    data->humidity = bme280_compensate_humidity(adc_H); // %
    PDEBUG("ADC-H=%d;  Read Humidity = %d\n", adc_P, data->humidity);
}

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

#if 0
    struct sensor_dev *dev;
    PDEBUG("open");
    dev = container_of(inode->i_cdev, struct sensor_dev, cdev);
    filp->private_data = dev;
    return 0;
#endif
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
 * @brief  Reads temperature data from the sensor and copies it to user space.
 *
 * @param filp  Pointer to the file structure representing the open file descriptor.
 * @param buf   User-space buffer to receive the sensor data.
 * @param count Number of bytes requested to read.
 * @param f_pos Pointer to the current file position.
 */
ssize_t user_read(struct file *filp, char __user *buf, size_t count,
                  loff_t *f_pos)
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

struct file_operations bme280_sensor_fops = {
    .owner = THIS_MODULE,
    .read = user_read,
    .open = user_open,
    .release = user_release,
};

/* -------- HIGH TEMP DEVICE -------- */

ssize_t high_temp_read(struct file *f, char __user *buf,
                       size_t len, loff_t *off)
{
    wait_event_interruptible(wq_high, high_flag != 0);

    high_flag = 0;

    return 0;
}

static const struct file_operations fops_high = {
    .owner = THIS_MODULE,
    .read = high_temp_read,
};

static struct miscdevice dev_high = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "high_temperature_event",
    .fops = &fops_high,
};

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
        memcpy(&sensor_data, &local_data, sizeof(BME280_Data)); // copies to sensory_data(shared memory between kthread and user_read)
        mutex_unlock(&hw->lock);
        msleep(100);
    }
    PDEBUG("sensor_read: stopping\n");

    return 0;
}

/* Thread simulating temperature */
static int synthetic_data_event_thread(void *data)
{
    while (!kthread_should_stop())
    {
        ssleep(1);

        current_temp = get_random_u32_max(HIGH_TEMP_THRESHOLD + 5);

        if (current_temp >= HIGH_TEMP_THRESHOLD)
        {
            high_flag = 1;
            wake_up_interruptible(&wq_high);
            PDEBUG("DEBUG: HIGH TEMP EVENT: %d\n", current_temp);
            current_temp = 0;
        }
    }
    return 0;
}

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
static int bme280_sensor_probe(struct i2c_client *client,
                               const struct i2c_device_id *id)
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

    bme280_init();

    mutex_init(&g_hw.lock);

    /*Events*/

    init_waitqueue_head(&wq_high);
    status = misc_register(&dev_high);
    if (status)
    {
        PDEBUG("ERROR: Failed to register high temperature dev node\n");
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
