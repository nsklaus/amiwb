#include "syntax_highlight.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <regex.h>

// Default colors that work well on gray background
static const uint32_t default_colors[SYNTAX_MAX] = {
    [SYNTAX_NORMAL]       = 0x000000,  // Black
    [SYNTAX_COMMENT]      = 0x6b3a07,  // Brown (your preference)
    [SYNTAX_STRING]       = 0xAA4444,  // Dark red
    [SYNTAX_KEYWORD]      = 0x0000EE,  // Blue
    [SYNTAX_TYPE]         = 0x00AA00,  // Dark green
    [SYNTAX_PREPROCESSOR] = 0xAA00AA,  // Magenta
    [SYNTAX_NUMBER]       = 0x00AAAA,  // Dark cyan
    [SYNTAX_FUNCTION]     = 0x0000AA,  // Dark blue
    [SYNTAX_OPERATOR]     = 0x000000,  // Black
};

// C keywords
static const char *c_keywords[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if",
    "int", "long", "register", "return", "short", "signed", "sizeof", "static",
    "struct", "switch", "typedef", "union", "unsigned", "void", "volatile", "while",
    "_Bool", "_Complex", "_Imaginary", "inline", "restrict", NULL
};

// C types
static const char *c_types[] = {
    "FILE", "size_t", "ssize_t", "pid_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "int8_t", "int16_t", "int32_t", "int64_t", "bool", "true", "false",
    "NULL", NULL
};

// Python keywords
static const char *python_keywords[] = {
    "and", "as", "assert", "break", "class", "continue", "def", "del",
    "elif", "else", "except", "finally", "for", "from", "global", "if",
    "import", "in", "is", "lambda", "nonlocal", "not", "or", "pass",
    "raise", "return", "try", "while", "with", "yield", "True", "False", "None", NULL
};

// Shell keywords - TODO: Implement shell highlighting
// static const char *shell_keywords[] = {
//     "if", "then", "else", "elif", "fi", "case", "esac", "for", "while",
//     "do", "done", "function", "return", "break", "continue", "exit", "export",
//     "local", "readonly", "shift", "source", "alias", "unalias", NULL
// };

// Create syntax highlighter
SyntaxHighlight* syntax_create(void) {
    SyntaxHighlight *sh = calloc(1, sizeof(SyntaxHighlight));
    if (!sh) return NULL;
    
    // Copy default colors
    memcpy(sh->colors, default_colors, sizeof(default_colors));
    
    sh->lang = LANG_NONE;
    sh->in_multiline_comment = false;
    sh->in_multiline_string = false;
    
    return sh;
}

// Destroy syntax highlighter
void syntax_destroy(SyntaxHighlight *sh) {
    if (sh) free(sh);
}

// Load colors from config file
void syntax_load_colors(SyntaxHighlight *sh, const char *config_path) {
    if (!sh || !config_path) return;
    
    FILE *f = fopen(config_path, "r");
    if (!f) return;
    
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64], value[64];
        
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n') continue;
        
        // Parse key = value
        if (sscanf(line, "%63s = %63s", key, value) != 2) continue;
        
        // Convert hex color to uint32_t
        uint32_t color = 0;
        if (value[0] == '#') {
            color = strtol(value + 1, NULL, 16);
        }
        
        // Map config keys to our color indices
        if (strcmp(key, "syntax.comment") == 0) {
            sh->colors[SYNTAX_COMMENT] = color;
        } else if (strcmp(key, "syntax.string") == 0) {
            sh->colors[SYNTAX_STRING] = color;
        } else if (strcmp(key, "syntax.keyword") == 0) {
            sh->colors[SYNTAX_KEYWORD] = color;
        } else if (strcmp(key, "syntax.type") == 0) {
            sh->colors[SYNTAX_TYPE] = color;
        } else if (strcmp(key, "syntax.preprocessor") == 0) {
            sh->colors[SYNTAX_PREPROCESSOR] = color;
        } else if (strcmp(key, "syntax.number") == 0) {
            sh->colors[SYNTAX_NUMBER] = color;
        } else if (strcmp(key, "syntax.function") == 0) {
            sh->colors[SYNTAX_FUNCTION] = color;
        } else if (strcmp(key, "syntax.operator") == 0) {
            sh->colors[SYNTAX_OPERATOR] = color;
        } else if (strcmp(key, "syntax.normal") == 0) {
            sh->colors[SYNTAX_NORMAL] = color;
        }
    }
    
    fclose(f);
}

// Detect language from filename
Language syntax_detect_language(const char *filename) {
    if (!filename) return LANG_NONE;
    
    // Find extension
    const char *ext = strrchr(filename, '.');
    if (!ext) {
        // Check for special files without extension
        const char *base = strrchr(filename, '/');
        if (!base) base = filename;
        else base++;
        
        if (strcmp(base, "Makefile") == 0 || strcmp(base, "makefile") == 0) {
            return LANG_MAKEFILE;
        }
        return LANG_NONE;
    }
    
    ext++; // Skip the dot
    
    // C/C++
    if (strcmp(ext, "c") == 0 || strcmp(ext, "h") == 0) return LANG_C;
    if (strcmp(ext, "cpp") == 0 || strcmp(ext, "cc") == 0 || 
        strcmp(ext, "cxx") == 0 || strcmp(ext, "hpp") == 0) return LANG_CPP;
    
    // Python
    if (strcmp(ext, "py") == 0) return LANG_PYTHON;
    
    // Shell
    if (strcmp(ext, "sh") == 0 || strcmp(ext, "bash") == 0) return LANG_SHELL;
    
    // JavaScript
    if (strcmp(ext, "js") == 0 || strcmp(ext, "jsx") == 0) return LANG_JAVASCRIPT;
    
    // Makefile
    if (strcmp(ext, "mk") == 0) return LANG_MAKEFILE;
    
    // Markdown
    if (strcmp(ext, "md") == 0 || strcmp(ext, "markdown") == 0) return LANG_MARKDOWN;
    
    return LANG_NONE;
}

// Set language explicitly
void syntax_set_language(SyntaxHighlight *sh, Language lang) {
    if (!sh) return;
    sh->lang = lang;
    sh->in_multiline_comment = false;
    sh->in_multiline_string = false;
}

// Check if word is a keyword
static bool is_keyword(const char *word, const char **keywords) {
    if (!word || !keywords) return false;
    
    for (int i = 0; keywords[i]; i++) {
        if (strcmp(word, keywords[i]) == 0) return true;
    }
    return false;
}

// Check if character is word boundary
static bool is_word_boundary(char c) {
    return !isalnum(c) && c != '_';
}

// Extract word at position
static bool extract_word(const char *line, int pos, char *word, int max_len) {
    // Find word start
    int start = pos;
    while (start > 0 && !is_word_boundary(line[start - 1])) start--;
    
    // Find word end
    int end = pos;
    while (line[end] && !is_word_boundary(line[end])) end++;
    
    int len = end - start;
    if (len <= 0 || len >= max_len) return false;
    
    strncpy(word, line + start, len);
    word[len] = '\0';
    return true;
}

// Highlight C/C++ line
static SyntaxColor* highlight_c_line(SyntaxHighlight *sh, const char *line, int line_num) {
    int len = strlen(line);
    SyntaxColor *colors = calloc(len + 1, sizeof(SyntaxColor));
    if (!colors) return NULL;
    
    // Initialize all to normal
    for (int i = 0; i < len; i++) {
        colors[i] = SYNTAX_NORMAL;
    }
    
    // State tracking
    bool in_string = false;
    bool in_char = false;
    bool in_comment = sh->in_multiline_comment;
    char string_delim = 0;
    
    for (int i = 0; i < len; i++) {
        // Handle multi-line comments
        if (in_comment) {
            colors[i] = SYNTAX_COMMENT;
            if (i < len - 1 && line[i] == '*' && line[i + 1] == '/') {
                colors[i + 1] = SYNTAX_COMMENT;
                in_comment = false;
                sh->in_multiline_comment = false;
                i++;
            }
            continue;
        }
        
        // Start multi-line comment
        if (i < len - 1 && line[i] == '/' && line[i + 1] == '*') {
            in_comment = true;
            sh->in_multiline_comment = true;
            colors[i] = SYNTAX_COMMENT;
            colors[i + 1] = SYNTAX_COMMENT;
            i++;
            continue;
        }
        
        // Single-line comment
        if (i < len - 1 && line[i] == '/' && line[i + 1] == '/') {
            for (int j = i; j < len; j++) {
                colors[j] = SYNTAX_COMMENT;
            }
            break;
        }
        
        // String literals
        if (!in_char && (line[i] == '"' || (in_string && string_delim == '"'))) {
            if (!in_string) {
                in_string = true;
                string_delim = '"';
            } else if (line[i] == '"' && (i == 0 || line[i - 1] != '\\')) {
                in_string = false;
            }
            colors[i] = SYNTAX_STRING;
            continue;
        }
        
        // Character literals
        if (!in_string && line[i] == '\'') {
            if (!in_char) {
                in_char = true;
            } else if (i == 0 || line[i - 1] != '\\') {
                in_char = false;
            }
            colors[i] = SYNTAX_STRING;
            continue;
        }
        
        // If we're inside a string or char, color it
        if (in_string || in_char) {
            colors[i] = SYNTAX_STRING;
            continue;
        }
        
        // Preprocessor directives
        if (i == 0 && line[i] == '#') {
            for (int j = i; j < len; j++) {
                colors[j] = SYNTAX_PREPROCESSOR;
            }
            break;
        }
        
        // Numbers
        if (isdigit(line[i]) || (line[i] == '.' && i + 1 < len && isdigit(line[i + 1]))) {
            colors[i] = SYNTAX_NUMBER;
            // Color the whole number
            int j = i + 1;
            while (j < len && (isdigit(line[j]) || line[j] == '.' || 
                   line[j] == 'x' || line[j] == 'X' || 
                   (line[j] >= 'a' && line[j] <= 'f') ||
                   (line[j] >= 'A' && line[j] <= 'F') ||
                   line[j] == 'L' || line[j] == 'l' ||
                   line[j] == 'U' || line[j] == 'u' ||
                   line[j] == 'F' || line[j] == 'f')) {
                colors[j] = SYNTAX_NUMBER;
                j++;
            }
            i = j - 1;
            continue;
        }
        
        // Keywords and types
        if (isalpha(line[i]) || line[i] == '_') {
            char word[64];
            if (extract_word(line, i, word, sizeof(word))) {
                SyntaxColor color = SYNTAX_NORMAL;
                
                if (is_keyword(word, c_keywords)) {
                    color = SYNTAX_KEYWORD;
                } else if (is_keyword(word, c_types)) {
                    color = SYNTAX_TYPE;
                } else if (i + strlen(word) < len && line[i + strlen(word)] == '(') {
                    // Function call
                    color = SYNTAX_FUNCTION;
                }
                
                // Color the whole word
                for (int j = 0; j < strlen(word); j++) {
                    colors[i + j] = color;
                }
                i += strlen(word) - 1;
            }
        }
        
        // Operators
        if (strchr("+-*/%=<>!&|^~?:", line[i])) {
            colors[i] = SYNTAX_OPERATOR;
        }
    }
    
    return colors;
}

// Highlight Python line
static SyntaxColor* highlight_python_line(SyntaxHighlight *sh, const char *line, int line_num) {
    int len = strlen(line);
    SyntaxColor *colors = calloc(len + 1, sizeof(SyntaxColor));
    if (!colors) return NULL;
    
    // Initialize all to normal
    for (int i = 0; i < len; i++) {
        colors[i] = SYNTAX_NORMAL;
    }
    
    // State tracking
    bool in_string = false;
    bool in_triple_string = sh->in_multiline_string;
    char string_delim = 0;
    
    for (int i = 0; i < len; i++) {
        // Triple-quoted strings
        if (i < len - 2 && 
            ((line[i] == '"' && line[i+1] == '"' && line[i+2] == '"') ||
             (line[i] == '\'' && line[i+1] == '\'' && line[i+2] == '\''))) {
            if (!in_triple_string) {
                in_triple_string = true;
                sh->in_multiline_string = true;
                string_delim = line[i];
            } else if (line[i] == string_delim) {
                in_triple_string = false;
                sh->in_multiline_string = false;
            }
            colors[i] = colors[i+1] = colors[i+2] = SYNTAX_STRING;
            i += 2;
            continue;
        }
        
        if (in_triple_string) {
            colors[i] = SYNTAX_STRING;
            continue;
        }
        
        // Comments
        if (line[i] == '#') {
            for (int j = i; j < len; j++) {
                colors[j] = SYNTAX_COMMENT;
            }
            break;
        }
        
        // String literals
        if (line[i] == '"' || line[i] == '\'') {
            if (!in_string) {
                in_string = true;
                string_delim = line[i];
            } else if (line[i] == string_delim && (i == 0 || line[i - 1] != '\\')) {
                in_string = false;
            }
            colors[i] = SYNTAX_STRING;
            continue;
        }
        
        if (in_string) {
            colors[i] = SYNTAX_STRING;
            continue;
        }
        
        // Numbers
        if (isdigit(line[i]) || (line[i] == '.' && i + 1 < len && isdigit(line[i + 1]))) {
            colors[i] = SYNTAX_NUMBER;
            int j = i + 1;
            while (j < len && (isdigit(line[j]) || line[j] == '.' || line[j] == 'e' || line[j] == 'E')) {
                colors[j] = SYNTAX_NUMBER;
                j++;
            }
            i = j - 1;
            continue;
        }
        
        // Keywords
        if (isalpha(line[i]) || line[i] == '_') {
            char word[64];
            if (extract_word(line, i, word, sizeof(word))) {
                SyntaxColor color = SYNTAX_NORMAL;
                
                if (is_keyword(word, python_keywords)) {
                    color = SYNTAX_KEYWORD;
                } else if (i + strlen(word) < len && line[i + strlen(word)] == '(') {
                    // Function call
                    color = SYNTAX_FUNCTION;
                }
                
                for (int j = 0; j < strlen(word); j++) {
                    colors[i + j] = color;
                }
                i += strlen(word) - 1;
            }
        }
        
        // Operators
        if (strchr("+-*/%=<>!&|^~?:", line[i])) {
            colors[i] = SYNTAX_OPERATOR;
        }
    }
    
    return colors;
}

// Highlight markdown line - using same colors as C
static SyntaxColor* highlight_markdown_line(SyntaxHighlight *sh, const char *line, int line_num) {
    if (!sh || !line) return NULL;
    
    int len = strlen(line);
    SyntaxColor *colors = calloc(len + 1, sizeof(SyntaxColor));
    if (!colors) return NULL;
    
    // Initialize all to normal
    for (int i = 0; i < len; i++) {
        colors[i] = SYNTAX_NORMAL;
    }
    
    // Track if we're in a C code block
    static bool in_c_block = false;
    static bool in_python_block = false;
    static bool in_js_block = false;
    static bool in_generic_block = false;
    
    int i = 0;
    
    // Check for code block markers
    if (strncmp(line, "```", 3) == 0) {
        // Check if it's a language-specific block start
        if (len > 3) {
            if (strncmp(line + 3, "c", 1) == 0 && (len == 4 || !isalpha(line[4]))) {
                in_c_block = true;
                in_python_block = false;
                in_js_block = false;
                in_generic_block = false;
            } else if (strncmp(line + 3, "cpp", 3) == 0 || strncmp(line + 3, "c++", 3) == 0) {
                in_c_block = true;  // Use C highlighting for C++ too
                in_python_block = false;
                in_js_block = false;
                in_generic_block = false;
            } else if (strncmp(line + 3, "python", 6) == 0 || strncmp(line + 3, "py", 2) == 0) {
                in_python_block = true;
                in_c_block = false;
                in_js_block = false;
                in_generic_block = false;
            } else if (strncmp(line + 3, "javascript", 10) == 0 || strncmp(line + 3, "js", 2) == 0) {
                in_js_block = true;
                in_c_block = false;
                in_python_block = false;
                in_generic_block = false;
            } else if (len > 3) {
                // Some other language or generic code block
                in_generic_block = true;
                in_c_block = false;
                in_python_block = false;
                in_js_block = false;
            }
        } else {
            // End of code block
            if (in_c_block || in_python_block || in_js_block || in_generic_block) {
                in_c_block = false;
                in_python_block = false;
                in_js_block = false;
                in_generic_block = false;
            } else {
                // Start of generic code block
                in_generic_block = true;
            }
        }
        
        // Color the ``` markers
        for (i = 0; i < len; i++) {
            colors[i] = SYNTAX_COMMENT;
        }
        return colors;
    }
    
    // If we're inside a code block, highlight accordingly
    if (in_c_block) {
        // Use C highlighter for content
        SyntaxHighlight temp_sh = *sh;
        temp_sh.lang = LANG_C;
        SyntaxColor *c_colors = highlight_c_line(&temp_sh, line, line_num);
        if (c_colors) {
            free(colors);
            return c_colors;
        }
    } else if (in_python_block) {
        // Use Python highlighter
        SyntaxHighlight temp_sh = *sh;
        temp_sh.lang = LANG_PYTHON;
        SyntaxColor *py_colors = highlight_python_line(&temp_sh, line, line_num);
        if (py_colors) {
            free(colors);
            return py_colors;
        }
    } else if (in_js_block) {
        // JavaScript uses C highlighter
        SyntaxHighlight temp_sh = *sh;
        temp_sh.lang = LANG_JAVASCRIPT;
        SyntaxColor *js_colors = highlight_c_line(&temp_sh, line, line_num);
        if (js_colors) {
            free(colors);
            return js_colors;
        }
    } else if (in_generic_block) {
        // Generic code block - use STRING color
        for (i = 0; i < len; i++) {
            colors[i] = SYNTAX_STRING;
        }
        return colors;
    }
    
    // Headers (# ## ### etc) - use KEYWORD color (blue)
    if (line[0] == '#') {
        int level = 0;
        while (i < len && line[i] == '#' && level < 6) {
            colors[i] = SYNTAX_KEYWORD;
            i++;
            level++;
        }
        // Color the rest of the header line
        while (i < len) {
            colors[i] = SYNTAX_KEYWORD;
            i++;
        }
        return colors;
    }
    
    // Bullet lists (* - +) at start - use OPERATOR color
    if ((line[0] == '*' || line[0] == '-' || line[0] == '+') && 
        (len > 1 && line[1] == ' ')) {
        colors[0] = SYNTAX_OPERATOR;
        i = 1;
    }
    
    // Numbered lists (1. 2. etc) - use NUMBER color
    if (isdigit(line[0])) {
        int j = 0;
        while (j < len && isdigit(line[j])) j++;
        if (j < len && line[j] == '.') {
            for (int k = 0; k <= j; k++) {
                colors[k] = SYNTAX_NUMBER;
            }
            i = j + 1;
        }
    }
    
    // Process inline elements
    while (i < len) {
        // Bold (**text** or __text__) - use TYPE color (green)
        if ((i < len - 1) && 
            ((line[i] == '*' && line[i+1] == '*') || 
             (line[i] == '_' && line[i+1] == '_'))) {
            colors[i] = SYNTAX_TYPE;
            colors[i+1] = SYNTAX_TYPE;
            i += 2;
            // Find closing
            while (i < len - 1) {
                if ((line[i] == '*' && line[i+1] == '*') || 
                    (line[i] == '_' && line[i+1] == '_')) {
                    colors[i] = SYNTAX_TYPE;
                    colors[i+1] = SYNTAX_TYPE;
                    i += 2;
                    break;
                }
                colors[i] = SYNTAX_TYPE;
                i++;
            }
            continue;
        }
        
        // Italic (*text* or _text_) - use FUNCTION color (dark blue)
        if ((line[i] == '*' || line[i] == '_') && 
            (i == 0 || !isalnum(line[i-1]))) {
            char marker = line[i];
            colors[i] = SYNTAX_FUNCTION;
            i++;
            // Find closing
            while (i < len) {
                if (line[i] == marker && (i == len-1 || !isalnum(line[i+1]))) {
                    colors[i] = SYNTAX_FUNCTION;
                    i++;
                    break;
                }
                colors[i] = SYNTAX_FUNCTION;
                i++;
            }
            continue;
        }
        
        // Inline code `code` - use STRING color (dark red)
        if (line[i] == '`') {
            colors[i] = SYNTAX_STRING;
            i++;
            // Find closing backtick
            while (i < len) {
                colors[i] = SYNTAX_STRING;
                if (line[i] == '`') {
                    i++;
                    break;
                }
                i++;
            }
            continue;
        }
        
        // Links [text](url) - use PREPROCESSOR color (magenta)
        if (line[i] == '[') {
            colors[i] = SYNTAX_PREPROCESSOR;
            i++;
            // Find closing ]
            while (i < len && line[i] != ']') {
                colors[i] = SYNTAX_PREPROCESSOR;
                i++;
            }
            if (i < len && line[i] == ']') {
                colors[i] = SYNTAX_PREPROCESSOR;
                i++;
                // Check for (url)
                if (i < len && line[i] == '(') {
                    colors[i] = SYNTAX_PREPROCESSOR;
                    i++;
                    while (i < len && line[i] != ')') {
                        colors[i] = SYNTAX_PREPROCESSOR;
                        i++;
                    }
                    if (i < len && line[i] == ')') {
                        colors[i] = SYNTAX_PREPROCESSOR;
                        i++;
                    }
                }
            }
            continue;
        }
        
        // Blockquotes > - use COMMENT color (brown)
        if (i == 0 && line[i] == '>') {
            while (i < len) {
                colors[i] = SYNTAX_COMMENT;
                i++;
            }
            return colors;
        }
        
        i++;
    }
    
    return colors;
}

// Main highlight function
SyntaxColor* syntax_highlight_line(SyntaxHighlight *sh, const char *line, int line_num) {
    if (!sh || !line) return NULL;
    
    switch (sh->lang) {
        case LANG_C:
        case LANG_CPP:
            return highlight_c_line(sh, line, line_num);
            
        case LANG_PYTHON:
            return highlight_python_line(sh, line, line_num);
            
        case LANG_SHELL:
            // TODO: Implement shell highlighting
            break;
            
        case LANG_JAVASCRIPT:
            // JavaScript is similar to C for basic highlighting
            return highlight_c_line(sh, line, line_num);
            
        case LANG_MAKEFILE:
            // TODO: Implement makefile highlighting
            break;
            
        case LANG_MARKDOWN:
            return highlight_markdown_line(sh, line, line_num);
            
        default:
            break;
    }
    
    // No highlighting - return all normal
    int len = strlen(line);
    SyntaxColor *colors = calloc(len + 1, sizeof(SyntaxColor));
    if (colors) {
        for (int i = 0; i < len; i++) {
            colors[i] = SYNTAX_NORMAL;
        }
    }
    return colors;
}

// Get RGB color for syntax element
uint32_t syntax_get_color(SyntaxHighlight *sh, SyntaxColor color) {
    if (!sh || color >= SYNTAX_MAX) return 0x000000;
    return sh->colors[color];
}