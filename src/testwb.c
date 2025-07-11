#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <pixman.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include "icon_loader.h"
#include "file_icon.h"
#include "config.h"
#include "logging.h"

// Constants for icon layout
#define ICON_SPACING 74
#define MAX_PER_COLUMN 6
#define DOUBLE_CLICK_TIME 300
#define MOTION_THROTTLE_MS 5
#define MENUBAR_HEIGHT 20
#define Y_OFFSET 40
#define LABEL_OFFSET 5

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
static long last_expose_time = 0;
static int expose_count = 0;
static int motion_count = 0;
static Bool needs_redraw = True;
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
char *font_name = NULL;
FT_Library ft_library;
FT_Face ft_face;
extern int debug_enabled;

// Get current time in milliseconds
long get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// X11 error handler
int x_error_handler(Display *dpy, XErrorEvent *e) {
    char error_text[256];
    XGetErrorText(dpy, e->error_code, error_text, sizeof(error_text));
    log_message(LOG_ERROR, "[x_error_handler] X11 error: %s (error_code=%d, request_code=%d, resource_id=0x%lx)",
                error_text, e->error_code, e->request_code, e->resourceid);
    return 0;
}

// Convert XImage to Pixman image
pixman_image_t *ximage_to_pixman(Display *dpy, XImage *ximage) {
    if (!ximage) {
        log_message(LOG_ERROR, "[ximage_to_pixman] Null XImage");
        return NULL;
    }
    int width = ximage->width;
    int height = ximage->height;
    int stride = width * 4;
    uint32_t *data = malloc(stride * height);
    if (!data) {
        log_message(LOG_ERROR, "[ximage_to_pixman] Failed to allocate buffer for %dx%d", width, height);
        return NULL;
    }
    memcpy(data, ximage->data, stride * height);
    pixman_image_t *image = pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, data, stride);
    if (!image) {
        log_message(LOG_ERROR, "[ximage_to_pixman] Failed to create Pixman image for %dx%d", width, height);
        free(data);
    } else {
        log_message(LOG_DEBUG, "[ximage_to_pixman] Created Pixman image: %dx%d, stride=%d", width, height, stride);
    }
    return image;
}

// Create Pixman image for text using FreeType
pixman_image_t *create_text_image(Display *dpy, const char *text, uint32_t color) {
    if (!ft_face) {
        log_message(LOG_ERROR, "[create_text_image] FreeType face not initialized");
        return NULL;
    }
    int width = 0, height = ft_face->size->metrics.height >> 6;
    for (int i = 0; text[i]; i++) {
        if (FT_Load_Char(ft_face, text[i], FT_LOAD_RENDER)) continue;
        width += ft_face->glyph->advance.x >> 6;
    }
    if (width <= 0 || height <= 0) {
        log_message(LOG_ERROR, "[create_text_image] Invalid text dimensions: %dx%d", width, height);
        return NULL;
    }
    pixman_image_t *text_image = pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, NULL, width * 4);
    if (!text_image) {
        log_message(LOG_ERROR, "[create_text_image] Failed to create Pixman text image");
        return NULL;
    }
    uint32_t *data = pixman_image_get_data(text_image);
    memset(data, 0, width * height * 4); // Clear to transparent
    int x = 0;
    for (int i = 0; text[i]; i++) {
        if (FT_Load_Char(ft_face, text[i], FT_LOAD_RENDER)) continue;
        FT_Bitmap *bitmap = &ft_face->glyph->bitmap;
        int y = height - bitmap->rows; // Align to bottom
        for (int row = 0; row < bitmap->rows; row++) {
            for (int col = 0; col < bitmap->width; col++) {
                int px = x + col + ft_face->glyph->bitmap_left;
                int py = y + row;
                if (px >= 0 && px < width && py >= 0 && py < height) {
                    uint8_t alpha = bitmap->buffer[row * bitmap->pitch + col];
                    uint32_t pixel = (alpha << 24) | (color & 0xFFFFFF);
                    data[py * width + px] = pixel;
                }
            }
        }
        x += ft_face->glyph->advance.x >> 6;
    }
    return text_image;
}

// Scan directory for icons
void scan_directory(Display *dpy, const char *path, Visual *visual, int depth) {
    DIR *dir = opendir(path);
    if (!dir) {
        log_message(LOG_ERROR, "[scan_directory] Failed to open directory: %s", path);
        return;
    }

    struct dirent *entry;
    char *files[1024];
    int file_types[1024];
    int file_count = 0;
    while ((entry = readdir(dir)) && file_count < 1024) {
        if (entry->d_name[0] == '.') continue;
        if (entry->d_type == DT_REG || entry->d_type == DT_DIR) {
            files[file_count] = strdup(entry->d_name);
            file_types[file_count] = (entry->d_type == DT_DIR) ? 1 : (strlen(entry->d_name) > 5 && strcmp(entry->d_name + strlen(entry->d_name) - 5, ".info") == 0) ? 2 : 0;
            file_count++;
        }
    }
    closedir(dir);

    pixman_icon_buffers = calloc(file_count, sizeof(pixman_image_t *));
    if (!pixman_icon_buffers) {
        log_message(LOG_ERROR, "[scan_directory] Failed to allocate pixman_icon_buffers");
        for (int i = 0; i < file_count; i++) free(files[i]);
        return;
    }

    for (int i = 0; i < file_count; i++) {
        FileIcon *new_icons = realloc(icons, (num_icons + 1) * sizeof(FileIcon));
        if (!new_icons) {
            log_message(LOG_ERROR, "[scan_directory] Memory allocation failed for icon %s", files[i]);
            free(files[i]);
            continue;
        }
        icons = new_icons;
        FileIcon *icon = &icons[num_icons];
        icon->filename = strdup(files[i]);
        icon->path = strdup(path);
        icon->label = truncate_filename(files[i]);
        icon->type = file_types[i];
        icon->window = None;
        icon->gc = NULL;
        icon->shape_mask = None;

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

        pixman_icon_buffers[num_icons] = ximage_to_pixman(dpy, icon->icon.image);
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
    }

    for (int i = 0; i < file_count; i++) {
        free(files[i]);
    }
    log_message(LOG_DEBUG, "[scan_directory] Loaded %d icons from %s", num_icons, path);
}

// Re-layout icons in columns
void layout_icons(Display *dpy) {
    for (int i = 0; i < num_icons; i++) {
        int column = i / MAX_PER_COLUMN;
        int row = i % MAX_PER_COLUMN;
        int new_x = column * ICON_SPACING + 10;
        int new_y = row * ICON_SPACING + Y_OFFSET;
        icons[i].x = new_x;
        icons[i].y = new_y;
    }
    needs_redraw = True;
    log_message(LOG_DEBUG, "[layout_icons] Laid out %d icons", num_icons);
}

// Resize the rendering buffer
void resize_buffer(Display *dpy, Window win, int new_width, int new_height) {
    if (new_width <= 0 || new_height <= 0) {
        log_message(LOG_ERROR, "[resize_buffer] Invalid dimensions: %dx%d", new_width, new_height);
        return;
    }
    if (render_image) {
        char *data = render_image->data;
        render_image->data = NULL;
        XDestroyImage(render_image);
        free(data);
        render_image = NULL;
    }
    if (pixman_buffer) {
        pixman_image_unref(pixman_buffer);
        pixman_buffer = NULL;
    }
    window_width = new_width > 100 ? new_width : 100;
    window_height = new_height > 100 ? new_height : 100;
    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, win, &attr)) {
        log_message(LOG_ERROR, "[resize_buffer] Failed to get window attributes");
        return;
    }
    render_image = XCreateImage(dpy, attr.visual, attr.depth, ZPixmap, 0,
                               malloc(window_width * window_height * 4), window_width, window_height, 32, 0);
    if (!render_image) {
        log_message(LOG_ERROR, "[resize_buffer] Failed to create render XImage");
        return;
    }
    pixman_buffer = pixman_image_create_bits(PIXMAN_a8r8g8b8, window_width, window_height, NULL, window_width * 4);
    if (!pixman_buffer) {
        log_message(LOG_ERROR, "[resize_buffer] Failed to create Pixman buffer");
        free(render_image->data);
        XDestroyImage(render_image);
        render_image = NULL;
        return;
    }
    log_message(LOG_DEBUG, "[resize_buffer] Resized buffer to %dx%d, depth=%d", window_width, window_height, attr.depth);
    XResizeWindow(dpy, win, window_width, window_height);
    needs_redraw = True;
    layout_icons(dpy);
}

// Render icons using Pixman
void render_icons(Display *dpy, Window win, int clip_x, int clip_y, int clip_width, int clip_height) {
    if (!pixman_buffer || !render_image || !buffer_gc) {
        log_message(LOG_ERROR, "[render_icons] Invalid resources: pixman_buffer=%p, render_image=%p, buffer_gc=%p",
                    pixman_buffer, render_image, buffer_gc);
        return;
    }

    // Use full window for rendering
    clip_x = 0;
    clip_y = 0;
    clip_width = window_width;
    clip_height = window_height;

    // Clear buffer with background color
    pixman_color_t bg = {
        .red = ((bg_color >> 16) & 0xFF) * 257,
        .green = ((bg_color >> 8) & 0xFF) * 257,
        .blue = (bg_color & 0xFF) * 257,
        .alpha = 0xFFFF
    };
    pixman_image_t *bg_fill = pixman_image_create_solid_fill(&bg);
    if (!bg_fill) {
        log_message(LOG_ERROR, "[render_icons] Failed to create background fill");
        return;
    }
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
            pixman_image_t *text_image = create_text_image(dpy, icons[i].label, label_color);
            if (text_image) {
                int label_width = 0;
                for (int j = 0; icons[i].label[j]; j++) {
                    if (FT_Load_Char(ft_face, icons[i].label[j], FT_LOAD_RENDER)) continue;
                    label_width += ft_face->glyph->advance.x >> 6;
                }
                pixman_image_composite(PIXMAN_OP_OVER, text_image, NULL, pixman_buffer,
                                       0, 0, 0, 0, x + (icons[i].width - label_width) / 2,
                                       y + icons[i].icon.height + LABEL_OFFSET, pixman_image_get_width(text_image), pixman_image_get_height(text_image));
                pixman_image_unref(text_image);
            }
        }
    }
    if (dragged_icon_index >= 0 && pixman_icon_buffers[dragged_icon_index]) {
        int x = icons[dragged_icon_index].x;
        int y = icons[dragged_icon_index].y;
        pixman_image_composite(PIXMAN_OP_OVER, pixman_icon_buffers[dragged_icon_index], NULL, pixman_buffer,
                               0, 0, 0, 0, x, y, icons[dragged_icon_index].icon.width, icons[dragged_icon_index].icon.height);
        pixman_image_t *text_image = create_text_image(dpy, icons[dragged_icon_index].label, label_color);
        if (text_image) {
            int label_width = 0;
            for (int j = 0; icons[dragged_icon_index].label[j]; j++) {
                if (FT_Load_Char(ft_face, icons[dragged_icon_index].label[j], FT_LOAD_RENDER)) continue;
                label_width += ft_face->glyph->advance.x >> 6;
            }
            pixman_image_composite(PIXMAN_OP_OVER, text_image, NULL, pixman_buffer,
                                   0, 0, 0, 0, x + (icons[dragged_icon_index].width - label_width) / 2,
                                   y + icons[dragged_icon_index].icon.height + LABEL_OFFSET, pixman_image_get_width(text_image), pixman_image_get_height(text_image));
            pixman_image_unref(text_image);
        }
    }

    // Copy Pixman buffer to XImage
    uint32_t *src = pixman_image_get_data(pixman_buffer);
    uint32_t *dst = (uint32_t *)render_image->data;
    if (src && dst) {
        memcpy(dst, src, window_width * window_height * 4);
        log_message(LOG_DEBUG, "[render_icons] Copied Pixman buffer to XImage");
    } else {
        log_message(LOG_ERROR, "[render_icons] Failed to copy buffer: src=%p, dst=%p", src, dst);
        return;
    }

    // Send to X server
    XSetErrorHandler(x_error_handler);
    Bool sent = XPutImage(dpy, win, buffer_gc, render_image, 0, 0, 0, 0, window_width, window_height);
    XSetErrorHandler(NULL);
    if (!sent) {
        log_message(LOG_ERROR, "[render_icons] XPutImage failed");
    } else {
        log_message(LOG_DEBUG, "[render_icons] Sent image to window, size=%dx%d", window_width, window_height);
    }
    XSync(dpy, False);
    needs_redraw = False;
}

// Main entry point
int main(int argc, char *argv[]) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        log_message(LOG_ERROR, "[main] Cannot open display");
        exit(1);
    }
    int screen = DefaultScreen(dpy);
    Window root = DefaultRootWindow(dpy);

    // Initialize FreeType
    if (FT_Init_FreeType(&ft_library)) {
        log_message(LOG_ERROR, "[main] Failed to initialize FreeType");
        XCloseDisplay(dpy);
        exit(1);
    }
    if (FT_New_Face(ft_library, "/usr/share/fonts/TTF/DejaVuSans.ttf", 0, &ft_face)) {
        log_message(LOG_ERROR, "[main] Failed to load TTF font");
        FT_Done_FreeType(ft_library);
        XCloseDisplay(dpy);
        exit(1);
    }
    FT_Set_Pixel_Sizes(ft_face, 0, 12);

    // Create window with dynamic visual/depth
    XVisualInfo vinfo;
    XSetWindowAttributes attrs;
    attrs.background_pixel = bg_color;
    attrs.border_pixel = 0;
    Window win;
    Visual *visual = NULL;
    int depth = 0;
    if (XMatchVisualInfo(dpy, screen, 24, TrueColor, &vinfo)) {
        visual = vinfo.visual;
        depth = vinfo.depth;
        attrs.colormap = XCreateColormap(dpy, root, visual, AllocNone);
        win = XCreateWindow(dpy, root, 0, MENUBAR_HEIGHT, window_width, window_height, 0,
                            depth, InputOutput, visual,
                            CWColormap | CWBackPixel | CWBorderPixel, &attrs);
        log_message(LOG_DEBUG, "[main] Created TrueColor window: depth=%d, visual=0x%lx", depth, (unsigned long)visual);
    } else {
        log_message(LOG_WARNING, "[main] 24-bit TrueColor visual not available, using default");
        visual = DefaultVisual(dpy, screen);
        depth = DefaultDepth(dpy, screen);
        win = XCreateSimpleWindow(dpy, root, 0, MENUBAR_HEIGHT, window_width, window_height, 0,
                                  BlackPixel(dpy, screen), bg_color);
        log_message(LOG_DEBUG, "[main] Created simple window: depth=%d", depth);
    }

    Atom no_shadow = XInternAtom(dpy, "_KDE_NET_WM_SHADOW", False);
    unsigned long no_shadow_value = 0;
    XChangeProperty(dpy, win, no_shadow, XA_CARDINAL, 32, PropModeReplace, (unsigned char*)&no_shadow_value, 1);

    // Initialize rendering buffer
    render_image = XCreateImage(dpy, visual, depth, ZPixmap, 0, malloc(window_width * window_height * 4),
                                window_width, window_height, 32, 0);
    if (!render_image) {
        log_message(LOG_ERROR, "[main] Failed to create render XImage");
        FT_Done_Face(ft_face);
        FT_Done_FreeType(ft_library);
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        exit(1);
    }
    pixman_buffer = pixman_image_create_bits(PIXMAN_a8r8g8b8, window_width, window_height, NULL, window_width * 4);
    if (!pixman_buffer) {
        log_message(LOG_ERROR, "[main] Failed to create Pixman buffer");
        free(render_image->data);
        XDestroyImage(render_image);
        FT_Done_Face(ft_face);
        FT_Done_FreeType(ft_library);
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        exit(1);
    }
    log_message(LOG_DEBUG, "[main] Created Pixman buffer: %dx%d, depth=%d", window_width, window_height, depth);

    // Create GC
    buffer_gc = XCreateGC(dpy, win, 0, NULL);
    if (!buffer_gc) {
        log_message(LOG_ERROR, "[main] Failed to create buffer GC");
        free(render_image->data);
        XDestroyImage(render_image);
        pixman_image_unref(pixman_buffer);
        FT_Done_Face(ft_face);
        FT_Done_FreeType(ft_library);
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        exit(1);
    }

    // Load configuration
    def_tool_path = strdup("/home/klaus/Sources/amiwb/icons/def_tool.info");
    def_drawer_path = strdup("/home/klaus/Sources/amiwb/icons/def_drawer.info");
    parse_amiwbrc(dpy, screen, NULL, NULL, &label_color, &bg_color, &def_tool_path, &def_drawer_path, NULL, &font_name, NULL);

    XSetWindowBackground(dpy, win, bg_color);
    XClearWindow(dpy, win);
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask);
    XMapWindow(dpy, win);
    log_init(debug_enabled);
    scan_directory(dpy, "/home/klaus/Sources/amiwb/icons", visual, depth);
    if (num_icons == 0) {
        log_message(LOG_ERROR, "[main] No icons loaded");
    }
    layout_icons(dpy);
    render_icons(dpy, win, 0, 0, 0, 0);

    XEvent ev;
    XEvent next_ev;
    while (1) {
        XNextEvent(dpy, &ev);
        if (ev.type == Expose) {
            long current_time = get_time_ms();
            expose_count++;
            if (current_time - last_expose_time >= 1000) {
                log_message(LOG_DEBUG, "[main] Expose events per second: %d", expose_count);
                expose_count = 0;
                last_expose_time = current_time;
            }
            needs_redraw = True;
            log_message(LOG_DEBUG, "[main] Expose event: x=%d, y=%d, width=%d, height=%d",
                        ev.xexpose.x, ev.xexpose.y, ev.xexpose.width, ev.xexpose.height);
            render_icons(dpy, win, 0, 0, 0, 0);
        } else if (ev.type == ConfigureNotify) {
            if (ev.xconfigure.width != window_width || ev.xconfigure.height != window_height) {
                log_message(LOG_INFO, "[main] ConfigureNotify: resized to width=%d, height=%d", ev.xconfigure.width, ev.xconfigure.height);
                resize_buffer(dpy, win, ev.xconfigure.width, ev.xconfigure.height);
            }
            needs_redraw = True;
            render_icons(dpy, win, 0, 0, 0, 0);
        } else if (ev.type == ButtonPress && ev.xbutton.button == Button1) {
            log_message(LOG_DEBUG, "[main] ButtonPress: x=%d, y=%d", ev.xbutton.x, ev.xbutton.y);
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
                        log_message(LOG_DEBUG, "[main] Icon %d drag start at (%d, %d), mouse at (%d, %d)",
                                    i, win_start_x, win_start_y, drag_start_x, drag_start_y);
                        XGrabPointer(dpy, win, False, PointerMotionMask | ButtonReleaseMask,
                                     GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
                        XSync(dpy, False);
                        last_motion_time = current_time;
                        last_expose_time = current_time;
                        expose_count = 0;
                        motion_count = 0;
                    } else if (click_count == 1 && (current_time - last_click_time) <= DOUBLE_CLICK_TIME) {
                        click_count = 0;
                        last_click_time = 0;
                        log_message(LOG_DEBUG, "[main] Launching xterm for %s", icons[i].filename);
                        system("xterm &");
                    }
                    break;
                }
            }
        } else if (ev.type == MotionNotify && dragged_icon_index >= 0) {
            while (XCheckTypedEvent(dpy, MotionNotify, &next_ev)) {
                ev = next_ev;
            }
            long current_time = get_time_ms();
            if (current_time - last_motion_time < MOTION_THROTTLE_MS) {
                continue;
            }
            motion_count++;
            if (current_time - last_expose_time >= 1000) {
                log_message(LOG_DEBUG, "[main] Motion events per second: %d", motion_count);
                log_message(LOG_DEBUG, "[main] Expose events per second: %d", expose_count);
                motion_count = 0;
                expose_count = 0;
                last_expose_time = current_time;
            }
            last_motion_time = current_time;
            XMotionEvent *e = &ev.xmotion;
            int new_x = e->x - (drag_start_x - win_start_x);
            int new_y = e->y - (drag_start_y - win_start_y);
            if (new_x < 0) new_x = 0;
            if (new_y < 0) new_y = 0;
            if (new_x + icons[dragged_icon_index].width > window_width)
                new_x = window_width - icons[dragged_icon_index].width;
            if (new_y + icons[dragged_icon_index].height > window_height)
                new_y = window_height - icons[dragged_icon_index].height;
            icons[dragged_icon_index].x = new_x;
            icons[dragged_icon_index].y = new_y;
            log_message(LOG_DEBUG, "[main] Moved icon %d to (%d, %d)", dragged_icon_index, new_x, new_y);
            needs_redraw = True;
            render_icons(dpy, win, 0, 0, 0, 0);
        } else if (ev.type == ButtonRelease && dragged_icon_index >= 0) {
            XUngrabPointer(dpy, CurrentTime);
            XAllowEvents(dpy, AsyncPointer, CurrentTime);
            dragged_icon_index = -1;
            last_motion_time = 0;
            last_click_time = 0;
            log_message(LOG_DEBUG, "[main] Motion events per second: %d", motion_count);
            log_message(LOG_DEBUG, "[main] Expose events per second: %d", expose_count);
            motion_count = 0;
            expose_count = 0;
            needs_redraw = True;
            render_icons(dpy, win, 0, 0, 0, 0);
        }
    }

    for (int i = 0; i < num_icons; i++) {
        log_message(LOG_DEBUG, "[main] Cleaning up icon %d: %s", i, icons[i].filename);
        if (icons[i].icon.image) {
            XDestroyImage(icons[i].icon.image);
            icons[i].icon.image = NULL;
        }
        if (pixman_icon_buffers[i]) {
            uint32_t *data = (uint32_t *)pixman_image_get_data(pixman_icon_buffers[i]);
            pixman_image_unref(pixman_icon_buffers[i]);
            free(data);
            pixman_icon_buffers[i] = NULL;
        }
        free(icons[i].filename);
        free(icons[i].path);
        free(icons[i].label);
    }
    free(icons);
    free(pixman_icon_buffers);
    if (buffer_gc) XFreeGC(dpy, buffer_gc);
    if (render_image) {
        char *data = render_image->data;
        render_image->data = NULL;
        XDestroyImage(render_image);
        free(data);
    }
    if (pixman_buffer) {
        pixman_image_unref(pixman_buffer);
    }
    if (ft_face) FT_Done_Face(ft_face);
    if (ft_library) FT_Done_FreeType(ft_library);
    if (def_tool_path) free(def_tool_path);
    if (def_drawer_path) free(def_drawer_path);
    if (font_name) free(font_name);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    log_message(LOG_DEBUG, "[main] Program terminated normally");
    return 0;
}
