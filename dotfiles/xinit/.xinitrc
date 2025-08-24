#!/bin/bash

export XDG_SESSION_TYPE="x11"
export XDG_SESSION_DESKTOP="xfce"
export XDG_DATA_DIRS="/usr/share:/usr/local/share:$HOME/.local/share"
export XDG_CONFIG_DIRS="/etc/xdg"
export XDG_CURRENT_DESKTOP="XFCE"

# Source system initialization scripts to import environment variables
if [ -d /etc/X11/xinit/xinitrc.d ] ; then
  for f in /etc/X11/xinit/xinitrc.d/?*.sh ; do
    [ -x "$f" ] && . "$f"
  done
  unset f
fi

# Enable touchpad tapping
TOUCHPAD_ID=$(xinput list | grep -i touchpad | grep -oP 'id=\K\d+')
if [ -n "$TOUCHPAD_ID" ]; then
    xinput set-prop $TOUCHPAD_ID "libinput Tapping Enabled" 1
fi

# Disable Asus WMI hotkeys
ASUS_WMI_ID=$(xinput list | grep -i "Asus WMI hotkeys" | grep -oP 'id=\K\d+')
if [ -n "$ASUS_WMI_ID" ]; then
    xinput disable $ASUS_WMI_ID
fi

# Load X resources (for proper DPI/font settings)
if [ -f ~/.Xresources ]; then
    xrdb -merge ~/.Xresources
fi

# Start PolicyKit authentication agent
lxpolkit &

xrandr -s 1440x900 &

exec dbus-launch --exit-with-session amiwb
