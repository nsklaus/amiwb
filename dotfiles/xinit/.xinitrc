#!/bin/bash

export XDG_SESSION_TYPE="x11"
export XDG_DATA_DIRS="/usr/share:/usr/local/share:$HOME/.local/share"
export XDG_CONFIG_DIRS="/etc/xdg"
export XDG_CURRENT_DESKTOP="amiwb"

# Source system initialization scripts to import environment variables
if [ -d /etc/X11/xinit/xinitrc.d ] ; then
  for f in /etc/X11/xinit/xinitrc.d/?*.sh ; do
    [ -x "$f" ] && . "$f"
  done
  unset f
fi

# Enable touchpad tapping and ensure no "natural" scrolling
# list all properties: xinput list-props <dev_id>
TOUCHPAD_ID=$(xinput list | grep -i touch | grep -oP 'id=\K\d+')
if [ -n "$TOUCHPAD_ID" ]; then
    xinput set-prop $TOUCHPAD_ID "libinput Tapping Enabled" 1
    xinput set-prop $TOUCHPAD_ID "libinput Natural Scrolling Enabled" 0
fi

# Disable Asus WMI hotkeys
# ASUS_WMI_ID=$(xinput list | grep -i "Asus WMI hotkeys" | grep -oP 'id=\K\d+')
# if [ -n "$ASUS_WMI_ID" ]; then
#    xinput disable $ASUS_WMI_ID
# fi

# Load X resources (for proper DPI/font settings, ex: lxpolkit) 
if [ -f ~/.Xresources ]; then
    xrdb -merge ~/.Xresources
fi

# Start PolicyKit authentication agent
lxpolkit &

xrandr -s 1440x900 &

# scaling: 
# to get effective 1440x900 from 3024x1890:
#  - Scale X: 1440/3024 = 0.476
#  - Scale Y: 900/1890 = 0.476
# xrandr --output eDP-1 --scale 0.476x0.476

exec dbus-launch --exit-with-session amiwb

# exec dbus-launch --exit-with-session valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --track-fds=yes --num-callers=20 --log -file=/home/klaus/Sources/amiwb/valgrind_new.log /usr/local/bin/amiwb

