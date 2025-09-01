# amiwb
window manager for linux using x11 and xlib, look at screenshots dir.

dependencies:
```
-lSM -lICE -lXext -lXmu -lX11 -lXrender -lXft -lXfixes 
-lXdamage -lXrandr -lXcomposite -lm -lImlib2 -lfontconfig 
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

status: the base environment is mostly complete.

- menubar:
	all entries implemented. with global shortcut system (not user configurable, 
	shortcuts hardcoded in for now). menus are not dynamic yet, there's only 
	system menus. app menus substitutions later.
	additional custom menus parsed from file, toolsdaemon style. 
	show date and time. 

- window decorations:
	sliders, arrows, close, resize, iconify, lower and maximize buttons work. 

- it handle fullscreen for apps, and X resolutions changes and resizes accordingly

- iconifying windows:
	"workbench" windows use a fixed "filer.info" icon for now, and clients use 
	a name matching system, so if a app has a "kitty" name it will look for 
	a "kitty.info" icon in /usr/local/share/amiwb/icons/
	if matching result fail it will show in file amiwb.log see config.h

- icons:
	show amiga icons, both normal and selected images.
	the icon matching system works like on amiga: "xyz" dir will display image 
	from "xyz.info" icon next to it, while hidding the .info file. same for 
	files. multiselection works. some menu actions do support it, like delete. 
	for now, one icon only can be dragged around at a time.
	file execution works through xdg-open for now.
	def_icons system works, it's simple and based on file extensions for matching.

- background pictures for desktop and windows, both can do tiling  

- file operations:
    copy, move, drag and drop (with progress dialog)
    delete (with warning dialog)
    rename (rename dialog) 
    execute (execute dialog, with file completion)
    information (icon info dialog)

- reqasl:
	browse the filesystem, using mouse or keyboard. 
	open files (xdg-open) and dirs (in wb windows), 
	clipboard support for inputfields.

- toolkit:
	using amiwb UI elements in new apps:
    button, listview, inputfield, progress bar 


TODO:

- make reqasl a file picker to load and save files from apps (wip)
- make a GUI for system settings 
- auto mount external drives as icons on the desktop

