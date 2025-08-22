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

- Green (#77BB77) → Darker green (#009619)
- Light blue (#88BBFF, #53BFFC) → Amiga blue (#000cda)
- Grays (#D9D9D9, #C3C3C3) → Amiga gray (#a0a2a0)
- Gray comments (#a0a1a7) → Purple (#9d5f76)
- Black backgrounds → Gray (#a0a2a0)

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

Current: v1.96

### Recent Updates
- v1.96: Added custom text selection color (faint light blue)
- v1.95: Refined scrollbar design (black thumb, no 3D effects)
- v1.91: Fixed GitHub header backgrounds
- v1.88: Aggressive GitHub white background removal
- v1.85: GitHub-specific CSS injection for code blocks
- v1.80: CrowdBunker video overlay fixes