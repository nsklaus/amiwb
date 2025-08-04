# amiwb
window manager for linux using x11 and xlib, look at screenshots dir.

dependencies:
```
-lSM -lICE -lXext -lXmu -lX11 -lXrender -lXft
```

install:
```
$ make
$ make install
```

start the environment:  (stop it with: ESC key or menu > workbench > quit)
```
$ Xephyr -br -ac -noreset -screen 800x600 :1
$ DISPLAY=:1 ./amiwb
```

status:

- menubar:
	works but very few entries are implemented, 
	"Tools" menu have some entries to launch a few apps (kitty,	xcalc, sublime-text, brave, ..), 
	"Workbench" menu have "Quit AmiWB", 
	"Window" menu have "Clean up" and that's about it for now.
	menus are not dynamic yet, there's only system menus, 
	app menus substitutions later, if i ever succeed with it..

- window decorations:
	sliders, arrows, close, resize, iconify, lower and maximize buttons work. 
	(maximize should be made toggle, right now it's just maximize once and done)

- it handle resolutions changes and resizes accordingly

- iconifying windows:
	"workbench" windows use a fixed "filer.info" icon for now, and clients use a name matching system, 
	so if a app has a "kitty" name it will look for a "kitty.info" icon in /usr/local/share/amiwb/icons/
	matching results (fail or pass) will show on terminal output, with path.

- icons:
	show amiga icons, both normal and selected images.
	the icon matching system works like on amiga: "xyz" dir will display image from "xyz.info", 
	same for files. (todo: def_icons system).
	multiselection works but nothing is implemented to work with the selected icons. 
	not even moving a multiselection. for now, one icon only can be dragged around at a time.
	no rename, no delete, no copy, no nothing yet.
	file execution works through xdg-open for now.

TODO:

- xdg-portal aware reqasl file picker
- fullscreen support
- workbench "Show by names" listers
- custom menus, similar to toolsdaemon
- drag and drop support with copy progress bar
- auto mount external drives as icons on the desktop
- icon information UI window