// Web Sanitizer - Targeted modifications only where explicitly configured
console.log('Web Sanitizer v2.0 loaded - Whitelist mode');

// Configuration - what to modify on each domain
const SITE_MODIFICATIONS = {
  'crowdbunker.com': {
    selectors: {
      // Gray background for all main containers
      'html, body, #app, .v-application, .container, main, div.v-main': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0',
        'background-image': 'none'
      },
      
      // Remove white/dark/blue backgrounds from Vue components
      '.white, .grey, .black, .blue, .primary, .secondary, [class*="lighten"], [class*="darken"], [class*="accent"]': {
        'background-color': '#A0A2A0'
      },
      
      // Headers and cards
      '.v-card, .v-sheet, .v-toolbar, header, footer': {
        'background-color': '#A0A2A0'
      },
      
      // Navigation drawer on left
      '.v-navigation-drawer': {
        'background-color': '#A0A2A0'
      },
      
      // Bottom area of navbar
      '.v-navigation-drawer .secondary, .v-navigation-drawer.secondary': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      
      // Fix header when scrolled - Vuetify adds classes on scroll
      '.v-app-bar--is-scrolled, .v-toolbar--prominent, .v-app-bar.v-app-bar--is-scrolled, .v-toolbar.v-app-bar--is-scrolled, header.v-app-bar--is-scrolled, [class*="scrolled"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0',
        'background-image': 'none'
      },
      
      // Override any elevation/shadow styles that come with scroll
      '.v-app-bar--is-scrolled.v-sheet--outlined, .elevation-4, .elevation-8, .elevation-12, [class*="elevation-"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0',
        'box-shadow': 'none'
      },
      
      // Channel header area with blue secondary background
      '.channel-block.secondary, .top-block.secondary, div.secondary.top-block': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      
      // Target blue backgrounds more aggressively
      '.primary, .secondary, .blue, .info, [class*="primary"], [class*="secondary"], [class*="blue"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      
      // Fix specific CrowdBunker blue areas
      '.background-gradient': {
        'background': '#A0A2A0',
        'background-image': 'none'
      },
      
      '.transparent-header, .header-bar': {
        'background': '#A0A2A0',
        'background-color': '#A0A2A0'
      },
      
      // Links and video titles should be blue
      'a, a span, a div, .v-card__title, .v-card__subtitle, .v-card-title, .v-card-subtitle, [class*="title"], [class*="subtitle"]': {
        'color': '#000cda'
      },
      
      // Override white text specifically
      '.white--text, [class*="white--text"]': {
        'color': '#000cda'
      },
      
      // Hide play overlay on video thumbnails
      '.play-wrapper, .play-wrapper .poster-icon, .poster-icon, .mdi-play-circle, .mdi-play-circle-outline, .player-poster:hover .play-wrapper, .v-responsive__content .v-icon.mdi-play-circle, .v-image .v-icon.mdi-play-circle': {
        'display': 'none',
        'opacity': '0',
        'visibility': 'hidden'
      },
      
      // Block all hover overlays on video thumbnails
      'a[href*="/video/"] .v-responsive__content > *:not(img), a[href*="/v/"] .v-responsive__content > *:not(img), .v-responsive:hover .v-responsive__content > div, .v-image:hover .v-responsive__content > div': {
        'display': 'none',
        'opacity': '0',
        'pointer-events': 'none'
      }
    }
  },
  
  'odysee.com': {
    selectors: {
      // Override CSS variables from base-theme.scss
      ':root': {
        '--color-white': '#A0A2A0',
        '--color-white-alt': '#A0A2A0',
        '--color-background': '#A0A2A0',
        '--color-card-background': '#A0A2A0',
        '--color-card-background-highlighted': '#A0A2A0',
        '--color-header-background': '#A0A2A0',
        '--color-gray-1': '#A0A2A0',
        '--color-gray-2': '#A0A2A0',
        '--color-gray-3': '#A0A2A0',
        '--color-button-alt-bg': '#A0A2A0',
        '--color-input-bg': '#A0A2A0',
        '--color-input-toggle-bg': '#A0A2A0',
        '--color-input-prefix-bg': '#A0A2A0',
        '--color-menu-background': '#A0A2A0',
        '--color-follow-bg': '#A0A2A0',
        '--color-view-bg': '#A0A2A0',
        '--color-thumbnail-background': '#A0A2A0',
        '--color-placeholder-background': '#A0A2A0',
        '--color-file-viewer-background': '#A0A2A0',
        '--color-tabs-background': '#A0A2A0',
        '--color-modal-background': '#A0A2A0',
        '--color-ads-background': '#A0A2A0',
        '--color-comment-highlighted': '#A0A2A0',
        '--color-comment-threadline': '#A0A2A0',
        '--color-link-focus-bg': '#A0A2A0',
        '--color-visibility-label': '#A0A2A0',
        '--color-hyperchat-4': '#A0A2A0'
      },
      // Main backgrounds
      'body': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'html': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '#app': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Common React app containers
      '.main, .content, .container': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Header/Navigation
      'header, nav, .header, .navigation, .navbar': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Video player areas - keep player controls visible
      '.video-js, .vjs-tech': {
        'background-color': 'transparent',
        'background': 'transparent'
      },
      // Cards and content blocks
      '.card, .claim-preview, .media, .content-wrapper': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Sidebar
      '.sidebar, aside': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Comments section
      '.comments, .comment, .comment-list': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Modal/Dialog backgrounds
      '.modal, .dialog, .popup': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Any white backgrounds (case insensitive)
      '[style*="background: white" i], [style*="background-color: white" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background:#fff" i], [style*="background-color:#fff" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background: #fff" i], [style*="background-color: #fff" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="rgb(255, 255, 255)" i], [style*="rgb(255,255,255)" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Fix text visibility
      'p, span, div, td, th, li, h1, h2, h3, h4, h5, h6, article, section': {
        'color': '#000000'
      },
      // Preserve link colors
      'a:not(:visited)': {
        'color': '#000cda'
      },
      'a:visited': {
        'color': '#551a8b'
      },
      // Remove shadows
      '*': {
        'box-shadow': 'none',
        'text-shadow': 'none'
      }
    }
  },
  
  'www.odysee.com': {
    selectors: {
      // Override CSS variables from base-theme.scss
      ':root': {
        '--color-white': '#A0A2A0',
        '--color-white-alt': '#A0A2A0',
        '--color-background': '#A0A2A0',
        '--color-card-background': '#A0A2A0',
        '--color-card-background-highlighted': '#A0A2A0',
        '--color-header-background': '#A0A2A0',
        '--color-gray-1': '#A0A2A0',
        '--color-gray-2': '#A0A2A0',
        '--color-gray-3': '#A0A2A0',
        '--color-button-alt-bg': '#A0A2A0',
        '--color-input-bg': '#A0A2A0',
        '--color-input-toggle-bg': '#A0A2A0',
        '--color-input-prefix-bg': '#A0A2A0',
        '--color-menu-background': '#A0A2A0',
        '--color-follow-bg': '#A0A2A0',
        '--color-view-bg': '#A0A2A0',
        '--color-thumbnail-background': '#A0A2A0',
        '--color-placeholder-background': '#A0A2A0',
        '--color-file-viewer-background': '#A0A2A0',
        '--color-tabs-background': '#A0A2A0',
        '--color-modal-background': '#A0A2A0',
        '--color-ads-background': '#A0A2A0',
        '--color-comment-highlighted': '#A0A2A0',
        '--color-comment-threadline': '#A0A2A0',
        '--color-link-focus-bg': '#A0A2A0',
        '--color-visibility-label': '#A0A2A0',
        '--color-hyperchat-4': '#A0A2A0'
      },
      // Same rules for www.odysee.com
      'body': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'html': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '#app': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.main, .content, .container': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'header, nav, .header, .navigation, .navbar': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.video-js, .vjs-tech': {
        'background-color': 'transparent',
        'background': 'transparent'
      },
      '.card, .claim-preview, .media, .content-wrapper': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.sidebar, aside': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.comments, .comment, .comment-list': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.modal, .dialog, .popup': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background: white" i], [style*="background-color: white" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background:#fff" i], [style*="background-color:#fff" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background: #fff" i], [style*="background-color: #fff" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="rgb(255, 255, 255)" i], [style*="rgb(255,255,255)" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'p, span, div, td, th, li, h1, h2, h3, h4, h5, h6, article, section': {
        'color': '#000000'
      },
      'a:not(:visited)': {
        'color': '#000cda'
      },
      'a:visited': {
        'color': '#551a8b'
      },
      '*': {
        'box-shadow': 'none',
        'text-shadow': 'none'
      }
    }
  },
  
  // DEFAULT FALLBACK FOR ALL UNDEFINED SITES
  '*.*': {
    selectors: {
      // NUCLEAR OPTION - BAN ALL BACKGROUNDS ON EVERYTHING
      '*': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0',
        'background-image': 'none',
        'box-shadow': 'none',
        'text-shadow': 'none'
      },
      // Force all text black except links
      '*:not(a)': {
        'color': '#000000'
      },
      // Links
      'a:not(:visited)': {
        'color': '#000cda'
      },
      'a:visited': {
        'color': '#551a8b'
      },
      // Keep images/videos visible
      'img, video, svg, canvas': {
        'background-color': 'transparent',
        'background': 'transparent'
      }
    }
  },
  
  'github.com': {
    selectors: {
      // Override CSS variables for theme colors
      ':root, [data-color-mode], [data-color-mode="light"], [data-color-mode="auto"]': {
        '--bgColor-default': '#A0A2A0',
        '--bgColor-muted': '#A0A2A0', 
        '--bgColor-inset': '#A0A2A0',
        '--bgColor-emphasis': '#A0A2A0',
        '--bgColor-inverse': '#A0A2A0',
        '--bgColor-neutral-muted': '#A0A2A0',
        '--bgColor-accent-muted': '#A0A2A0',
        '--color-canvas-default': '#A0A2A0',
        '--color-canvas-subtle': '#A0A2A0',
        '--color-canvas-inset': '#A0A2A0',
        '--color-neutral-muted': '#A0A2A0'
      },
      // Main backgrounds - replace white with gray
      'body': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.bg-white, .bg-gray-light': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Repository main content
      'main, .repository-content, .container-lg, .container-xl': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Code view backgrounds
      '.Box, .Box-body, .Box-header': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.blob-wrapper, .blob-code, .blob-code-inner': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Code table and line numbers
      '.js-file-line-container, .js-code-nav-container': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'table.highlight, .highlight': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.blob-num, .blob-code-content': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Code lines
      'td.blob-code, td.blob-num': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.blob-code-hunk, .blob-expanded': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // React code view components
      '[class*="react-code-"], .react-code-text, .react-code-lines': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.react-line-number, .react-code-line-contents': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // File browser
      '.js-navigation-item, .file-wrap': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Data attributes for color modes
      '[data-color-mode="light"] .Box': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[data-color-mode="auto"] .Box': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // CRITICAL: Replace gray syntax highlighting (comments) with brown
      '.pl-c, .pl-c1, .pl-c2': {  // Comment classes
        'color': '#6b3a07 !important'
      },
      '.text-gray-500, .text-gray-600, .text-gray-700': {
        'color': '#000000 !important'
      },
      // Replace any inline gray colors with brown for comments
      '[class*="pl-"][style*="color: #6a737d"], [class*="pl-"][style*="color: #969896"]': {
        'color': '#6b3a07 !important'
      },
      // Keep other syntax colors intact by NOT touching them
      // Green strings, blue keywords, etc. remain as-is
      
      // Fix general gray text (non-code)
      '.text-muted, .text-gray': {
        'color': '#000000'
      },
      // Remove shadows
      '*': {
        'box-shadow': 'none',
        'text-shadow': 'none'
      }
    }
  },
  
  'www.github.com': {
    selectors: {
      // Override CSS variables for theme colors
      ':root, [data-color-mode], [data-color-mode="light"], [data-color-mode="auto"]': {
        '--bgColor-default': '#A0A2A0',
        '--bgColor-muted': '#A0A2A0', 
        '--bgColor-inset': '#A0A2A0',
        '--bgColor-emphasis': '#A0A2A0',
        '--bgColor-inverse': '#A0A2A0',
        '--bgColor-neutral-muted': '#A0A2A0',
        '--bgColor-accent-muted': '#A0A2A0',
        '--color-canvas-default': '#A0A2A0',
        '--color-canvas-subtle': '#A0A2A0',
        '--color-canvas-inset': '#A0A2A0',
        '--color-neutral-muted': '#A0A2A0'
      },
      // Same rules for www.github.com
      'body': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.bg-white, .bg-gray-light': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'main, .repository-content, .container-lg, .container-xl': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.Box, .Box-body, .Box-header': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.blob-wrapper, .blob-code, .blob-code-inner': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Code table and line numbers
      '.js-file-line-container, .js-code-nav-container': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'table.highlight, .highlight': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.blob-num, .blob-code-content': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Code lines
      'td.blob-code, td.blob-num': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.blob-code-hunk, .blob-expanded': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // React code view components
      '[class*="react-code-"], .react-code-text, .react-code-lines': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.react-line-number, .react-code-line-contents': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.js-navigation-item, .file-wrap': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Data attributes for color modes
      '[data-color-mode="light"] .Box': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[data-color-mode="auto"] .Box': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.pl-c, .pl-c1, .pl-c2': {
        'color': '#6b3a07 !important'
      },
      '.text-gray-500, .text-gray-600, .text-gray-700': {
        'color': '#000000 !important'
      },
      '[class*="pl-"][style*="color: #6a737d"], [class*="pl-"][style*="color: #969896"]': {
        'color': '#6b3a07 !important'
      },
      '.text-muted, .text-gray': {
        'color': '#000000'
      },
      '*': {
        'box-shadow': 'none',
        'text-shadow': 'none'
      }
    }
  },
  
  'reseauinternational.net': {
    selectors: {
      // Main body and page wrapper
      'body': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0',
        'color': '#000000'  // Ensure text is black
      },
      '.tn-main-page-wrap': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Main content areas
      '.main-content-wrap, .tn-content-wrap, .tn-section-content-wrap': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Container elements
      '.tn-container, .container, .container-fluid': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Navigation and header
      '.main-nav-wrap, .main-nav-holder, .main-nav-inner': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Content blocks
      '.block1-content, .block2-content, .block3-content, .block4-content, .block6-content': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.big-carousel-content, .block-big-slider-content-wrap': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Article and post content
      '.module-post-content, .text-content-wrapper': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Comment sections
      '.comment-widget-content, .comment-widget-content-wrap': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Sidebar if present
      '.sidebar, .widget, .widget-content': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Form inputs that were styled white
      'input[type="text"], input[type="email"], input[type="password"], textarea, select': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Any elements with inline white background styles
      '[style*="background-color: white"], [style*="background: white"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background-color:#fff"], [style*="background:#fff"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background-color: #fff"], [style*="background: #fff"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Fix gray text on gray background - turn gray text black
      'p, span, div, td, th, li, h1, h2, h3, h4, h5, h6, article, section': {
        'color': '#000000'
      },
      // Specific gray text colors to override
      '[style*="color: #777"], [style*="color: #888"], [style*="color: #999"], [style*="color: #aaa"], [style*="color: #bbb"], [style*="color: #ccc"]': {
        'color': '#000000 !important'
      },
      '[style*="color: rgb(119"], [style*="color: rgb(136"], [style*="color: rgb(153"], [style*="color: rgb(170"], [style*="color: rgb(187"], [style*="color: rgb(204"]': {
        'color': '#000000 !important'
      },
      // Preserve link colors
      'a:not(:visited)': {
        'color': '#000cda'
      },
      'a:visited': {
        'color': '#551a8b'
      },
      // Remove shadows
      '*': {
        'box-shadow': 'none',
        'text-shadow': 'none'
      }
    }
  },
  
  'rt.com': {
    selectors: {
      // Main layout backgrounds - replace black with gray
      '.layout': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.layout__content, .layout__wide-content--black': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Header - replace black/white with gray
      '.header, .header__section': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.header__content': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0 !important'  // Override image backgrounds
      },
      // Content areas - replace various blacks and whites
      '.layout__content-before_decoration': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.layout__lightning, .layout__breaking_bg_color2d2d2d': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.layout__upper-content': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Article cards and content blocks
      '.card, .card__header, .card__summary': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Media blocks
      '.media, .media__item': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Footer
      '.footer, .footer-top': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Any remaining white backgrounds
      '[style*="background-color: rgb(255, 255, 255)"], [style*="background: rgb(255, 255, 255)"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background-color:#fff"], [style*="background:#fff"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Any remaining black backgrounds
      '[style*="background-color: rgb(0, 0, 0)"], [style*="background: rgb(0, 0, 0)"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background-color:#000"], [style*="background:#000"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Fix text visibility - ensure black text
      'p, span, div, td, th, li, h1, h2, h3, h4, h5, h6, article, section': {
        'color': '#000000'
      },
      // Fix white text that was on green backgrounds
      '[style*="color: #FFF"], [style*="color:#FFF"], [style*="color: #fff"], [style*="color:#fff"]': {
        'color': '#000000 !important'
      },
      '[style*="color: white"], [style*="color:white"]': {
        'color': '#000000 !important'
      },
      // Fix any white text that was on black backgrounds
      '[style*="color: white"], [style*="color: #fff"], [style*="color: rgb(255, 255, 255)"]': {
        'color': '#000000 !important'
      },
      // Preserve link colors
      'a:not(:visited)': {
        'color': '#000cda'
      },
      'a:visited': {
        'color': '#551a8b'
      },
      // Remove shadows
      '*': {
        'box-shadow': 'none',
        'text-shadow': 'none'
      }
    }
  },
  
  'www.rt.com': {
    selectors: {
      // Same rules for www.rt.com
      '.layout': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.layout__content, .layout__wide-content--black': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.header, .header__section': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.header__content': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0 !important'
      },
      '.layout__content-before_decoration': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.layout__lightning, .layout__breaking_bg_color2d2d2d': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.layout__upper-content': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.card, .card__header, .card__summary': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.media, .media__item': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.footer, .footer-top': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background-color: rgb(255, 255, 255)"], [style*="background: rgb(255, 255, 255)"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background-color:#fff"], [style*="background:#fff"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background-color: rgb(0, 0, 0)"], [style*="background: rgb(0, 0, 0)"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background-color:#000"], [style*="background:#000"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '*': {
        'box-shadow': 'none',
        'text-shadow': 'none'
      }
    }
  },
  
  'online-go.com': {
    selectors: {
      // Main backgrounds - replace white with gray
      'body.light #main-content, body.light #main-content > div, body.light body, body.light html': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0',
        'color': '#000000'  // Ensure text is black
      },
      // Navigation bar
      '.NavBar, header.NavBar': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Navigation menu dropdowns
      'body.light header.NavBar .Menu .Menu-children': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'header.NavBar .Menu:hover .Menu-children': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Menu items on hover
      'body.light header.NavBar .Menu .MenuLink:hover': {
        'background-color': '#909090',  // Slightly darker gray for hover
        'background': '#909090'
      },
      // Mobile/hamburger menu
      'body.light header.NavBar.hamburger-expanded > .left .Menu': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // General menu containers
      '.Menu, .Menu-children': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Dropdown menus and selects
      'body.light select, body.light option': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Replace shade5 (nearly white) backgrounds
      'body.light .bg-shade5': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Replace shade4 (very light gray) backgrounds
      'body.light .bg-shade4': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Game board container backgrounds
      '.Game, .game-container, .game-view': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Player cards/info areas
      '.player-container, .player-card, .player-info': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Chat and side panels
      '.chat-container, .side-panel, .right-col': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Game chat components
      '.GameChat, .chat-log-container, .chat-input-container': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.ChatIndicator, .chat-line-container': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Log player container inside chat
      '.log-player-container, .GameChat .log-player-container': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Chat header and its inputs
      '.ChatHeader, body.light .ChatHeader': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'body.light .ChatHeader input': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Chat log area
      '.chat-log, .GameChat .chat-log': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Input group (container for chat input)
      '.input-group, .chat-input-container.input-group': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Chat input fields
      '.chat-input, .chat-input.main, input.chat-input': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Chat tabs if present
      '.chat-tabs, #gotv-container .chat-tabs': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'body.light .react-tabs__tab--selected': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Dock (right sidebar/toolbar)
      '.Dock': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.Dock > *, .Dock .dock-content': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Modal/dialog backgrounds
      '.Modal, .modal-content, .dialog': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Tooltips and popovers
      '.tooltip, .popover, .dropdown-menu': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Input fields
      'body.light input, body.light textarea': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Fix gray text visibility
      'p, span, div, td, th, li, h1, h2, h3, h4, h5, h6': {
        'color': '#000000'
      },
      // Preserve link colors
      'a:not(:visited)': {
        'color': '#000cda'
      },
      'a:visited': {
        'color': '#551a8b'
      },
      // Remove shadows
      '*': {
        'box-shadow': 'none',
        'text-shadow': 'none'
      }
    }
  },
  
  'www.phoronix.com': {
    selectors: {
      // Main body and page backgrounds
      'body': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'html': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Content background wrapper
      '#content-bg, #content': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Main content area
      '#main-wrap, #main': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Container elements
      '.wcontainer': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Article details sections
      '.details': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Header areas (except the dark header which is already dark)
      '#headerwrap': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Menu wrap - replace green background
      '#menuwrap': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Any white-styled elements
      '[style*="background: white"], [style*="background-color: white"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background:#fff"], [style*="background-color:#fff"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background: #fff"], [style*="background-color: #fff"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Replace green #348755 backgrounds with gray (including capital B)
      '[style*="background: #348755"], [style*="background-color: #348755"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background:#348755"], [style*="background-color:#348755"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="Background: #348755"], [style*="Background-color: #348755"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="Background:#348755"], [style*="Background-color:#348755"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Replace green RGB equivalent of #348755
      '[style*="rgb(52, 135, 85)"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="rgb(52,135,85)"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Fix text visibility - ensure black text
      'p, span, div, td, th, li, h1, h2, h3, h4, h5, h6, article, section': {
        'color': '#000000'
      },
      // Fix white text that was on green backgrounds
      '[style*="color: #FFF"], [style*="color:#FFF"], [style*="color: #fff"], [style*="color:#fff"]': {
        'color': '#000000 !important'
      },
      '[style*="color: white"], [style*="color:white"]': {
        'color': '#000000 !important'
      },
      // Preserve link colors
      'a:not(:visited)': {
        'color': '#000cda'
      },
      'a:visited': {
        'color': '#551a8b'
      },
      // Remove shadows
      '*': {
        'box-shadow': 'none',
        'text-shadow': 'none'
      }
    }
  },
  
  'phoronix.com': {
    selectors: {
      // Same rules without www
      'body': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'html': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '#content-bg, #content': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '#main-wrap, #main': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.wcontainer': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.details': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '#headerwrap': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Menu wrap - replace green background
      '#menuwrap': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background: white"], [style*="background-color: white"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background:#fff"], [style*="background-color:#fff"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background: #fff"], [style*="background-color: #fff"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Replace green #348755 backgrounds with gray (including capital B)
      '[style*="background: #348755"], [style*="background-color: #348755"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background:#348755"], [style*="background-color:#348755"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="Background: #348755"], [style*="Background-color: #348755"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="Background:#348755"], [style*="Background-color:#348755"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Replace green RGB equivalent of #348755
      '[style*="rgb(52, 135, 85)"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="rgb(52,135,85)"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'p, span, div, td, th, li, h1, h2, h3, h4, h5, h6, article, section': {
        'color': '#000000'
      },
      'a:not(:visited)': {
        'color': '#000cda'
      },
      'a:visited': {
        'color': '#551a8b'
      },
      '*': {
        'box-shadow': 'none',
        'text-shadow': 'none'
      }
    }
  },
  
  'www.mobygames.com': {
    selectors: {
      // Main body and html
      'body': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'html': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Root and main containers
      '#root': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '#main': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Container classes
      '.container': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Grid layouts
      '.grid-split-2-1': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Header - override blue background and CSS variable
      'header': {
        '--navbar-bg-color': '#A0A2A0',
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Navbar elements
      '.navbar-menu, .navbar-search, .navbar-links': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Trending sections
      '#trending-games, #trending-companies, #trending-people, #trending-groups': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // List groups
      '.list-group': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Notice backgrounds
      '.bg-notice': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Any inline white backgrounds (case insensitive)
      '[style*="background: white" i], [style*="background-color: white" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background:#fff" i], [style*="background-color:#fff" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background: #fff" i], [style*="background-color: #fff" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background:#ffffff" i], [style*="background-color:#ffffff" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background: #ffffff" i], [style*="background-color: #ffffff" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="rgb(255, 255, 255)" i], [style*="rgb(255,255,255)" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Replace specific MacRumors colors
      '[style*="#e2eaf3" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="#600" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="#d7e2ef" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="#d5d3cd" i]': {
        'border-color': '#A0A2A0'
      },
      // Additional MacRumors colors from CSS
      '[style*="#f2f1ec" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="#00ad00" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="#009400" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="#152a44" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="#b21b1b" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Replace blue header background
      '[style*="rgb(22, 116, 192)" i], [style*="rgb(22,116,192)" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Fix text visibility
      'p, span, div, td, th, li, h1, h2, h3, h4, h5, h6, article, section': {
        'color': '#000000'
      },
      // Fix muted text specifically
      '.text-muted': {
        'color': '#000000 !important'
      },
      // Preserve link colors
      'a:not(:visited)': {
        'color': '#000cda'
      },
      'a:visited': {
        'color': '#551a8b'
      },
      // Remove shadows
      '*': {
        'box-shadow': 'none',
        'text-shadow': 'none'
      }
    }
  },
  
  'mobygames.com': {
    selectors: {
      // Same rules without www
      'body': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'html': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '#root': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '#main': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.container': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.grid-split-2-1': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.navbar-menu, .navbar-search, .navbar-links': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '#trending-games, #trending-companies, #trending-people, #trending-groups': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.list-group': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.bg-notice': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background: white" i], [style*="background-color: white" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background:#fff" i], [style*="background-color:#fff" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background: #fff" i], [style*="background-color: #fff" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background:#ffffff" i], [style*="background-color:#ffffff" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background: #ffffff" i], [style*="background-color: #ffffff" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="rgb(255, 255, 255)" i], [style*="rgb(255,255,255)" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Replace specific MacRumors colors
      '[style*="#e2eaf3" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="#600" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="#d7e2ef" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="#d5d3cd" i]': {
        'border-color': '#A0A2A0'
      },
      // Additional MacRumors colors from CSS
      '[style*="#f2f1ec" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="#00ad00" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="#009400" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="#152a44" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="#b21b1b" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Replace blue header background
      '[style*="rgb(22, 116, 192)" i], [style*="rgb(22,116,192)" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'p, span, div, td, th, li, h1, h2, h3, h4, h5, h6, article, section': {
        'color': '#000000'
      },
      '.text-muted': {
        'color': '#000000 !important'
      },
      'a:not(:visited)': {
        'color': '#000cda'
      },
      'a:visited': {
        'color': '#551a8b'
      },
      '*': {
        'box-shadow': 'none',
        'text-shadow': 'none'
      }
    }
  },
  
  'www.macrumors.com': {
    selectors: {
      // NUCLEAR OPTION - BAN ALL BACKGROUNDS ON EVERYTHING
      '*': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0',
        'background-image': 'none',
        'box-shadow': 'none',
        'text-shadow': 'none'
      },
      // Force all text black except links
      '*:not(a)': {
        'color': '#000000'
      },
      // Links
      'a:not(:visited)': {
        'color': '#000cda'
      },
      'a:visited': {
        'color': '#551a8b'
      },
      // Keep images/videos visible
      'img, video, svg, canvas': {
        'background-color': 'transparent',
        'background': 'transparent'
      }
    }
  },
  
  'macrumors.com': {
    selectors: {
      // NUCLEAR OPTION - BAN ALL BACKGROUNDS ON EVERYTHING
      '*': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0',
        'background-image': 'none',
        'box-shadow': 'none',
        'text-shadow': 'none'
      },
      // Force all text black except links
      '*:not(a)': {
        'color': '#000000'
      },
      // Links
      'a:not(:visited)': {
        'color': '#000cda'
      },
      'a:visited': {
        'color': '#551a8b'
      },
      // Keep images/videos visible
      'img, video, svg, canvas': {
        'background-color': 'transparent',
        'background': 'transparent'
      }
    }
  },
  
  'www.youtube.com': {
    selectors: {
      // Override CSS variables that might define white
      ':root': {
        '--yt-spec-base-background': '#A0A2A0',
        '--yt-spec-raised-background': '#A0A2A0',
        '--yt-spec-menu-background': '#A0A2A0',
        '--yt-spec-inverted-background': '#A0A2A0',
        '--ytd-searchbox-background': '#A0A2A0',
        '--paper-tabs-selection-bar-color': '#000cda',
        '--yt-spec-general-background-a': '#A0A2A0',
        '--yt-spec-general-background-b': '#A0A2A0',
        '--yt-spec-general-background-c': '#A0A2A0'
      },
      '#root': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '#maincontent': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '#front': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.contentWrap--qVat7btW': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.app--1VMnmaJb': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.navigation--OvbFtbNW': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.nav--1MeUHLb_': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.primary--1a8mg7a_': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.primaryInner--3QIb3zWn': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.roundup': {
        'background-image': 'none',
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '#roundups, #roundups-desktop': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '#featured_slider, #featured_slider2': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '.button': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Secondary sections with light blue background
      '.secondary--2SVUcmMQ': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0',
        'border-bottom-color': '#A0A2A0'
      },
      // Titlebar with dark red background
      '.titlebar--3N4MCKxL': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Widget with light blue background
      '.widget--3ewetJyi': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Content area - fix border color too
      '.content--2u3grYDr': {
        'border-color': '#A0A2A0'
      },
      // Article elements - use attribute selectors for any class containing these words
      '[class*="article"], [class*="Article"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[class*="post"], [class*="Post"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[class*="byline"], [class*="Byline"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[class*="content"], [class*="Content"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[class*="expandWrap"], [class*="ExpandWrap"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0',
        'box-shadow': 'none'
      },
      // Specific colors from the CSS
      '[class*="article"]': {
        'background': '#A0A2A0 !important'  // Override #fff
      },
      '[class*="byline"]': {
        'background': '#A0A2A0 !important'  // Override #f2f1ec
      },
      '[class*="expandWrap"]': {
        'background': '#A0A2A0 !important',  // Override #fff
        'box-shadow': 'none !important'
      },
      // Title elements with dark blue background
      '[class*="title"], [class*="Title"]': {
        'background-color': '#A0A2A0 !important',
        'background': '#A0A2A0 !important'
      },
      '.post--article [class*="title"]': {
        'background-color': '#A0A2A0 !important',
        'background': '#A0A2A0 !important'
      },
      // FORCE that specific compound selector
      // Match the STABLE PREFIXES - this catches ALL variations
      '[class^="post--"][class*="article"] [class^="title--"]': {
        'background-color': '#A0A2A0 !important',
        'background': '#A0A2A0 !important'
      },
      // Just target the ACTUAL problem - elements styled with #152a44
      '[style*="background-color: #152a44" i], [style*="background:#152a44" i], [style*="background: #152a44" i]': {
        'background-color': '#A0A2A0 !important',
        'background': '#A0A2A0 !important'
      },
      // And RGB equivalent of #152a44 (21, 42, 68)
      '[style*="rgb(21, 42, 68)" i], [style*="rgb(21,42,68)" i]': {
        'background-color': '#A0A2A0 !important',
        'background': '#A0A2A0 !important'
      },
      // Ribbon elements
      '[class*="ribbon"], [class*="Ribbon"]': {
        'background-color': '#A0A2A0 !important',
        'background': '#A0A2A0 !important'
      },
      '[style*="linear-gradient" i], [style*="radial-gradient" i]': {
        'background-image': 'none',
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="-webkit-gradient" i], [style*="-moz-linear-gradient" i]': {
        'background-image': 'none',
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background: white" i], [style*="background-color: white" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background:#fff" i], [style*="background-color:#fff" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background: #fff" i], [style*="background-color: #fff" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background:#ffffff" i], [style*="background-color:#ffffff" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="background: #ffffff" i], [style*="background-color: #ffffff" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="rgb(255, 255, 255)" i], [style*="rgb(255,255,255)" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Replace specific MacRumors colors
      '[style*="#e2eaf3" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="#600" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="#d7e2ef" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="#d5d3cd" i]': {
        'border-color': '#A0A2A0'
      },
      // Additional MacRumors colors from CSS
      '[style*="#f2f1ec" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="#00ad00" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="#009400" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="#152a44" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '[style*="#b21b1b" i]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'p, span, div, td, th, li, h1, h2, h3, h4, h5, h6, article, section': {
        'color': '#000000'
      },
      'a:not(:visited)': {
        'color': '#000cda'
      },
      'a:visited': {
        'color': '#551a8b'
      },
      '*': {
        'box-shadow': 'none',
        'text-shadow': 'none'
      }
    }
  },
  
  'www.youtube.com': {
    selectors: {
      // Override CSS variables that might define white
      ':root': {
        '--yt-spec-base-background': '#A0A2A0',
        '--yt-spec-raised-background': '#A0A2A0',
        '--yt-spec-menu-background': '#A0A2A0',
        '--yt-spec-inverted-background': '#A0A2A0',
        '--ytd-searchbox-background': '#A0A2A0',
        '--paper-tabs-selection-bar-color': '#000cda',
        '--yt-spec-general-background-a': '#A0A2A0',
        '--yt-spec-general-background-b': '#A0A2A0',
        '--yt-spec-general-background-c': '#A0A2A0'
      },
      // Main page background
      'ytd-app, #content': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Two-column layout backgrounds
      'ytd-two-column-browse-results-renderer': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Page manager background
      'ytd-page-manager': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Browse feed
      'ytd-browse[page-subtype="channels"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Rich grid renderer (video grid container)
      'ytd-rich-grid-renderer': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Section list renderer
      'ytd-section-list-renderer': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Individual video items - keep transparent so thumbnails show
      'ytd-rich-item-renderer': {
        'background-color': 'transparent',
        'background': 'transparent'
      },
      // Masthead/header - MUST override the !important white background
      'ytd-masthead, ytd-masthead.shell': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '#masthead-container': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // HTML root to override white background
      'html': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Channel header and banner areas
      '#channel-header, #channel-header-container, ytd-channel-sub-menu-renderer': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Channel banner and all its components
      'ytd-c4-tabbed-header-renderer, ytd-channel-tagline-renderer': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Banner inner containers
      '#channel-container, #header, .banner-visible-area, #channel-header-banner': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // App header and its containers
      'tp-yt-app-header, tp-yt-app-header-layout, tp-yt-app-toolbar': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Paper tabs container (the tabs below banner)
      'tp-yt-paper-tabs, yt-page-navigation-container, #tabsContainer': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Inner header content
      '#header-inner, #tabs-inner-container, #tabs-container': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Tab menu under channel header
      'tp-yt-paper-tabs, ytd-feed-filter-chip-bar-renderer, #chips': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Sidebar (when present)
      '#guide-renderer, ytd-guide-renderer, #guide-content': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Mini guide (collapsed sidebar)
      'ytd-mini-guide-renderer': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      // Remove any box shadows for cleaner look
      '*': {
        'box-shadow': 'none',
        'text-shadow': 'none'
      }
    }
  },
  
  'youtube.com': {
    selectors: {
      // Override CSS variables
      ':root': {
        '--yt-spec-base-background': '#A0A2A0',
        '--yt-spec-raised-background': '#A0A2A0',
        '--yt-spec-menu-background': '#A0A2A0',
        '--yt-spec-inverted-background': '#A0A2A0',
        '--ytd-searchbox-background': '#A0A2A0',
        '--paper-tabs-selection-bar-color': '#000cda',
        '--yt-spec-general-background-a': '#A0A2A0',
        '--yt-spec-general-background-b': '#A0A2A0',
        '--yt-spec-general-background-c': '#A0A2A0'
      },
      // Same rules for youtube.com (without www)
      'ytd-app, #content': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'ytd-two-column-browse-results-renderer': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'ytd-page-manager': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'ytd-browse[page-subtype="channels"]': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'ytd-rich-grid-renderer': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'ytd-section-list-renderer': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'ytd-rich-item-renderer': {
        'background-color': 'transparent',
        'background': 'transparent'
      },
      'ytd-masthead, ytd-masthead.shell': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '#masthead-container': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'html': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '#channel-header, #channel-header-container, ytd-channel-sub-menu-renderer': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'ytd-c4-tabbed-header-renderer, ytd-channel-tagline-renderer': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '#channel-container, #header, .banner-visible-area, #channel-header-banner': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'tp-yt-app-header, tp-yt-app-header-layout, tp-yt-app-toolbar': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '#header-inner, #tabs-inner-container, #tabs-container': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'tp-yt-paper-tabs, ytd-feed-filter-chip-bar-renderer, #chips': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '#guide-renderer, ytd-guide-renderer, #guide-content': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      'ytd-mini-guide-renderer': {
        'background-color': '#A0A2A0',
        'background': '#A0A2A0'
      },
      '*': {
        'box-shadow': 'none',
        'text-shadow': 'none'
      }
    }
  }
};

// Global styles that apply to all sites (things like scrollbars, selection)
const GLOBAL_STYLES = `
  /* Scrollbars */
  ::-webkit-scrollbar {
    width: 12px !important;
    height: 12px !important;
  }
  
  ::-webkit-scrollbar-track {
    background: #A0A2A0 !important;
  }
  
  ::-webkit-scrollbar-thumb {
    background: #606060 !important;
    border: 1px solid #000000 !important;
  }
  
  /* Selection */
  ::selection {
    background-color: #000cda !important;
    color: #FFFFFF !important;
  }
  
  ::-moz-selection {
    background-color: #000cda !important;
    color: #FFFFFF !important;
  }
  
  /* Video overlays - preserve original backgrounds and images */
  .video-overlay {
    background-color: initial !important;
    background: initial !important;
    background-image: initial !important;
  }
`;

// Main function - inject styles
function injectStyles() {
  const hostname = window.location.hostname;
  let css = GLOBAL_STYLES;
  
  // Check if we have modifications for this site, otherwise use fallback
  const siteMods = SITE_MODIFICATIONS[hostname] || SITE_MODIFICATIONS['*.*'];
  if (siteMods && siteMods.selectors) {
    css += '\n\n/* Site-specific modifications */\n';
    
    // Build CSS from the configuration
    for (const [selector, styles] of Object.entries(siteMods.selectors)) {
      css += `${selector} {\n`;
      for (const [property, value] of Object.entries(styles)) {
        css += `  ${property}: ${value} !important;\n`;
      }
      css += '}\n\n';
    }
  }
  
  // Only inject if we have something to inject
  if (css.trim()) {
    const style = document.createElement('style');
    style.id = 'web-sanitizer-main';
    style.textContent = css;
    
    if (document.head) {
      document.head.appendChild(style);
    } else if (document.documentElement) {
      document.documentElement.appendChild(style);
    } else {
      setTimeout(injectStyles, 0);
      return;
    }
    
    console.log(`Web Sanitizer: Applied modifications for ${hostname}`);
  }
}

// Run immediately
injectStyles();

// Also run when DOM is ready (in case we were too early)
if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', injectStyles);
}