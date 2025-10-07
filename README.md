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
    shortcuts hardcoded in for now). 
    app menus substitutions works (for amiwb apps).
    additional custom menus parsed from file, toolsdaemon style. 
    show optional menu addons (cpu, mem, sensors) 

- window decorations:
    sliders, arrows, close, resize, iconify, lower and maximize buttons work. 

- iconifying windows:
    "workbench" windows use a fixed "filer.info" icon for now, and clients use 
    a name matching system, so if a app has a "kitty" name it will look for 
    a "kitty.info" icon in /usr/local/share/amiwb/icons/
    if matching check fails, it will show in file amiwb.log see config.h

- icons:
    show amiga icons, both normal and selected images. (classic, mwb, glowicons supported).
    the icon matching system works like on amiga: "xyz" dir will display image 
    from "xyz.info" icon next to it, while hidding the .info file itself. same for 
    files. multiselection works. some menu actions do support it, like delete. 
    for now, one icon only can be dragged around at a time.
    file execution works through xdg-open for now.
    def_icons system works, it's simple and based on file extensions for matching.

- auto mount external drives as device icons on the desktop 
- background pictures for desktop and windows, both can do optional tiling  
- it handles: fullscreen for apps, and X resolutions changes and resizes accordingly

- file operations:
    copy, move, dnd (with progress dialog and preserve extended attributes for comments)  
    delete (with warning dialog)
    rename (rename dialog) 
    execute (execute dialog, with file completion)
    information (icon info dialog, with comments, dir size, and xdg-open support)
    extract (extract archive in place in a new dir, with progress dialog)

- reqasl:
    browse the filesystem, using mouse or keyboard. 
    open files (xdg-open) and dirs (in wb windows), 
    clipboard support for inputfields.
    can add bookmarks, select all/none, show hidden

- toolkit:
    using amiwb UI elements in new apps:
    button, listview, textview, inputfield, progress bar 


TODO:

- make reqasl a file picker to load and save files from apps (wip)
- make a GUI for system settings 
