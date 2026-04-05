/**
 * @file bme280_sensor.c
 * @brief
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

// Expected chip ID
#define BME280_CHIP_ID 0x60

MODULE_AUTHOR("Venetia Furtado");
MODULE_LICENSE("Dual BSD/GPL");

int sensor_major = 0; // use dynamic major
int sensor_minor = 0;

static struct i2c_client *client_singleton;

struct sensor_dev sensor_device;

// Global calibration data
BME280_CalibData calib;
int32_t t_fine; // Used for temperature compensation

/**
 * @brief Writes a single byte to a BME280 register.
 *
 * Sends a command to write a value to the given register address
 * using either SPI or I2C depending on the build configuration.
 *
 * @param reg   Register address to write to.
 * @param value Byte value to write into the register.
 */
void BME280_WriteReg(const uint8_t reg, const uint8_t value)
{
    i2c_smbus_write_byte_data(client_singleton, reg, value);
}

/**
 * @brief Reads a single byte from a BME280 register.
 *
 * Retrieves a value from the specified register using either
 * SPI or I2C depending on the build configuration.
 *
 * @param reg Register address to read from.
 * @return uint8_t Value read from the register.
 */
uint8_t BME280_ReadReg(const uint8_t reg)
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
void BME280_ReadRegs(uint8_t reg, uint8_t *buffer, uint8_t len)
{
    uint8_t i = 0;
    for (i = 0; i < len; i++)
    {
        buffer[i] = BME280_ReadReg(reg++);
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
uint8_t BME280_Init(void)
{
    uint8_t chip_id;
    uint8_t calib_data[32];
    int i = 0;

    // Check chip ID
    chip_id = BME280_ReadReg(BME280_REG_CHIP_ID);
    if (chip_id != BME280_CHIP_ID)
    {
        return 0; // Wrong chip ID
    }

    // Soft reset
    BME280_WriteReg(BME280_REG_RESET, 0xB6);

    // Wait for reset to complete
    for (i = 0; i < 100000; i++)
        ;

    // Read calibration data (Temperature & Pressure)
    BME280_ReadRegs(BME280_REG_CALIB_00, calib_data, 26);

    calib.dig_T1 = (calib_data[1] << 8) | calib_data[0];
    calib.dig_T2 = (calib_data[3] << 8) | calib_data[2];
    calib.dig_T3 = (calib_data[5] << 8) | calib_data[4];

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
    BME280_ReadRegs(BME280_REG_CALIB_26, calib_data, 7);

    calib.dig_H2 = (calib_data[1] << 8) | calib_data[0];
    calib.dig_H3 = calib_data[2];
    calib.dig_H4 = (calib_data[3] << 4) | (calib_data[4] & 0x0F);
    calib.dig_H5 = (calib_data[5] << 4) | (calib_data[4] >> 4);
    calib.dig_H6 = calib_data[6];

    // Configure sensor
    // Humidity oversampling x1
    BME280_WriteReg(BME280_REG_CTRL_HUM, 0x01);

    // Temperature oversampling x1, Pressure oversampling x1, Normal mode
    BME280_WriteReg(BME280_REG_CTRL_MEAS, 0x27);

    // Standby time 0.5ms, filter off
    BME280_WriteReg(BME280_REG_CONFIG, 0x00);

    return 1; // Success
}

// ========== BME280 Compensation Functions ==========
/**
 * @brief Compensates raw temperature ADC value.
 *
 * @param adc_T Raw temperature ADC value (20-bit).
 * @return int32_t Temperature in hundredths of a degree Celsius (°C × 100).
 */
int32_t BME280_CompensateTemp(int32_t adc_T)
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
uint32_t BME280_CompensatePressure(int32_t adc_P)
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
uint32_t BME280_CompensateHumidity(int32_t adc_H)
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
void BME280_ReadAll(BME280_Data *data)
{
    uint8_t raw_data[8];
    int32_t adc_T, adc_P, adc_H;

    // Read all sensor data (0xF7 to 0xFE)
    BME280_ReadRegs(BME280_REG_PRESS_MSB, raw_data, 8);

    // Parse raw data
    adc_P = ((uint32_t)raw_data[0] << 12) | ((uint32_t)raw_data[1] << 4) |
            ((uint32_t)raw_data[2] >> 4);
    adc_T = ((uint32_t)raw_data[3] << 12) | ((uint32_t)raw_data[4] << 4) |
            ((uint32_t)raw_data[5] >> 4);
    adc_H = ((uint32_t)raw_data[6] << 8) | (uint32_t)raw_data[7];

    // Compensate and convert to float
    data->temperature = BME280_CompensateTemp(adc_T) / 100;    // °C
    data->pressure = BME280_CompensatePressure(adc_P) / 25600; // hPa
    data->humidity = BME280_CompensateHumidity(adc_H) / 1024;  // %
}

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
    BME280_Data data;

    // error checking
    if (filp == NULL || buf == NULL || f_pos == NULL)
    {
        PDEBUG("ERROR: input argument error");
        return -EINVAL;
    }

    BME280_ReadAll(&data);
    val = data.temperature;

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

    client_singleton = client;

    who = i2c_smbus_read_byte_data(client, BME280_REG_CHIP_ID);
    if (who != BME280_CHIP_ID)
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

    BME280_Init();

    return 0;

    /*
    TODO: deregister if register and we have error
err_misc:
    misc_deregister(&bme280_sensor_device);
    return ret;
    */
}

static void bme280_sensor_remove(struct i2c_client *client)
{
    misc_deregister(&bme280_sensor_device);
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

// module_init(sensor_init_module);
// module_exit(sensor_cleanup_module);