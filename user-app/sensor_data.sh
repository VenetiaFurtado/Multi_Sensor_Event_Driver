#!/bin/sh

while true; do
    temp=$(hexdump -v -e '"%d\n"' -n 4 /dev/bme280_temp)
    echo "$(awk "BEGIN {printf \"Sensor data: Temperature reading = %.2f °C\", $temp/100}")"

    humidity=$(hexdump -v -e '"%d\n"' -n 4 /dev/bme280_humidity)
    echo "$(awk "BEGIN {printf \"Sensor data: Humidity reading = %.2f %%\", $humidity/1024}")"

    pressure=$(hexdump -v -e '"%d\n"' -n 4 /dev/bme280_pressure)
    echo "$(awk "BEGIN {printf \"Sensor data: Pressure reading = %.2f hPa\", $pressure/25600}")"

    echo "-----------------------------"
    sleep 2   # adjust delay (seconds) as needed
done