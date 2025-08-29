# Simple Color Replacer - AmigaOS Web Style

A browser extension that makes the web look like AmigaOS from the 1990s.

## Features

- **Gray background everywhere** (#a0a2a0) - the classic Amiga Workbench color
- **Removes modern web cruft** - no gradients, shadows, overlays, or transparency
- **Preserves important content** - images, videos, and syntax highlighting remain visible
- **Fixes readability issues** - automatically fixes gray-on-gray text
- **Respects user font settings** - uses your browser's configured fonts
- **AmigaOS-style scrollbars** - black thumb with gray track, no arrow buttons
- **Custom text selection** - faint light blue selection color instead of harsh blue

## Installation

1. Open your browser's extension management page:
   - Chrome/Brave: `chrome://extensions/` or `brave://extensions/`
   - Firefox: `about:debugging`

2. Enable "Developer mode"

3. Click "Load unpacked" and select this folder

4. For local HTML files, enable "Allow access to file URLs" in extension details

## Color Replacements

The extension uses the `hexColorMap` in `replace.js` to define color replacements:

### How it works:
1. **Regular website colors** - Always replaced if they're in hexColorMap
2. **Syntax highlighting colors** - By default PRESERVED, but ONLY replaced if that specific color is in hexColorMap

### The logic:
- If a syntax color (e.g., #FF5733 for strings) is NOT in hexColorMap → it stays as-is
- If a syntax color (e.g., #DB3279) IS in hexColorMap → it gets replaced with your chosen color (#9d5f76)

### Current mappings:
- Green (#77BB77) → Darker green (#009619)
- Light blue (#88BBFF, #53BFFC) → Amiga blue (#000cda)
- Grays (#D9D9D9, #C3C3C3) → Amiga gray (#a0a2a0)
- Gray comments (#a0a1a7) → Purple (#9d5f76)
- Black backgrounds → Gray (#a0a2a0)
- Pink (#DB3279) → Purple (#9d5f76)

To add new color replacements, edit the `hexColorMap` object in `replace.js`.

## Special Handling

- **GitHub**: 
  - Preserves syntax highlighting with custom colors
  - Fixes white backgrounds on code blocks with black borders
  - Removes navigation menu clutter
  - Applies gray theme to all UI elements
- **Claude.ai**: Maintains code syntax colors
- **YouTube**: Disables video preview overlays
- **CrowdBunker**: Custom rules for proper gray backgrounds and removes play overlays
- **Local files**: Fixes gray-on-gray text in documentation

## Version

Current: v2.01

### Recent Updates
- v2.01: Extended syntax highlighting support to all containers (not just code/pre tags)
- v2.00: Optimized performance for syntax color replacement
- v1.99: Added computed style checking for code elements
- v1.98: Added hexColorMap support for syntax highlighting colors
- v1.96: Added custom text selection color (faint light blue)
- v1.95: Refined scrollbar design (black thumb, no 3D effects)