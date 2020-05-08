//  Wayland stuff

#pragma once

// Only define wlserver_t on the C side, as wlroots can't build as C++
#ifdef C_SIDE

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/xwayland.h>

#define WLSERVER_BUTTON_COUNT 4

struct wlserver_t {
	struct wl_display *wl_display;
	struct wl_event_loop *wl_event_loop;
	int wl_event_loop_fd;

	struct {
		struct wlr_backend *multi_backend;
		struct wlr_backend *headless_backend;
		struct wlr_backend *libinput_backend;

		struct wlr_renderer *renderer;
		struct wlr_compositor *compositor;
		struct wlr_xwayland_server *xwayland;
		struct wlr_session *session;	
		struct wlr_seat *seat;
		struct wlr_output *output;
	} wlr;
	
	struct wlr_surface *mouse_focus_surface;
	double mouse_surface_cursorx;
	double mouse_surface_cursory;
	
	bool button_held[ WLSERVER_BUTTON_COUNT ];
};

struct wlserver_keyboard {
	struct wlr_input_device *device;
	
	struct wl_listener modifiers;
	struct wl_listener key;
};

struct wlserver_pointer {
	struct wlr_input_device *device;
	
	struct wl_listener motion;
	struct wl_listener button;
};

struct wlserver_touch {
	struct wlr_input_device *device;
	
	struct wl_listener down;
	struct wl_listener up;
	struct wl_listener motion;
};

extern struct wlserver_t wlserver;

#endif

#ifndef C_SIDE
extern "C" {
#endif
	
extern bool run;

extern int g_nTouchClickMode;

void xwayland_surface_role_commit(struct wlr_surface *wlr_surface);

int wlserver_init( int argc, char **argv, bool bIsNested );

int wlserver_run(void);

void nudge_steamcompmgr(void);

void wlserver_lock(void);
void wlserver_unlock(void);

void wlserver_keyboardfocus( struct wlr_surface *surface );
void wlserver_key( uint32_t key, bool press, uint32_t time );

void wlserver_mousefocus( struct wlr_surface *wlrsurface );
void wlserver_mousemotion( int x, int y, uint32_t time );
void wlserver_mousebutton( int button, bool press, uint32_t time );
void wlserver_mousewheel( int x, int y, uint32_t time );

void wlserver_send_frame_done( struct wlr_surface *surf, const struct timespec *when );
struct wlr_surface *wlserver_get_surface( long surfaceID );

const char *wlserver_get_nested_display( void );

#ifndef C_SIDE
}
#endif
