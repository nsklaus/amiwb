# amiwb
window manager for linux using x11 and xlib, look at screenshots dir.

dependencies:
```
-lSM -lICE -lXext -lXmu -lX11 -lXrender -lXft -lXfixes -lXdamage
-lXrandr -lXcomposite -lm -lImlib2 -lfontconfig 
```

install:
```
$ make
$ make install
```

start the environment:
```
$ Xephyr -br -ac -noreset -screen 800x600 :1
$ DISPLAY=:1 ./amiwb
OR
$ startx ~/.xinitrc_amiwb -- :2
```

status: the base environment is nearly complete.

- menubar:
	most entries implemented. with global shortcut system.
	menus are not dynamic yet, there's only system menus, 
	app menus substitutions later, i'll have to look into it.
	additional custom menus parsed from file toolsdaemon style. 
	show date and time.

- window decorations:
	sliders, arrows, close, resize, iconify, lower and maximize buttons work. 

- it handle fullscreen for apps, and X resolutions changes and resizes accordingly

- iconifying windows:
	"workbench" windows use a fixed "filer.info" icon for now, and clients use a name matching system, 
	so if a app has a "kitty" name it will look for a "kitty.info" icon in /usr/local/share/amiwb/icons/
	if matching result fail it will show in file amiwb.log see config.h

- icons:
	show amiga icons, both normal and selected images.
	the icon matching system works like on amiga: "xyz" dir will display image from 
	"xyz.info" icon next to it, while hidding the .info file. same for files. 
	multiselection works. some menu actions do support it, like delete. 
	for now, one icon only can be dragged around at a time.
	file execution works through xdg-open for now.
	def_icons system works, it's simple and based on file extensions for matching.

TODO:

- xdg-portal aware reqasl file picker
- make a GUI for system settings 
- progress bar (copy/delete)
- auto mount external drives as icons on the desktop
- icon information UI window