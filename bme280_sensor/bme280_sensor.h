/*
 * bme280_sensor.h
 *
 *  Created on: Oct 23, 2019
 *      Author: Dan Walkes
 */

#ifndef SENSOR_DRIVER_H_
#define SENSOR_DRIVER_H_


#define SENSOR_DEBUG 1 // Remove comment on this line to enable debug

#undef PDEBUG /* undef it, just in case */
#ifdef AESD_DEBUG
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
#endif /* SENSOR_DRIVER_H_ */