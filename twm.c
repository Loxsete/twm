#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/select.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <ctype.h>

#define MAX_WINDOWS 64
#define MAX_WORKSPACES 9
#define MAX_KEYBINDS 100
#define MAX_AUTOSTART 32
#define CONFIG_FILE "~/.config/twm/twm.conf"

typedef struct {
    KeySym keysym;
    unsigned int modifier;
    char *command;
} Keybind;

typedef struct {
    Window window;
    int is_fullscreen;
    int is_floating;
    int x, y, width, height;
    int workspace;
    int managed;
} WindowState;

Display *display;
Window root;
Window windows[MAX_WINDOWS];
WindowState window_states[MAX_WINDOWS];
int window_count = 0;
Window focused = None;
int dragging = 0;
int resizing = 0;
Window drag_window;
int drag_start_x, drag_start_y;
int drag_start_width, drag_start_height;
Window last_focused[MAX_WORKSPACES + 1];
int current_workspace = 1;

Window bar = 0;
int bar_height = 24;
GC bar_gc = 0;
XFontStruct *bar_font = NULL;
unsigned long bar_bg = 0x222222;
unsigned long bar_fg = 0xFFFFFF;
unsigned long background_color = 0x000000;
unsigned long border_color = 0x444444;
unsigned long border_focus_color = 0x0000FF;
char *wallpaper_path = NULL;

const int gap_inner = 8;
const int gap_outer = 16;
const int border_width = 1;

Atom net_number_of_desktops;
Atom net_current_desktop;
Atom net_wm_desktop;
Atom net_desktop_names;

Keybind *keybinds = NULL;
int keybind_count = 0;
char *autostart_commands[MAX_AUTOSTART];
int autostart_count = 0;

char **saved_argv;
int saved_argc;

time_t last_bar_update = 0;

void trim(char *str) {
    char *end = str + strlen(str) - 1;
    while (end > str && isspace(*end)) end--;
    *(end + 1) = '\0';
    while (*str && isspace(*str)) str++;
    memmove(str, str, strlen(str) + 1);
}

unsigned int parse_modifier(const char *mod_str) {
    unsigned int mod = 0;
    if (strstr(mod_str, "Mod4")) mod |= Mod4Mask;
    if (strstr(mod_str, "Shift")) mod |= ShiftMask;
    if (strstr(mod_str, "Control")) mod |= ControlMask;
    return mod;
}

KeySym parse_keysym(const char *key) {
    if (strlen(key) == 1 && isalnum(key[0])) {
        return XStringToKeysym(key);
    }
    return XStringToKeysym(key);
}

void load_config() {
    char config_path[256];
    const char *home = getenv("HOME");
    snprintf(config_path, sizeof(config_path), "%s/.config/twm/twm.conf", home);
    
    FILE *f = fopen(config_path, "r");
    if (!f) return;

    char line[512];
    char section[64] = "";
    keybinds = malloc(MAX_KEYBINDS * sizeof(Keybind));
    keybind_count = 0;
    autostart_count = 0;

    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!line[0] || line[0] == '#') continue;

        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end) {
                *end = '\0';
                strcpy(section, line + 1);
                continue;
            }
        }

        char *key = strtok(line, "=");
        char *value = strtok(NULL, "=");
        if (!key || !value) continue;
        trim(key);
        trim(value);

        if (strcmp(section, "General") == 0) {
            if (strcmp(key, "wallpaper") == 0) {
                wallpaper_path = strdup(value);
            } else if (strcmp(key, "border_color") == 0) {
                border_color = strtoul(value, NULL, 0);
            } else if (strcmp(key, "border_focus_color") == 0) {
                border_focus_color = strtoul(value, NULL, 0);
            } else if (strcmp(key, "bar_bg") == 0) {
                bar_bg = strtoul(value, NULL, 0);
            } else if (strcmp(key, "bar_fg") == 0) {
                bar_fg = strtoul(value, NULL, 0);
            } else if (strcmp(key, "background_color") == 0) {
                background_color = strtoul(value, NULL, 0);
            }
        } else if (strcmp(section, "Keybinds") == 0 && keybind_count < MAX_KEYBINDS) {
            char *mod_part = strtok(key, "+");
            char *key_part = strtok(NULL, "+");
            if (mod_part && key_part) {
                unsigned int mod = parse_modifier(mod_part);
                KeySym keysym = parse_keysym(key_part);
                if (keysym != NoSymbol) {
                    keybinds[keybind_count].keysym = keysym;
                    keybinds[keybind_count].modifier = mod;
                    keybinds[keybind_count].command = strdup(value);
                    keybind_count++;
                }
            }
        } else if (strcmp(section, "Autostart") == 0 && autostart_count < MAX_AUTOSTART) {
            autostart_commands[autostart_count] = strdup(value);
            autostart_count++;
        }
    }

    fclose(f);
}

void apply_window_border(Window w, Bool is_focused) {
    XSetWindowBorderWidth(display, w, border_width);
    XSetWindowBorder(display, w, is_focused ? border_focus_color : border_color);
}

void set_background() {
    int screen = DefaultScreen(display);
    Window root_window = RootWindow(display, screen);
    XSetWindowBackground(display, root_window, background_color);
    XClearWindow(display, root_window);
}

void spawn(const char *cmd) {
    if (!cmd || fork() != 0) return;
    setsid();
    chdir(getenv("HOME"));
    if (strchr(cmd, '=') && strchr(cmd, ' ')) {
        execlp("sh", "sh", "-c", cmd, NULL);
    } else {
        execlp(cmd, cmd, NULL);
    }
    exit(1);
}

int idx_of_window(Window w) {
    for (int i = 0; i < window_count; i++)
        if (window_states[i].managed && window_states[i].window == w)
            return i;
    return -1;
}

void toggle_floating(Window w) {
    int i = idx_of_window(w);
    if (i < 0) return;
    WindowState *state = &window_states[i];
    state->is_floating = !state->is_floating;
    if (!state->is_floating) {
        tile_windows();
    } else {
        XRaiseWindow(display, w);
    }
}

void fullscreen_window(Window w) {
    int i = idx_of_window(w);
    if (i < 0) return;
    WindowState *state = &window_states[i];
    if (!state->is_fullscreen) {
        XWindowAttributes attr;
        if (!XGetWindowAttributes(display, w, &attr)) return;
        state->x = attr.x;
        state->y = attr.y;
        state->width = attr.width;
        state->height = attr.height;
        state->is_fullscreen = 1;
        int screen = DefaultScreen(display);
        int w_width = DisplayWidth(display, screen);
        int w_height = DisplayHeight(display, screen);
        XMoveResizeWindow(display, w, 0, 0, w_width, w_height);
    } else {
        XMoveResizeWindow(display, w, state->x, state->y, state->width, state->height);
        state->is_fullscreen = 0;
    }
    apply_window_border(w, True);
}

int is_window_on_current_ws(WindowState *s) {
    return s->workspace == current_workspace;
}

void tile_windows() {
    int visible_count = 0;
    for (int i = 0; i < window_count; i++)
        if (window_states[i].managed && is_window_on_current_ws(&window_states[i]) && 
            !window_states[i].is_fullscreen && !window_states[i].is_floating)
            visible_count++;
    if (visible_count == 0) {
        draw_bar();
        return;
    }
    int screen = DefaultScreen(display);
    int screen_width = DisplayWidth(display, screen);
    int screen_height = DisplayHeight(display, screen);
    int available_y = bar_height;
    int available_h = screen_height - bar_height;
    int i_vis = 0;
    for (int i = 0; i < window_count; i++) {
        if (!window_states[i].managed || !is_window_on_current_ws(&window_states[i]) || 
            window_states[i].is_fullscreen || window_states[i].is_floating)
            continue;
        Window w = window_states[i].window;
        int target_x, target_y, target_w, target_h;
        if (i_vis == 0) {
            int master_width = screen_width / 2 - gap_outer - gap_inner / 2;
            target_x = gap_outer;
            target_y = available_y + gap_outer;
            target_w = master_width;
            target_h = available_h - 2 * gap_outer;
        } else {
            int stack_count = visible_count - 1;
            int master_width = screen_width / 2 - gap_outer - gap_inner / 2;
            int stack_width = screen_width - master_width - 2 * gap_outer - gap_inner;
            int stack_height = (available_h - 2 * gap_outer - (stack_count - 1) * gap_inner) / stack_count;
            target_x = gap_outer + master_width + gap_inner;
            target_y = available_y + gap_outer + (stack_height + gap_inner) * (i_vis - 1);
            target_w = stack_width;
            target_h = stack_height;
        }
        XMoveResizeWindow(display, w, target_x, target_y, target_w, target_h);
        apply_window_border(w, w == focused);
        i_vis++;
    }
    draw_bar();
}

void set_wm_desktop(Window w, int ws) {
    if (ws < 1 || ws > MAX_WORKSPACES) return;
    long desktop = ws - 1;
    XChangeProperty(display, w, net_wm_desktop, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&desktop, 1);
}

void add_window(Window w) {
    if (window_count >= MAX_WINDOWS) return;
    windows[window_count] = w;
    window_states[window_count] = (WindowState){ w, 0, 0, 0, 0, 0, 0, current_workspace, 1 };
    window_count++;
    XSelectInput(display, w, EnterWindowMask | FocusChangeMask | PropertyChangeMask);
    apply_window_border(w, False);
    XMapWindow(display, w);
    set_wm_desktop(w, current_workspace);
    tile_windows();
}

void remove_window(Window w) {
    int idx = idx_of_window(w);
    if (idx < 0) return;
    for (int j = idx; j < window_count - 1; j++) {
        windows[j] = windows[j + 1];
        window_states[j] = window_states[j + 1];
    }
    window_count--;
    if (focused == w) focused = None;
    if (last_focused[current_workspace] == w) last_focused[current_workspace] = None;
    tile_windows();
}

void close_focused_window() {
    if (focused == None) return;
    XKillClient(display, focused);
    focused = None;
}

void update_focus() {
    if (focused != None && idx_of_window(focused) >= 0) {
        XSetInputFocus(display, focused, RevertToPointerRoot, CurrentTime);
        XRaiseWindow(display, focused);
    } else {
        focused = None;
        for (int i = 0; i < window_count; i++) {
            if (window_states[i].managed && is_window_on_current_ws(&window_states[i])) {
                focused = window_states[i].window;
                XSetInputFocus(display, focused, RevertToPointerRoot, CurrentTime);
                XRaiseWindow(display, focused);
                break;
            }
        }
    }
}

void switch_workspace(int ws) {
    if (ws < 1 || ws > MAX_WORKSPACES || ws == current_workspace) return;
    if (focused != None && idx_of_window(focused) >= 0) {
        last_focused[current_workspace] = focused;
    }
    const int OFFSCREEN_X = -10000;
    for (int i = 0; i < window_count; i++) {
        if (!window_states[i].managed || window_states[i].workspace != current_workspace) continue;
        XMoveWindow(display, window_states[i].window, OFFSCREEN_X, window_states[i].y);
    }
    current_workspace = ws;
    long desktop = ws - 1;
    XChangeProperty(display, root, net_current_desktop, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&desktop, 1);
    for (int i = 0; i < window_count; i++) {
        if (!window_states[i].managed || window_states[i].workspace != current_workspace) continue;
        XMapWindow(display, window_states[i].window);
    }
    focused = last_focused[current_workspace];
    update_focus();
    tile_windows();
}

void move_focused_to_workspace(int ws) {
    if (focused == None || ws < 1 || ws > MAX_WORKSPACES) return;
    int i = idx_of_window(focused);
    if (i < 0) return;
    window_states[i].workspace = ws;
    set_wm_desktop(focused, ws);
    const int OFFSCREEN_X = -10000;
    if (ws != current_workspace) {
        XMoveWindow(display, window_states[i].window, OFFSCREEN_X, window_states[i].y);
        last_focused[current_workspace] = None;
        focused = None;
        update_focus();
    }
    tile_windows();
}

void create_bar() {
    int screen = DefaultScreen(display);
    int screen_width = DisplayWidth(display, screen);
    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    attrs.background_pixel = bar_bg;
    bar = XCreateWindow(display, RootWindow(display, screen), 0, 0,
                        (unsigned)screen_width, (unsigned)bar_height, 0,
                        CopyFromParent, InputOutput, CopyFromParent,
                        CWOverrideRedirect | CWBackPixel, &attrs);
    XSelectInput(display, bar, ExposureMask | StructureNotifyMask);
    XMapRaised(display, bar);
    bar_gc = XCreateGC(display, bar, 0, NULL);
    XSetForeground(display, bar_gc, bar_fg);
    bar_font = XLoadQueryFont(display, "fixed");
    if (!bar_font) bar_font = XLoadQueryFont(display, "6x13");
    if (bar_font) XSetFont(display, bar_gc, bar_font->fid);
}

void get_time_string(char *buf, size_t bufsz) {
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    strftime(buf, bufsz, "%H:%M:%S", lt);
}

const char* get_layout_label() {
    XkbStateRec state;
    if (XkbGetState(display, XkbUseCoreKbd, &state) == Success) {
        unsigned group = state.group;
        switch (group) {
            case 0: return "US";
            case 1: return "RU";
            default: return "??";
        }
    }
    return "KB";
}

int count_windows_on_ws(int ws) {
    int c = 0;
    for (int i = 0; i < window_count; i++)
        if (window_states[i].managed && window_states[i].workspace == ws)
            c++;
    return c;
}

void get_battery_status(char *buf, size_t bufsz) {
    FILE *f = fopen("/sys/class/power_supply/BAT0/capacity", "r");
    if (!f) {
        snprintf(buf, bufsz, "BAT: --%%");
        return;
    }
    int capacity = -1;
    if (f) {
        fscanf(f, "%d", &capacity);
        fclose(f);
    }
    snprintf(buf, bufsz, "BAT: %d%%", capacity >= 0 ? capacity : 0);
}

void draw_bar() {
    if (!bar) return;
    time_t now = time(NULL);
    if (now == last_bar_update) return;
    last_bar_update = now;
    
    int screen = DefaultScreen(display);
    int screen_width = DisplayWidth(display, screen);
    XSetForeground(display, bar_gc, bar_bg);
    XFillRectangle(display, bar, bar_gc, 0, 0, (unsigned)screen_width, (unsigned)bar_height);
    XSetForeground(display, bar_gc, bar_fg);
    
    char timebuf[64];
    get_time_string(timebuf, sizeof timebuf);
    char batbuf[64];
    get_battery_status(batbuf, sizeof batbuf);
    char netbuf[64];
    get_network_status(netbuf, sizeof netbuf);
    
    char wsbuf[128];
    wsbuf[0] = '\0';
    char tmp[16];
    for (int i = 1; i <= MAX_WORKSPACES; i++) {
        int n = count_windows_on_ws(i);
        if (i == current_workspace)
            snprintf(tmp, sizeof tmp, "[%d:%d] ", i, n);
        else
            snprintf(tmp, sizeof tmp, "%d:%d ", i, n);
        strncat(wsbuf, tmp, sizeof(wsbuf) - strlen(wsbuf) - 1);
    }
    
    const char *layout = get_layout_label();
    char right[256];
    snprintf(right, sizeof right, "%s | %s | %s | %s", layout, netbuf, batbuf, timebuf);
    
    int y = (bar_height + (bar_font ? bar_font->ascent - bar_font->descent : 10)) / 2;
    XDrawString(display, bar, bar_gc, 8, y, wsbuf, (int)strlen(wsbuf));
    int right_w = XTextWidth(bar_font, right, (int)strlen(right));
    XDrawString(display, bar, bar_gc, screen_width - right_w - 8, y, right, (int)strlen(right));
}

void get_network_status(char *buf, size_t bufsz) {
    struct ifaddrs *ifaddr, *ifa;
    int online = 0;
    if (getifaddrs(&ifaddr) == -1) {
        snprintf(buf, bufsz, "NET: err");
        return;
    }
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_name || !(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK))
            continue;
        online = 1;
        break;
    }
    freeifaddrs(ifaddr);
    snprintf(buf, bufsz, "NET: %s", online ? "ON" : "OFF");
}

void init_ewmh() {
    net_number_of_desktops = XInternAtom(display, "_NET_NUMBER_OF_DESKTOPS", False);
    net_current_desktop = XInternAtom(display, "_NET_CURRENT_DESKTOP", False);
    net_wm_desktop = XInternAtom(display, "_NET_WM_DESKTOP", False);
    net_desktop_names = XInternAtom(display, "_NET_DESKTOP_NAMES", False);

    long num_desktops = MAX_WORKSPACES;
    XChangeProperty(display, root, net_number_of_desktops, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&num_desktops, 1);

    long current_desktop = current_workspace - 1;
    XChangeProperty(display, root, net_current_desktop, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&current_desktop, 1);
}

void handle_client_message(XClientMessageEvent *ev) {
    if (ev->message_type == net_wm_desktop) {
        long desktop = ev->data.l[0] + 1;
        if (desktop >= 1 && desktop <= MAX_WORKSPACES) {
            int i = idx_of_window(ev->window);
            if (i >= 0) {
                move_focused_to_workspace(desktop);
            }
        }
    }
}

void handle_keybind(KeySym keysym, unsigned int state) {
    for (int i = 0; i < keybind_count; i++) {
        if (keybinds[i].keysym == keysym && (state & keybinds[i].modifier) == keybinds[i].modifier) {
            const char *cmd = keybinds[i].command;
            if (strcmp(cmd, "close") == 0) {
                close_focused_window();
            } else if (strcmp(cmd, "fullscreen") == 0 && focused != None) {
                fullscreen_window(focused);
            } else if (strcmp(cmd, "float") == 0 && focused != None) {
                toggle_floating(focused);
            } else if (strncmp(cmd, "ws", 2) == 0) {
                int ws = atoi(cmd + 2);
                switch_workspace(ws);
            } else if (strncmp(cmd, "movews", 6) == 0) {
                int ws = atoi(cmd + 6);
                move_focused_to_workspace(ws);
            } else if (strcmp(cmd, "reload") == 0) {
                execvp(saved_argv[0], saved_argv);
            } else {
                spawn(cmd);
            }
        }
    }
}

void init_globals() {
    for (int i = 0; i <= MAX_WORKSPACES; i++) {
        last_focused[i] = None;
    }
}

void init_autostart() {
    for (int i = 0; i < autostart_count; i++) {
        if (fork() == 0) {
            setsid();
            chdir(getenv("HOME"));
            execlp("/bin/sh", "/bin/sh", "-c", autostart_commands[i], NULL);
            fprintf(stderr, "Failed to execute autostart command: %s\n", autostart_commands[i]);
            exit(1);
        }
    }
}

void grab_keys() {
    root = DefaultRootWindow(display);
    for (int i = 0; i < keybind_count; i++) {
        KeyCode code = XKeysymToKeycode(display, keybinds[i].keysym);
        if (code != 0) {
            XGrabKey(display, code, keybinds[i].modifier, root, True, GrabModeAsync, GrabModeAsync);
        }
    }
    XGrabButton(display, Button1, Mod4Mask, root, True,
                ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(display, Button3, Mod4Mask, root, True,
                ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                GrabModeAsync, GrabModeAsync, None, None);
}

int xerror(Display *dpy, XErrorEvent *ee) {
    return 0;
}

int main(int argc, char *argv[]) {
    saved_argc = argc;
    saved_argv = argv;

    display = XOpenDisplay(NULL);
    if (!display) return 1;

    load_config();
    init_globals();
    init_autostart();
    XSetErrorHandler(xerror);
    int screen = DefaultScreen(display);
    root = RootWindow(display, screen);
    XSelectInput(display, root, SubstructureRedirectMask | SubstructureNotifyMask |
                ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                KeyPressMask | EnterWindowMask | FocusChangeMask | PropertyChangeMask);
    grab_keys();
    Cursor cursor = XCreateFontCursor(display, XC_left_ptr);
    XDefineCursor(display, root, cursor);
    create_bar();
    init_ewmh();
    set_background();
    XSync(display, False);

    int xfd = ConnectionNumber(display);
    while (1) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        struct timeval tv = {1, 0};
        int r = select(xfd + 1, &fds, NULL, NULL, &tv);
        if (r > 0) {
            while (XPending(display)) {
                XEvent ev;
                XNextEvent(display, &ev);
                switch (ev.type) {
                    case Expose:
                        if (ev.xexpose.window == bar)
                            draw_bar();
                        break;
                    case ConfigureNotify:
                        if (ev.xconfigure.window == bar)
                            draw_bar();
                        break;
                    case KeyPress:
                        handle_keybind(XkbKeycodeToKeysym(display, ev.xkey.keycode, 0, 0), ev.xkey.state);
                        break;
                    case MapRequest:
                        XMapWindow(display, ev.xmaprequest.window);
                        add_window(ev.xmaprequest.window);
                        break;
                    case UnmapNotify:
                        remove_window(ev.xunmap.window);
                        break;
                    case EnterNotify:
                        if (ev.xcrossing.window != root && ev.xcrossing.window != bar) {
                            focused = ev.xcrossing.window;
                            last_focused[current_workspace] = focused;
                            update_focus();
                            tile_windows();
                        }
                        break;
                    case ButtonPress:
                        if (ev.xbutton.subwindow != None && (ev.xbutton.state & Mod4Mask)) {
                            drag_window = ev.xbutton.subwindow;
                            if (drag_window == bar) {
                                drag_window = None;
                                break;
                            }
                            int i = idx_of_window(drag_window);
                            if (i >= 0 && !window_states[i].is_floating) {
                                toggle_floating(drag_window);
                            }
                            XWindowAttributes attr;
                            if (XGetWindowAttributes(display, drag_window, &attr)) {
                                if (ev.xbutton.button == Button1) {
                                    dragging = 1;
                                    drag_start_x = ev.xbutton.x_root;
                                    drag_start_y = ev.xbutton.y_root;
                                } else if (ev.xbutton.button == Button3) {
                                    resizing = 1;
                                    drag_start_x = ev.xbutton.x_root;
                                    drag_start_y = ev.xbutton.y_root;
                                    drag_start_width = attr.width;
                                    drag_start_height = attr.height;
                                }
                            }
                        }
                        break;
                    case MotionNotify:
                        if (drag_window != None) {
                            XWindowAttributes attr;
                            if (XGetWindowAttributes(display, drag_window, &attr)) {
                                if (dragging) {
                                    int dx = ev.xmotion.x_root - drag_start_x;
                                    int dy = ev.xmotion.y_root - drag_start_y;
                                    XMoveWindow(display, drag_window, attr.x + dx, attr.y + dy);
                                    drag_start_x = ev.xmotion.x_root;
                                    drag_start_y = ev.xmotion.y_root;
                                } else if (resizing) {
                                    int dx = ev.xmotion.x_root - drag_start_x;
                                    int dy = ev.xmotion.y_root - drag_start_y;
                                    int new_width = drag_start_width + dx;
                                    int new_height = drag_start_height + dy;
                                    if (new_width > 100 && new_height > 100) {
                                        XResizeWindow(display, drag_window, new_width, new_height);
                                    }
                                }
                            }
                        }
                        break;
                    case ButtonRelease:
                        dragging = 0;
                        resizing = 0;
                        drag_window = None;
                        break;
                    case ClientMessage:
                        handle_client_message(&ev.xclient);
                        break;
                }
            }
        }
        if (r == 0) {
            draw_bar();
        }
    }

    for (int i = 0; i < keybind_count; i++) {
        free(keybinds[i].command);
    }
    free(keybinds);
    for (int i = 0; i < autostart_count; i++) {
        free(autostart_commands[i]);
    }
    free(wallpaper_path);
    if (bar_font) XFreeFont(display, bar_font);
    if (bar_gc) XFreeGC(display, bar_gc);
    XCloseDisplay(display);
    return 0;
}
