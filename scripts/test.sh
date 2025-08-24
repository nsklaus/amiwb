#!/bin/bash
# test.sh - Set all fans to constant 2% speed
# good riddance asusctl and supergfxctl, escape from rust hell

echo "Setting all fans to minimum speed..."

# Set thermal policy to SILENT (2) - quietest mode
echo 2 | sudo tee /sys/devices/platform/asus-nb-wmi/throttle_thermal_policy > /dev/null

# Set all fans to auto mode (2) - accepted mode
for fan in 1 2 3; do
    echo 2 | sudo tee /sys/devices/platform/asus-nb-wmi/hwmon/hwmon10/pwm${fan}_enable > /dev/null
done

# Set ALL points to start at 0°C with MINIMUM speed
for fan in 1 2 3; do
    for point in {1..8}; do
        # Start from 0°C to ensure it's always active
        echo $((point * 5)) | sudo tee /sys/devices/platform/asus-nb-wmi/hwmon/hwmon10/pwm${fan}_auto_point${point}_temp > /dev/null
        echo 10 | sudo tee /sys/devices/platform/asus-nb-wmi/hwmon/hwmon10/pwm${fan}_auto_point${point}_pwm > /dev/null
    done
done

echo "Done. All fans set to minimum with silent mode."
echo "Current status:"
sensors | grep -E "Tctl|cpu_fan|gpu_fan|mid_fan"