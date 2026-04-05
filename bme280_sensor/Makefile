KDIR   ?= $(LINUX_DIR)
PWD    := $(shell pwd)

obj-m  := bme280_sensor.o

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean