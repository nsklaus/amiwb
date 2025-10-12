# amiwb
a desktop and window manager for linux using x11 and xlib. 
it tries to reproduce the UX/UI of the amiga workbench
as a workstation. 

for those that had an amiga equipped with a gfx and a 060 cpu card, 
you know what i'm talking about.
it's more like draco motion/a4k 'amiga' rather than a500/cd32 'amiga'.
the former was professionnal grade workstation using softwares like:
tv-paint, adpro, art-effect, lightwave, imagine, movie-shop, samplitude, ...
the later was more like a home gaming system. 
look at screenshots dir.

### status: the base environment is complete. 

dependencies:
```
-lSM -lICE -lXext -lXmu -lX11 -lXrender -lXft -lXfixes 
-lXdamage -lXrandr -lXcomposite -lm -lImlib2 -lfontconfig 
```

install:
```
$ make
$ sudo make install
```

start the environment:
```
$ Xephyr -br -ac -noreset -screen 800x600 :1
$ DISPLAY=:1 ./amiwb
OR
$ startx ~/.xinitrc_amiwb -- :2
```

### details, in practice:
what is amiwb ? it's a full compositing, stacking wm,
the compiled binary (that contains the whole desktop) is less than 1Mb, 
it could fit on a floppy disk. it's around 25k lines of code total, 
it handles clients (linux apps) and have its own file manager and app launcher.
in other words: it's a desktop ..

it have "on-demand" and "continuous" render modes as options:
- in on-demand mode the desktop will render as little as possbile 
(at idle, it's like 1 or 2 FPS, but the desktop is still extremely reactive, 
with average rendering frame time of 0.05ms, as soon as you just blink the FPS
are climbing back up to reach up to target fps if need be).
- in continuous mode it always render at the target fps, like 60 or 120fps.

on general usage, for me: 10-20 windows opened, browser with 20 tabs or such,
terminal with 3-8 tabs, the desktop itself usualy take between 0 and >1% cpu time.
the filemanager 'workbench' can display very large directories instantly.
i have a dir with 2000 books (epubs, small files), workbench display that 
instantly with all the icons (def_icons), and i can scroll through them without
any slowdowns or stuttering, coughing, sneezing or whatever. it just flows.
workbench can show files in "Icons" or "Names" modes and its windows can be made
"spatial" or "lister".
( spatial is: open a new window each time you open a dir.
lister is: reuse the same window, keep it, all the time ).
icons being used can be customised very easily. 
you can make it all mwb, or all glowicons, for example.
amiwb scans `/usr/local/share/amiwb/icons/def_icons/` at start.
files like "def_zip.info" or "def_mp3.info" are automatically picked up and
will be used as default icon for the filetype.
and it looks for iconify icons in  `/usr/local/share/amiwb/icons/`
or in `~/.config/amiwb/icons/` and `def_icons/`.
general settings are in file `~/.config/amiwb/amiwbrc`.

shortcuts, all use the "super" key. 
example: 
- `super+shift+R` hot-restarts amiwb
- `super+E` calls the "execute command" dialog
- `super+P` opens parent window in file manager
- `super+R` calls rename dialog for the selected icon 
- `super+shift+Q` quits amiwb
..

### the desktop elements:

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
    if matching check fails, it will show in file amiwb.log see:
    config.h ( #define LOG_FILE_PATH )

- icons:
    show amiga icons, both normal and selected images. 
    (classic, mwb, glowicons supported).
    the icon matching system works like on amiga: "xyz" dir will display image 
    from "xyz.info" icon next to it, while hidding the .info file itself. same for files. multiselection works. some menu actions do support it (most don't yet.)
    for now, one icon only can be dragged around at a time.
    file execution works through xdg-open for now.
    def_icons system works, it's simple and based on file extensions for matching.

- auto mount external drives as device icons on the desktop 
- background pictures for desktop and windows, both can do optional tiling  
- it handles: fullscreen for apps,
- auto resize and adapt itself upon X resolutions changes

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
