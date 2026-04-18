
#include <linux/printk.h>
#include <linux/i2c.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/types.h>
#include "bme280_driver.h"
#include "i2c.h"

MODULE_AUTHOR("Venetia Furtado");
MODULE_LICENSE("Dual BSD/GPL");

// BME280 calibration parameters
typedef struct
{
    uint16_t dig_T1;
    int16_t dig_T2;
    int16_t dig_T3;

    uint16_t dig_P1;
    int16_t dig_P2;
    int16_t dig_P3;
    int16_t dig_P4;
    int16_t dig_P5;
    int16_t dig_P6;
    int16_t dig_P7;
    int16_t dig_P8;
    int16_t dig_P9;

    uint8_t dig_H1;
    int16_t dig_H2;
    uint8_t dig_H3;
    int16_t dig_H4;
    int16_t dig_H5;
    int8_t dig_H6;
} BME280_CalibData;

BME280_CalibData calib; // Global calibration data
int32_t t_fine;         // Used for temperature compensation
static struct i2c_client *client_singleton;

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
uint8_t bme280_init(struct i2c_client *client_ptr)
{
    uint8_t chip_id;
    uint8_t calib_data[32];
    volatile int i = 0;
    volatile int j = 0;
    client_singleton = client_ptr;

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
    // PDEBUG("ADC-T=%d;  Read Temperature = %d\n", adc_T, data->temperature);
    data->pressure = bme280_compensate_pressure(adc_P); // hPa
    // PDEBUG("ADC-P=%d;  Read Pressure = %d\n", adc_P, data->pressure);
    data->humidity = bme280_compensate_humidity(adc_H); // %
    // PDEBUG("ADC-H=%d;  Read Humidity = %d\n", adc_P, data->humidity);
}
