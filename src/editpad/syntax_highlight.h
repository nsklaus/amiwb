#ifndef SYNTAX_HIGHLIGHT_H
#define SYNTAX_HIGHLIGHT_H

#include <stdint.h>
#include <stdbool.h>

// Color definitions for syntax elements
typedef enum {
    SYNTAX_NORMAL = 0,      // Default text (black)
    SYNTAX_COMMENT,         // Comments (brown #6b3a07)
    SYNTAX_STRING,          // Strings (dark red)
    SYNTAX_KEYWORD,         // Keywords (blue)
    SYNTAX_TYPE,            // Types (dark green)
    SYNTAX_PREPROCESSOR,    // Preprocessor (magenta)
    SYNTAX_NUMBER,          // Numbers (dark cyan)
    SYNTAX_FUNCTION,        // Function names (dark blue)
    SYNTAX_OPERATOR,        // Operators
    SYNTAX_MAX
} SyntaxColor;

// Language types we support
typedef enum {
    LANG_NONE = 0,
    LANG_C,
    LANG_CPP,
    LANG_PYTHON,
    LANG_SHELL,
    LANG_MAKEFILE,
    LANG_JAVASCRIPT,
    LANG_MAX
} Language;

// Syntax highlighter state
typedef struct {
    Language lang;
    uint32_t colors[SYNTAX_MAX];  // RGB colors for each syntax element
    
    // State for multi-line constructs
    bool in_multiline_comment;
    bool in_multiline_string;
} SyntaxHighlight;

// Initialize syntax highlighter
SyntaxHighlight* syntax_create(void);
void syntax_destroy(SyntaxHighlight *sh);

// Load colors from editpadrc config file
void syntax_load_colors(SyntaxHighlight *sh, const char *config_path);

// Detect language from filename
Language syntax_detect_language(const char *filename);

// Set language explicitly
void syntax_set_language(SyntaxHighlight *sh, Language lang);

// Highlight a line of text
// Returns array of colors (one per character) - caller must free
SyntaxColor* syntax_highlight_line(SyntaxHighlight *sh, const char *line, int line_num);

// Get RGB color for syntax element
uint32_t syntax_get_color(SyntaxHighlight *sh, SyntaxColor color);

#endif // SYNTAX_HIGHLIGHT_H