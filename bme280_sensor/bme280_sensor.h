/*
 * bme280_sensor.h
 *
 *  Created on: Oct 23, 2019
 *  Author: Dan Walkes
 *
 * References:
 * 1. https://github.com/sparkfun/SparkFun_BME280_Arduino_Library/tree/870c17da1f4c76561e14b8ffcc7cdffd63136e10/src
 * 2. https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bme280-ds002.pdf
 * 3. https://github.com/cu-ecen-aeld/assignments-3-and-later-VenetiaFurtado
 */

#ifndef SENSOR_DRIVER_H_
#define SENSOR_DRIVER_H_

#define SENSOR_DEBUG 1 // Remove comment on this line to enable debug

#undef PDEBUG /* undef it, just in case */
#ifdef SENSOR_DEBUG
#ifdef __KERNEL__
/* This one if debugging is on, and kernel space */
#define PDEBUG(fmt, args...) printk(KERN_DEBUG "sensor: " fmt, ##args)
#else
/* This one for user space */
#define PDEBUG(fmt, args...) fprintf(stderr, fmt, ##args)
#endif
#else
#define PDEBUG(fmt, args...) /* not debugging: nothing */
#endif

struct sensor_dev
{
    struct cdev cdev; /* Char device structure      */
};

// BME280 measurement data
typedef struct
{
    int32_t temperature; // °C
    int32_t pressure;    // hPa
    int32_t humidity;    // %
} BME280_Data;
#endif /* SENSOR_DRIVER_H_ */