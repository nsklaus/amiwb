# Web Sanitizer

A browser extension that replaces white backgrounds with gray (#A0A2A0) across websites, making the web more unified, less flashy. Black on gray is better for the eyes. Blue links (without underline), purple for visited ones. Use in conjunction with uBlock Origin. If a site doesn't play well with the global rule, then define a new one for that specific domain and get custom tailored results.

## Features
- **Surgical approach** - Only modifies explicitly configured sites
- **Fallback pattern** - Undefined sites automatically get nuclear gray treatment
- **CSS variable aware** - Overrides modern theming systems
- **Fast** - Pure CSS injection, no DOM watching
- **Lightweight** - ~34KB, no dependencies

## Installation
1. Open your browser's extension management page
2. Enable "Developer mode"
3. Click "Load unpacked"
4. Select this `web-sanitizer` folder

## How It Works
Sites are configured in `disinfectant.js`:
- **Defined sites** get custom rules (YouTube, GitHub, etc.)
- **Undefined sites** fall back to `*.*` pattern (nuclear gray everything)

## Configuration
Edit `SITE_MODIFICATIONS` in `disinfectant.js` to add/modify sites:
```javascript
'example.com': {
  selectors: {
    'body': {
      'background-color': '#A0A2A0',
      'background': '#A0A2A0'
    }
  }
}
```

## Standard Colors
- Background: `#A0A2A0` (gray)
- Text: `#000000` (black)
- Links: `#000cda` (blue)
- Visited: `#551a8b` (purple)

## Note
The extension automatically adds `!important` to all CSS rules - don't include it in configurations.