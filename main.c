#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <math.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <errno.h>
#include <linux/input-event-codes.h> 
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <GL/gl.h>
#include "protocol/wlr-layer-shell-unstable-v1-client-protocol.h"
#include <linux/uinput.h>

// --- Macros and Basic Defines ---
#define INVALID_FINGER_ID -1

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

// Define MIN/MAX macros
#ifndef MAX
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

// Debug macro
#define D(fmt, ...) fprintf(stderr, "[DEBUG] %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)

// --- Core Data Structures and Enumerations ---

// Mappable Keys
typedef struct {
    int keycode;        // Linux input event code (e.g., KEY_A)
    const char *label;  // Human-readable label (e.g., "A", "Spc", "Ctrl")
} MappableKey;

// Widget System
#define WIDGET_TYPE_LIST \
  X(JOYSTICK, joystick)  \
  X(DPAD,     dpad)      \
  X(BUTTON,   button)

typedef enum {
  #define X(name, func) WIDGET_##name,
    WIDGET_TYPE_LIST
  #undef X
  WIDGET_MAX // Sentinel value
} WidgetType;

typedef struct {
    float x, y;
} Vec2;

typedef struct {
    GLubyte r, g, b, a;
} Color;

typedef struct {
    int id;
    WidgetType type;
    Vec2 normCenter;     // Normalized center [0..1]
    float normHalfSize;  // Normalized half-size

    // Calculated absolute values
    Vec2 absCenter;
    float absRadius; // Also used for button radius if needed
    Vec2 absTopLeft;
    float absSize;

    // Interaction state
    int controllingFinger; // Slot index controlling this widget, INVALID_FINGER_ID if none

    // Type-specific data
    union {
        // Joystick/DPad key mapping and press state for 4 directions
        struct {
            int keycode[4];            // Up, Down, Left, Right
            const char *mappedLabel[4];
        } analog;
        // Button data
        struct {
            int keycode;
            const char *mappedLabel;
            bool isPressed;
        } button;
    } data;

    // Common output value (used by joystick/dpad for direction, potentially others)
    Vec2 outputValue;
} Widget;

// Application State
typedef enum {
    APP_STATE_RUNNING,     // Normal operation or trackpad mode
    APP_STATE_EDIT_MODE,   // Edit mode active, no specific action
    APP_STATE_MENU_ADD_WIDGET, // Add button pressed, showing selection menu 
    APP_STATE_MENU_WIDGET_PROPERTIES, // Editing properties of a selected widget
    APP_STATE_MENU_REMAP_ACTION, // Selecting which direction/action to remap for DPad/Joystick
    APP_STATE_MENU_REMAP_KEY // Remapping keys 
} ApplicationState;

// Edit Mode State
typedef enum {
    EDIT_NONE,
    EDIT_MOVE,
    EDIT_RESIZE
} EditAction;

typedef struct {
    Widget* targetWidget;
    EditAction action;
    Vec2 startTouchPos;
    Vec2 startWidgetCenter;
    float startWidgetHalfSize;
    float startTouchDistance;
} EditState;

// Input System
typedef enum {
    EVT_KEY_DOWN,
    EVT_KEY_UP
} EventType;

typedef struct {
    int widget_id;
    EventType type;
    int keycode;  // Linux KEY_ code
} InputEvent;

typedef enum {
    DIR_UP = 0,
    DIR_DOWN,
    DIR_LEFT,
    DIR_RIGHT
} Direction;

// Touch Input System
typedef struct {
    bool active;
    bool was_down;
    double x;
    double y;
} MTSlot;

typedef enum {
    SLOT_IDLE = 0,
    SLOT_WIDGET,
    SLOT_TRACKPAD
} SlotMode;

// UI Menu System
typedef struct {
    const char* label;
    Color bgColor;
} MenuItem;

typedef enum {
    PROP_ACTION_DELETE,
    PROP_ACTION_REMAP
    /*, PROP_ACTION_OPACITY ... */
} PropertyAction;


// --- Global Constants ---

// Mappable Keys Data
static const MappableKey gMappableKeys[] = {
    {KEY_A, "A"}, {KEY_B, "B"}, {KEY_C, "C"}, {KEY_D, "D"},
    {KEY_E, "E"}, {KEY_F, "F"}, {KEY_G, "G"}, {KEY_H, "H"},
    {KEY_I, "I"}, {KEY_J, "J"}, {KEY_K, "K"}, {KEY_L, "L"},
    {KEY_M, "M"}, {KEY_N, "N"}, {KEY_O, "O"}, {KEY_P, "P"},
    {KEY_Q, "Q"}, {KEY_R, "R"}, {KEY_S, "S"}, {KEY_T, "T"},
    {KEY_U, "U"}, {KEY_V, "V"}, {KEY_W, "W"}, {KEY_X, "X"},
    {KEY_Y, "Y"}, {KEY_Z, "Z"},
    {KEY_1, "1"}, {KEY_2, "2"}, {KEY_3, "3"}, {KEY_4, "4"},
    {KEY_5, "5"}, {KEY_6, "6"}, {KEY_7, "7"}, {KEY_8, "8"},
    {KEY_9, "9"}, {KEY_0, "0"},
    {KEY_ESC, "Esc"},
    {KEY_SPACE, "Spc"}, {KEY_ENTER, "Ent"}, {KEY_BACKSPACE, "Bk"}, {KEY_TAB, "Tab"},
    {KEY_LEFTCTRL, "Ctrl"}, {KEY_LEFTSHIFT, "Shft"}, {KEY_LEFTALT, "Alt"},
    {KEY_UP, "Up"}, {KEY_DOWN, "Dn"}, {KEY_LEFT, "Lt"}, {KEY_RIGHT, "Rt"},
    {BTN_LEFT, "LMB"}, {BTN_RIGHT, "RMB"},
};
static const int gNumMappableKeys = sizeof(gMappableKeys) / sizeof(gMappableKeys[0]);

// UInput integration 
static int uinput_fd = -1;

static bool uinput_init(void) {
    uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uinput_fd < 0) { perror("Failed to open /dev/uinput"); return false; }

    if (ioctl(uinput_fd, UI_SET_EVBIT, EV_REL) < 0) { perror("Failed to set EV_REL"); close(uinput_fd); return false; }
    if (ioctl(uinput_fd, UI_SET_RELBIT, REL_X) < 0 || ioctl(uinput_fd, UI_SET_RELBIT, REL_Y) < 0) { perror("Failed to set REL_X/REL_Y"); close(uinput_fd); return false; }
    if (ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY) < 0) { perror("Failed to set EV_KEY"); close(uinput_fd); return false; }

    const int extra_keys[] = {BTN_LEFT, BTN_RIGHT, KEY_VOLUMEDOWN, KEY_VOLUMEUP};
    for (size_t i = 0; i < sizeof(extra_keys)/sizeof(extra_keys[0]); ++i) {
        if (ioctl(uinput_fd, UI_SET_KEYBIT, extra_keys[i]) < 0) {
            char err_msg[64];
            snprintf(err_msg, sizeof(err_msg), "Failed to set keybit for extra key %d", extra_keys[i]);
            perror(err_msg);
            close(uinput_fd);
            return false;
        }
    }

    for (int i = 0; i < gNumMappableKeys; ++i) {
        if (ioctl(uinput_fd, UI_SET_KEYBIT, gMappableKeys[i].keycode) < 0) {
            char err_msg[64];
            snprintf(err_msg, sizeof(err_msg), "Failed to set keybit for keycode %d", gMappableKeys[i].keycode);
            perror(err_msg);
            close(uinput_fd);
            return false;
        }
    }

    if (ioctl(uinput_fd, UI_SET_EVBIT, EV_SYN) < 0) { perror("Failed to set EV_SYN"); close(uinput_fd); return false; }

    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof(uidev));
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "wlr_gamepad_uinput");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor  = 0x1234;
    uidev.id.product = 0x5678;
    uidev.id.version = 1;

    if (write(uinput_fd, &uidev, sizeof(uidev)) < 0) { perror("Failed to write uinput_user_dev"); close(uinput_fd); return false; }
    if (ioctl(uinput_fd, UI_DEV_CREATE) < 0) { perror("Failed to create uinput device"); close(uinput_fd); return false; }

    fprintf(stderr, "[UINPUT] initialized: fd=%d\n", uinput_fd);
    return true;
}

static void uinput_move(int dx, int dy) {
    if (uinput_fd < 0) return;
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_REL;
    ev.code = REL_X;
    ev.value = dx;
    write(uinput_fd, &ev, sizeof(ev));
    ev.code = REL_Y;
    ev.value = dy;
    write(uinput_fd, &ev, sizeof(ev));
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    write(uinput_fd, &ev, sizeof(ev));
}

static void uinput_key(int keycode, bool pressed) {
    if (uinput_fd < 0) return;
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_KEY;
    ev.code = keycode;
    ev.value = pressed ? 1 : 0;
    write(uinput_fd, &ev, sizeof(ev));
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    write(uinput_fd, &ev, sizeof(ev));
}

static void uinput_destroy(void) {
    if (uinput_fd < 0) return;
    fprintf(stderr, "[UINPUT] destroying device fd=%d\n", uinput_fd);
    ioctl(uinput_fd, UI_DEV_DESTROY);
    close(uinput_fd);
    uinput_fd = -1;
}

// Key Selection Menu Layout Constants
static const int kKeyGridCols = 8;
static const float kKeyButtonSize = 60.0f;
static const float kKeyButtonSpacing = 10.0f;

// Grid layout helper struct for arranging items in a grid
typedef struct {
    int rows;
    int cols;
    float cellSize;
    float cellSpacing;
    float totalWidth;
    float totalHeight;
    float startX;
    float startY;
} GridLayout;

// Calculate grid layout parameters for a uniform grid
static void CalculateGridLayout(
    int screenW,
    int screenH,
    int itemCount,
    int numCols,
    float baseCellSize,
    float baseCellSpacing,
    float offsetTop,
    GridLayout *out)
{
    // Empty layout if no columns or items
    if (numCols <= 0 || itemCount <= 0) {
        out->rows = 0;
        out->cols = numCols;
        out->cellSize = baseCellSize;
        out->cellSpacing = baseCellSpacing;
        out->totalWidth = 0;
        out->totalHeight = 0;
        out->startX = 0;
        out->startY = offsetTop;
        return;
    }
    // Compute rows and required grid size
    int rows = (itemCount + numCols - 1) / numCols;
    float reqW = numCols * (baseCellSize + baseCellSpacing) - baseCellSpacing;
    float reqH = rows    * (baseCellSize + baseCellSpacing) - baseCellSpacing;

    // Available space
    float availW = (float)screenW;
    float availH = (float)screenH - offsetTop;

    // Uniform scale to fit both dimensions
    float scaleW = availW / reqW;
    float scaleH = availH / reqH;
    float scale  = (scaleW < scaleH ? scaleW : scaleH);
    if (scale > 1.0f) scale = 1.0f;
    if (scale < 0.1f) scale = 0.1f;

    // Populate layout
    out->rows        = rows;
    out->cols        = numCols;
    out->cellSize    = baseCellSize * scale;
    out->cellSpacing = baseCellSpacing * scale;
    out->totalWidth  = numCols * (out->cellSize + out->cellSpacing) - out->cellSpacing;
    out->totalHeight = rows    * (out->cellSize + out->cellSpacing) - out->cellSpacing;
    out->startX      = (availW >= out->totalWidth
                         ? (availW - out->totalWidth) * 0.5f
                         : 0.0f);
    // Center grid vertically within available height (below offsetTop)
    {
        float availH = (float)screenH - offsetTop;
        out->startY = offsetTop + MAX(0.0f, (availH - out->totalHeight) * 0.5f);
    }
}

// Widget Selection Menu
static const WidgetType availableWidgetTypes[] = {WIDGET_JOYSTICK, WIDGET_DPAD, WIDGET_BUTTON};
static const char* availableWidgetNames[] = {"Joystick", "DPad", "Button"};
static const int numAvailableWidgetTypes = sizeof(availableWidgetTypes) / sizeof(availableWidgetTypes[0]);

// Analog Widget Remapping
static const char* availableAnalogActionNames[] = {"Up", "Down", "Left", "Right"};
static const int numAnalogActions = sizeof(availableAnalogActionNames) / sizeof(availableAnalogActionNames[0]);

// Widget Property Editing
static const PropertyAction availablePropertyActions[] = {PROP_ACTION_REMAP, PROP_ACTION_DELETE /*, ... */};
static const char* availablePropertyNames[] = {"Remap", "Delete" /*, ... */};
static const int numAvailablePropertyActions = sizeof(availablePropertyActions) / sizeof(availablePropertyActions[0]);

// Max Limits
#define MAX_WIDGETS 15
#define MAX_MT_SLOTS 10
#define MAX_INPUT_EVENTS 64

// UI Constants: Positioning and Sizes
static const float kEditButtonX = 10.0f, kEditButtonY = 10.0f;
static const float kEditButtonW = 80.0f, kEditButtonH = 40.0f;
static const float kAddButtonX = kEditButtonX + kEditButtonW + 10.0f;
static const float kAddButtonY = kEditButtonY;
static const float kAddButtonW = kEditButtonW;
static const float kAddButtonH = kEditButtonH;
static const float kPropsButtonX = kAddButtonX + kAddButtonW + 10.0f;
static const float kPropsButtonY = kAddButtonY;
static const float kPropsButtonW = kAddButtonW;
static const float kPropsButtonH = kAddButtonH;
static const float kHandleSize = 20.0f;

// Add constant for outline thickness
static const float kOutlineThickness = 2.0f;

// Widget Selection Menu Layout
static const float kMenuButtonW = 150.0f;
static const float kMenuButtonH = 50.0f;
static const float kMenuButtonSpacing = 10.0f;

// UI Colors
static const Color kMenuOverlayColor = {0, 0, 0, 150}; // Semi-transparent black
static const Color kColorIdle = {200, 200, 200, 255};
static const Color kColorActive = {100, 255, 100, 255};
static const Color kColorRed = {255, 100, 100, 255};
static const Color kColorEditMode = {100, 100, 255, 255};
static const Color kColorEditModeHandle = {100, 100, 255, 255};
static const Color kColorBlack = {0, 0, 0, 255};
static const Color kColorWhite = {255, 255, 255, 255};
static const Color kColorDisabled = {150, 150, 150, 255}; // Greyed out color for disabled buttons

// Master opacity for entire UI [0.0 .. 1.0]
static float gMasterOpacity = 0.5f;
// Helper macro to apply master opacity
#define SET_COLOR(c) glColor4ub((c).r, (c).g, (c).b, (GLubyte)((c).a * ((gAppState == APP_STATE_RUNNING) ? gMasterOpacity : 1.0f)))

// Input
static const float kTrackpadSensitivity = 1.0f;

// --- Global Variables ---

// Widget Management
static Widget gWidgets[MAX_WIDGETS];
static int gNumWidgets = 0;
static int FindWidgetIndexById(int widgetId);
static void enqueue_event(int widget_id, EventType type, int keycode);

// Application State
static ApplicationState gAppState = APP_STATE_RUNNING;
static EditState gEditState = {NULL, EDIT_NONE, {0,0}, {0,0}, 0.0f, 0.0f};
static int gSelectedWidgetId = 0;    // ID of the widget selected for editing properties (0 = none)
static int gRemappingWidgetId = 0;   // ID of the widget currently being remapped
static int gRemapAction = -1;        // Which direction/action is being remapped for analog widgets

// UI Interaction State
static int gLastUIFinger = -1;

// Scaled UI Values (calculated at runtime)
static float gScaledKeyButtonSize = 60.0f;
static float gScaledKeyButtonSpacing = 10.0f;

// Cached layout for key selection grid (recomputed on resize)
static GridLayout gKeyGridLayout = {0};

// Wayland and EGL Globals
static struct wl_display *display = NULL;
static struct wl_registry *registry = NULL;
static struct wl_compositor *compositor = NULL;
static struct zwlr_layer_shell_v1 *layer_shell = NULL;
static struct wl_surface *surface = NULL;
static struct zwlr_layer_surface_v1 *layer_surface = NULL;
static struct wl_egl_window *egl_window = NULL;
static EGLDisplay egl_display = EGL_NO_DISPLAY;
static EGLContext egl_context = EGL_NO_CONTEXT;
static EGLSurface egl_surface = EGL_NO_SURFACE;
static EGLint egl_major, egl_minor;
static int width = 0, height = 0;

// Input Event Queue
static InputEvent gInputEvents[MAX_INPUT_EVENTS];
static int gInputEventCount = 0;

// Raw Touch Input (evdev)
static MTSlot mt_slots[MAX_MT_SLOTS] = {0};
static int gTouchDevFd = -1;
static int touch_min_x = 0, touch_max_x = 0;
static int touch_min_y = 0, touch_max_y = 0;
static int current_slot = 0; // Current slot being processed by evdev
static SlotMode slot_mode[MAX_MT_SLOTS] = {SLOT_IDLE};
static double track_last_x[MAX_MT_SLOTS];
static double track_last_y[MAX_MT_SLOTS];
static double track_accum_x[MAX_MT_SLOTS];
static double track_accum_y[MAX_MT_SLOTS];
static bool track_moved[MAX_MT_SLOTS];
static bool gLandscapeMode = false;
static bool gViewportChanged = true;

// Overlay toggle globals
static int gVolDevFd = -1;
static int gVolUpDevFd = -1; 
static bool gOverlayActive = true;
// Volume-down long-press state
static bool gVolDown = false;
static struct timespec gVolTs;
#define LONG_PRESS_NS (250 * 1000000L)
static bool gVolToggled = false;
static bool gVolUpDown = false;
static struct timespec gVolUpTs;

// --- Forward Declarations ---

// Widget Specific Handlers (Generated by X-Macro)
#define X(name, func) \
  void func##_draw   (Widget *w); \
  void func##_process(Widget *w);
WIDGET_TYPE_LIST
#undef X

// Input Handling
static void InputState_Update(void);
static void InputState_Flush(void);
static void init_touch_device(const char *device);
static void handle_evdev_event(const struct input_event *ev);
static const char* find_touchscreen_device(void);

// Helper to find a device with a given event type and code (e.g. volume-down key)
static int find_input_device(int ev_type, int ev_code) {
    unsigned long bits[(KEY_MAX/(8*sizeof(long)))+1] = {0};
    for (int i = 0; i < 32; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        if (ioctl(fd, EVIOCGBIT(ev_type, sizeof(bits)), bits) >= 0 &&
            (bits[ev_code/(8*sizeof(long))] & (1UL << (ev_code%(8*sizeof(long))))) ) {
            return fd;
        }
        close(fd);
    }
    return -1;
}

// Toggle overlay on/off by grabbing/ungrabbing the touch device
static void toggle_overlay(void) {
    gOverlayActive = !gOverlayActive;
    ioctl(gTouchDevFd, EVIOCGRAB, gOverlayActive);
    // Reset all touch state to avoid stale slots blocking new input
    for (int i = 0; i < MAX_MT_SLOTS; ++i) {
        mt_slots[i].active = false;
        mt_slots[i].was_down = false;
        slot_mode[i] = SLOT_IDLE;
        track_moved[i] = false;
    }
    gLastUIFinger = -1;
    gEditState = (EditState){NULL, EDIT_NONE, {0,0}, {0,0}, 0.0f, 0.0f};
    gSelectedWidgetId = 0;
    gRemappingWidgetId = 0;
    gRemapAction = -1;
    if (!gOverlayActive) {
        // Immediately clear screen when turned off
        glClear(GL_COLOR_BUFFER_BIT);
        eglSwapBuffers(egl_display, egl_surface);
        // Release any pressed keys when overlay turned off
        for (int i = 0; i < gNumMappableKeys; ++i) uinput_key(gMappableKeys[i].keycode, false);
    }
}

// Drawing Primitives & Helpers
void DrawRect(float x, float y, float w, float h, Color col);
void DrawOutlinedRect(float x, float y, float w, float h, float thickness, Color col);
void DrawLine(float x1, float y1, float x2, float y2, float thickness, Color col);
void DrawCircle(float cx, float cy, float r, int segments, float thickness, Color col);
void DrawFilledCircle(float cx, float cy, float r, int segments, Color col);
void DrawTriangle(Vec2 a, Vec2 b, Vec2 c, Color col, float thickness);
void DrawTriangleFilled(Vec2 a, Vec2 b, Vec2 c, Color col);
void RenderText(const char *text, float x, float y, float pixelSize, Color col);
float CalculateFittingPixelSize(const char* text, float maxWidth, float maxHeight);

// UI Element Drawing
void DrawGenericButton(float x, float y, float w, float h, const char *label, Color bgColor, Color textColor);
void DrawMainButton(bool isActive);
void DrawAddButton(bool isActive, bool isDisabled);
void DrawPropertiesButton(bool isActive);
void DrawGenericMenu(int screenW, int screenH, const MenuItem items[], int numItems, float itemW, float itemH, float itemSpacing, Color overlayColor);
void DrawWidgetSelectionMenu(int screenW, int screenH);
void DrawWidgetPropertiesMenu(int screenW, int screenH);
void DrawKeySelectionMenu(int screenW, int screenH);
void DrawAnalogActionSelectionMenu(int screenW, int screenH);
void DrawAllWidgets(int screenW, int screenH, bool editMode);
void DrawUserInterface(bool editMode);

// Main Rendering
void RenderFrame(int w, int h, EGLDisplay display, EGLSurface surface);

// Wayland Callbacks
static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t name, const char *interface, uint32_t version);
static void layer_surface_handle_configure(void *data,
                                           struct zwlr_layer_surface_v1 *layer_surface,
                                           uint32_t serial,
                                           uint32_t w, uint32_t h);


// --- Widget Dispatch Tables ---
// Initialized after widget handler forward declarations

static void (*widget_draw_tbl[WIDGET_MAX])(Widget*) = {
  #define X(name, func) func##_draw,
    WIDGET_TYPE_LIST
  #undef X
};

static void (*widget_proc_tbl[WIDGET_MAX])(Widget*) = {
  #define X(name, func) func##_process,
    WIDGET_TYPE_LIST
  #undef X
};

// --- Utility Functions ---

static float clampf(float v, float mn, float mx) {
    return v < mn ? mn : (v > mx ? mx : v);
}

static float dist(Vec2 p1, Vec2 p2) {
    float dx = p1.x - p2.x;
    float dy = p1.y - p2.y;
    return sqrtf(dx * dx + dy * dy);
}

// Helper to lookup label for a given keycode
static const char* GetMappableKeyLabel(int keycode) {
    for (int i = 0; i < gNumMappableKeys; ++i) {
        if (gMappableKeys[i].keycode == keycode) {
            return gMappableKeys[i].label;
        }
    }
    return "";
}

// Helper function to calculate text pixel size to fit within bounds
float CalculateFittingPixelSize(const char* text, float maxWidth, float maxHeight) {
    if (!text || strlen(text) == 0) {
        return 1.0f; // Default size if no text
    }

    // Initial estimate based on height (e.g., aim for 50% height)
    float pixelSizeHeight = (maxHeight * 0.5f) / 8.0f; // Font is 8 pixels high
    if (pixelSizeHeight <= 0) {
        pixelSizeHeight = 1.0f; // Avoid zero/negative size
    }

    // Calculate width based on height estimate
    int len = strlen(text);
    float textWidth = len * 6.0f * pixelSizeHeight; // Font is 6 pixels wide

    // If width exceeds bounds, recalculate based on width
    float pixelSizeWidth = pixelSizeHeight;
    float targetWidth = maxWidth * 0.9f; // Use 90% of width for padding
    if (textWidth > targetWidth && len > 0) { // check len > 0 to avoid division by zero
        pixelSizeWidth = targetWidth / (len * 6.0f);
    }
    if (pixelSizeWidth <= 0) {
        pixelSizeWidth = 1.0f; // Avoid zero/negative size
    }

    // Return the smaller of the two calculated sizes
    return MIN(pixelSizeHeight, pixelSizeWidth);
}

// --- Text Rendering ---
// Minimal 6x8 bitmap font for lowercase a–z, digits 0–9, and uppercase A-Z
static const uint8_t FONT6x8[62][6] = {
    {0x20,0x54,0x54,0x54,0x78,0x00}, // a
    {0x7F,0x28,0x44,0x44,0x38,0x00}, // b
    {0x38,0x44,0x44,0x44,0x28,0x00}, // c
    {0x38,0x44,0x44,0x28,0x7F,0x00}, // d
    {0x38,0x54,0x54,0x54,0x18,0x00}, // e
    {0x08,0x7E,0x09,0x01,0x02,0x00}, // f
    {0x18,0xA4,0xA4,0xA8,0x7C,0x00}, // g
    {0x7F,0x08,0x04,0x04,0x78,0x00}, // h
    {0x00,0x44,0x7D,0x40,0x00,0x00}, // i
    {0x00,0x40,0x80,0x84,0x7D,0x00}, // j
    {0x00,0x7F,0x10,0x28,0x44,0x00}, // k
    {0x00,0x41,0x7F,0x40,0x00,0x00}, // l
    {0x7C,0x04,0x78,0x04,0x78,0x00}, // m
    {0x7C,0x08,0x04,0x04,0x78,0x00}, // n
    {0x38,0x44,0x44,0x44,0x38,0x00}, // o
    {0xFC,0x28,0x44,0x44,0x38,0x00}, // p
    {0x38,0x44,0x44,0x28,0xFC,0x00}, // q
    {0x44,0x78,0x44,0x04,0x08,0x00}, // r
    {0x48,0x54,0x54,0x54,0x24,0x00}, // s
    {0x04,0x3F,0x44,0x40,0x20,0x00}, // t
    {0x3C,0x40,0x40,0x20,0x7C,0x00}, // u
    {0x1C,0x20,0x40,0x20,0x1C,0x00}, // v
    {0x3C,0x40,0x30,0x40,0x3C,0x00}, // w
    {0x44,0x28,0x10,0x28,0x44,0x00}, // x
    {0x1C,0xA0,0xA0,0xA0,0x7C,0x00}, // y
    {0x44,0x64,0x54,0x4C,0x44,0x00}, // z
    {0x3E,0x51,0x49,0x45,0x3E,0x00}, // 0
    {0x00,0x42,0x7F,0x40,0x00,0x00}, // 1
    {0x62,0x51,0x49,0x49,0x46,0x00}, // 2
    {0x22,0x49,0x49,0x49,0x36,0x00}, // 3
    {0x18,0x14,0x52,0x7F,0x50,0x00}, // 4
    {0x27,0x45,0x45,0x45,0x39,0x00}, // 5
    {0x3C,0x4A,0x49,0x49,0x30,0x00}, // 6
    {0x01,0x01,0x79,0x05,0x03,0x00}, // 7
    {0x36,0x49,0x49,0x49,0x36,0x00}, // 8
    {0x06,0x49,0x49,0x29,0x1E,0x00}, // 9
    {0x7E,0x11,0x11,0x11,0x7E,0x00}, // A
    {0x7F,0x49,0x49,0x49,0x36,0x00}, // B
    {0x3E,0x41,0x41,0x41,0x22,0x00}, // C
    {0x7F,0x41,0x41,0x22,0x1C,0x00}, // D
    {0x7F,0x49,0x49,0x49,0x41,0x00}, // E
    {0x7F,0x09,0x09,0x09,0x01,0x00}, // F
    {0x3E,0x41,0x41,0x51,0x72,0x00}, // G
    {0x7F,0x08,0x08,0x08,0x7F,0x00}, // H
    {0x00,0x41,0x7F,0x41,0x00,0x00}, // I
    {0x30,0x40,0x40,0x40,0x3F,0x00}, // J
    {0x7F,0x08,0x14,0x22,0x41,0x00}, // K
    {0x7F,0x40,0x40,0x40,0x40,0x00}, // L
    {0x7F,0x06,0x18,0x06,0x7F,0x00}, // M
    {0x7F,0x06,0x08,0x30,0x7F,0x00}, // N
    {0x3E,0x41,0x41,0x41,0x3E,0x00}, // O
    {0x7F,0x09,0x09,0x09,0x06,0x00}, // P
    {0x3E,0x41,0x51,0x21,0x5E,0x00}, // Q
    {0x7F,0x09,0x09,0x19,0x66,0x00}, // R
    {0x26,0x49,0x49,0x49,0x32,0x00}, // S
    {0x01,0x01,0x7F,0x01,0x01,0x00}, // T
    {0x3F,0x40,0x40,0x40,0x3F,0x00}, // U
    {0x07,0x18,0x60,0x18,0x07,0x00}, // V
    {0x7F,0x20,0x18,0x20,0x7F,0x00}, // W
    {0x63,0x14,0x08,0x14,0x63,0x00}, // X
    {0x03,0x0C,0x78,0x0C,0x03,0x00}, // Y
    {0x61,0x51,0x49,0x45,0x43,0x00}, // Z
};

// Map ASCII chars to FONT6x8 index: a–z → 0–25, 0–9 → 26–35, A-Z -> 36-61
static int map6x8(char c) {
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= '0' && c <= '9') return 26 + (c - '0');
    if (c >= 'A' && c <= 'Z') return 36 + (c - 'A');
    return -1; // Character not in font
}

// Lightweight bitmap blitter - Batched Immediate Mode
void RenderText(const char *text, float x, float y, float pixelSize, Color col) {
    if (!text || *text == '\0') { // Early exit for empty or NULL string
        return;
    }

    // Set color once for all pixels in this text string
    SET_COLOR(col);
    glBegin(GL_QUADS); // Start batching quads

    float currentX = x;
    for (; *text; ++text) {
        int idx = map6x8(*text);
        if (idx < 0) { // Handle characters not in font (e.g., space)
            currentX += 6 * pixelSize; // Advance by character width
            continue;
        }
        const uint8_t *glyph = FONT6x8[idx];
        for (int cx = 0; cx < 6; ++cx) { // Character width
            uint8_t bits = glyph[cx];
            for (int ry = 0; ry < 8; ++ry) { // Character height
                if (bits & (1 << ry)) {
                    // Directly emit vertices for this pixel's quad
                    float px = currentX + cx * pixelSize;
                    float py = y + ry * pixelSize;
                    glVertex2f(px, py);
                    glVertex2f(px + pixelSize, py);
                    glVertex2f(px + pixelSize, py + pixelSize);
                    glVertex2f(px, py + pixelSize);
                }
            }
        }
        currentX += 6 * pixelSize; // Advance to next character position
    }
    glEnd(); // End batching quads
}

// --- Drawing Primitives ---

void DrawRect(float x, float y, float w, float h, Color col) {
    SET_COLOR(col);
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

void DrawOutlinedRect(float x, float y, float w, float h, float thickness, Color col) {
    SET_COLOR(col);
    glLineWidth(thickness);
    glBegin(GL_LINE_LOOP);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

void DrawLine(float x1, float y1, float x2, float y2, float thickness, Color col) {
    SET_COLOR(col);
    glLineWidth(thickness);
    glBegin(GL_LINES);
    glVertex2f(x1, y1);
    glVertex2f(x2, y2);
    glEnd();
}

void DrawCircle(float cx, float cy, float r, int segments, float thickness, Color col) {
    SET_COLOR(col);
    glLineWidth(thickness);
    glBegin(GL_LINE_LOOP);
    for (int i = 0; i < segments; i++) {
        float a = (2.0f * M_PI * i) / segments;
        glVertex2f(cx + cosf(a) * r, cy + sinf(a) * r);
    }
    glEnd();
}

void DrawFilledCircle(float cx, float cy, float r, int segments, Color col) {
    SET_COLOR(col);
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(cx, cy); // Center point
    for (int i = 0; i <= segments; i++) { // Loop to segments + 1 to close the circle
        float a = (2.0f * M_PI * i) / segments;
        glVertex2f(cx + cosf(a) * r, cy + sinf(a) * r);
    }
    glEnd();
}

void DrawTriangle(Vec2 a, Vec2 b, Vec2 c, Color col, float thickness) {
    SET_COLOR(col);
    glLineWidth(thickness);
    glBegin(GL_LINE_LOOP);
    glVertex2f(a.x, a.y);
    glVertex2f(b.x, b.y);
    glVertex2f(c.x, c.y);
    glEnd();
}

void DrawTriangleFilled(Vec2 a, Vec2 b, Vec2 c, Color col) {
    SET_COLOR(col);
    glBegin(GL_TRIANGLES);
    glVertex2f(a.x, a.y);
    glVertex2f(b.x, b.y);
    glVertex2f(c.x, c.y);
    glEnd();
}

// --- UI Element Drawing Functions ---

static void DrawButton(float x, float y, float w, float h, Color col) { // Old static, keep for consistency if used
    DrawOutlinedRect(x, y, w, h, kOutlineThickness, col);
}

void DrawGenericButton(float x, float y, float w, float h, const char *label, Color bgColor, Color textColor) {
    DrawButton(x, y, w, h, bgColor); // Draw outlined background

    if (label && strlen(label) > 0) {
        float pixelSize = CalculateFittingPixelSize(label, w, h);
        float textWidth = strlen(label) * 6.0f * pixelSize;
        float textHeight = 8.0f * pixelSize;
        float textX = x + (w - textWidth) * 0.5f;
        float textY = y + (h - textHeight) * 0.5f;
        RenderText(label, textX, textY, pixelSize, textColor);
    }
}

void DrawMainButton(bool isActive) {
    if (!isActive) {
        // Transparent in running state: skip drawing
        return;
    }
    Color btnCol = isActive ? kColorActive : kColorIdle;
    DrawGenericButton(kEditButtonX, kEditButtonY, kEditButtonW, kEditButtonH, NULL, btnCol, kColorWhite);
}

void DrawAddButton(bool isActive, bool isDisabled) {
    Color btnCol;
    if (isDisabled) {
        btnCol = kColorDisabled;
    } else if (isActive) {
        btnCol = kColorActive;
    } else {
        btnCol = kColorIdle;
    }
    DrawGenericButton(kAddButtonX, kAddButtonY, kAddButtonW, kAddButtonH, "Add", btnCol, kColorWhite);
}

void DrawPropertiesButton(bool isActive) {
    Color btnCol = isActive ? kColorActive : kColorIdle;
    DrawGenericButton(kPropsButtonX, kPropsButtonY, kPropsButtonW, kPropsButtonH, "Edit", btnCol, kColorWhite);
}

void DrawGenericMenu(int screenW, int screenH, const MenuItem items[], int numItems, float itemW, float itemH, float itemSpacing, Color overlayColor) {
    DrawRect(0, 0, (float)screenW, (float)screenH, overlayColor);

    float totalMenuHeight = (itemH + itemSpacing) * numItems - itemSpacing;
    float startY = ((float)screenH - totalMenuHeight) * 0.5f;
    float startX = ((float)screenW - itemW) * 0.5f;

    for (int i = 0; i < numItems; ++i) {
        float itemY = startY + i * (itemH + itemSpacing);
        DrawGenericButton(startX, itemY, itemW, itemH, items[i].label, items[i].bgColor, kColorWhite);
    }
}

void DrawWidgetSelectionMenu(int screenW, int screenH) {
    MenuItem menuItems[numAvailableWidgetTypes];
    for (int i = 0; i < numAvailableWidgetTypes; ++i) {
        menuItems[i] = (MenuItem){availableWidgetNames[i], kColorIdle};
    }
    DrawGenericMenu(screenW, screenH, menuItems, numAvailableWidgetTypes,
                    kMenuButtonW, kMenuButtonH, kMenuButtonSpacing, kMenuOverlayColor);
}

void DrawWidgetPropertiesMenu(int screenW, int screenH) {
    MenuItem menuItems[numAvailablePropertyActions];
    for (int i = 0; i < numAvailablePropertyActions; ++i) {
        Color btnColor = (availablePropertyActions[i] == PROP_ACTION_DELETE) ? kColorRed : kColorIdle;
        menuItems[i] = (MenuItem){availablePropertyNames[i], btnColor};
    }
    DrawGenericMenu(screenW, screenH, menuItems, numAvailablePropertyActions,
                    kMenuButtonW, kMenuButtonH, kMenuButtonSpacing, kMenuOverlayColor);
}

void DrawKeySelectionMenu(int screenW, int screenH) {
    DrawRect(0, 0, (float)screenW, (float)screenH, kMenuOverlayColor);

    int widgetIndex = FindWidgetIndexById(gRemappingWidgetId);
    Widget *targetWidget = (widgetIndex != -1) ? &gWidgets[widgetIndex] : NULL;
    bool isAnalog = (targetWidget && (targetWidget->type == WIDGET_JOYSTICK || targetWidget->type == WIDGET_DPAD));
    int currentKeycode = -1;
    if (targetWidget) {
        if (isAnalog && gRemapAction >= 0 && gRemapAction < numAnalogActions) {
            currentKeycode = targetWidget->data.analog.keycode[gRemapAction];
        } else if (targetWidget->type == WIDGET_BUTTON) {
            currentKeycode = targetWidget->data.button.keycode;
        }
    }

    // Prepare title text
    char titleBuffer[128];
    if (!isAnalog) {
        snprintf(titleBuffer, sizeof(titleBuffer), "Select Key for Button %d", gRemappingWidgetId);
    } else {
        const char *wname = (targetWidget->type == WIDGET_JOYSTICK ? "Joystick" : "DPad");
        if (gRemapAction >= 0 && gRemapAction < numAnalogActions) {
             snprintf(titleBuffer, sizeof(titleBuffer), "Select Key for %s '%s'", wname, availableAnalogActionNames[gRemapAction]);
        } else {
             snprintf(titleBuffer, sizeof(titleBuffer), "Select Key for %s", wname);
        }
    }

    // Title and grid group vertical centering
    float titlePixelSize = 2.0f;
    float titleTextRenderHeight = 8.0f * titlePixelSize;
    float paddingBelowTitle = 10.0f;
    // Compute overall group height (title + padding + grid)
    float groupHeight = titleTextRenderHeight + paddingBelowTitle + gKeyGridLayout.totalHeight;
    float groupStartY = ((float)screenH - groupHeight) * 0.5f;
    float titleWidth = strlen(titleBuffer) * 6.0f * titlePixelSize;
    float titleX = ((float)screenW - titleWidth) * 0.5f;
    float titleY = groupStartY;
    RenderText(titleBuffer, titleX, titleY, titlePixelSize, kColorWhite);
    // Position grid below title
    gKeyGridLayout.startY = titleY + titleTextRenderHeight + paddingBelowTitle;

    // Draw key buttons using cached grid layout (horizontally centered, startY updated)
    for (int i = 0; i < gNumMappableKeys; ++i) {
        int row = i / gKeyGridLayout.cols;
        int col = i % gKeyGridLayout.cols;
        float buttonX = gKeyGridLayout.startX + col * (gKeyGridLayout.cellSize + gKeyGridLayout.cellSpacing);
        float buttonY = gKeyGridLayout.startY + row * (gKeyGridLayout.cellSize + gKeyGridLayout.cellSpacing);

        Color btnColor = (gMappableKeys[i].keycode == currentKeycode) ? kColorActive : kColorIdle;
        DrawGenericButton(buttonX, buttonY, gKeyGridLayout.cellSize, gKeyGridLayout.cellSize,
                          gMappableKeys[i].label, btnColor, kColorWhite);
    }
}

void DrawAnalogActionSelectionMenu(int screenW, int screenH) {
    DrawRect(0, 0, (float)screenW, (float)screenH, kMenuOverlayColor);
    MenuItem items[numAnalogActions];
    for (int i = 0; i < numAnalogActions; ++i) {
        items[i] = (MenuItem){availableAnalogActionNames[i], kColorIdle};
    }
    DrawGenericMenu(screenW, screenH, items, numAnalogActions,
                    kMenuButtonW, kMenuButtonH, kMenuButtonSpacing, kMenuOverlayColor);
}

// --- Widget Structure and Core Logic ---

void Widget_UpdateAbsCoords(Widget* w, int screenW, int screenH) {
    w->absCenter.x = w->normCenter.x * screenW;
    w->absCenter.y = w->normCenter.y * screenH;
    float minDim = (float)MIN(screenW, screenH);
    w->absRadius = w->normHalfSize * minDim;
    w->absSize = w->absRadius * 2.0f;
    w->absTopLeft.x = w->absCenter.x - w->absRadius;
    w->absTopLeft.y = w->absCenter.y - w->absRadius;
}

bool Widget_IsInside(const Widget* w, Vec2 p) {
    return p.x >= w->absTopLeft.x && p.x <= w->absTopLeft.x + w->absSize &&
           p.y >= w->absTopLeft.y && p.y <= w->absTopLeft.y + w->absSize;
}

void Widget_ClampToScreen(Widget* w, int screenW, int screenH) {
    w->normCenter.x = clampf(w->normCenter.x, 0.0f, 1.0f);
    w->normCenter.y = clampf(w->normCenter.y, 0.0f, 1.0f);
    Widget_UpdateAbsCoords(w, screenW, screenH); // Re-calculate absolute after clamping normalized
}

void CreateWidget(WidgetType type, Vec2 normCenter, float normHalfSize) {
    if (gNumWidgets >= MAX_WIDGETS) {
        D("Cannot create widget: MAX_WIDGETS reached");
        return;
    }

    Widget newWidget = {
        .id = 0, // ID assigned later
        .type = type,
        .normCenter = normCenter,
        .normHalfSize = normHalfSize,
        .controllingFinger = INVALID_FINGER_ID,
        .outputValue = {0, 0}
    };

    static int nextWidgetId = 1; // Stays function-local static
    newWidget.id = nextWidgetId++;

    if (type == WIDGET_BUTTON) {
        D("Creating Button widget ID %d", newWidget.id);
        newWidget.data.button.keycode = KEY_E;
        newWidget.data.button.mappedLabel = "E";
        newWidget.data.button.isPressed = false;
    } else if (type == WIDGET_JOYSTICK || type == WIDGET_DPAD) {
        const int defaultKeys[4] = {KEY_W, KEY_S, KEY_A, KEY_D};
        D("Creating %s widget ID %d with default analog mapping",
          (type == WIDGET_JOYSTICK ? "Joystick" : "DPad"), newWidget.id);
        for (int d = 0; d < numAnalogActions; ++d) {
            newWidget.data.analog.keycode[d] = defaultKeys[d];
            newWidget.data.analog.mappedLabel[d] = GetMappableKeyLabel(defaultKeys[d]);
        }
    }

    gWidgets[gNumWidgets++] = newWidget;
    D("Widget created. gNumWidgets = %d", gNumWidgets);
    Widget_UpdateAbsCoords(&gWidgets[gNumWidgets - 1], width, height);
}

int FindWidgetIndexById(int widgetId) {
    if (widgetId == 0) return -1;
    for (int i = 0; i < gNumWidgets; ++i) {
        if (gWidgets[i].id == widgetId) {
            return i;
        }
    }
    return -1;
}

void RemoveWidgetByIndex(int index) {
    if (index < 0 || index >= gNumWidgets) {
        return;
    }
    D("Removing widget at index %d (ID: %d)", index, gWidgets[index].id);
    
    int removedWidgetId = gWidgets[index].id; // Store ID before potentially overwriting

    for (int i = index; i < gNumWidgets - 1; ++i) {
        gWidgets[i] = gWidgets[i + 1];
    }
    gNumWidgets--;

    if (gSelectedWidgetId == removedWidgetId) {
        gSelectedWidgetId = 0;
    }
}

void UpdateAllWidgetCoords(int screenW, int screenH) {
    for (int i = 0; i < gNumWidgets; ++i) {
        Widget_UpdateAbsCoords(&gWidgets[i], screenW, screenH);
    }
}

// --- Widget-Specific Implementations (Draw) ---

void joystick_draw(Widget *w) {
    DrawCircle(w->absCenter.x, w->absCenter.y, w->absRadius, 64, kOutlineThickness, kColorIdle);
    Vec2 dotPos = {
        w->absCenter.x + w->outputValue.x * w->absRadius,
        w->absCenter.y + w->outputValue.y * w->absRadius
    };
    DrawFilledCircle(dotPos.x, dotPos.y, w->absRadius * 0.3f, 32, kColorWhite);
}

void dpad_draw(Widget *w) {
    float arrowDist = w->absRadius;
    float arrowW    = w->absRadius * 0.667f;
    float th        = kOutlineThickness;

    Vec2 c  = w->absCenter;
    Vec2 upT    = {c.x,                 c.y - arrowDist};
    Vec2 upBL   = {c.x - arrowW * 0.5f, c.y - arrowDist + arrowW};
    Vec2 upBR   = {c.x + arrowW * 0.5f, c.y - arrowDist + arrowW};
    Vec2 downT  = {c.x,                 c.y + arrowDist};
    Vec2 downBL = {c.x - arrowW * 0.5f, c.y + arrowDist - arrowW};
    Vec2 downBR = {c.x + arrowW * 0.5f, c.y + arrowDist - arrowW};
    Vec2 leftT  = {c.x - arrowDist,     c.y};
    Vec2 leftBL = {c.x - arrowDist + arrowW, c.y - arrowW * 0.5f};
    Vec2 leftBR = {c.x - arrowDist + arrowW, c.y + arrowW * 0.5f};
    Vec2 rightT = {c.x + arrowDist,     c.y};
    Vec2 rightBL= {c.x + arrowDist - arrowW, c.y - arrowW * 0.5f};
    Vec2 rightBR= {c.x + arrowDist - arrowW, c.y + arrowW * 0.5f};

    DrawTriangle(upT, upBL, upBR, kColorIdle, th);
    DrawTriangle(downT, downBL, downBR, kColorIdle, th);
    DrawTriangle(leftT, leftBL, leftBR, kColorIdle, th);
    DrawTriangle(rightT, rightBL, rightBR, kColorIdle, th);

    if (w->outputValue.y < -0.5f) DrawTriangleFilled(upT, upBL, upBR, kColorActive);
    if (w->outputValue.y >  0.5f) DrawTriangleFilled(downT, downBL, downBR, kColorActive);
    if (w->outputValue.x < -0.5f) DrawTriangleFilled(leftT, leftBL, leftBR, kColorActive);
    if (w->outputValue.x >  0.5f) DrawTriangleFilled(rightT, rightBL, rightBR, kColorActive);
}

void button_draw(Widget *w) {
    Color btnCol = w->data.button.isPressed ? kColorActive : kColorIdle;
    DrawOutlinedRect(w->absTopLeft.x, w->absTopLeft.y, w->absSize, w->absSize, kOutlineThickness, btnCol);

    if (w->data.button.mappedLabel && strlen(w->data.button.mappedLabel) > 0) {
        const char *label = w->data.button.mappedLabel;
        float pixelSize = CalculateFittingPixelSize(label, w->absSize, w->absSize);
        float textWidth = strlen(label) * 6.0f * pixelSize;
        float textHeight = 8.0f * pixelSize;
        float textX = w->absCenter.x - textWidth * 0.5f;
        float textY = w->absCenter.y - textHeight * 0.5f;
        RenderText(label, textX, textY, pixelSize, kColorWhite);
    }
}

// --- Widget-Specific Implementations (Process) ---

void joystick_process(Widget* w) {
    if (w->controllingFinger != INVALID_FINGER_ID && mt_slots[w->controllingFinger].active) {
        int slot_idx = w->controllingFinger;
        Vec2 touchPos = {mt_slots[slot_idx].x, mt_slots[slot_idx].y};
        Vec2 delta = {touchPos.x - w->absCenter.x, touchPos.y - w->absCenter.y};
        Vec2 norm = {
            (w->absRadius > 1e-5f) ? delta.x / w->absRadius : 0.0f,
            (w->absRadius > 1e-5f) ? delta.y / w->absRadius : 0.0f
        };

        float lenSq = norm.x * norm.x + norm.y * norm.y;
        if (lenSq > 1.0f) {
            float len = sqrtf(lenSq);
            norm.x /= len;
            norm.y /= len;
        }
        w->outputValue = norm;
    } else {
        if (w->controllingFinger != INVALID_FINGER_ID) {
            D("Joystick %d lost finger slot %d, resetting", w->id, w->controllingFinger);
            w->controllingFinger = INVALID_FINGER_ID;
        }
        w->outputValue = (Vec2){0, 0};
    }
}

void dpad_process(Widget* w) {
    if (w->controllingFinger != INVALID_FINGER_ID && mt_slots[w->controllingFinger].active) {
        int slot_idx = w->controllingFinger;
        Vec2 touchPos = {mt_slots[slot_idx].x, mt_slots[slot_idx].y};
        Vec2 delta = {touchPos.x - w->absCenter.x, touchPos.y - w->absCenter.y};

        w->outputValue = (Vec2){0, 0};
        const float deadzoneSq = 0.1f * 0.1f; // Use squared distance for efficiency
        if (delta.x * delta.x + delta.y * delta.y > deadzoneSq * w->absRadius * w->absRadius) {
            if (fabsf(delta.x) > fabsf(delta.y)) {
                w->outputValue.x = (delta.x > 0 ? 1.0f : -1.0f);
            } else {
                w->outputValue.y = (delta.y > 0 ? 1.0f : -1.0f); // Screen Y is down, output Y positive means down
            }
        }
    } else {
        if (w->controllingFinger != INVALID_FINGER_ID) {
            D("DPad %d lost finger slot %d, resetting", w->id, w->controllingFinger);
            w->controllingFinger = INVALID_FINGER_ID;
        }
        w->outputValue = (Vec2){0, 0};
    }
}

void button_process(Widget* w) {
    bool fingerIsDownOnButton = (w->controllingFinger != INVALID_FINGER_ID && mt_slots[w->controllingFinger].active);

    if (fingerIsDownOnButton) {
        int slot_idx = w->controllingFinger;
        Vec2 touchPos = {mt_slots[slot_idx].x, mt_slots[slot_idx].y};
        if (!Widget_IsInside(w, touchPos)) {
            fingerIsDownOnButton = false;
            D("Button %d: Finger slot %d slid off", w->id, slot_idx);
        }
    }

    if (fingerIsDownOnButton && !w->data.button.isPressed) {
        D("Button %d pressed by finger slot %d", w->id, w->controllingFinger);
        w->data.button.isPressed = true;
        enqueue_event(w->id, EVT_KEY_DOWN, w->data.button.keycode);
        w->outputValue = (Vec2){0, 0};
    } else if (!fingerIsDownOnButton && w->data.button.isPressed) {
        D("Button %d released (finger slot %d)", w->id, (w->controllingFinger != INVALID_FINGER_ID ? w->controllingFinger : -2)); // -2 if already cleared
        w->data.button.isPressed = false;
        enqueue_event(w->id, EVT_KEY_UP, w->data.button.keycode);
        w->outputValue = (Vec2){0, 0};
        if (w->controllingFinger != INVALID_FINGER_ID) { // Clear if it was ours and not already cleared
            w->controllingFinger = INVALID_FINGER_ID;
        }
    }
}


// --- Application UI and Widget Drawing ---

void DrawAllWidgets(int screenW, int screenH, bool editMode) {
    for (int i = 0; i < gNumWidgets; ++i) {
        widget_draw_tbl[gWidgets[i].type](&gWidgets[i]);

        if (editMode) {
            Widget* w = &gWidgets[i];
            bool isSelected = (w->id == gSelectedWidgetId);
            Color boxColor = isSelected ? kColorActive : kColorEditMode;
            Color handleColor = isSelected ? kColorActive : kColorEditModeHandle;

            Vec2 tl = w->absTopLeft;
            float s = w->absSize;

            // Bounding box
            DrawOutlinedRect(tl.x, tl.y, s, s, kOutlineThickness, boxColor);

            // Resize handle
            Vec2 br = {tl.x + s, tl.y + s};
            DrawRect(br.x - kHandleSize, br.y - kHandleSize, kHandleSize, kHandleSize, handleColor);
        }
    }
}

void DrawUserInterface(bool editMode) { // editMode is (gAppState != APP_STATE_RUNNING)
    DrawMainButton(editMode); // This is our main "Back/Cancel" or "Enter/Exit Edit Mode" button

    // Only show Add and Properties buttons when in the main edit mode screen
    if (gAppState == APP_STATE_EDIT_MODE) { // Or APP_STATE_EDIT_IDLE if not renamed
        // Draw Add button as a simple action button, not indicating menu state
        DrawAddButton(false, false);

        if (gSelectedWidgetId != 0) {
            // Draw Properties button as a simple action button if a widget is selected
            DrawPropertiesButton(false);
        }
    }
    // If gAppState is any of the _MENU_ states, Add/Properties buttons will not be drawn.
}

// --- Input Processing Logic ---

static int map_key(int widget_id, Direction d) {
    int idx = FindWidgetIndexById(widget_id);
    if (idx != -1) {
        Widget *w = &gWidgets[idx];
        if (w->type == WIDGET_JOYSTICK || w->type == WIDGET_DPAD) {
            return w->data.analog.keycode[d];
        }
    }
    // Fallback for safety, though should always be mapped for analog widgets
    switch (d) {
        case DIR_UP:    return KEY_W;
        case DIR_DOWN:  return KEY_S;
        case DIR_LEFT:  return KEY_A;
        case DIR_RIGHT: return KEY_D;
        default:        return 0; // Should not happen
    }
}

static void enqueue_event(int widget_id, EventType type, int keycode) {
    if (gInputEventCount < MAX_INPUT_EVENTS) {
        gInputEvents[gInputEventCount++] = (InputEvent){widget_id, type, keycode};
    } else {
        D("Input event queue full!");
    }
}

void InputState_Update(void) {
    for (int i = 0; i < gNumWidgets; ++i) {
        Widget* w = &gWidgets[i];

        if (w->type == WIDGET_JOYSTICK || w->type == WIDGET_DPAD) {
            float x = w->outputValue.x;
            float y = w->outputValue.y;
            bool now_up    = (y < -0.5f);
            bool now_down  = (y >  0.5f);
            bool now_left  = (x < -0.5f);
            bool now_right = (x >  0.5f);

            static bool prev_up[MAX_WIDGETS] = {false};
            static bool prev_down[MAX_WIDGETS] = {false};
            static bool prev_left[MAX_WIDGETS] = {false};
            static bool prev_right[MAX_WIDGETS] = {false};

            if (w->controllingFinger == INVALID_FINGER_ID || (w->controllingFinger < MAX_MT_SLOTS && !mt_slots[w->controllingFinger].active) ) {
                if (prev_up[i])    enqueue_event(w->id, EVT_KEY_UP, map_key(w->id, DIR_UP));
                if (prev_down[i])  enqueue_event(w->id, EVT_KEY_UP, map_key(w->id, DIR_DOWN));
                if (prev_left[i])  enqueue_event(w->id, EVT_KEY_UP, map_key(w->id, DIR_LEFT));
                if (prev_right[i]) enqueue_event(w->id, EVT_KEY_UP, map_key(w->id, DIR_RIGHT));
                prev_up[i] = prev_down[i] = prev_left[i] = prev_right[i] = false;
                continue;
            }

            if (now_up != prev_up[i]) {
                enqueue_event(w->id, now_up ? EVT_KEY_DOWN : EVT_KEY_UP, map_key(w->id, DIR_UP));
                prev_up[i] = now_up;
            }
            if (now_down != prev_down[i]) {
                enqueue_event(w->id, now_down ? EVT_KEY_DOWN : EVT_KEY_UP, map_key(w->id, DIR_DOWN));
                prev_down[i] = now_down;
            }
            if (now_left != prev_left[i]) {
                enqueue_event(w->id, now_left ? EVT_KEY_DOWN : EVT_KEY_UP, map_key(w->id, DIR_LEFT));
                prev_left[i] = now_left;
            }
            if (now_right != prev_right[i]) {
                enqueue_event(w->id, now_right ? EVT_KEY_DOWN : EVT_KEY_UP, map_key(w->id, DIR_RIGHT));
                prev_right[i] = now_right;
            }
        }
    }
}

void InputState_Flush(void) {
    for (int i = 0; i < gInputEventCount; ++i) {
        InputEvent *e = &gInputEvents[i];
        uinput_key(e->keycode, e->type == EVT_KEY_DOWN);
    }
    gInputEventCount = 0;
}

void ProcessAllWidgetsInput(void) {
    if (gAppState != APP_STATE_RUNNING) {
        return;
    }
    for (int i = 0; i < gNumWidgets; ++i) {
        widget_proc_tbl[gWidgets[i].type](&gWidgets[i]);
    }
}

// --- Touch Event Handling and UI Interaction ---

bool HandleUITouchDown(Vec2 p, uint32_t id) {
    if (id == gLastUIFinger) {
        return true; // Debounce same finger
    }

    // Edit/Cancel button
    if (p.x >= kEditButtonX && p.x <= kEditButtonX + kEditButtonW &&
        p.y >= kEditButtonY && p.y <= kEditButtonY + kEditButtonH) {
        D("UI: Edit/Cancel button pressed id=%d at (%.1f,%.1f)", id, p.x, p.y);
        if (gAppState == APP_STATE_RUNNING) {
            gAppState = APP_STATE_EDIT_MODE;
            D("UI: gAppState -> APP_STATE_EDIT_MODE");
        } else if (gAppState == APP_STATE_EDIT_MODE || gAppState == APP_STATE_MENU_ADD_WIDGET || gAppState == APP_STATE_MENU_WIDGET_PROPERTIES) {
            gAppState = APP_STATE_RUNNING;
            gSelectedWidgetId = 0;
            gEditState = (EditState){0};
            D("UI: gAppState -> APP_STATE_RUNNING");
        } else if (gAppState == APP_STATE_MENU_REMAP_ACTION) {
            D("UI: Cancel remap action for widget %d", gRemappingWidgetId);
            // gRemappingWidgetId stays, gRemapAction stays
            gAppState = APP_STATE_MENU_WIDGET_PROPERTIES;
            D("UI: gAppState -> APP_STATE_MENU_WIDGET_PROPERTIES");
        } else if (gAppState == APP_STATE_MENU_REMAP_KEY) {
            D("UI: Cancel remap key for widget %d", gRemappingWidgetId);
            gRemappingWidgetId = 0; // Clear remapping target
            gRemapAction = -1;      // Clear remapping action
            gAppState = APP_STATE_MENU_WIDGET_PROPERTIES; // Go back to properties of the (previously) selected widget
            D("UI: gAppState -> APP_STATE_MENU_WIDGET_PROPERTIES");
        }
        gLastUIFinger = id;
        return true;
    }

    // Add button
    if (p.x >= kAddButtonX && p.x <= kAddButtonX + kAddButtonW &&
        p.y >= kAddButtonY && p.y <= kAddButtonY + kAddButtonH) {
        if (gAppState == APP_STATE_EDIT_MODE) {
            D("UI: Add button pressed (Enter Select) id=%d at (%.1f,%.1f)", id, p.x, p.y);
            gAppState = APP_STATE_MENU_ADD_WIDGET;
            D("UI: gAppState -> APP_STATE_MENU_ADD_WIDGET");
            gLastUIFinger = id;
            return true;
        } else if (gAppState == APP_STATE_MENU_ADD_WIDGET) {
            D("UI: Add button pressed (Exit Select) id=%d at (%.1f,%.1f)", id, p.x, p.y);
            gAppState = APP_STATE_EDIT_MODE;
            D("UI: gAppState -> APP_STATE_EDIT_MODE");
            gLastUIFinger = id;
            return true;
        }
    }
    
    // Properties ("Edit") button
    if (p.x >= kPropsButtonX && p.x <= kPropsButtonX + kPropsButtonW &&
        p.y >= kPropsButtonY && p.y <= kPropsButtonY + kPropsButtonH) {
        if (gAppState == APP_STATE_EDIT_MODE && gSelectedWidgetId != 0) {
            D("UI: Edit button pressed (Enter Properties) for widget %d. id=%d at (%.1f,%.1f)", gSelectedWidgetId, id, p.x, p.y);
            gAppState = APP_STATE_MENU_WIDGET_PROPERTIES;
            D("UI: gAppState -> APP_STATE_MENU_WIDGET_PROPERTIES");
            gLastUIFinger = id;
            return true;
        } else if (gAppState == APP_STATE_MENU_WIDGET_PROPERTIES) {
            D("UI: Edit button pressed (Exit Properties). id=%d at (%.1f,%.1f)", id, p.x, p.y);
            gAppState = APP_STATE_EDIT_MODE;
            D("UI: gAppState -> APP_STATE_EDIT_MODE");
            gLastUIFinger = id;
            return true;
        }
    }
    return false;
}

bool HandleWidgetEditAction(Vec2 touchPos) {
    if (gAppState != APP_STATE_EDIT_MODE || !gEditState.targetWidget) {
        return false;
    }
    
    Widget* w = gEditState.targetWidget;
    
    if (gEditState.action == EDIT_MOVE) {
        Vec2 delta = {touchPos.x - gEditState.startTouchPos.x, touchPos.y - gEditState.startTouchPos.y};
        Vec2 newCenter = {gEditState.startWidgetCenter.x + delta.x, gEditState.startWidgetCenter.y + delta.y};
        w->normCenter.x = newCenter.x / width;
        w->normCenter.y = newCenter.y / height;
        Widget_ClampToScreen(w, width, height);
        return true;
    } else if (gEditState.action == EDIT_RESIZE) {
        Vec2 center = w->absCenter;
        float distance = dist(touchPos, center);
        float ratio = (gEditState.startTouchDistance > 1e-5f) ? distance / gEditState.startTouchDistance : 1.0f;
        w->normHalfSize = gEditState.startWidgetHalfSize * ratio;
        Widget_ClampToScreen(w, width, height);
        return true;
    }
    return false;
}

static const char* find_touchscreen_device(void) {
    static char path[32]; // Stays function-local static
    int fd;
    struct input_absinfo abs_info;
    
    for (int i = 0; i < 20; i++) { // Check event0 to event19
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        fd = open(path, O_RDONLY | O_NONBLOCK); // Use O_NONBLOCK to avoid hanging
        if (fd >= 0) {
            if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &abs_info) == 0) {
                // Further check if it has actual range, some devices report 0 range
                if (abs_info.maximum > abs_info.minimum) {
                    close(fd);
                    D("Found touchscreen: %s", path);
                    return path;
                }
            }
            close(fd);
        }
    }
    D("No touchscreen device found.");
    return NULL;
}

static void init_touch_device(const char *device) {
    gTouchDevFd = open(device, O_RDONLY | O_NONBLOCK);
    if (gTouchDevFd < 0) {
        perror("open touch device");
        exit(EXIT_FAILURE);
    }
    if (ioctl(gTouchDevFd, EVIOCGRAB, (void*)1) < 0) { // Cast 1 to void* or int depending on ioctl
        perror("EVIOCGRAB");
        close(gTouchDevFd); // Close fd on failure
        exit(EXIT_FAILURE);
    }
    struct input_absinfo absinfo;
    if (ioctl(gTouchDevFd, EVIOCGABS(ABS_MT_POSITION_X), &absinfo) == 0) {
        touch_min_x = absinfo.minimum;
        touch_max_x = absinfo.maximum;
    } else { /* Handle error or set defaults */ }
    if (ioctl(gTouchDevFd, EVIOCGABS(ABS_MT_POSITION_Y), &absinfo) == 0) {
        touch_min_y = absinfo.minimum;
        touch_max_y = absinfo.maximum;
    } else { /* Handle error or set defaults */ }
    D("Touchscreen initialized: X(%d-%d), Y(%d-%d)", touch_min_x, touch_max_x, touch_min_y, touch_max_y);
}

static void handle_evdev_event(const struct input_event *ev) {
    switch (ev->type) {
        case EV_ABS:
            if (ev->code == ABS_MT_SLOT) {
                current_slot = ev->value % MAX_MT_SLOTS;
            } else if (ev->code == ABS_MT_TRACKING_ID) {
                if (current_slot < 0 || current_slot >= MAX_MT_SLOTS) break; // Invalid slot
                if (ev->value >= 0) { // Touch down
                    mt_slots[current_slot].active = true;
                    mt_slots[current_slot].was_down = false; // Will be set true after processing this event batch
                    slot_mode[current_slot] = SLOT_IDLE;   // Default mode
                } else { // Touch up
                    mt_slots[current_slot].active = false;
                    // was_down will be handled in SYN_REPORT
                }
            } else if (ev->code == ABS_MT_POSITION_X) {
                if (current_slot < 0 || current_slot >= MAX_MT_SLOTS) break;
                if (gLandscapeMode) {
                    mt_slots[current_slot].y = (touch_max_x > touch_min_x) ?
                        (double)(touch_max_x - ev->value) / (touch_max_x - touch_min_x) * height : 0;
                } else {
                    mt_slots[current_slot].x = (touch_max_x > touch_min_x) ?
                        (double)(ev->value - touch_min_x) / (touch_max_x - touch_min_x) * width : 0;
                }
            } else if (ev->code == ABS_MT_POSITION_Y) {
                if (current_slot < 0 || current_slot >= MAX_MT_SLOTS) break;
                if (gLandscapeMode) {
                    mt_slots[current_slot].x = (touch_max_y > touch_min_y) ?
                        (double)(ev->value - touch_min_y) / (touch_max_y - touch_min_y) * width : 0;
                } else {
                    mt_slots[current_slot].y = (touch_max_y > touch_min_y) ?
                        (double)(ev->value - touch_min_y) / (touch_max_y - touch_min_y) * height : 0;
                }
            }
            break;

        case EV_SYN:
            if (ev->code == SYN_REPORT) {
                for (int s = 0; s < MAX_MT_SLOTS; ++s) {
                    MTSlot *slot = &mt_slots[s];
                    Vec2 p = {slot->x, slot->y};
                    bool handled_by_ui_button = false;

                    // Touch Down Logic
                    if (slot->active && !slot->was_down) {
                        handled_by_ui_button = HandleUITouchDown(p, s);
                        if (handled_by_ui_button) {
                            slot_mode[s] = SLOT_WIDGET; // UI buttons are treated as widgets for flow
                        } else {
                            switch (gAppState) {
                                case APP_STATE_RUNNING:
                                    bool overWidget = false; Widget* hitWidget = NULL;
                                    for (int i = 0; i < gNumWidgets; ++i) {
                                        if (Widget_IsInside(&gWidgets[i], p)) {
                                            overWidget = true; hitWidget = &gWidgets[i]; break;
                                        }
                                    }
                                    if (overWidget) {
                                        D("Widget control START for widget %d by slot %d", hitWidget->id, s);
                                        slot_mode[s] = SLOT_WIDGET;
                                        hitWidget->controllingFinger = s;
                                        if (widget_proc_tbl[hitWidget->type]) {
                                            widget_proc_tbl[hitWidget->type](hitWidget); // Initial process
                                        }
                                    } else {
                                        D("Trackpad START for slot %d", s);
                                        slot_mode[s] = SLOT_TRACKPAD;
                                        track_last_x[s] = p.x; track_last_y[s] = p.y;
                                        track_accum_x[s] = 0; track_accum_y[s] = 0;
                                        track_moved[s] = false;
                                    }
                                    break;
                                case APP_STATE_EDIT_MODE:
                                    bool hitWidgetAction = false;
                                    gEditState.targetWidget = NULL; gEditState.action = EDIT_NONE;
                                    for (int i = gNumWidgets - 1; i >= 0; --i) { // Iterate backwards for top-most
                                        Widget* w = &gWidgets[i];
                                        if (Widget_IsInside(w, p)) {
                                            if (w->id == gSelectedWidgetId) { // Interacting with already selected widget
                                                Vec2 tl = w->absTopLeft; float sz = w->absSize;
                                                if (p.x >= tl.x + sz - kHandleSize && p.y >= tl.y + sz - kHandleSize) { // Resize handle
                                                    D("Edit: Start RESIZE for selected widget %d, slot %d", w->id, s);
                                                    gEditState.targetWidget = w; gEditState.action = EDIT_RESIZE;
                                                    gEditState.startTouchPos = p; gEditState.startWidgetHalfSize = w->normHalfSize;
                                                    gEditState.startTouchDistance = dist(p, w->absCenter);
                                                } else { // Move selected widget
                                                    D("Edit: Start MOVE for selected widget %d, slot %d", w->id, s);
                                                    gEditState.targetWidget = w; gEditState.action = EDIT_MOVE;
                                                    gEditState.startTouchPos = p; gEditState.startWidgetCenter = w->absCenter;
                                                }
                                            } else { // Select a new widget
                                                D("Edit: SELECT widget %d (deselecting %d) with slot %d", w->id, gSelectedWidgetId, s);
                                                gSelectedWidgetId = w->id;
                                                // Also prepare for immediate move
                                                gEditState.targetWidget = w;
                                                gEditState.action = EDIT_MOVE;
                                                gEditState.startTouchPos = p;
                                                gEditState.startWidgetCenter = w->absCenter;
                                            }
                                            hitWidgetAction = true; slot_mode[s] = SLOT_WIDGET;
                                            break; 
                                        }
                                    }
                                    if (!hitWidgetAction) { // Clicked on background
                                        D("Touch in APP_STATE_EDIT_MODE on background (slot %d) -> Deselecting widget %d", s, gSelectedWidgetId);
                                        gSelectedWidgetId = 0;
                                        // slot_mode[s] remains SLOT_IDLE or is handled by UI
                                    }
                                    break;
                                case APP_STATE_MENU_ADD_WIDGET:
                                    {
                                        float totalMenuHeight = (kMenuButtonH + kMenuButtonSpacing) * numAvailableWidgetTypes - kMenuButtonSpacing;
                                        float startY = ((float)height - totalMenuHeight) * 0.5f;
                                        float startX = ((float)width - kMenuButtonW) * 0.5f;
                                        for (int i = 0; i < numAvailableWidgetTypes; ++i) {
                                            float btnX = startX; float btnY = startY + i * (kMenuButtonH + kMenuButtonSpacing);
                                            if (p.x >= btnX && p.x <= btnX + kMenuButtonW && p.y >= btnY && p.y <= btnY + kMenuButtonH) {
                                                D("Menu item %d ('%s') selected", i, availableWidgetNames[i]);
                                                CreateWidget(availableWidgetTypes[i], (Vec2){0.5f, 0.5f}, 0.1f);
                                                gAppState = APP_STATE_EDIT_MODE;
                                                D("State transition -> APP_STATE_EDIT_MODE");
                                                slot_mode[s] = SLOT_WIDGET; // Consumed by menu
                                                break;
                                            }
                                        }
                                    }
                                    break;
                                case APP_STATE_MENU_WIDGET_PROPERTIES:
                                    {
                                        float totalMenuHeight = (kMenuButtonH + kMenuButtonSpacing) * numAvailablePropertyActions - kMenuButtonSpacing;
                                        float startY = ((float)height - totalMenuHeight) * 0.5f;
                                        float startX = ((float)width - kMenuButtonW) * 0.5f;
                                        for (int i = 0; i < numAvailablePropertyActions; ++i) {
                                            float btnX = startX; float btnY = startY + i * (kMenuButtonH + kMenuButtonSpacing);
                                            if (p.x >= btnX && p.x <= btnX + kMenuButtonW && p.y >= btnY && p.y <= btnY + kMenuButtonH) {
                                                PropertyAction action = availablePropertyActions[i];
                                                D("Properties Menu item %d ('%s') selected for widget %d", i, availablePropertyNames[i], gSelectedWidgetId);
                                                if (action == PROP_ACTION_DELETE) {
                                                    int idx = FindWidgetIndexById(gSelectedWidgetId);
                                                    if (idx != -1) RemoveWidgetByIndex(idx);
                                                    gSelectedWidgetId = 0; gAppState = APP_STATE_EDIT_MODE;
                                                } else if (action == PROP_ACTION_REMAP) {
                                                    int idx = FindWidgetIndexById(gSelectedWidgetId);
                                                    if (idx != -1) {
                                                        gRemappingWidgetId = gSelectedWidgetId;
                                                        if (gWidgets[idx].type == WIDGET_BUTTON) gAppState = APP_STATE_MENU_REMAP_KEY;
                                                        else if (gWidgets[idx].type == WIDGET_JOYSTICK || gWidgets[idx].type == WIDGET_DPAD) {
                                                            gRemapAction = 0; // Default to "Up"
                                                            gAppState = APP_STATE_MENU_REMAP_ACTION;
                                                        } else gAppState = APP_STATE_MENU_WIDGET_PROPERTIES; // Unsupported
                                                    }
                                                }
                                                slot_mode[s] = SLOT_WIDGET;
                                                break;
                                            }
                                        }
                                    }
                                    break;
                                case APP_STATE_MENU_REMAP_ACTION:
                                    {
                                        float totalMenuHeight = (kMenuButtonH + kMenuButtonSpacing) * numAnalogActions - kMenuButtonSpacing;
                                        float startY = ((float)height - totalMenuHeight) * 0.5f;
                                        float startX = ((float)width - kMenuButtonW) * 0.5f;
                                        for (int i = 0; i < numAnalogActions; ++i) {
                                            float btnX = startX; float btnY = startY + i * (kMenuButtonH + kMenuButtonSpacing);
                                            if (p.x >= btnX && p.x <= btnX + kMenuButtonW && p.y >= btnY && p.y <= btnY + kMenuButtonH) {
                                                D("Analog Action Selection: picked '%s' for widget %d", availableAnalogActionNames[i], gRemappingWidgetId);
                                                gRemapAction = i; gAppState = APP_STATE_MENU_REMAP_KEY;
                                                slot_mode[s] = SLOT_WIDGET;
                                                break;
                                            }
                                        }
                                    }
                                    break;
                                case APP_STATE_MENU_REMAP_KEY:
                                    {
                                        // Detect touch hit on key buttons using cached grid layout
                                        for (int i = 0; i < gNumMappableKeys; ++i) {
                                            int row = i / gKeyGridLayout.cols;
                                            int col = i % gKeyGridLayout.cols;
                                            float btnX = gKeyGridLayout.startX + col * (gKeyGridLayout.cellSize + gKeyGridLayout.cellSpacing);
                                            float btnY = gKeyGridLayout.startY + row * (gKeyGridLayout.cellSize + gKeyGridLayout.cellSpacing);
                                            if (p.x >= btnX && p.x <= btnX + gKeyGridLayout.cellSize &&
                                                p.y >= btnY && p.y <= btnY + gKeyGridLayout.cellSize) {
                                                D("Key Selection: Hit button %d ('%s')", i, gMappableKeys[i].label);
                                                int idx = FindWidgetIndexById(gRemappingWidgetId);
                                                if (idx != -1) {
                                                    Widget* w = &gWidgets[idx];
                                                    if (w->type == WIDGET_BUTTON) {
                                                        w->data.button.keycode = gMappableKeys[i].keycode;
                                                        w->data.button.mappedLabel = gMappableKeys[i].label;
                                                    } else if (w->type == WIDGET_JOYSTICK || w->type == WIDGET_DPAD) {
                                                        if (gRemapAction >= 0 && gRemapAction < numAnalogActions) {
                                                            w->data.analog.keycode[gRemapAction] = gMappableKeys[i].keycode;
                                                            w->data.analog.mappedLabel[gRemapAction] = gMappableKeys[i].label;
                                                        }
                                                    }
                                                }
                                                gRemappingWidgetId = 0; gRemapAction = -1;
                                                gAppState = APP_STATE_EDIT_MODE;
                                                D("State transition -> APP_STATE_EDIT_MODE (from remap_key)");
                                                slot_mode[s] = SLOT_WIDGET;
                                                break;
                                            }
                                        }
                                         }
                                         break;
                            } // end switch gAppState
                        } // end else !handled_by_ui_button
                        // Global catch: any tap outside active menu should cancel it
                        if (!handled_by_ui_button
                            && slot_mode[s] == SLOT_IDLE
                            && gAppState != APP_STATE_RUNNING
                            && gAppState != APP_STATE_EDIT_MODE)
                        {
                            D("Touch outside menu -> cancelling current menu");
                            gAppState = APP_STATE_EDIT_MODE;
                            slot_mode[s] = SLOT_WIDGET; // consume this touch
                        }
                        slot->was_down = true; // Mark as processed for down state
                    }
                    // Motion Logic
                    else if (slot->active && slot->was_down) {
                        if (slot_mode[s] == SLOT_WIDGET) {
                            if (gAppState == APP_STATE_EDIT_MODE && gEditState.targetWidget && gEditState.action != EDIT_NONE) {
                                HandleWidgetEditAction(p);
                            }
                            // In RUNNING state, widget_process called in main loop handles motion via controllingFinger
                        } else if (slot_mode[s] == SLOT_TRACKPAD) {
                            if (gAppState == APP_STATE_RUNNING) {
                                double dx = p.x - track_last_x[s]; double dy = p.y - track_last_y[s];
                                if (dx != 0 || dy != 0) {
                                    track_last_x[s] = p.x; track_last_y[s] = p.y;
                                    track_accum_x[s] += dx * kTrackpadSensitivity;
                                    track_accum_y[s] += dy * kTrackpadSensitivity;
                                    int mx = (int)track_accum_x[s], my = (int)track_accum_y[s];
                                    if (mx || my) {
                                        track_moved[s] = true; uinput_move(mx, my);
                                        track_accum_x[s] -= mx; track_accum_y[s] -= my;
                                    }
                                }
                            }
                        }
                    }
                    // Touch Up Logic
                    else if (!slot->active && slot->was_down) {
                        if (s == gLastUIFinger) gLastUIFinger = -1; // Debounce UI finger

                        if (slot_mode[s] == SLOT_WIDGET) {
                            D("Slot %d WIDGET release in state %d", s, gAppState);
                            if (gAppState == APP_STATE_EDIT_MODE) {
                                if (gEditState.targetWidget && gEditState.action != EDIT_NONE) {
                                    // If this slot was driving an edit action, finalize it.
                                    // The check relies on gEditState.targetWidget being set by this slot's touch-down.
                                    D("Slot %d WIDGET release in APP_STATE_EDIT_MODE -> Resetting edit state", s);
                                    gEditState = (EditState){NULL, EDIT_NONE, {0,0}, {0,0}, 0.0f, 0.0f};
                                }
                            } else if (gAppState == APP_STATE_RUNNING) {
                                D("Slot %d WIDGET release in RUNNING -> Handle normal release", s);
                                for (int i = 0; i < gNumWidgets; ++i) {
                                    if (gWidgets[i].controllingFinger == s) {
                                        D("Releasing finger from widget %d (slot %d)", gWidgets[i].id, s);
                                        gWidgets[i].controllingFinger = INVALID_FINGER_ID;
                                        // Widget's _process or InputState_Update will handle state reset.
                                        break;
                                    }
                                }
                            }
                        } else if (slot_mode[s] == SLOT_TRACKPAD) {
                            D("Slot %d TRACKPAD release in state %d", s, gAppState);
                            if (gAppState == APP_STATE_RUNNING) {
                                if (!track_moved[s]) { // If no movement, it's a click
                                    uinput_key(BTN_LEFT, true); uinput_key(BTN_LEFT, false);
                                    D("Trackpad click generated for slot %d", s);
                                }
                            }
                        } else {
                             D("Slot %d release in IDLE/unexpected mode (%d)", s, slot_mode[s]);
                        }
                        slot_mode[s] = SLOT_IDLE; // Reset mode on release
                        slot->was_down = false;   // Mark as processed for up state
                    }
                } // end for each slot
            } // end if SYN_REPORT
            break; // end EV_SYN
    } // end switch ev->type
}


// --- Wayland Setup and Callbacks ---

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = NULL,
};

static void registry_handle_global(void *data, struct wl_registry *registry_ptr,
                                   uint32_t name, const char *interface, uint32_t version) {
    D("registry_handle_global: interface=%s", interface);
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(registry_ptr, name, &wl_compositor_interface, 4);
        D("Bound wl_compositor: %p", compositor);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        layer_shell = wl_registry_bind(registry_ptr, name, &zwlr_layer_shell_v1_interface, MIN(version, 4)); // Bind to min of offered and supported
        D("Bound zwlr_layer_shell_v1: %p (version %u)", layer_shell, MIN(version, 4));
    }
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = NULL, // TODO: Handle closed event to exit cleanly
};

static void layer_surface_handle_configure(void *data,
                                           struct zwlr_layer_surface_v1 *surface_v1,
                                           uint32_t serial,
                                           uint32_t w, uint32_t h) {
    D("layer_surface_handle_configure: w=%u h=%u serial=%u", w, h, serial);
    width = w;
    height = h;
    gViewportChanged = true; // Signal that viewport dimensions have changed

    if (egl_window) {
        D("Resizing EGL window to %u x %u", width, height);
        wl_egl_window_resize(egl_window, width, height, 0, 0);
    }
    zwlr_layer_surface_v1_ack_configure(surface_v1, serial);

    if (surface && compositor) { // Ensure surface and compositor are valid
        struct wl_region *empty_region = wl_compositor_create_region(compositor);
        wl_surface_set_input_region(surface, empty_region);
        wl_region_destroy(empty_region);
        wl_surface_commit(surface); // Commit surface changes
    }
    
    // Recalculate Key Grid Layout
    {
        float menuContentStartY = kEditButtonY + kEditButtonH + 20.0f;
        float titleActualPixelSize = 2.0f;
        float titleTextRenderHeight = 8.0f * titleActualPixelSize;
        float paddingBelowTitle = 10.0f;
        float offsetTop = menuContentStartY + titleTextRenderHeight + paddingBelowTitle;
        CalculateGridLayout(width, height, gNumMappableKeys,
                            kKeyGridCols, kKeyButtonSize, kKeyButtonSpacing,
                            offsetTop, &gKeyGridLayout);
        gScaledKeyButtonSize = gKeyGridLayout.cellSize;
        gScaledKeyButtonSpacing = gKeyGridLayout.cellSpacing;
    }
}

// --- Main Application Logic ---

void RenderFrame(int w_param, int h_param, EGLDisplay dpy, EGLSurface surf) {
    if (gViewportChanged) {
        glViewport(0, 0, w_param, h_param);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0.0, (double)w_param, (double)h_param, 0.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        gViewportChanged = false;
    }

    glClear(GL_COLOR_BUFFER_BIT);

    bool showEditBoxes = (gAppState != APP_STATE_RUNNING);
    DrawAllWidgets(w_param, h_param, showEditBoxes);

    if (gAppState == APP_STATE_MENU_ADD_WIDGET) {
        DrawWidgetSelectionMenu(w_param, h_param);
    } else if (gAppState == APP_STATE_MENU_WIDGET_PROPERTIES) {
        DrawWidgetPropertiesMenu(w_param, h_param);
    } else if (gAppState == APP_STATE_MENU_REMAP_ACTION) {
        DrawAnalogActionSelectionMenu(w_param, h_param);
    } else if (gAppState == APP_STATE_MENU_REMAP_KEY) {
        DrawKeySelectionMenu(w_param, h_param);
    }
    
    DrawUserInterface(showEditBoxes);
    
    eglSwapBuffers(dpy, surf);
}

int main(void) {
    display = wl_display_connect(NULL);
    if (!display) { fprintf(stderr, "wl_display_connect failed\n"); return EXIT_FAILURE; }

    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display); // Initial roundtrip to get globals

    if (!compositor || !layer_shell) {
        fprintf(stderr, "Failed to bind Wayland globals (compositor or layer_shell)\n");
        return EXIT_FAILURE;
    }

    surface = wl_compositor_create_surface(compositor);
    if (!surface) { fprintf(stderr, "wl_compositor_create_surface failed\n"); return EXIT_FAILURE; }
    
    layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "gamepad_overlay");
    if (!layer_surface) { fprintf(stderr, "get_layer_surface failed\n"); return EXIT_FAILURE; }

    zwlr_layer_surface_v1_add_listener(layer_surface, &layer_surface_listener, NULL);
    zwlr_layer_surface_v1_set_size(layer_surface, 0, 0); // Size 0,0 means compositor decides
    zwlr_layer_surface_v1_set_anchor(layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, -1); // Cover entire screen

    wl_surface_set_opaque_region(surface, NULL); // Transparent background
    wl_surface_commit(surface);
    wl_display_roundtrip(display); // Second roundtrip for configure event

    egl_window = wl_egl_window_create(surface, width, height);
    if (!egl_window) { fprintf(stderr, "wl_egl_window_create failed\n"); return EXIT_FAILURE; }

    egl_display = eglGetDisplay((EGLNativeDisplayType)display);
    if (egl_display == EGL_NO_DISPLAY) { fprintf(stderr, "eglGetDisplay failed\n"); return EXIT_FAILURE; }
    if (!eglInitialize(egl_display, &egl_major, &egl_minor)) { fprintf(stderr, "eglInitialize failed\n"); return EXIT_FAILURE; }
    
    eglBindAPI(EGL_OPENGL_API); // For desktop GL immediate mode
    EGLConfig egl_config;
    EGLint num_config;
    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, // For desktop GL
        EGL_NONE
    };
    if (!eglChooseConfig(egl_display, config_attribs, &egl_config, 1, &num_config) || num_config == 0) {
        fprintf(stderr, "eglChooseConfig failed\n"); return EXIT_FAILURE;
    }

    egl_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, NULL); // No specific attributes for compatibility profile
    if (egl_context == EGL_NO_CONTEXT) { fprintf(stderr, "eglCreateContext failed\n"); return EXIT_FAILURE; }
    
    egl_surface = eglCreateWindowSurface(egl_display, egl_config, (EGLNativeWindowType)egl_window, NULL);
    if (egl_surface == EGL_NO_SURFACE) { fprintf(stderr, "eglCreateWindowSurface failed\n"); return EXIT_FAILURE; }

    if (!eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context)) {
        fprintf(stderr, "eglMakeCurrent failed\n"); return EXIT_FAILURE;
    }
    
    eglSwapInterval(egl_display, 1); // Enable vsync

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);

    UpdateAllWidgetCoords(width, height); // Initial widget coordinate calculation
    
    if (!uinput_init()) {
        fprintf(stderr, "uinput_init failed\n");
        // Proper cleanup would be needed here
        return EXIT_FAILURE;
    }

    const char *touch_path = find_touchscreen_device();
    if (!touch_path) {
        fprintf(stderr, "No touchscreen found, exiting.\n");
        return EXIT_FAILURE;
    } else {
        init_touch_device(touch_path);
    }

    // Find and grab volume-down device for toggle
    gVolDevFd = find_input_device(EV_KEY, KEY_VOLUMEDOWN);
    if (gVolDevFd < 0) {
        fprintf(stderr, "No volume-down device found\n");
        return EXIT_FAILURE;
    }
    ioctl(gVolDevFd, EVIOCGRAB, 1);

    // Find and grab volume-up device for landscape toggle
    gVolUpDevFd = find_input_device(EV_KEY, KEY_VOLUMEUP);
    if (gVolUpDevFd >= 0) {
        ioctl(gVolUpDevFd, EVIOCGRAB, 1);
    }

    int wl_fd = wl_display_get_fd(display);
    struct pollfd fds[4];  // Expanded to include volume-up
    int nfds = 0;
    // Watch Wayland, touch, and volume-down fds
    fds[nfds++] = (struct pollfd){.fd = wl_fd,      .events = POLLIN};
    if (gTouchDevFd >= 0) {
        fds[nfds++] = (struct pollfd){.fd = gTouchDevFd, .events = POLLIN};
    }
    if (gVolDevFd >= 0) {
        fds[nfds++] = (struct pollfd){.fd = gVolDevFd,   .events = POLLIN};
    }
    if (gVolUpDevFd >= 0) {
        fds[nfds++] = (struct pollfd){.fd = gVolUpDevFd, .events = POLLIN};
    }
    
    // Initial render before loop
    RenderFrame(width, height, egl_display, egl_surface);

    bool running = true;
    while (running) {
        // Dispatch pending Wayland events without blocking
        while (wl_display_prepare_read(display) != 0) {
            if (wl_display_dispatch_pending(display) == -1) {
                running = false; break; // Error in dispatch
            }
        }
        if (!running) break;

        if (wl_display_flush(display) == -1) { // Flush outstanding Wayland requests
             running = false; break; // Error in flush
        }
        
        // If wl_display_prepare_read succeeded, we can poll
        int timeout_ms = -1;
        if (gVolDown && !gVolToggled) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long dt_ns = (now.tv_sec - gVolTs.tv_sec) * 1000000000L + (now.tv_nsec - gVolTs.tv_nsec);
            long rem_ns = LONG_PRESS_NS - dt_ns;
            if (rem_ns < 0) rem_ns = 0;
            timeout_ms = (int)(rem_ns / 1000000L);
        }
        int ret = poll(fds, nfds, timeout_ms); // Block until event or timeout
        if (ret < 0) {
            perror("poll");
            running = false; // Error in poll
            wl_display_cancel_read(display); // Cancel the read intent
            break;
        }

        if (wl_display_read_events(display) == -1) { // Process Wayland events
            running = false; break; // Error reading events
        }

        // Handle volume-down press/release for long-press toggle
        if (gVolDevFd >= 0 && fds[2].revents & POLLIN) {
            struct input_event ev;
            while (read(gVolDevFd, &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type == EV_KEY && ev.code == KEY_VOLUMEDOWN) {
                    if (ev.value == 1) {
                        clock_gettime(CLOCK_MONOTONIC, &gVolTs);
                        gVolDown = true;
                        gVolToggled = false;
                    } else if (ev.value == 0 && gVolDown) {
                        // Quick press: forward volume-down to system if released before threshold
                        struct timespec now;
                        clock_gettime(CLOCK_MONOTONIC, &now);
                        long dt = (now.tv_sec - gVolTs.tv_sec) * 1000000000L + (now.tv_nsec - gVolTs.tv_nsec);
                        if (dt < LONG_PRESS_NS && !gVolToggled) {
                            uinput_key(KEY_VOLUMEDOWN, true);
                            uinput_key(KEY_VOLUMEDOWN, false);
                        }
                        gVolDown = false;
                        gVolToggled = false;
                    }
                }
            }
        }
        // Handle volume-up press for landscape toggle
        if (gVolUpDevFd >= 0 && fds[3].revents & POLLIN) {
            struct input_event ev;
            while (read(gVolUpDevFd, &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type == EV_KEY && ev.code == KEY_VOLUMEUP) {
                    if (!gOverlayActive) {
                        // overlay hidden: just forward the event
                        uinput_key(KEY_VOLUMEUP, ev.value == 1);
                    } else {
                        if (ev.value == 1) {
                            // record press time
                            clock_gettime(CLOCK_MONOTONIC, &gVolUpTs);
                            gVolUpDown = true;
                        } else if (ev.value == 0 && gVolUpDown) {
                            // on release, decide tap vs hold
                            struct timespec now;
                            clock_gettime(CLOCK_MONOTONIC, &now);
                            long dt = (now.tv_sec - gVolUpTs.tv_sec) * 1000000000L + (now.tv_nsec - gVolUpTs.tv_nsec);
                            if (dt < LONG_PRESS_NS) {
                                // quick tap: forward volume-up
                                uinput_key(KEY_VOLUMEUP, true);
                                uinput_key(KEY_VOLUMEUP, false);
                            } else {
                                // hold: toggle landscape
                                gLandscapeMode = !gLandscapeMode;
                            }
                            gVolUpDown = false;
                        }
                    }
                }
            }
        }
        // Immediate toggle once hold threshold is reached
        if (gVolDown && !gVolToggled) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long dt = (now.tv_sec - gVolTs.tv_sec)*1000000000L + (now.tv_nsec - gVolTs.tv_nsec);
            if (dt >= LONG_PRESS_NS) {
                toggle_overlay();
                gVolToggled = true;
            }
        }
        // Skip all input/render when overlay is off
        if (!gOverlayActive) {
            continue;
        }

        if (gTouchDevFd >= 0 && fds[1].revents & POLLIN) {
            struct input_event ev;
            ssize_t read_len;
            while ((read_len = read(gTouchDevFd, &ev, sizeof(ev))) == sizeof(ev)) {
                handle_evdev_event(&ev);
            }
            if (read_len < 0 && errno != EAGAIN) {
                 perror("read touch device"); running = false; break;
            }
        }
        
        UpdateAllWidgetCoords(width, height);
        ProcessAllWidgetsInput();
        InputState_Update();
        InputState_Flush();
        RenderFrame(width, height, egl_display, egl_surface);
    }

    // Cleanup
    uinput_destroy();
    if (gTouchDevFd >= 0) close(gTouchDevFd);
    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (egl_surface != EGL_NO_SURFACE) eglDestroySurface(egl_display, egl_surface);
    if (egl_context != EGL_NO_CONTEXT) eglDestroyContext(egl_display, egl_context);
    if (egl_window) wl_egl_window_destroy(egl_window);
    eglTerminate(egl_display);
    if (layer_surface) zwlr_layer_surface_v1_destroy(layer_surface);
    if (surface) wl_surface_destroy(surface);
    if (registry) wl_registry_destroy(registry);
    if (display) wl_display_disconnect(display);

    return EXIT_SUCCESS;
}
