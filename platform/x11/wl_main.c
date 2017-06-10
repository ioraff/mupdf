#include "pdfapp.h"

#include <pixman.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xdg-shell-unstable-v5-client-protocol.h>
#include <xkbcommon/xkbcommon.h>

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>

#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

#ifndef timeradd
#define timeradd(a, b, result) \
	do { \
		(result)->tv_sec = (a)->tv_sec + (b)->tv_sec; \
		(result)->tv_usec = (a)->tv_usec + (b)->tv_usec; \
		if ((result)->tv_usec >= 1000000) \
		{ \
			++(result)->tv_sec; \
			(result)->tv_usec -= 1000000; \
		} \
	} while (0)
#endif

#ifndef timersub
#define timersub(a, b, result) \
	do { \
		(result)->tv_sec = (a)->tv_sec - (b)->tv_sec; \
		(result)->tv_usec = (a)->tv_usec - (b)->tv_usec; \
		if ((result)->tv_usec < 0) { \
			--(result)->tv_sec; \
			(result)->tv_usec += 1000000; \
		} \
	} while (0)
#endif

struct image {
	pixman_image_t *pixman;
	fz_pixmap *pixmap;
	size_t size;
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
};

struct screen {
	int width;
	int height;
	int width_mm;
	int height_mm;
	int resolution;
};

struct window {
	struct wl_surface *surface;
	struct xdg_surface *xdgsurface;
	struct wl_callback *frame;
	int width, height;
	struct image *image;
	struct screen *screen;
	int maximized;
	int dirty, dirtysearch;
	struct {
		int x, y, id;
		struct wl_cursor_image *image;
	} cursor;
};

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_keyboard *keyboard;
static struct wl_pointer *pointer;
static struct wl_data_device_manager *datadevicemanager;
static struct wl_data_device *datadevice;
static struct xdg_shell *shell;
static struct screen *firstscreen;
static struct {
	struct xkb_context *ctx;
	struct xkb_keymap *map;
	struct xkb_state *state;
	xkb_mod_index_t ctrl, shift;
	int mods;
} xkb;
static struct {
	struct wl_cursor_theme *theme;
	struct wl_surface *surface;
} cursor;
static struct {
	struct wl_data_source *source;
	char utf8[1024 * 48];
	size_t len;
	uint32_t serial;
} selection;
static int wlfd;
static int justcopied = 0;
static char *password = "";
static pixman_color_t bgcolor = {0x7000, 0x7000, 0x7000};
static pixman_color_t shcolor = {0x4000, 0x4000, 0x4000};
static pixman_color_t white = {0xffff, 0xffff, 0xffff, 0xffff};
static pixman_image_t *solidwhite;
static char *filename;
static char message[1024] = "";

static pdfapp_t gapp;
static struct window gwin;
static fz_font *font;
static int closing = 0;
static int reloading = 0;
static int showingpage = 0;
static int showingmessage = 0;

static int advance_scheduled = 0;
static struct timeval tmo;
static struct timeval tmo_advance;
static struct timeval tmo_at;

static void winblit(pdfapp_t *app);
static void updatecursor(struct window *win);
static void onmouse(int x, int y, int btn, int modifiers, int state);
static void onkey(int c, int modifiers);
static void docopy(pdfapp_t *app);
static void cleanup(pdfapp_t *app);

/*
 * Wayland listeners
 */
static void output_geometry(void *data, struct wl_output *wlout, int32_t x, int32_t y, int32_t w_mm, int32_t h_mm, int32_t subpixel, const char *make, const char *model, int32_t transform)
{
	struct screen *screen = data;

	screen->width_mm = w_mm;
	screen->height_mm = w_mm;
}

static void output_mode(void *data, struct wl_output *wlout, uint32_t flags, int32_t w, int32_t h, int32_t refresh)
{
	struct screen *screen = data;

	if (flags & WL_OUTPUT_MODE_CURRENT)
	{
		screen->width = w;
		screen->height = h;
	}
}

static void output_done(void *data, struct wl_output *wlout)
{
	struct screen *screen = data;

	screen->resolution = screen->width * 25.4 / screen->width_mm + 0.5;
}

static void output_scale(void *data, struct wl_output *wlout, int32_t factor)
{
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name, const char *interface, uint32_t version)
{
	if (strcmp(interface, "wl_compositor") == 0)
		compositor = wl_registry_bind(reg, name, &wl_compositor_interface, MIN(version, 1));
	else if (strcmp(interface, "wl_shm") == 0)
		shm = wl_registry_bind(reg, name, &wl_shm_interface, MIN(version, 1));
	else if (strcmp(interface, "wl_seat") == 0)
		seat = wl_registry_bind(reg, name, &wl_seat_interface, MIN(version, 4));
	else if (strcmp(interface, "wl_data_device_manager") == 0)
		datadevicemanager = wl_registry_bind(reg, name, &wl_data_device_manager_interface, MIN(version, 2));
	else if (strcmp(interface, "wl_output") == 0)
	{
		struct wl_output *output;
		struct screen *screen;

		screen = malloc(sizeof(*screen));
		if (screen)
		{
			output = wl_registry_bind(reg, name, &wl_output_interface, MIN(version, 2));
			if (output)
			{
				wl_output_add_listener(output, &output_listener, screen);
				if (!firstscreen)
					firstscreen = screen;
			}
			else
				free(screen);
		}
	}
	else if (strcmp(interface, "xdg_shell") == 0)
		shell = wl_registry_bind(reg, name, &xdg_shell_interface, MIN(version, 1));
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
}

static void
keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd, uint32_t size)
{
	char *str;

	str = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (str == MAP_FAILED)
		return;

	xkb_keymap_unref(xkb.map);
	xkb.map = xkb_keymap_new_from_buffer(xkb.ctx, str, strnlen(str, size), XKB_KEYMAP_FORMAT_TEXT_V1, 0);
	if (!xkb.map)
		return;
	munmap(str, size);

	xkb_state_unref(xkb.state);
	xkb.state = xkb_state_new(xkb.map);
	if (!xkb.state)
		return;

	xkb.ctrl = xkb_keymap_mod_get_index(xkb.map, XKB_MOD_NAME_CTRL);
	xkb.shift = xkb_keymap_mod_get_index(xkb.map, XKB_MOD_NAME_SHIFT);
}

static void
keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys)
{
}

static void
keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface)
{
}

static void
keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t code, uint32_t state)
{
	xkb_keysym_t keysym;
	uint32_t c = 0;

	if (!xkb.state || !xkb.map || state == WL_KEYBOARD_KEY_STATE_RELEASED)
		return;

	code += 8;
	keysym = xkb_state_key_get_one_sym(xkb.state, code);
	if (!gapp.issearching)
	{
		switch (keysym) {
		case XKB_KEY_Escape:
			c = '\033';
			break;
		case XKB_KEY_Up:
			c = 'k';
			break;
		case XKB_KEY_Down:
			c = 'j';
			break;
		case XKB_KEY_Left:
			c = 'b';
			break;
		case XKB_KEY_Right:
			c = ' ';
			break;
		case XKB_KEY_Page_Up:
			c = ',';
			break;
		case XKB_KEY_Page_Down:
			c = '.';
			break;
		case XKB_KEY_NoSymbol:
			c = 0;
		}
	}
	if (!c)
		c = xkb_state_key_get_utf32(xkb.state, code);
	if (xkb.mods & (1<<2) && keysym == XKB_KEY_c)
		docopy(&gapp);
	else if (c)
		onkey(c, xkb.mods);

	onmouse(gwin.cursor.x, gwin.cursor.y, 0, 0, 0);
}

static void
keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
	if (!xkb.state)
		return;

	xkb_state_update_mask(xkb.state, depressed, latched, locked, group, 0, 0);

	xkb.mods = 0;
	if (xkb.shift != XKB_MOD_INVALID && xkb_state_mod_index_is_active(xkb.state, xkb.shift, XKB_STATE_MODS_EFFECTIVE))
		xkb.mods |= 1<<0;
	if (xkb.ctrl != XKB_MOD_INVALID && xkb_state_mod_index_is_active(xkb.state, xkb.ctrl, XKB_STATE_MODS_EFFECTIVE))
		xkb.mods |= 1<<2;
}

static void
keyboard_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay)
{
	/* FIXME: implement key repeat */
}

static struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

static void
pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t fx, wl_fixed_t fy)
{
	struct window *win = wl_surface_get_user_data(surface);

	updatecursor(win);
	win->cursor.x = wl_fixed_to_int(fx);
	win->cursor.y = wl_fixed_to_int(fy);
	onmouse(win->cursor.x, win->cursor.y, 0, xkb.mods, 0);
}

static void
pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial, struct wl_surface *surface)
{
}

static void
pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t fx, wl_fixed_t fy)
{
	gwin.cursor.x = wl_fixed_to_int(fx);
	gwin.cursor.y = wl_fixed_to_int(fy);
	onmouse(gwin.cursor.x, gwin.cursor.y, 0, xkb.mods, 0);
}

static void
pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
	int b;

	switch (button) {
	case BTN_LEFT:
		b = 1;
		break;
	case BTN_MIDDLE:
		b = 2;
		break;
	case BTN_RIGHT:
		b = 3;
		break;
	default:
		return;
	}

	onmouse(gwin.cursor.x, gwin.cursor.y, b, xkb.mods, state == WL_POINTER_BUTTON_STATE_PRESSED ? 1 : -1);
	if (state == WL_POINTER_BUTTON_STATE_RELEASED)
		selection.serial = serial;
}

static void
pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis, wl_fixed_t value)
{
	int amount, b;

	amount = wl_fixed_to_int(value);
	switch (axis) {
	case WL_POINTER_AXIS_VERTICAL_SCROLL:
		b = amount < 0 ? 4 : 5;
		break;
	case WL_POINTER_AXIS_HORIZONTAL_SCROLL:
		b = amount < 0 ? 6 : 7;
		break;
	default:
		return;
	}
	for (amount = abs(amount); amount > 0; amount -= 10)
		onmouse(gwin.cursor.x, gwin.cursor.y, b, 0, 1);
}

static struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
};

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void
seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
	if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD && !keyboard) {
		keyboard = wl_seat_get_keyboard(seat);
		if (keyboard)
			wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
	}
	if (capabilities & WL_SEAT_CAPABILITY_POINTER && !pointer) {
		pointer = wl_seat_get_pointer(seat);
		if (pointer)
			wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void selection_target(void *data, struct wl_data_source *source, const char *mimetype)
{
}

static void selection_send(void *data, struct wl_data_source *source, const char *mimetype, int32_t fd)
{
	/* FIXME: this should be done concurrently with normal event processing */
	char *p;
	size_t n;
	ssize_t nw;

	p = selection.utf8;
	n = selection.len;

	while (n)
	{
		nw = write(fd, p, n);
		if (n < 0)
			break;
		n -= nw;
	}

	close(fd);
}

static void selection_cancelled(void *data, struct wl_data_source *source)
{
	wl_data_source_destroy(source);
	selection.source = NULL;
}

static const struct wl_data_source_listener selection_listener = {
	.target = selection_target,
	.send = selection_send,
	.cancelled = selection_cancelled,
};

static void surface_enter(void *data, struct wl_surface *surface, struct wl_output *output)
{
	struct window *win = data;
	struct screen *screen = wl_output_get_user_data(output);

	win->screen = screen;
	gapp.scrw = screen->width;
	gapp.scrh = screen->height;
}

static void surface_leave(void *data, struct wl_surface *surface, struct wl_output *output)
{
}

static const struct wl_surface_listener surface_listener = {
	.enter = surface_enter,
	.leave = surface_leave,
};

static void
frame_done(void *data, struct wl_callback *callback, uint32_t time)
{
	struct window *win = data;

	wl_callback_destroy(callback);
	win->frame = NULL;
}

static struct wl_callback_listener frame_listener = {
	.done = frame_done,
};

static void xdgsurface_configure(void *data, struct xdg_surface *xdgsurface, int32_t w, int32_t h, struct wl_array *states, uint32_t serial)
{
	struct window *win = data;
	uint32_t *state;

	if (gapp.winw != w)
		win->width = w;
	if (gapp.winh != h)
		win->height = h;
	win->maximized = 0;
	wl_array_for_each (state, states)
	{
		if (*state == XDG_SURFACE_STATE_MAXIMIZED || *state == XDG_SURFACE_STATE_FULLSCREEN)
		{
			win->maximized = 1;
			break;
		}
	}
	xdg_surface_ack_configure(win->xdgsurface, serial);
}

static void
xdgsurface_close(void *data, struct xdg_surface *xdgsurface)
{
	closing = 1;
}

static const struct xdg_surface_listener xdgsurface_listener = {
	.configure = xdgsurface_configure,
	.close = xdgsurface_close,
};

/*
 * Wayland buffers
 */
static struct image *createimage(int w, int h)
{
	char template[] = "/tmp/mupdf-XXXXXX";
	struct image *img;
	int p, fd;
	void *data;

	img = malloc(sizeof(*img));
	if (!img)
		goto err0;
	p = w * 4;
	img->size = p * h;
	fd = mkstemp(template);
	if (fd < 0)
		goto err1;
	if (unlink(template) < 0)
		goto err2;
	if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
		goto err2;
	if (posix_fallocate(fd, 0, img->size) < 0)
		goto err2;
	data = mmap(NULL, img->size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED)
		goto err2;
	if (!shm)
		return NULL;
	img->pool = wl_shm_create_pool(shm, fd, img->size);
	if (!img->pool)
		goto err3;
	img->buffer = wl_shm_pool_create_buffer(img->pool, 0, w, h, p, WL_SHM_FORMAT_XRGB8888);
	if (!img->buffer)
		goto err4;
	img->pixman = pixman_image_create_bits_no_clear(PIXMAN_x8r8g8b8, w, h, data, p);
	if (!img->pixman)
		goto err5;
	img->pixmap = fz_new_pixmap_with_data(gapp.ctx, fz_device_rgb(gapp.ctx), w, h, 1, p, data);
	close(fd);

	return img;

err5:
	wl_buffer_destroy(img->buffer);
err4:
	wl_shm_pool_destroy(img->pool);
err3:
	munmap(data, img->size);
err2:
	close(fd);
err1:
	free(img);
err0:
	return NULL;
}

static void destroyimage(struct image *img)
{
	munmap(pixman_image_get_data(img->pixman), img->size);
	pixman_image_unref(img->pixman);
	fz_drop_pixmap(gapp.ctx, img->pixmap);
	wl_buffer_destroy(img->buffer);
	wl_shm_pool_destroy(img->pool);
	free(img);
}

static void resize(struct window *win)
{
	struct image *img;
	int w, h;

	w = gapp.winw;
	h = gapp.winh;
	if (win->width != -1)
		w = win->width;
	if (win->height != -1)
		h = win->height;
	img = createimage(w, h);
	if (!img)
		return;

	if (win->image)
		destroyimage(win->image);
	win->image = img;
	win->width = -1;
	win->height = -1;
	pdfapp_onresize(&gapp, w, h);
}

/*
 * Dialog boxes
 */
static void showmessage(pdfapp_t *app, int timeout, char *msg)
{
	struct timeval now;

	showingmessage = 1;
	showingpage = 0;

	fz_strlcpy(message, msg, sizeof message);

	if ((!tmo_at.tv_sec && !tmo_at.tv_usec) || tmo.tv_sec < timeout)
	{
		tmo.tv_sec = timeout;
		tmo.tv_usec = 0;
		gettimeofday(&now, NULL);
		timeradd(&now, &tmo, &tmo_at);
	}
}

void winerror(pdfapp_t *app, char *msg)
{
	fprintf(stderr, "mupdf: error: %s\n", msg);
	cleanup(app);
	exit(1);
}

void winwarn(pdfapp_t *app, char *msg)
{
	char buf[1024];
	snprintf(buf, sizeof buf, "warning: %s", msg);
	showmessage(app, 10, buf);
	fprintf(stderr, "mupdf: %s\n", buf);
}

void winalert(pdfapp_t *app, pdf_alert_event *alert)
{
	char buf[1024];
	snprintf(buf, sizeof buf, "Alert %s: %s", alert->title, alert->message);
	fprintf(stderr, "%s\n", buf);
	switch (alert->button_group_type)
	{
	case PDF_ALERT_BUTTON_GROUP_OK:
	case PDF_ALERT_BUTTON_GROUP_OK_CANCEL:
		alert->button_pressed = PDF_ALERT_BUTTON_OK;
		break;
	case PDF_ALERT_BUTTON_GROUP_YES_NO:
	case PDF_ALERT_BUTTON_GROUP_YES_NO_CANCEL:
		alert->button_pressed = PDF_ALERT_BUTTON_YES;
		break;
	}
}

void winprint(pdfapp_t *app)
{
	fprintf(stderr, "The MuPDF library supports printing, but this application currently does not\n");
}

char *winpassword(pdfapp_t *app, char *filename)
{
	char *r = password;
	password = NULL;
	return r;
}

char *wintextinput(pdfapp_t *app, char *inittext, int retry)
{
	/* We don't support text input on the wayland viewer */
	return NULL;
}

int winchoiceinput(pdfapp_t *app, int nopts, char *opts[], int *nvals, char *vals[])
{
	/* FIXME: temporary dummy implementation */
	return 0;
}

/*
 * Wayland magic
 */

static void wininit(void)
{
	struct wl_registry *reg;

	solidwhite = pixman_image_create_solid_fill(&white);
	if (!solidwhite)
		fz_throw(gapp.ctx, FZ_ERROR_GENERIC, "cannot allocate solid white");

	font = fz_load_fallback_font(gapp.ctx, UCDN_SCRIPT_LATIN, 0, 0, 0, 0);
	if (!font)
		fz_throw(gapp.ctx, FZ_ERROR_GENERIC, "cannot load search font");

	display = wl_display_connect(NULL);
	if (!display)
		fz_throw(gapp.ctx, FZ_ERROR_GENERIC, "cannot open display");

	reg = wl_display_get_registry(display);
	if (!reg)
		fz_throw(gapp.ctx, FZ_ERROR_MEMORY, "cannot get registry");
	wl_registry_add_listener(reg, &registry_listener, NULL);

	wl_display_roundtrip(display);

	if (!compositor)
		fz_throw(gapp.ctx, FZ_ERROR_GENERIC, "display has no wl_compositor");
	if (!shm)
		fz_throw(gapp.ctx, FZ_ERROR_GENERIC, "display has no wl_shm");
	if (!seat)
		fz_throw(gapp.ctx, FZ_ERROR_GENERIC, "display has no wl_seat");
	if (!shell)
		fz_throw(gapp.ctx, FZ_ERROR_GENERIC, "display has no xdg_shell");
	if (!firstscreen)
		fz_throw(gapp.ctx, FZ_ERROR_GENERIC, "display has no wl_output");
	if (datadevicemanager)
		datadevice = wl_data_device_manager_get_data_device(datadevicemanager, seat);
	wl_seat_add_listener(seat, &seat_listener, NULL);
	xdg_shell_use_unstable_version(shell, XDG_SHELL_VERSION_CURRENT);

	xkb.ctx = xkb_context_new(0);
	if (!xkb.ctx)
		fz_throw(gapp.ctx, FZ_ERROR_GENERIC, "cannot create XKB context");

	cursor.theme = wl_cursor_theme_load(NULL, 32, shm);
	if (!cursor.theme)
		fz_throw(gapp.ctx, FZ_ERROR_GENERIC, "cannot load cursor theme");
	cursor.surface = wl_compositor_create_surface(compositor);
	if (!cursor.surface)
		fz_throw(gapp.ctx, FZ_ERROR_MEMORY, "cannot create cursor surface");

	wlfd = wl_display_get_fd(display);
}

static void winopen(void)
{
	pixman_box32_t box = {0};

	wininit();

	gwin.width = 800;
	gwin.height = 600;
	gwin.frame = NULL;
	gapp.userdata = &gwin;
	gwin.screen = firstscreen;

	gwin.surface = wl_compositor_create_surface(compositor);
	if (!gwin.surface)
		fz_throw(gapp.ctx, FZ_ERROR_MEMORY, "cannot create window surface");
	wl_surface_add_listener(gwin.surface, &surface_listener, &gwin);
	gwin.xdgsurface = xdg_shell_get_xdg_surface(shell, gwin.surface);
	if (!gwin.xdgsurface)
		fz_throw(gapp.ctx, FZ_ERROR_MEMORY, "cannot create window shell surface");
	xdg_surface_add_listener(gwin.xdgsurface, &xdgsurface_listener, &gwin);

	/* check for initial configure */
	wl_display_roundtrip(display);

	gwin.image = createimage(gwin.width, gwin.height);
	if (!gwin.image)
		fz_throw(gapp.ctx, FZ_ERROR_GENERIC, "cannot create window image");

	box.x2 = gwin.width;
	box.y2 = gwin.height;
	pixman_image_fill_boxes(PIXMAN_OP_SRC, gwin.image->pixman, &bgcolor, 1, &box);
	wl_surface_attach(gwin.surface, gwin.image->buffer, 0, 0);
	wl_surface_damage(gwin.surface, 0, 0, gwin.width, gwin.height);
	gwin.frame = wl_surface_frame(gwin.surface);
	if (gwin.frame)
		wl_callback_add_listener(gwin.frame, &frame_listener, &gwin);
	wl_surface_commit(gwin.surface);

	/* wait for wl_surface.enter */
	wl_display_roundtrip(display);
}

void winclose(pdfapp_t *app)
{
	if (pdfapp_preclose(app))
	{
		closing = 1;
	}
}

int winsavequery(pdfapp_t *app)
{
	fprintf(stderr, "mupdf: discarded changes to document\n");
	/* FIXME: temporary dummy implementation */
	return DISCARD;
}

int wingetsavepath(pdfapp_t *app, char *buf, int len)
{
	/* FIXME: temporary dummy implementation */
	return 0;
}

void winreplacefile(char *source, char *target)
{
	rename(source, target);
}

void wincopyfile(char *source, char *target)
{
	char *buf = malloc(strlen(source)+strlen(target)+5);
	if (buf)
	{
		sprintf(buf, "cp %s %s", source, target);
		system(buf);
		free(buf);
	}
}

void cleanup(pdfapp_t *app)
{
	fz_context *ctx = app->ctx;

	pdfapp_close(app);

	wl_surface_destroy(gwin.surface);
	xdg_surface_destroy(gwin.xdgsurface);
	if (gwin.frame)
		wl_callback_destroy(gwin.frame);
	destroyimage(gwin.image);

	if (xkb.state)
		xkb_state_unref(xkb.state);
	if (xkb.map)
		xkb_map_unref(xkb.map);
	xkb_context_unref(xkb.ctx);
	wl_surface_destroy(cursor.surface);
	wl_cursor_theme_destroy(cursor.theme);
	xdg_shell_destroy(shell);
	wl_shm_destroy(shm);
	if (keyboard)
		wl_keyboard_release(keyboard);
	if (pointer)
		wl_pointer_release(pointer);
	wl_seat_release(seat);
	wl_data_device_destroy(datadevice);
	wl_data_device_manager_destroy(datadevicemanager);
	wl_compositor_destroy(compositor);
	wl_display_disconnect(display);

	pixman_image_unref(solidwhite);

	fz_drop_context(ctx);
}

static int winresolution()
{
	return gwin.screen->resolution;
}

static void updatecursor(struct window *win)
{
	struct wl_cursor_image *image = win->cursor.image;
	struct wl_buffer *buffer;

	if (!image)
		return;
	buffer = wl_cursor_image_get_buffer(image);
	wl_pointer_set_cursor(pointer, 0, cursor.surface, image->hotspot_x, image->hotspot_y);
	wl_surface_damage(cursor.surface, 0, 0, image->width, image->height);
	wl_surface_attach(cursor.surface, buffer, 0, 0);
	wl_surface_commit(cursor.surface);
	/* need to flush here, because pdfapp may block while searching the page. */
	wl_display_flush(display);
}

void wincursor(pdfapp_t *app, int curs)
{
	struct window *win = app->userdata;
	const char *str;
	struct wl_cursor *wlcursor;

	if (win->cursor.id == curs)
		return;

	switch (curs)
	{
	case ARROW:
	default:
		str = "left_ptr";
		break;
	case HAND:
		str = "hand2";
		break;
	case WAIT:
		str = "watch";
		break;
	case CARET:
		str = "xterm";
		break;
	}

	wlcursor = wl_cursor_theme_get_cursor(cursor.theme, str);
	if (!wlcursor)
		return;

	win->cursor.id = curs;
	win->cursor.image = wlcursor->images[0];
	updatecursor(win);
}

void wintitle(pdfapp_t *app, char *s)
{
	struct window *win = app->userdata;

	xdg_surface_set_title(win->xdgsurface, s);
}

void winhelp(pdfapp_t *app)
{
	fprintf(stderr, "%s\n%s", pdfapp_version(app), pdfapp_usage(app));
}

void winresize(pdfapp_t *app, int w, int h)
{
	struct window *win = app->userdata;

	if (!win->maximized)
	{
		win->width = w;
		win->height = h;
	}
}

void winfullscreen(pdfapp_t *app, int state)
{
	struct window *win = app->userdata;

	xdg_surface_set_fullscreen(win->xdgsurface, NULL);
}

static void pushbox(pixman_box32_t *boxes, int *n, int x0, int y0, int x1, int y1)
{
	x0 = MIN(MAX(x0, 0), gapp.winw);
	y0 = MIN(MAX(y0, 0), gapp.winh);
	x1 = MIN(MAX(x1, 0), gapp.winw);
	y1 = MIN(MAX(y1, 0), gapp.winh);
	if (x0 >= x1 || y0 >= y1)
		return;
	boxes[(*n)++] = (pixman_box32_t){x0, y0, x1, y1};
}

static void winblitstatusbar(pdfapp_t *app)
{
	struct window *win = app->userdata;
	struct pixman_box32 b = {0, 0, gapp.winw, MIN(30, gapp.winh)};

	if (gapp.issearching)
	{
		char buf[sizeof(gapp.search) + 50];
		sprintf(buf, "Search: %s", gapp.search);
		pixman_image_fill_boxes(PIXMAN_OP_SRC, win->image->pixman, &white, 1, &b);
		b.y1 = b.y2;
		b.y2++;
		pixman_image_fill_boxes(PIXMAN_OP_SRC, win->image->pixman, &shcolor, 1, &b);
		windrawstring(&gapp, 10, 20, buf);
	}
	else if (showingmessage)
	{
		pixman_image_fill_boxes(PIXMAN_OP_SRC, win->image->pixman, &white, 1, &b);
		b.y1 = b.y2;
		b.y2++;
		pixman_image_fill_boxes(PIXMAN_OP_SRC, win->image->pixman, &shcolor, 1, &b);
		windrawstring(&gapp, 10, 20, message);
	}
	else if (showingpage)
	{
		char buf[42];
		snprintf(buf, sizeof buf, "Page %d/%d", gapp.pageno, gapp.pagecount);
		windrawstring(&gapp, 10, 20, buf);
	}
}


static void winblit(pdfapp_t *app)
{
	struct window *win = app->userdata;

	if (gapp.image)
	{
		pixman_image_t *image, *mask = NULL;
		int image_x = fz_pixmap_x(gapp.ctx, gapp.image);
		int image_y = fz_pixmap_y(gapp.ctx, gapp.image);
		int image_w = fz_pixmap_width(gapp.ctx, gapp.image);
		int image_h = fz_pixmap_height(gapp.ctx, gapp.image);
		int image_stride = fz_pixmap_stride(gapp.ctx, gapp.image);
		int image_n = fz_pixmap_components(gapp.ctx, gapp.image);
		unsigned char *image_samples = fz_pixmap_samples(gapp.ctx, gapp.image);
		unsigned char *s, *d;
		int x0 = gapp.panx;
		int y0 = gapp.pany;
		int x1 = gapp.panx + image_w;
		int y1 = gapp.pany + image_h;
		int w = MIN(x1, gapp.winw) - x0;
		int h = MIN(y1, gapp.winh) - y0;
		pixman_box32_t boxes[4];
		int i, n, x, y;

		n = 0;
		pushbox(boxes, &n, 0, 0, x0, gapp.winh);
		pushbox(boxes, &n, x1, 0, gapp.winw, gapp.winh);
		pushbox(boxes, &n, x0, 0, x1, y0);
		pushbox(boxes, &n, x0, y1, x1, gapp.winh);
		pixman_image_fill_boxes(PIXMAN_OP_SRC, win->image->pixman, &bgcolor, n, boxes);

		n = 0;
		pushbox(boxes, &n, x0 + 2, y1, x0 + image_w + 2, y1 + 2);
		pushbox(boxes, &n, x1, y0 + 2, x1 + 2, y0 + image_h);
		pixman_image_fill_boxes(PIXMAN_OP_SRC, win->image->pixman, &shcolor, n, boxes);

		switch (image_n)
		{
		case 4:
			image = pixman_image_create_bits_no_clear(PIXMAN_x8b8g8r8, image_w, image_h, (uint32_t *)image_samples, image_stride);
			if (!image)
				return;
			break;
		case 2:
			image = pixman_image_ref(solidwhite);
			mask = pixman_image_create_bits(PIXMAN_a8, image_w, image_h, NULL, 0);
			if (!mask)
				return;
			d = (void *)pixman_image_get_data(mask);
			s = image_samples;
			for (y = 0; y < image_h; ++y) {
				for (x = 0; x < image_w; ++x)
					d[x] = s[2 * x];
				d += pixman_image_get_stride(mask);
				s += image_stride;
			}
			break;
		default:
			return;
		}

		pixman_image_composite32(PIXMAN_OP_SRC, image, mask, win->image->pixman, 0, 0, 0, 0, x0, y0, w, h);
		pixman_image_unref(image);
		if (mask)
			pixman_image_unref(mask);

		x = MAX(0, gapp.selr.x0);
		y = MAX(0, gapp.selr.y0);
		w = MIN(gapp.selr.x1, image_w) - x;
		h = MIN(gapp.selr.y1, image_h) - y;
		x += x0;
		y += y0;
		pixman_image_composite32(PIXMAN_OP_DIFFERENCE, solidwhite, NULL, win->image->pixman, 0, 0, 0, 0, x, y, w, h);

		double scale = app->resolution / 72.;
		for (i = 0; i < gapp.hit_count; i++)
		{
			switch (gapp.rotate % 360) {
			case 0:
				x = gapp.hit_bbox[i].x0;
				y = gapp.hit_bbox[i].y0;
				w = gapp.hit_bbox[i].x1 - gapp.hit_bbox[i].x0;
				h = gapp.hit_bbox[i].y1 - gapp.hit_bbox[i].y0;
				break;
			case 90:
				x = -gapp.hit_bbox[i].y1;
				y = gapp.hit_bbox[i].x0;
				w = gapp.hit_bbox[i].y1 - gapp.hit_bbox[i].y0;
				h = gapp.hit_bbox[i].x1 - gapp.hit_bbox[i].x0;
				break;
			case 180:
				x = -gapp.hit_bbox[i].x1;
				y = -gapp.hit_bbox[i].y1;
				w = gapp.hit_bbox[i].x1 - gapp.hit_bbox[i].x0;
				h = gapp.hit_bbox[i].y1 - gapp.hit_bbox[i].y0;
				break;
			case 270:
				x = gapp.hit_bbox[i].y0;
				y = -gapp.hit_bbox[i].x1;
				w = gapp.hit_bbox[i].y1 - gapp.hit_bbox[i].y0;
				h = gapp.hit_bbox[i].x1 - gapp.hit_bbox[i].x0;
				break;
			}
			x = x * scale + x0 - image_x;
			y = y * scale + y0 - image_y;
			w *= scale;
			h *= scale;
			pixman_image_composite32(PIXMAN_OP_DIFFERENCE, solidwhite, NULL, win->image->pixman, 0, 0, 0, 0, x, y, w, h);
		}
	}
	else
	{
		pixman_box32_t b = {0, 0, gapp.winw, gapp.winh};
		pixman_image_fill_boxes(PIXMAN_OP_SRC, win->image->pixman, &bgcolor, 1, &b);
	}

	winblitstatusbar(app);
}

void winrepaint(pdfapp_t *app)
{
	struct window *win = app->userdata;

	win->dirty = 1;
}

void winrepaintsearch(pdfapp_t *app)
{
	struct window *win = app->userdata;

	win->dirtysearch = 1;
}

void winadvancetimer(pdfapp_t *app, float duration)
{
	struct timeval now;

	gettimeofday(&now, NULL);
	memset(&tmo_advance, 0, sizeof(tmo_advance));
	tmo_advance.tv_sec = (int)duration;
	tmo_advance.tv_usec = 1000000 * (duration - tmo_advance.tv_sec);
	timeradd(&tmo_advance, &now, &tmo_advance);
	advance_scheduled = 1;
}

void windrawstring(pdfapp_t *app, int x, int y, char *s)
{
	struct window *win = app->userdata;
	float color[3] = {0};
	fz_device *dev = NULL;
	fz_text *text = NULL;
	fz_matrix mat;
	int resolution = winresolution();

	fz_translate(&mat, 5, 20);
	fz_pre_scale(&mat, resolution / 72., resolution / 72.);

	fz_try(app->ctx)
	{
		dev = fz_new_draw_device(app->ctx, &mat, win->image->pixmap);
		text = fz_new_text(app->ctx);

		fz_scale(&mat, 12, -12);
		fz_show_string(app->ctx, text, font, &mat, s, 0, 0, 0, 0);
		fz_fill_text(app->ctx, dev, text, &fz_identity, fz_device_rgb(app->ctx), color, 1);
	}
	fz_always(app->ctx)
	{
		fz_drop_device(app->ctx, dev);
		fz_drop_text(app->ctx, text);
	}
	fz_catch(app->ctx)
	{
		winerror(app, "cannot draw string");
	}
}

void docopy(pdfapp_t *app)
{
	unsigned short copyucs2[16 * 1024];
	char *utf8 = selection.utf8;
	unsigned short *ucs2;
	int ucs;

	pdfapp_oncopy(&gapp, copyucs2, 16 * 1024);

	for (ucs2 = copyucs2; ucs2[0] != 0; ucs2++)
	{
		ucs = ucs2[0];

		utf8 += fz_runetochar(utf8, ucs);
	}

	*utf8 = 0;
	selection.len = utf8 - selection.utf8;

	if (selection.source)
		wl_data_source_destroy(selection.source);
	selection.source = wl_data_device_manager_create_data_source(datadevicemanager);
	if (selection.source)
	{
		wl_data_source_add_listener(selection.source, &selection_listener, NULL);
		wl_data_source_offer(selection.source, "text/plain;charset=utf-8");
		wl_data_device_set_selection(datadevice, selection.source, selection.serial);
	}

	justcopied = 1;
}

void windocopy(pdfapp_t *app)
{
	docopy(app);
}

void winreloadpage(pdfapp_t *app)
{
	pdfapp_reloadpage(app);
}

void winopenuri(pdfapp_t *app, char *buf)
{
	char *browser = getenv("BROWSER");
	pid_t pid;
	if (!browser)
	{
#ifdef __APPLE__
		browser = "open";
#else
		browser = "xdg-open";
#endif
	}
	/* Fork once to start a child process that we wait on. This
	 * child process forks again and immediately exits. The
	 * grandchild process continues in the background. The purpose
	 * of this strange two-step is to avoid zombie processes. See
	 * bug 695701 for an explanation. */
	pid = fork();
	if (pid == 0)
	{
		if (fork() == 0)
		{
			execlp(browser, browser, buf, (char*)0);
			fprintf(stderr, "cannot exec '%s'\n", browser);
		}
		exit(0);
	}
	waitpid(pid, NULL, 0);
}

static void onkey(int c, int modifiers)
{
	advance_scheduled = 0;

	if (justcopied)
	{
		justcopied = 0;
		winrepaint(&gapp);
	}

	if (!gapp.issearching && c == 'P')
	{
		struct timeval now;
		struct timeval tmo;
		tmo.tv_sec = 2;
		tmo.tv_usec = 0;
		gettimeofday(&now, NULL);
		timeradd(&now, &tmo, &tmo_at);
		showingpage = 1;
		winrepaint(&gapp);
		return;
	}

	pdfapp_onkey(&gapp, c, modifiers);

	if (gapp.issearching)
	{
		showingpage = 0;
		showingmessage = 0;
	}
}

static void onmouse(int x, int y, int btn, int modifiers, int state)
{
	if (state != 0)
		advance_scheduled = 0;

	if (state != 0 && justcopied)
	{
		justcopied = 0;
		winrepaint(&gapp);
	}

	pdfapp_onmouse(&gapp, x, y, btn, modifiers, state);
}

static void signal_handler(int signal)
{
	if (signal == SIGHUP)
		reloading = 1;
}

static void usage(void)
{
	fprintf(stderr, "usage: mupdf [options] file.pdf [page]\n");
	fprintf(stderr, "\t-p -\tpassword\n");
	fprintf(stderr, "\t-r -\tresolution\n");
	fprintf(stderr, "\t-A -\tset anti-aliasing quality in bits (0=off, 8=best)\n");
	fprintf(stderr, "\t-C -\tRRGGBB (tint color in hexadecimal syntax)\n");
	fprintf(stderr, "\t-W -\tpage width for EPUB layout\n");
	fprintf(stderr, "\t-H -\tpage height for EPUB layout\n");
	fprintf(stderr, "\t-I -\tinvert colors\n");
	fprintf(stderr, "\t-S -\tfont size for EPUB layout\n");
	fprintf(stderr, "\t-U -\tuser style sheet for EPUB layout\n");
	fprintf(stderr, "\t-X\tdisable document styles for EPUB layout\n");
	exit(1);
}

int main(int argc, char **argv)
{
	int c;
	int resolution = -1;
	int pageno = 1;
	fd_set fds;
	fz_context *ctx;
	struct timeval now;
	struct timeval *timeout;
	struct timeval tmo_advance_delay;
	int bps = 0;

	ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
	if (!ctx)
	{
		fprintf(stderr, "cannot initialise context\n");
		exit(1);
	}

	pdfapp_init(ctx, &gapp);

	while ((c = fz_getopt(argc, argv, "Ip:r:A:C:W:H:S:U:Xb:")) != -1)
	{
		switch (c)
		{
		case 'C':
			c = strtol(fz_optarg, NULL, 16);
			gapp.tint = 1;
			gapp.tint_r = (c >> 16) & 255;
			gapp.tint_g = (c >> 8) & 255;
			gapp.tint_b = (c) & 255;
			break;
		case 'p': password = fz_optarg; break;
		case 'r': resolution = atoi(fz_optarg); break;
		case 'I': gapp.invert = 1; break;
		case 'A': fz_set_aa_level(ctx, atoi(fz_optarg)); break;
		case 'W': gapp.layout_w = fz_atof(fz_optarg); break;
		case 'H': gapp.layout_h = fz_atof(fz_optarg); break;
		case 'S': gapp.layout_em = fz_atof(fz_optarg); break;
		case 'U': gapp.layout_css = fz_optarg; break;
		case 'X': gapp.layout_use_doc_css = 0; break;
		case 'b': bps = (fz_optarg && *fz_optarg) ? fz_atoi(fz_optarg) : 4096; break;
		default: usage();
		}
	}

	if (argc - fz_optind == 0)
		usage();

	filename = argv[fz_optind++];

	if (argc - fz_optind == 1)
		pageno = atoi(argv[fz_optind++]);

	winopen();

	if (resolution == -1)
		resolution = winresolution();
	if (resolution < MINRES)
		resolution = MINRES;
	if (resolution > MAXRES)
		resolution = MAXRES;

	gapp.transitions_enabled = 1;
	gapp.resolution = resolution;
	gapp.pageno = pageno;

	tmo_at.tv_sec = 0;
	tmo_at.tv_usec = 0;
	timeout = NULL;

	if (bps)
		pdfapp_open_progressive(&gapp, filename, 0, bps);
	else
		pdfapp_open(&gapp, filename, 0);

	FD_ZERO(&fds);

	signal(SIGHUP, signal_handler);

	while (!closing)
	{
		wl_display_dispatch_pending(display);

		if (closing)
			continue;

		if (gwin.width != -1 || gwin.height != -1 || !gwin.image)
			resize(&gwin);
		if ((gwin.dirty || gwin.dirtysearch) && !gwin.frame)
		{
			if (gwin.dirty)
				winblit(&gapp);
			else if (gwin.dirtysearch)
				winblitstatusbar(&gapp);
			gwin.dirtysearch = 0;
			gwin.dirty = 0;

			wl_surface_attach(gwin.surface, gwin.image->buffer, 0, 0);
			wl_surface_damage(gwin.surface, 0, 0, gapp.winw, gapp.winh);
			gwin.frame = wl_surface_frame(gwin.surface);
			wl_callback_add_listener(gwin.frame, &frame_listener, &gwin);
			wl_surface_commit(gwin.surface);

			pdfapp_postblit(&gapp);
		}

		timeout = NULL;

		if (tmo_at.tv_sec || tmo_at.tv_usec)
		{
			gettimeofday(&now, NULL);
			timersub(&tmo_at, &now, &tmo);
			if (tmo.tv_sec <= 0)
			{
				tmo_at.tv_sec = 0;
				tmo_at.tv_usec = 0;
				timeout = NULL;
				showingpage = 0;
				showingmessage = 0;
				winrepaint(&gapp);
			}
			else
				timeout = &tmo;
		}

		if (advance_scheduled)
		{
			gettimeofday(&now, NULL);
			timersub(&tmo_advance, &now, &tmo_advance_delay);
			if (tmo_advance_delay.tv_sec <= 0)
			{
				/* Too late already */
				onkey(' ', 0);
				onmouse(gwin.cursor.x, gwin.cursor.y, 0, 0, 0);
				advance_scheduled = 0;
			}
			else if (timeout == NULL)
			{
				timeout = &tmo_advance_delay;
			}
			else
			{
				struct timeval tmp;
				timersub(&tmo_advance_delay, timeout, &tmp);
				if (tmp.tv_sec < 0)
				{
					timeout = &tmo_advance_delay;
				}
			}
		}

		wl_display_flush(display);

		FD_SET(wlfd, &fds);
		if (select(wlfd + 1, &fds, NULL, NULL, timeout) < 0)
		{
			if (reloading)
			{
				pdfapp_reloadfile(&gapp);
				reloading = 0;
			}
		}
		if (FD_ISSET(wlfd, &fds))
		{
			wl_display_dispatch(display);
		}
		else
		{
			if (timeout == &tmo_advance_delay)
			{
				onkey(' ', 0);
				onmouse(gwin.cursor.x, gwin.cursor.y, 0, 0, 0);
				advance_scheduled = 0;
			}
			else
			{
				tmo_at.tv_sec = 0;
				tmo_at.tv_usec = 0;
				timeout = NULL;
				showingpage = 0;
				showingmessage = 0;
				winrepaint(&gapp);
			}
		}
	}

	cleanup(&gapp);

	return 0;
}
