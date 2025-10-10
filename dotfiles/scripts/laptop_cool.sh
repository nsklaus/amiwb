#!/bin/bash
# Control all laptop power usage
# sets everything to low or off, 
# to avoid noise and heat.
#
# ******************************
# ** specific to asus rog g14 **
# ******************************

  HWMON_PATH="/sys/devices/platform/asus-nb-wmi/hwmon/hwmon10"

  # Function to check all fan curves
  check_curves() {
      for fan in 1 2 3; do
          case $fan in
              1) name="CPU FAN" ;;
              2) name="GPU FAN" ;;
              3) name="MID FAN" ;;
          esac

          echo "=== $name PWM$fan ==="
          for i in {1..8}; do
              TEMP=$(cat $HWMON_PATH/pwm${fan}_auto_point${i}_temp)
              PWM=$(cat $HWMON_PATH/pwm${fan}_auto_point${i}_pwm)
              echo "Point $i: ${TEMP}°C = PWM:${PWM}/255 ($(( PWM * 100 / 255 ))%)"
          done
          echo
      done
  }

  # Function to set all fan curves
  set_curves() {
      # Set all fans to auto mode (2) - allows proper fan control
      echo 2 | sudo tee /sys/devices/platform/asus-nb-wmi/hwmon/hwmon10/pwm1_enable > /dev/null
      echo 2 | sudo tee /sys/devices/platform/asus-nb-wmi/hwmon/hwmon10/pwm2_enable > /dev/null
      echo 2 | sudo tee /sys/devices/platform/asus-nb-wmi/hwmon/hwmon10/pwm3_enable > /dev/null
      
      # Set thermal policy to silent (2) - quietest mode
      echo 2 | sudo tee /sys/devices/platform/asus-nb-wmi/throttle_thermal_policy > /dev/null

      # FANLESS MODE: All fans stay at 0% until critical temps
      # CPU fan curve - completely off until 95°C (EC will takeover if it needs to anyway)
      cpu_temps=(50 60 70 80 85 90 95 100)
      cpu_pwms=(0 0 0 0 0 0 0 0)

      # GPU fan curve - completely off
      gpu_temps=(50 60 70 80 85 90 95 100)
      gpu_pwms=(0 0 0 0 0 0 0 0)

      # MID fan curve - completely off
      mid_temps=(50 60 70 80 85 90 95 100)
      mid_pwms=(0 0 0 0 0 0 0 0)

      # Set CPU fan
      for i in {1..8}; do
          echo ${cpu_temps[$((i-1))]} | sudo tee $HWMON_PATH/pwm1_auto_point${i}_temp > /dev/null
          echo ${cpu_pwms[$((i-1))]} | sudo tee $HWMON_PATH/pwm1_auto_point${i}_pwm > /dev/null
      done

      # Set GPU fan
      for i in {1..8}; do
          echo ${gpu_temps[$((i-1))]} | sudo tee $HWMON_PATH/pwm2_auto_point${i}_temp > /dev/null
          echo ${gpu_pwms[$((i-1))]} | sudo tee $HWMON_PATH/pwm2_auto_point${i}_pwm > /dev/null
      done

      # Set MID fan
      for i in {1..8}; do
          echo ${mid_temps[$((i-1))]} | sudo tee $HWMON_PATH/pwm3_auto_point${i}_temp > /dev/null
          echo ${mid_pwms[$((i-1))]} | sudo tee $HWMON_PATH/pwm3_auto_point${i}_pwm > /dev/null
      done

      # Keep in auto mode (2) for proper fan control
      echo "✓ Fan curves set to fanless mode with thermal policy 2 (silent)"
  }
  
  # LAPTOP MODE - Enable aggressive power saving
  echo 5 | sudo tee /proc/sys/vm/laptop_mode > /dev/null
  echo "✓ Laptop mode enabled"

  # platform_profile
  echo quiet | sudo tee /sys/firmware/acpi/platform_profile > /dev/null
  echo "✓ platform_profile set to quiet"

  # wifi power saving
  sudo iw dev wlan0 set power_save on
  echo "✓ wifi set to power saving"

  # PCI POWER MANAGEMENT
#  for pci in /sys/bus/pci/devices/*/power/control; do
#      echo auto | sudo tee $pci > /dev/null 2>&1
#  done

  for pci in /sys/bus/pci/devices/*/power/control; do
      timeout 1 bash -c "echo auto 2>/dev/null | sudo tee $pci >/dev/null 2>&1" || true
  done
  echo "✓ PCI power management enabled"

  # Main script
  case "$1" in
      check)
          check_curves
          ;;
      set|"")
          set_curves
          ;;
      *)
          echo "Usage: $0 [check]"
          echo "  check - Display current fan curves"
          echo "  (no args) - Set quiet fan curves (default)"
          exit 1
          ;;
  esac
