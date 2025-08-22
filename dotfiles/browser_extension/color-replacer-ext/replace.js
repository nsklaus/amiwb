const EXTENSION_VERSION = '1.73';
console.log(`Color replacer extension v${EXTENSION_VERSION} loaded!`);

// Note: Initial gray background and hiding is now handled by inject.css
// which loads before this JavaScript executes

// Convert hex to RGB
function hexToRgb(hex) {
  const r = parseInt(hex.slice(1, 3), 16);
  const g = parseInt(hex.slice(3, 5), 16);
  const b = parseInt(hex.slice(5, 7), 16);
  return `rgb(${r}, ${g}, ${b})`;
}

// Generate all color variations from hex input
function generateColorVariations(hexColors) {
  const variations = {};
  
  for (let [oldHex, newHex] of Object.entries(hexColors)) {
    variations[oldHex] = newHex;
    variations[oldHex.toLowerCase()] = newHex.toLowerCase();
    
    const oldRgb = hexToRgb(oldHex);
    const newRgb = hexToRgb(newHex);
    variations[oldRgb] = newRgb;
    variations[oldRgb.replace(/\s/g, '')] = newRgb.replace(/\s/g, '');
  }
  
  return variations;
}

// Define hex colors and auto-generate RGB variants
const hexColorMap = {
  '#77BB77': '#009619',
  '#88BBFF': '#000cda',
  '#53BFFC': '#000cda',
  '#D9D9D9': '#a0a2a0',  // Light gray to our gray
  '#C3C3C3': '#a0a2a0',  // Light gray to our gray
  '#56A8DA': '#000cda',
  '#a0a1a7': '#9d5f76',  // Gray comments to purple
  '#000000': '#a0a2a0',  // Black backgrounds to gray
  '#000': '#a0a2a0',     // Short black to gray
};

const colors = generateColorVariations(hexColorMap);

function replaceColors(text) {
  let result = text;
  for (let [old, newColor] of Object.entries(colors)) {
    result = result.replaceAll(old, newColor);
  }
  return result;
}

let processed = false;

function processAllStyles() {
  if (processed) return;
  
  // Process existing style elements - BUT NOT on GitHub
  if (window.location.hostname !== 'github.com') {
    document.querySelectorAll('style').forEach(style => {
      style.textContent = replaceColors(style.textContent);
    });
  }

  // Process inline styles efficiently - BUT NOT on GitHub
  if (window.location.hostname !== 'github.com') {
    document.querySelectorAll('[style]').forEach(element => {
      element.style.cssText = replaceColors(element.style.cssText);
    });
  }

  // Process stylesheets - BUT NOT on GitHub
  if (window.location.hostname !== 'github.com') {
    Array.from(document.styleSheets).forEach(sheet => {
      try {
        Array.from(sheet.cssRules || []).forEach(rule => {
          if (rule.style) {
            for (let prop of rule.style) {
              let value = rule.style[prop];
              let newValue = replaceColors(value);
              if (value !== newValue) {
                rule.style[prop] = newValue;
              }
            }
          }
        });
      } catch (e) {}
    });
  }
  
  // Apply smart color forcing
  applySmartColorForcing();
  
  processed = true;
  console.log('Color replacement completed');
  
  // Show page after processing
  // YouTube needs more time
  const delay = window.location.hostname.includes('youtube') ? 200 : 50;
  
  setTimeout(() => {
    // Remove the visibility:hidden from inject.css
    document.documentElement.style.visibility = 'visible';
    // Add a style to override the CSS hiding
    const showStyle = document.createElement('style');
    // CrowdBunker needs everything visible
    if (window.location.hostname.includes('crowdbunker')) {
      showStyle.textContent = `
        html, body, * { visibility: visible !important; }
      `;
    } else {
      showStyle.textContent = 'html { visibility: visible !important; }';
    }
    document.head.appendChild(showStyle);
  }, delay); // Small delay to ensure styles are applied
}

function applySmartColorForcing() {
  // Special handling for CrowdBunker - gray background everywhere
  if (window.location.hostname.includes('crowdbunker')) {
    const style = document.createElement('style');
    style.textContent = `
      /* Gray background for all main containers */
      html, body, #app, .v-application, .container, main, div.v-main {
        background-color: #a0a2a0 !important;
        background: #a0a2a0 !important;
        background-image: none !important;
      }
      
      /* Remove white/dark/blue backgrounds from Vue components */
      .white, .grey, .black, .blue, .primary, .secondary,
      [class*="lighten"], [class*="darken"], [class*="accent"] {
        background-color: #a0a2a0 !important;
      }
      
      /* Headers and cards */
      .v-card, .v-sheet, .v-toolbar, header, footer {
        background-color: #a0a2a0 !important;
      }
      
      /* Links and video titles should be blue */
      a, a span, a div,
      .v-card__title, .v-card__subtitle,
      .v-card-title, .v-card-subtitle,
      [class*="title"], [class*="subtitle"] {
        color: #000cda !important;
      }
      
      /* Override white text specifically */
      .white--text, [class*="white--text"] {
        color: #000cda !important;
      }
      
      /* Target blue backgrounds more aggressively */
      .primary, .secondary, .blue, .info,
      [class*="primary"], [class*="secondary"], [class*="blue"] {
        background-color: #a0a2a0 !important;
        background: #a0a2a0 !important;
      }
      
      /* Fix specific CrowdBunker blue areas */
      .background-gradient {
        background: #a0a2a0 !important;
        background-image: none !important;
      }
      
      .transparent-header, .header-bar {
        background: #a0a2a0 !important;
        background-color: #a0a2a0 !important;
      }
      
      /* Navigation drawer on left */
      .v-navigation-drawer {
        background-color: #a0a2a0 !important;
      }
      
      /* Bottom area of navbar */
      .v-navigation-drawer .secondary,
      .v-navigation-drawer.secondary {
        background-color: #a0a2a0 !important;
        background: #a0a2a0 !important;
      }
      
      /* Fix header when scrolled - Vuetify adds classes on scroll */
      .v-app-bar--is-scrolled,
      .v-toolbar--prominent,
      .v-app-bar.v-app-bar--is-scrolled,
      .v-toolbar.v-app-bar--is-scrolled,
      header.v-app-bar--is-scrolled,
      [class*="scrolled"] {
        background-color: #a0a2a0 !important;
        background: #a0a2a0 !important;
        background-image: none !important;
      }
      
      /* Override any elevation/shadow styles that come with scroll */
      .v-app-bar--is-scrolled.v-sheet--outlined,
      .elevation-4, .elevation-8, .elevation-12,
      [class*="elevation-"] {
        background-color: #a0a2a0 !important;
        background: #a0a2a0 !important;
        box-shadow: none !important;
      }
      
      /* Channel header area with blue secondary background */
      .channel-block.secondary,
      .top-block.secondary,
      div.secondary.top-block {
        background-color: #a0a2a0 !important;
        background: #a0a2a0 !important;
      }
      
      /* Hide play overlay on video thumbnails */
      .play-wrapper,
      .play-wrapper .poster-icon,
      .poster-icon,
      .mdi-play-circle,
      .mdi-play-circle-outline,
      .player-poster:hover .play-wrapper,
      .v-responsive__content .v-icon.mdi-play-circle,
      .v-image .v-icon.mdi-play-circle {
        display: none !important;
        opacity: 0 !important;
        visibility: hidden !important;
      }
    `;
    document.head.appendChild(style);
    console.log('Applied CrowdBunker styling with gray backgrounds and blue links');
    
    // Remove play overlays and prevent hover effects
    const fixVideoThumbnails = () => {
      // Find all video thumbnail links/images and disable hover effects
      document.querySelectorAll('a[href*="/video/"], a[href*="/v/"]').forEach(link => {
        // Remove any play icons inside video links
        link.querySelectorAll('[class*="play"], [class*="mdi-play"]').forEach(el => el.remove());
        
        // Disable pointer events on overlays
        const overlays = link.querySelectorAll('.v-responsive__content > *');
        overlays.forEach(overlay => {
          if (!overlay.querySelector('img')) {
            overlay.style.display = 'none';
          }
        });
      });
      
      // Remove any elements that look like play buttons
      document.querySelectorAll('i[class*="mdi-play"]').forEach(icon => {
        const parent = icon.closest('.v-responsive__content');
        if (parent) {
          icon.parentElement?.remove() || icon.remove();
        }
      });
      
      // Hide any overlay divs that appear on hover
      document.querySelectorAll('.v-responsive__content > div:not(:has(img))').forEach(div => {
        if (div.querySelector('.v-icon') || div.className.includes('overlay')) {
          div.style.display = 'none';
        }
      });
    };
    
    // Add CSS to prevent hover overlays
    const hoverFix = document.createElement('style');
    hoverFix.textContent = `
      /* Block all hover overlays on video thumbnails */
      a[href*="/video/"] .v-responsive__content > *:not(img),
      a[href*="/v/"] .v-responsive__content > *:not(img),
      .v-responsive:hover .v-responsive__content > div,
      .v-image:hover .v-responsive__content > div {
        display: none !important;
        opacity: 0 !important;
        pointer-events: none !important;
      }
    `;
    document.head.appendChild(hoverFix);
    
    // Run the fix multiple times
    fixVideoThumbnails();
    setTimeout(fixVideoThumbnails, 500);
    setTimeout(fixVideoThumbnails, 1500);
    setTimeout(fixVideoThumbnails, 3000);
    
    // Watch for changes
    const observer = new MutationObserver(fixVideoThumbnails);
    observer.observe(document.body, { childList: true, subtree: true });
    
    return; // Skip all other aggressive rules
  }
  
  // Special handling for GitHub
  if (window.location.hostname === 'github.com') {
    const githubStyle = document.createElement('style');
    githubStyle.textContent = `
      /* Minimal GitHub styling - just body and basic text */
      body {
        background-color: #a0a2a0 !important;
      }
      
      /* GitHub code viewing area - all the different elements they use */
      .react-code-view-wrapper,
      .react-line-numbers,
      .react-code-lines,
      .react-code-text,
      .react-file-line {
        background-color: #a0a2a0 !important;
      }
      
      /* GitHub code cells with syntax highlighting preserved */
      td.blob-code,
      td.blob-code-inner,
      .blob-code-content {
        background-color: #a0a2a0 !important;
        color: inherit !important;
      }
      
      /* GitHub line numbers */
      td.blob-num,
      .react-line-number {
        background-color: #909290 !important;
        color: #000000 !important;
      }
      
      /* Code container backgrounds */
      pre, code {
        background-color: #a0a2a0 !important;
      }
      
      /* The main code display divs */
      div[class*="Box-sc-"]:has(.react-code-lines) {
        background-color: #a0a2a0 !important;
      }
      
      /* GitHub header with repo info (Code, Issues, Pull requests, etc) */
      .AppHeader,
      .AppHeader-globalBar,
      .AppHeader-localBar,
      nav[aria-label="Repository"],
      .UnderlineNav,
      .UnderlineNav-body {
        background-color: #a0a2a0 !important;
      }
      
      /* Code file header bar */
      .Box-header,
      .file-header,
      .file-actions,
      .react-blob-header,
      .blob-interaction-bar,
      div[class*="BlobToolbar"] {
        background-color: #a0a2a0 !important;
      }
      
      /* Repository header area with name, fork, star buttons */
      .pagehead,
      .repohead,
      .repository-content .Box,
      .repository-content .Box-header,
      [data-turbo-frame="repo-content-turbo-frame"],
      div[class*="PageLayout"],
      div[class*="Box-sc-"]:not(:has(.react-code-lines)),
      .AppHeader-context,
      .AppHeader-context-full,
      header[class*="Box-sc-"],
      div[data-selector="repos-split-pane-content"] > div {
        background-color: #a0a2a0 !important;
      }
      
      /* Specific repo header with title and buttons */
      #repository-container-header,
      .repository-content > div:first-child,
      .pagehead-actions,
      .Box-row {
        background-color: #a0a2a0 !important;
      }
      
      /* Generic white boxes that should be gray */
      .Box:not([class*="color-"]),
      .BorderGrid,
      .BorderGrid-cell {
        background-color: #a0a2a0 !important;
      }
      
      /* Tabs and navigation */
      .tabnav,
      .tabnav-tabs,
      .tabnav-tab {
        background-color: #a0a2a0 !important;
      }
      
      /* Preserve syntax highlighting spans */
      .blob-code span[class^="pl-"] {
        background-color: transparent !important;
      }
      
      /* GitHub code colors */
      .pl-k { color: #d73a49 !important; }
      .pl-s, .pl-pds { color: #032f62 !important; }
      .pl-c1 { color: #005cc5 !important; }
      .pl-c { color: #9d5f76 !important; }
      .pl-smi { color: #24292e !important; }
      .pl-en { color: #6f42c1 !important; }
      .pl-e, .pl-ent { color: #22863a !important; }
      .pl-v { color: #e36209 !important; }
    `;
    document.head.appendChild(githubStyle);
    console.log('Applied minimal GitHub styling');
    return;
  }
  
  // Create style rules for AmigaOS-style gray backgrounds - AGGRESSIVE for non-GitHub
  const style = document.createElement('style');
  style.textContent = `
    /* Unset all fonts globally - use browser defaults */
    * {
      font-family: unset !important;
    }
    
    /* Force gray background on most elements but preserve thumbnails and code */
    body, div:not([class*="thumb"]):not([class*="video"]):not([class*="highlight"]), 
    section, article, main, header, footer, nav, aside,
    p, h1, h2, h3, h4, h5, h6, ul, ol, 
    li:not([class*="thumb"]):not([class*="video"]),
    dl, dt, dd, table, thead, tbody, tfoot, tr, td:not(.blob-code), th,
    form, fieldset, legend, label, input, textarea, select, button,
    blockquote, address, a {
      background-color: #a0a2a0 !important;
      box-shadow: none !important;
    }
    
    /* Span elements - but not syntax highlighting spans */
    span:not(.keyword):not(.type):not(.string):not(.comment):not(.function):not(.number):not(.operator):not([class*="highlight"]):not([class*="syntax"]) {
      background-color: #a0a2a0 !important;
    }
    
    /* Remove background images except for thumbnails */
    body, section, article, main, header, footer, nav, aside,
    p, h1, h2, h3, h4, h5, h6, ul, ol, dl, dt, dd,
    table, thead, tbody, tfoot, tr, td, th,
    form, fieldset, legend, label, input, textarea, select, button,
    blockquote, address, span, a {
      background-image: none !important;
    }
    
    /* Remove gradients and shadows but preserve thumbnails */
    *:not([class*="thumb"]):not([class*="video"]):not([id*="thumb"]):not(ytd-thumbnail):not(.yt-img-shadow) {
      background-image: none !important;
    }
    
    * {
      box-shadow: none !important;
    }
    
    /* Force solid backgrounds on common containers */
    .container, .wrapper, .content, .main, .sidebar, .header, .footer,
    [class*="container"], [class*="wrapper"], [class*="content"] {
      background-color: #a0a2a0 !important;
      background-image: none !important;
    }
    
    /* Convert any black backgrounds to gray */
    [style*="background-color: black"],
    [style*="background-color:#000"],
    [style*="background-color: #000"],
    [style*="background-color: rgb(0, 0, 0)"],
    [style*="background:black"],
    [style*="background:#000"],
    [style*="background: #000"],
    [style*="background: rgb(0, 0, 0)"],
    .bg-black, .bg-dark, .dark-bg, .black-bg,
    [class*="bg-black"], [class*="dark-bg"] {
      background-color: #a0a2a0 !important;
    }
    
    /* Headers and sections that commonly use dark backgrounds */
    header, section > header, .section-header, .page-header {
      background-color: #a0a2a0 !important;
    }
    
    /* Keep images and media visible but no shadows */
    img, video, canvas, picture, embed, object, audio {
      box-shadow: none !important;
    }
    
    /* SVG can have backgrounds */
    svg {
      background-color: transparent !important;
    }
    
    /* Code blocks get gray background but preserve all text colors */
    pre, code, pre code {
      background-color: #a0a2a0 !important;
      /* Don't force any text color - let syntax highlighting work */
    }
    
    /* But preserve syntax highlighting colors */
    .keyword, .type, .string, .comment, .function, .number, .operator,
    [class*="syntax"], [class*="highlight"],
    pre .keyword, pre .type, pre .string, pre .comment, pre .function, pre .number, pre .operator {
      background-color: transparent !important;
      /* Don't override their colors - let them use their defined colors */
    }
    
    
    /* Text should be black - but not if parent has gray background */
    h1, h2, h3, h4, h5, h6, label, blockquote {
      color: #000000 !important;
    }
    
    /* More selective text coloring to avoid conflicts */
    p:not([class*="hover"]):not([class*="text-"]),
    li:not([class*="hover"]):not([class*="text-"]),
    td:not([class*="hover"]):not([class*="text-"]),
    th:not([class*="hover"]):not([class*="text-"]),
    address {
      color: #000000 !important;
    }
    
    /* Fix gray text on gray background - force any gray-ish text to black */
    p[style*="color: #666"],
    p[style*="color: #777"],
    p[style*="color: #888"],
    p[style*="color: #999"],
    p[style*="color: #aaa"],
    p[style*="color: #bbb"],
    p[style*="color: #ccc"],
    p[style*="color: rgb(102"],
    p[style*="color: rgb(119"],
    p[style*="color: rgb(136"],
    p[style*="color: rgb(153"],
    p[style*="color: rgb(170"],
    p[style*="color: rgb(187"],
    p[style*="color: rgb(204"],
    *[style*="color: #a0a2a0"],
    *[style*="color: rgb(160, 162, 160"],
    .text-gray, .text-grey, .gray-text, .grey-text,
    .text-muted, .muted, .text-secondary,
    [class*="text-gray"], [class*="text-grey"],
    [class*="gray-text"], [class*="grey-text"] {
      color: #000000 !important;
    }
    
    /* Don't touch span/div text color if they have text- classes (Tailwind) or are syntax highlighting */
    body span:not([class*="text-"]):not([class*="hover"]):not([class*="transition"]):not(.keyword):not(.type):not(.string):not(.comment):not(.function):not(.number):not(.operator):not(pre span):not(code span),
    article > div:not([class*="text-"]):not([class*="hover"]) {
      color: #000000 !important;
    }
    
    /* Remove the aggressive opacity/visibility rules - they break sites */
    /* Only ensure images and videos are visible */
    img, video {
      opacity: 1 !important;
      visibility: visible !important;
    }
    
    /* Disable YouTube video preview on hover */
    ytd-thumbnail #hover-overlays,
    ytd-thumbnail ytd-thumbnail-overlay-toggle-button-renderer,
    ytd-thumbnail-overlay-time-status-renderer,
    ytd-thumbnail-overlay-now-playing-renderer,
    .ytp-inline-preview-ui,
    .video-preview,
    ytd-video-preview,
    #video-preview-container,
    .ytp-videowall-still-info-content {
      display: none !important;
    }
    
    /* Disable overlays - but NOT Tailwind hover: states */
    [class*="overlay"]:not([class*="hover:"]),
    [class*="hover-overlay"],
    [class*="popup"]:not([class*="hover:"]),
    [class*="popover"],
    [class*="tooltip"],
    .overlay,
    .hover-overlay,
    .image-overlay,
    .thumbnail-overlay,
    .video-overlay {
      display: none !important;
    }
    
    /* But keep the actual thumbnail images visible */
    [class*="thumb"] img,
    [class*="video"] img,
    ytd-thumbnail img,
    .yt-img-shadow {
      display: block !important;
      visibility: visible !important;
    }
    
    /* Links */
    a, a *, [href], [href] * {
      color: #000cda !important;
    }
    
    a:visited, a:visited * {
      color: #551a8b !important;
    }
    
    /* YouTube thumbnails */
    ytd-thumbnail, #thumbnail, .yt-img-shadow {
      background-color: transparent !important;
    }
    
    /* Remove background images */
    body, 
    div:not([class*="thumb"]):not([id*="thumb"]):not([class*="img"]),
    section, article, header, footer, nav {
      background-image: none !important;
    }
    
    /* Remove this rule - it was breaking GitHub's layout */
    /* The toolbar needs its positioning to work correctly */
    
    /* COMMENTED OUT - might be interfering with layout */
    /* .repository-content {
      overflow-x: auto !important;
      max-width: 100% !important;
    } */
  `;
  
  document.head.appendChild(style);
  console.log('Smart color forcing applied');
  
  // Fix gray text on gray background dynamically
  setTimeout(() => {
    fixGrayOnGrayText();
  }, 100);
  
  // Special handling for GitHub - wait for code to load then fix it
  if (window.location.hostname === 'github.com') {
    fixGitHubCode();
  }
}

function fixGrayOnGrayText() {
  // Find all elements and check their computed color
  const elements = document.querySelectorAll('body, div, p, span, li, td, th, code, pre');
  elements.forEach(el => {
    const computed = window.getComputedStyle(el);
    const color = computed.color;
    
    // Check if text color is our gray (#a0a2a0 = rgb(160, 162, 160))
    if (color === 'rgb(160, 162, 160)' || 
        color === 'rgb(160, 160, 160)' ||
        color === 'rgb(162, 162, 162)') {
      // Force it to black
      el.style.setProperty('color', '#000000', 'important');
    }
  });
  
  console.log('Fixed gray-on-gray text');
}

function fixGitHubCode() {
  // Wait for GitHub code to actually load
  const observer = new MutationObserver((mutations, obs) => {
    const codeSpans = document.querySelectorAll('.blob-code span, [class^="pl-"]');
    if (codeSpans.length > 0) {
      console.log('GitHub code detected, fixing colors...');
      obs.disconnect();
      
      // Remove any color overrides from our styles
      codeSpans.forEach(span => {
        // Remove any inline styles we might have added
        span.style.removeProperty('color');
        span.style.removeProperty('background-color');
        span.style.removeProperty('-webkit-text-fill-color');
      });
      
      // Add a more specific style for GitHub with EXPLICIT colors
      const githubFix = document.createElement('style');
      githubFix.textContent = `
        /* GitHub syntax colors - FORCE EXPLICIT COLORS */
        .blob-code .pl-k, td.blob-code .pl-k { color: #d73a49 !important; background: transparent !important; }
        .blob-code .pl-s, td.blob-code .pl-s, 
        .blob-code .pl-pds, td.blob-code .pl-pds { color: #032f62 !important; background: transparent !important; }
        .blob-code .pl-c1, td.blob-code .pl-c1 { color: #005cc5 !important; background: transparent !important; }
        .blob-code .pl-c, td.blob-code .pl-c { color: #9d5f76 !important; background: transparent !important; }
        .blob-code .pl-smi, td.blob-code .pl-smi { color: #24292e !important; background: transparent !important; }
        .blob-code .pl-en, td.blob-code .pl-en { color: #6f42c1 !important; background: transparent !important; }
        .blob-code .pl-e, td.blob-code .pl-e,
        .blob-code .pl-ent, td.blob-code .pl-ent { color: #22863a !important; background: transparent !important; }
        .blob-code .pl-v, td.blob-code .pl-v { color: #e36209 !important; background: transparent !important; }
        .blob-code .pl-kos, td.blob-code .pl-kos { color: #24292e !important; background: transparent !important; }
        .blob-code .pl-s1, td.blob-code .pl-s1 { color: #032f62 !important; background: transparent !important; }
        
        /* Default for any pl-* class we missed */
        .blob-code [class^="pl-"], 
        td.blob-code [class^="pl-"],
        .blob-code [class*=" pl-"],
        td.blob-code [class*=" pl-"] {
          color: #24292e !important;
          background: transparent !important;
          opacity: 1 !important;
          visibility: visible !important;
        }
      `;
      document.head.appendChild(githubFix);
      console.log('GitHub code colors fixed');
    }
  });
  
  observer.observe(document.body, {
    childList: true,
    subtree: true
  });
}

// Run the extension when DOM is ready
function runExtension() {
  if (document.body) {
    processAllStyles();
  } else {
    // If body doesn't exist yet, wait a bit
    setTimeout(runExtension, 10);
  }
}

if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', runExtension);
} else {
  runExtension();
}

// Handle dynamically added content
let observerTimeout = null;
function handleMutations() {
  // Skip mutation handling on GitHub to prevent layout issues
  if (window.location.hostname === 'github.com') {
    return;
  }
  
  clearTimeout(observerTimeout);
  observerTimeout = setTimeout(() => {
    document.querySelectorAll('[style]:not([data-color-processed])').forEach(element => {
      element.style.cssText = replaceColors(element.style.cssText);
      element.setAttribute('data-color-processed', 'true');
    });
    
    // Also fix any new gray-on-gray text
    fixGrayOnGrayText();
  }, 500);
}

setTimeout(() => {
  const observer = new MutationObserver(handleMutations);
  observer.observe(document.body, {
    childList: true,
    subtree: true,
    attributes: true,
    attributeFilter: ['style']
  });
}, 2000);