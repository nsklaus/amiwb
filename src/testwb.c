// testwb.c - Standalone icon viewer using Pixman for rendering and FreeType for labels

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <pixman-1/pixman.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>

// Constants for icon layout and behavior
#define ICON_SPACING 74
#define MAX_PER_COLUMN 6
#define DOUBLE_CLICK_TIME 300
#define MOTION_THROTTLE_MS 5
#define MENUBAR_HEIGHT 20
#define Y_OFFSET 40
#define LABEL_OFFSET 5
#define ICON_HEADER_SIZE 20

// Icon image data
typedef struct {
    XImage *image;  // X11 image for icon pixels
    int width;      // Icon width in pixels
    int height;     // Icon height in pixels
} Icon;

// File icon data (simplified for testwb, removed unused fields like window, gc, shape_mask)
typedef struct {
    char *filename;     // File or directory name
    char *path;         // Directory path
    char *label;        // Display label
    Icon icon;          // Icon image data
    int x, y;           // Position relative to parent
    int width, height;  // Dimensions for layout
    int type;           // 0=file, 1=dir, 2=.info
} FileIcon;

// Global variables
FileIcon *icons = NULL;
int num_icons = 0;
int dragged_icon_index = -1;
int drag_start_x = 0;
int drag_start_y = 0;
int win_start_x = 0;
int win_start_y = 0;
static long last_motion_time = 0;
static long last_click_time = 0;
static int click_count = 0;
static int window_width = 400;
static int window_height = 300;
XImage *render_image = NULL;
pixman_image_t *pixman_buffer = NULL;
pixman_image_t **pixman_icon_buffers = NULL;
GC buffer_gc = NULL;
unsigned long label_color = 0xFFFFFFFF;
unsigned long bg_color = 0xFFC0C0C0;
char *def_tool_path = NULL;
char *def_drawer_path = NULL;
FT_Library ft_library;
FT_Face ft_face;

// Get current time in milliseconds (for timing clicks and motion throttling)
long get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// X11 error handler (logs errors without crashing)
int x_error_handler(Display *dpy, XErrorEvent *e) {
    char error_text[256];
    XGetErrorText(dpy, e->error_code, error_text, sizeof(error_text));
    fprintf(stderr, "[x_error_handler] X11 error: %s (code=%d, request=%d, resource=0x%lx)\n",
            error_text, e->error_code, e->request_code, e->resourceid);
    return 0;
}

// Read 16-bit big-endian value (for icon header parsing)
static uint16_t read_be16(const uint8_t *p) {
    return (p[0] << 8) | p[1];
}

// Load icon file data (reads .info file into memory)
static int load_icon_file(const char *name, uint8_t **data, long *size) {
    FILE *fp = fopen(name, "rb");
    if (!fp) return 1;
    fseek(fp, 0, SEEK_END);
    *size = ftell(fp);
    rewind(fp);
    *data = malloc(*size);
    if (!*data || fread(*data, 1, *size, fp) != *size) {
        free(*data);
        fclose(fp);
        return 1;
    }
    fclose(fp);
    return 0;
}

// Parse icon header (extracts width, height, depth from .info header)
static int parse_icon_header(const uint8_t *header, long size, uint16_t *width, uint16_t *height, uint16_t *depth) {
    if (size < ICON_HEADER_SIZE) return 1;
    *width = read_be16(header + 4);
    *height = read_be16(header + 6);
    *depth = read_be16(header + 8);
    if (*width == 0 || *height == 0 || *depth == 0 || *depth > 8) return 1;
    return 0;
}

// Render icon into XImage (decodes planar bitmap to ARGB, using fixed palette)
static int render_icon(Display *dpy, Icon *icon, const uint8_t *data, uint16_t width, uint16_t height, uint16_t depth) {
    XVisualInfo vinfo;
    if (!XMatchVisualInfo(dpy, DefaultScreen(dpy), 32, TrueColor, &vinfo)) return 1;
    icon->image = XCreateImage(dpy, vinfo.visual, 32, ZPixmap, 0, malloc(width * height * 4), width, height, 32, 0);
    if (!icon->image) return 1;
    memset(icon->image->data, 0, width * height * 4);
    unsigned long colors[] = {0xFFAAAAAA, 0xFF000000, 0xFFFFFFFF, 0xFF6666BB, 0xFF999999, 0xFFBBBBBB, 0xFFBBAA99, 0xFFFFAA22};
    int row_bytes = ((width + 15) / 16) * 2;
    long plane_size = row_bytes * height;
    const uint8_t *planes = data;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int color = 0;
            for (int p = 0; p < depth; p++) {
                long offset = p * plane_size + y * row_bytes + (x >> 3);
                uint8_t byte = planes[offset];
                if (byte & (1 << (7 - (x & 7)))) color |= (1 << p);
            }
            XPutPixel(icon->image, x, y, colors[color & 7]);
        }
    }
    icon->width = width;
    icon->height = height;
    return 0;
}

// Load icon from .info file (loads, parses, renders icon data)
int load_icon(Display *dpy, const char *name, Icon *icon) {
    uint8_t *data;
    long size;
    if (load_icon_file(name, &data, &size)) return 1;
    int header_offset = (data[48] == 1 || data[48] == 2) ? 78 + 56 : 78;
    if (header_offset + ICON_HEADER_SIZE > size) {
        free(data);
        return 1;
    }
    uint16_t width, height, depth;
    if (parse_icon_header(data + header_offset, size - header_offset, &width, &height, &depth) ||
        render_icon(dpy, icon, data + header_offset + ICON_HEADER_SIZE, width, height, depth)) {
        free(data);
        return 1;
    }
    free(data);
    return 0;
}

// Convert XImage to Pixman image (copies data for compositing)
pixman_image_t *ximage_to_pixman(XImage *ximage) {
    if (!ximage) return NULL;
    int width = ximage->width;
    int height = ximage->height;
    int stride = width * 4;
    uint32_t *data = malloc(stride * height);
    if (!data) return NULL;
    memcpy(data, ximage->data, stride * height);
    pixman_image_t *image = pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, data, stride);
    if (!image) free(data);
    return image;
}

// Create Pixman image for text using FreeType (renders text with pre-multiplied alpha for correct blending)
pixman_image_t *create_text_image(const char *text, uint32_t color) {
    if (!ft_face) return NULL;
    int width = 0, height = ft_face->size->metrics.height >> 6;
    for (int i = 0; text[i]; i++) {
        if (FT_Load_Char(ft_face, text[i], FT_LOAD_RENDER)) continue;
        width += ft_face->glyph->advance.x >> 6;
    }
    if (width <= 0 || height <= 0) return NULL;
    pixman_image_t *text_image = pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, NULL, width * 4);
    if (!text_image) return NULL;
    uint32_t *data = pixman_image_get_data(text_image);
    memset(data, 0, width * height * 4);
    int x = 0;
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    for (int i = 0; text[i]; i++) {
        if (FT_Load_Char(ft_face, text[i], FT_LOAD_RENDER)) continue;
        FT_Bitmap *bitmap = &ft_face->glyph->bitmap;
        int y = height - bitmap->rows;
        for (int row = 0; row < bitmap->rows; row++) {
            for (int col = 0; col < bitmap->width; col++) {
                int px = x + col + ft_face->glyph->bitmap_left;
                int py = y + row;
                if (px >= 0 && px < width && py >= 0 && py < height) {
                    uint8_t alpha = bitmap->buffer[row * bitmap->pitch + col];
                    if (alpha == 0) continue;
                    uint8_t premul_r = (r * alpha) / 255;
                    uint8_t premul_g = (g * alpha) / 255;
                    uint8_t premul_b = (b * alpha) / 255;
                    data[py * width + px] = (alpha << 24) | (premul_r << 16) | (premul_g << 8) | premul_b;
                }
            }
        }
        x += ft_face->glyph->advance.x >> 6;
    }
    return text_image;
}

// Truncate filename for display (shortens long names with "...")
char *truncate_filename(const char *filename) {
    int len = strlen(filename);
    if (len <= 15) return strdup(filename);
    char *truncated = malloc(16);
    if (!truncated) return NULL;
    strncpy(truncated, filename, 12);
    truncated[12] = '\0';
    strcat(truncated, "...");
    return truncated;
}

// Parse .amiwbrc configuration (loads user preferences for colors, paths)
static void parse_config() {
    char config_path[512], *home = getenv("HOME");
    snprintf(config_path, sizeof(config_path), "%s/.amiwbrc", home);
    FILE *fp = fopen(config_path, "r");
    if (!fp) return;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *key = strtok(line, "="), *value = strtok(NULL, "\n");
        if (key && value) {
            while (*key == ' ') key++;
            while (*value == ' ') value++;
            if (strcmp(key, "desktop_label_color") == 0) {
                label_color = (strcmp(value, "white") == 0) ? 0xFFFFFFFF : (strcmp(value, "black") == 0) ? 0xFF000000 : 0xFFFFFFFF;
            } else if (strcmp(key, "def_tool_path") == 0) {
                free(def_tool_path);
                def_tool_path = strdup(value);
            } else if (strcmp(key, "def_drawer_path") == 0) {
                free(def_drawer_path);
                def_drawer_path = strdup(value);
            }
        }
    }
    fclose(fp);
}

// Scan directory for icons (loads and filters files, handles .info associations)
void scan_directory(Display *dpy, const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return;
int len = strlen(path);
    struct dirent *entry;
    char *files[1024];
    int file_types[1024];
    int file_count = 0;
    while ((entry = readdir(dir)) && file_count < 1024) {
        if (entry->d_name[0] == '.') continue;
        if (entry->d_type == DT_REG || entry->d_type == DT_DIR) {
            files[file_count] = strdup(entry->d_name);
            file_types[file_count] = (entry->d_type == DT_DIR) ? 1 : (len = strlen(entry->d_name), len > 5 && strcmp(entry->d_name + len - 5, ".info") == 0) ? 2 : 0;
            file_count++;
        }
    }
    closedir(dir);

    pixman_icon_buffers = calloc(file_count, sizeof(pixman_image_t *));
    if (!pixman_icon_buffers) {
        for (int i = 0; i < file_count; i++) free(files[i]);
        return;
    }

    for (int i = 0; i < file_count; i++) {
        size_t len = strlen(files[i]);
        char base_name[256];
        if (file_types[i] == 2) {
            if (len > 5) {
                memcpy(base_name, files[i], len - 5);
                base_name[len - 5] = '\0';
            } else {
                base_name[0] = '\0';
            }
            int has_match = 0;
            for (int j = 0; j < file_count; j++) {
                if (i != j && (file_types[j] == 0 || file_types[j] == 1) && strcmp(base_name, files[j]) == 0) {
                    has_match = 1;
                    break;
                }
            }
            if (has_match) {
                free(files[i]);
                continue;
            }
        }

        FileIcon *new_icons = realloc(icons, (num_icons + 1) * sizeof(FileIcon));
        if (!new_icons) {
            free(files[i]);
            continue;
        }
        icons = new_icons;
        FileIcon *icon = &icons[num_icons];
        icon->filename = strdup(files[i]);
        icon->path = strdup(path);
        icon->label = truncate_filename(files[i]);
        icon->type = file_types[i];

        char full_path[512];
        char *icon_path;
        if (file_types[i] == 2) {
            snprintf(full_path, sizeof(full_path), "%s/%s", path, files[i]);
            icon_path = full_path;
        } else {
            snprintf(full_path, sizeof(full_path), "%s/%s.info", path, files[i]);
            FILE *fp = fopen(full_path, "rb");
            if (fp) {
                fclose(fp);
                icon_path = full_path;
            } else {
                icon_path = file_types[i] == 1 ? def_drawer_path : def_tool_path;
            }
        }

        if (load_icon(dpy, icon_path, &icon->icon) != 0) {
            free(icon->filename);
            free(icon->path);
            free(icon->label);
            free(files[i]);
            continue;
        }

        pixman_icon_buffers[num_icons] = ximage_to_pixman(icon->icon.image);
        if (!pixman_icon_buffers[num_icons]) {
            XDestroyImage(icon->icon.image);
            free(icon->filename);
            free(icon->path);
            free(icon->label);
            free(files[i]);
            continue;
        }

        int label_width = 0;
        for (int j = 0; icon->label[j]; j++) {
            if (FT_Load_Char(ft_face, icon->label[j], FT_LOAD_RENDER)) continue;
            label_width += ft_face->glyph->advance.x >> 6;
        }
        icon->width = icon->icon.width > label_width ? icon->icon.width : label_width;
        icon->height = icon->icon.height + LABEL_OFFSET + (ft_face->size->metrics.height >> 6);
        num_icons++;
        free(files[i]);
    }
}

// Re-layout icons in columns (called only on resize if columns change)
void layout_icons() {
    for (int i = 0; i < num_icons; i++) {
        int column = i / MAX_PER_COLUMN;
        int row = i % MAX_PER_COLUMN;
        icons[i].x = column * ICON_SPACING + 10;
        icons[i].y = row * ICON_SPACING + Y_OFFSET;
    }
}

// Resize the rendering buffer (recreates image/buffer, relayout if needed)
void resize_buffer(Display *dpy, Window win, int new_width, int new_height) {
    if (new_width <= 0 || new_height <= 0) return;
    if (render_image) {
        XDestroyImage(render_image); // XDestroyImage frees the data if it owns it
        render_image = NULL;
    }
    if (pixman_buffer) {
        pixman_image_unref(pixman_buffer);
        pixman_buffer = NULL;
    }
    int old_columns = (window_width - 10) / ICON_SPACING;
    window_width = new_width > 100 ? new_width : 100;
    window_height = new_height > 100 ? new_height : 100;
    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, win, &attr)) return;
    char *data = malloc(window_width * window_height * 4); // Always allocate for 32-bit ARGB
    if (!data) return;
    render_image = XCreateImage(dpy, attr.visual, 32, ZPixmap, 0, data, window_width, window_height, 32, 0);
    if (!render_image) {
        free(data);
        return;
    }
    pixman_buffer = pixman_image_create_bits(PIXMAN_a8r8g8b8, window_width, window_height, (uint32_t *)data, window_width * 4);
    if (!pixman_buffer) {
        XDestroyImage(render_image);
        render_image = NULL;
        return;
    }
    XResizeWindow(dpy, win, window_width, window_height);
    int new_columns = (window_width - 10) / ICON_SPACING;
    if (new_columns != old_columns) layout_icons(); // Relayout only if columns change
}

// Render icons using Pixman (clears, composites icons/labels, copies to X)
void render_icons(Display *dpy, Window win) {
    if (!pixman_buffer || !render_image || !buffer_gc) return;

    // Clear buffer with background color
    pixman_color_t bg = {
        .red = ((bg_color >> 16) & 0xFF) * 257,
        .green = ((bg_color >> 8) & 0xFF) * 257,
        .blue = (bg_color & 0xFF) * 257,
        .alpha = 0xFFFF
    };
    pixman_image_t *bg_fill = pixman_image_create_solid_fill(&bg);
    if (!bg_fill) return;
    pixman_image_composite(PIXMAN_OP_SRC, bg_fill, NULL, pixman_buffer, 0, 0, 0, 0, 0, 0, window_width, window_height);
    pixman_image_unref(bg_fill);

    // Render all icons, drawing dragged icon last
    for (int i = 0; i < num_icons; i++) {
        if (i == dragged_icon_index) continue;
        if (pixman_icon_buffers[i]) {
            int x = icons[i].x;
            int y = icons[i].y;
            pixman_image_composite(PIXMAN_OP_OVER, pixman_icon_buffers[i], NULL, pixman_buffer,
                                   0, 0, 0, 0, x, y, icons[i].icon.width, icons[i].icon.height);
            pixman_image_t *text_image = create_text_image(icons[i].label, label_color);
            if (text_image) {
                int label_width = pixman_image_get_width(text_image);
                pixman_image_composite(PIXMAN_OP_OVER, text_image, NULL, pixman_buffer,
                                       0, 0, 0, 0, x + (icons[i].width - label_width) / 2,
                                       y + icons[i].icon.height + LABEL_OFFSET, label_width, pixman_image_get_height(text_image));
                pixman_image_unref(text_image);
            }
        }
    }
    if (dragged_icon_index >= 0 && pixman_icon_buffers[dragged_icon_index]) {
        int x = icons[dragged_icon_index].x;
        int y = icons[dragged_icon_index].y;
        pixman_image_composite(PIXMAN_OP_OVER, pixman_icon_buffers[dragged_icon_index], NULL, pixman_buffer,
                               0, 0, 0, 0, x, y, icons[dragged_icon_index].icon.width, icons[dragged_icon_index].icon.height);
        pixman_image_t *text_image = create_text_image(icons[dragged_icon_index].label, label_color);
        if (text_image) {
            int label_width = pixman_image_get_width(text_image);
            pixman_image_composite(PIXMAN_OP_OVER, text_image, NULL, pixman_buffer,
                                   0, 0, 0, 0, x + (icons[dragged_icon_index].width - label_width) / 2,
                                   y + icons[dragged_icon_index].icon.height + LABEL_OFFSET, label_width, pixman_image_get_height(text_image));
            pixman_image_unref(text_image);
        }
    }

    // Send to X server
    XSetErrorHandler(x_error_handler);
    XPutImage(dpy, win, buffer_gc, render_image, 0, 0, 0, 0, window_width, window_height);
    XSetErrorHandler(NULL);
    XSync(dpy, False);
}

// Free icon resources (cleans up memory for icon)
void free_icon(FileIcon *icon) {
    if (icon->icon.image) XDestroyImage(icon->icon.image);
    free(icon->filename);
    free(icon->path);
    free(icon->label);
}

// Main entry point (initializes, loops events, cleans up)
int main(int argc, char *argv[]) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "[main] Cannot open display\n");
        exit(1);
    }
    int screen = DefaultScreen(dpy);
    Window root = DefaultRootWindow(dpy);

    // Initialize FreeType
    if (FT_Init_FreeType(&ft_library)) {
        fprintf(stderr, "[main] Failed to initialize FreeType\n");
        XCloseDisplay(dpy);
        exit(1);
    }

    def_tool_path = strdup("/home/klaus/Sources/amiwb/icons/def_tool.info");
    def_drawer_path = strdup("/home/klaus/Sources/amiwb/icons/def_drawer.info");
    parse_config();

    const char *ft_font_path = "/usr/share/fonts/TTF/DejaVuSans.ttf";
    if (FT_New_Face(ft_library, ft_font_path, 0, &ft_face)) {
        fprintf(stderr, "[main] Failed to load TTF font: %s\n", ft_font_path);
        FT_Done_FreeType(ft_library);
        XCloseDisplay(dpy);
        exit(1);
    }
    FT_Set_Pixel_Sizes(ft_face, 0, 12);

    // Create window with 32-bit TrueColor visual for ARGB consistency
    XVisualInfo vinfo;
    if (!XMatchVisualInfo(dpy, screen, 32, TrueColor, &vinfo)) {
        fprintf(stderr, "[main] No 32-bit TrueColor visual available\n");
        FT_Done_Face(ft_face);
        FT_Done_FreeType(ft_library);
        XCloseDisplay(dpy);
        exit(1);
    }
    XSetWindowAttributes attrs;
    attrs.colormap = XCreateColormap(dpy, root, vinfo.visual, AllocNone);
    attrs.background_pixel = bg_color;
    attrs.border_pixel = 0;
    Window win = XCreateWindow(dpy, root, 0, MENUBAR_HEIGHT, window_width, window_height, 0,
                               vinfo.depth, InputOutput, vinfo.visual,
                               CWColormap | CWBackPixel | CWBorderPixel, &attrs);

    Atom no_shadow = XInternAtom(dpy, "_KDE_NET_WM_SHADOW", False);
    unsigned long no_shadow_value = 0;
    XChangeProperty(dpy, win, no_shadow, XA_CARDINAL, 32, PropModeReplace, (unsigned char*)&no_shadow_value, 1);

    // Initialize rendering buffer
    resize_buffer(dpy, win, window_width, window_height);
    if (!render_image || !pixman_buffer) {
        fprintf(stderr, "[main] Failed to initialize rendering buffer\n");
        FT_Done_Face(ft_face);
        FT_Done_FreeType(ft_library);
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        exit(1);
    }

    // Create GC
    buffer_gc = XCreateGC(dpy, win, 0, NULL);
    if (!buffer_gc) {
        fprintf(stderr, "[main] Failed to create buffer GC\n");
        resize_buffer(dpy, win, 0, 0); // Cleanup buffer
        FT_Done_Face(ft_face);
        FT_Done_FreeType(ft_library);
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        exit(1);
    }

    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask);
    XMapWindow(dpy, win);
    scan_directory(dpy, "/home/klaus/Sources/amiwb/icons/");
    if (num_icons == 0) fprintf(stderr, "[main] No icons loaded\n");
    layout_icons();
    render_icons(dpy, win);

    XEvent ev;
    while (1) {
        XNextEvent(dpy, &ev);
        if (ev.type == Expose) {
            render_icons(dpy, win);
        } else if (ev.type == ConfigureNotify) {
            if (ev.xconfigure.width != window_width || ev.xconfigure.height != window_height) {
                resize_buffer(dpy, win, ev.xconfigure.width, ev.xconfigure.height);
            }
            render_icons(dpy, win);
        } else if (ev.type == ButtonPress && ev.xbutton.button == Button1) {
            for (int i = 0; i < num_icons; i++) {
                if (ev.xbutton.x >= icons[i].x && ev.xbutton.x < icons[i].x + icons[i].width &&
                    ev.xbutton.y >= icons[i].y && ev.xbutton.y < icons[i].y + icons[i].height) {
                    long current_time = get_time_ms();
                    if (click_count == 0 || (current_time - last_click_time) > DOUBLE_CLICK_TIME) {
                        click_count = 1;
                        last_click_time = current_time;
                        dragged_icon_index = i;
                        drag_start_x = ev.xbutton.x;
                        drag_start_y = ev.xbutton.y;
                        win_start_x = icons[i].x;
                        win_start_y = icons[i].y;
                        XGrabPointer(dpy, win, False, PointerMotionMask | ButtonReleaseMask,
                                     GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
                        XSync(dpy, False);
                        last_motion_time = current_time;
                    } else if (click_count == 1 && (current_time - last_click_time) <= DOUBLE_CLICK_TIME) {
                        click_count = 0;
                        last_click_time = 0;
                        system("xterm &");
                    }
                    break;
                }
            }
        } else if (ev.type == MotionNotify && dragged_icon_index >= 0) {
            long current_time = get_time_ms();
            if (current_time - last_motion_time < MOTION_THROTTLE_MS) continue;
            last_motion_time = current_time;
            int new_x = ev.xmotion.x - (drag_start_x - win_start_x);
            int new_y = ev.xmotion.y - (drag_start_y - win_start_y);
            new_x = new_x < 0 ? 0 : new_x;
            new_y = new_y < 0 ? 0 : new_y;
            new_x = new_x + icons[dragged_icon_index].width > window_width ? window_width - icons[dragged_icon_index].width : new_x;
            new_y = new_y + icons[dragged_icon_index].height > window_height ? window_height - icons[dragged_icon_index].height : new_y;
            icons[dragged_icon_index].x = new_x;
            icons[dragged_icon_index].y = new_y;
            render_icons(dpy, win);
        } else if (ev.type == ButtonRelease && dragged_icon_index >= 0) {
            XUngrabPointer(dpy, CurrentTime);
            XAllowEvents(dpy, AsyncPointer, CurrentTime);
            dragged_icon_index = -1;
            last_motion_time = 0;
            last_click_time = 0;
            render_icons(dpy, win);
        }
    }

    // Cleanup
    for (int i = 0; i < num_icons; i++) {
        free_icon(&icons[i]);
        if (pixman_icon_buffers[i]) pixman_image_unref(pixman_icon_buffers[i]);
    }
    free(icons);
    free(pixman_icon_buffers);
    XFreeGC(dpy, buffer_gc);
    resize_buffer(dpy, win, 0, 0); // Cleanup buffer
    FT_Done_Face(ft_face);
    FT_Done_FreeType(ft_library);
    free(def_tool_path);
    free(def_drawer_path);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
