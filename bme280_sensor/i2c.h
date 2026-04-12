#ifndef I2C_H_
#define I2C_H_

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

uint8_t bme280_init(void);
void bme280_read_all(BME280_Data *data);

#endif