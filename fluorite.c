#include "config/config_fluorite.h"
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/extensions/Xcomposite.h>
#include <xdo.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/Xrandr.h>
#include <err.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>

#define STACK_NEW		0
#define STACK_DEL		1
#define STACK_UP		2
#define STACK_DOWN		3

#define MAX_WORKSPACES	10

#define NO_FOCUS		0
#define MASTER_FOCUS	1
#define SLAVE_FOCUS		2
#define FLOAT_FOCUS		3

// Thanks suckless for these
static unsigned int numlockmask = 0;
#define MODMASK(mask)	(mask & ~(numlockmask | LockMask) & (ShiftMask|ControlMask|Mod1Mask|Mod2Mask|Mod3Mask|Mod4Mask|Mod5Mask))
#define LENGTH(X)		(sizeof X / sizeof X[0])

typedef struct
{
	Window	frame;
	Window	fullscreen_frame;
	Window	window;
	int		pos_x;
	int		pos_y;
	int		width;
	int		height;
} WinFrames;

typedef struct
{
	Window	window;
	int		pos_x;
	int		pos_y;
	int		width;
	int		height;
} Floating;

typedef struct
{
	WinFrames	*master_winframe;
	WinFrames	**slaves_winframes;
	WinFrames	*tmp_winframe;
	Floating	**floating_windows;
	int			frames_count;
	int			slaves_count;
	int			floating_count;
	int			master_offset;
	int			is_stacked;
	int			is_fullscreen;
	int			is_organising;
	int			organizer_selected;
	int			floating_hidden;
	int			current_focus;
} Workspaces;

typedef struct
{
	int				start_pos_x;
	int				start_pos_y;
	int				start_frame_x;
	int				start_frame_y;
	unsigned int	start_frame_w;
	unsigned int	start_frame_h;
} Mouse;

typedef struct
{
	int pos_x;
	int pos_y;
	int width;
	int height;
	int workspace;
	int primary;
} Monitor;

typedef struct
{
	Display		*display;
	Window		root;
	int			screen;
	int			screen_width;
	int			screen_height;
	int			running;
	Workspaces	*workspaces;
	Monitor		*monitor;
	int			current_workspace;
	int			current_monitor;
	int			monitor_count;
	Mouse		mouse;
	int			log;
} Fluorite;

static Fluorite fluorite;

// Core function
static void fluorite_init();
static void fluorite_run();
static void fluorite_clean();

// Core utilities
static void			fluorite_handle_configuration(XConfigureRequestEvent e);
static void			fluorite_handle_mapping(XMapRequestEvent e);
static void			fluorite_handle_unmapping(Window e);
static void			fluorite_handle_keys(XKeyPressedEvent e);
static void			fluorite_handle_enternotify(XEvent e);
static void			fluorite_handle_buttonpress(XButtonPressedEvent e);
static void			fluorite_handle_motions(XMotionEvent e);
static void			fluorite_handle_specials(Window e);
static void			fluorite_handle_normals(Window e);
static int			fluorite_handle_errors(Display *display, XErrorEvent *e);
static WinFrames	*fluorite_create_winframe();
static void			fluorite_organise_stack(int mode, int offset);
static void			fluorite_redraw_windows();
static void			fluorite_change_monitor(int monitor);
static void			dwm_grabkeys();

// Bindings functions (defined in config_fluorite.h)
void fluorite_execute(char *argument, int mode)
{
	if (fluorite.workspaces[fluorite.current_workspace].is_fullscreen && mode == GUI)
		fluorite_change_layout(FULLSCREEN_TOGGLE);
	strcat(argument, " &");
	system(argument);
}

void fluorite_close_window()
{
	if (fluorite.workspaces[fluorite.current_workspace].frames_count == 0 && fluorite.workspaces[fluorite.current_workspace].floating_count == 0)
		return ;
	if (fluorite.workspaces[fluorite.current_workspace].is_organising)
		return ;

	if (fluorite.workspaces[fluorite.current_workspace].is_fullscreen)
		fluorite_change_layout(FULLSCREEN_TOGGLE);

	XEvent closing;
	Window focused;
	int revert;
	XGetInputFocus(fluorite.display, &focused, &revert);
	memset(&closing, 0, sizeof(closing));
	closing.xclient.type = ClientMessage;
	closing.xclient.message_type = XInternAtom(fluorite.display, "WM_PROTOCOLS", False);
	closing.xclient.format = 32;
	closing.xclient.data.l[0] = XInternAtom(fluorite.display, "WM_DELETE_WINDOW", False);
	closing.xclient.window = focused;
	XSendEvent(fluorite.display, focused, False, 0, &closing);
}

void fluorite_user_close()
{
	fluorite.running = 0;
	fluorite_clean();
}

void fluorite_change_monitor(int new_monitor)
{
	XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, BORDER_UNFOCUSED);
	for (int i = 0; i < fluorite.workspaces[fluorite.current_workspace].frames_count; i++)
	{
		if (i == 0)
			XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->frame, BORDER_UNFOCUSED);
		else
			XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->frame, BORDER_INACTIVE);
	}
	fluorite.current_monitor = new_monitor;
	fluorite.current_workspace = fluorite.monitor[new_monitor].workspace;
	XChangeProperty(fluorite.display, fluorite.root, XInternAtom(fluorite.display, "_NET_CURRENT_DESKTOP", False), XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&fluorite.current_workspace, 1);
	if (fluorite.workspaces[fluorite.current_workspace].frames_count > 0)
	{
		XSetInputFocus(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->window, RevertToPointerRoot, CurrentTime);
		if (!fluorite.workspaces[fluorite.current_workspace].is_fullscreen)
			XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, BORDER_FOCUSED);
		XChangeProperty(fluorite.display, fluorite.root, XInternAtom(fluorite.display, "_NET_ACTIVE_WINDOW", False), XA_WINDOW, 32, PropModeReplace, (const unsigned char *) &fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0]->window, 1);
	}
	else
	{
		XSetInputFocus(fluorite.display, fluorite.root, RevertToPointerRoot, CurrentTime);
		XDeleteProperty(fluorite.display, fluorite.root, XInternAtom(fluorite.display, "_NET_ACTIVE_WINDOW", False));
	}
}

void fluorite_redraw_organizer()
{
	int special_offset = fluorite.monitor[fluorite.current_monitor].width / fluorite.workspaces[fluorite.current_workspace].frames_count;
	int special_height = fluorite.monitor[fluorite.current_monitor].height / 8;

	for (int i = fluorite.workspaces[fluorite.current_workspace].slaves_count - 1; i >= 0; i--)
	{
		fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->pos_x = fluorite.monitor[fluorite.current_monitor].pos_x + ((i + 1) * special_offset) + (GAPS * 2);
		if (fluorite.monitor[fluorite.current_monitor].primary == True)
			fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->pos_y = fluorite.monitor[fluorite.current_monitor].pos_y + fluorite.monitor[fluorite.current_monitor].height / 16 + (GAPS * 2) + TOPBAR_GAPS;
		else
			fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->pos_y = fluorite.monitor[fluorite.current_monitor].pos_y + fluorite.monitor[fluorite.current_monitor].height / 16 + (GAPS * 2);
		fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->width = special_offset - (GAPS * 3) - (BORDER_WIDTH * 2);
		if (fluorite.monitor[fluorite.current_monitor].primary == True)
			fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->height = fluorite.monitor[fluorite.current_monitor].height - special_height - (BORDER_WIDTH * 2) - (GAPS * 4) - TOPBAR_GAPS - BOTTOMBAR_GAPS;
		else
			fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->height = fluorite.monitor[fluorite.current_monitor].height - special_height - (BORDER_WIDTH * 2) - (GAPS * 4);
		XSetWindowBorderWidth(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->frame, BORDER_WIDTH);
		XMoveResizeWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->frame, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->pos_x, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->pos_y, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->width, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->height);
		XMoveResizeWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->window, 0, 0, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->width, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->height);
		XRaiseWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->frame);
		XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->frame, BORDER_UNFOCUSED);
	}
	fluorite.workspaces[fluorite.current_workspace].master_winframe->pos_x = fluorite.monitor[fluorite.current_monitor].pos_x + (GAPS * 2);
	if (fluorite.monitor[fluorite.current_monitor].primary == True)
		fluorite.workspaces[fluorite.current_workspace].master_winframe->pos_y = fluorite.monitor[fluorite.current_monitor].pos_y + fluorite.monitor[fluorite.current_monitor].height / 16 + (GAPS * 2) + TOPBAR_GAPS;
	else
		fluorite.workspaces[fluorite.current_workspace].master_winframe->pos_y = fluorite.monitor[fluorite.current_monitor].pos_y + fluorite.monitor[fluorite.current_monitor].height / 16 + (GAPS * 2);
	fluorite.workspaces[fluorite.current_workspace].master_winframe->width = special_offset - (GAPS * 3) - (BORDER_WIDTH * 2);
	fluorite.workspaces[fluorite.current_workspace].master_winframe->height = fluorite.monitor[fluorite.current_monitor].height - special_height - (BORDER_WIDTH * 2) - (GAPS * 4);
	XSetWindowBorderWidth(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, BORDER_WIDTH);
	XMoveResizeWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, fluorite.workspaces[fluorite.current_workspace].master_winframe->pos_x, fluorite.workspaces[fluorite.current_workspace].master_winframe->pos_y, fluorite.workspaces[fluorite.current_workspace].master_winframe->width, fluorite.workspaces[fluorite.current_workspace].master_winframe->height);
	XMoveResizeWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->window, 0, 0, fluorite.workspaces[fluorite.current_workspace].master_winframe->width, fluorite.workspaces[fluorite.current_workspace].master_winframe->height);
	XRaiseWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame);
	XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, BORDER_UNFOCUSED);
	if (fluorite.workspaces[fluorite.current_workspace].organizer_selected != 0)
		XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[fluorite.workspaces[fluorite.current_workspace].organizer_selected - 1]->frame, BORDER_FOCUSED);
	else
		XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, BORDER_FOCUSED);
}

void fluorite_organizer_mapping(int mode)
{
	if (!fluorite.workspaces[fluorite.current_workspace].is_organising)
		return;
	switch (mode)
	{
		case SELECT_NEXT:
			if (fluorite.workspaces[fluorite.current_workspace].organizer_selected + 1 < fluorite.workspaces[fluorite.current_workspace].frames_count)
				fluorite.workspaces[fluorite.current_workspace].organizer_selected++;
			else
				fluorite.workspaces[fluorite.current_workspace].organizer_selected = 0;
			fluorite_redraw_organizer();
			break;
		case SELECT_PREV:
			if (fluorite.workspaces[fluorite.current_workspace].organizer_selected - 1 >= 0)
				fluorite.workspaces[fluorite.current_workspace].organizer_selected--;
			else
				fluorite.workspaces[fluorite.current_workspace].organizer_selected = fluorite.workspaces[fluorite.current_workspace].frames_count - 1;
			fluorite_redraw_organizer();
			break;
		case MOVE_RIGHT:
			if (fluorite.workspaces[fluorite.current_workspace].organizer_selected == 0) // Master
			{
				memcpy(fluorite.workspaces[fluorite.current_workspace].tmp_winframe, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0], sizeof(WinFrames));
				memcpy(fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0], fluorite.workspaces[fluorite.current_workspace].master_winframe, sizeof(WinFrames));
				memcpy(fluorite.workspaces[fluorite.current_workspace].master_winframe, fluorite.workspaces[fluorite.current_workspace].tmp_winframe, sizeof(WinFrames));
			}
			else if (fluorite.workspaces[fluorite.current_workspace].organizer_selected + 1 == fluorite.workspaces[fluorite.current_workspace].frames_count) // Last slave
				fluorite_organise_stack(STACK_DOWN, -1);
			else
			{
				int pos = fluorite.workspaces[fluorite.current_workspace].organizer_selected - 1;
				memcpy(fluorite.workspaces[fluorite.current_workspace].tmp_winframe, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[pos + 1], sizeof(WinFrames));
				memcpy(fluorite.workspaces[fluorite.current_workspace].slaves_winframes[pos + 1], fluorite.workspaces[fluorite.current_workspace].slaves_winframes[pos], sizeof(WinFrames));
				memcpy(fluorite.workspaces[fluorite.current_workspace].slaves_winframes[pos], fluorite.workspaces[fluorite.current_workspace].tmp_winframe, sizeof(WinFrames));
			}
			fluorite_organizer_mapping(SELECT_NEXT);
			break;
		case MOVE_LEFT:
			if (fluorite.workspaces[fluorite.current_workspace].organizer_selected == 0) // Master
				fluorite_organise_stack(STACK_UP, -1);
			else if (fluorite.workspaces[fluorite.current_workspace].organizer_selected - 1 == 0) // First slave
			{
				memcpy(fluorite.workspaces[fluorite.current_workspace].tmp_winframe, fluorite.workspaces[fluorite.current_workspace].master_winframe, sizeof(WinFrames));
				memcpy(fluorite.workspaces[fluorite.current_workspace].master_winframe, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0], sizeof(WinFrames));
				memcpy(fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0], fluorite.workspaces[fluorite.current_workspace].tmp_winframe, sizeof(WinFrames));
			}
			else
			{
				int pos = fluorite.workspaces[fluorite.current_workspace].organizer_selected - 1;
				memcpy(fluorite.workspaces[fluorite.current_workspace].tmp_winframe, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[pos - 1], sizeof(WinFrames));
				memcpy(fluorite.workspaces[fluorite.current_workspace].slaves_winframes[pos - 1], fluorite.workspaces[fluorite.current_workspace].slaves_winframes[pos], sizeof(WinFrames));
				memcpy(fluorite.workspaces[fluorite.current_workspace].slaves_winframes[pos], fluorite.workspaces[fluorite.current_workspace].tmp_winframe, sizeof(WinFrames));
			}
			fluorite_organizer_mapping(SELECT_PREV);
			break;
	}
}

void fluorite_change_layout(int mode)
{
	if (fluorite.workspaces[fluorite.current_workspace].is_fullscreen && mode != FULLSCREEN_TOGGLE)
		return ;
	if (fluorite.workspaces[fluorite.current_workspace].is_organising && mode != ORGANIZER_TOGGLE)
		return ;
	switch(mode)
	{
		case FOCUS_TOP:
			fluorite_organise_stack(STACK_UP, -1);
			fluorite_redraw_windows();
			break;
		case FOCUS_BOTTOM:
			fluorite_organise_stack(STACK_DOWN, -1);
			fluorite_redraw_windows();
			break;
		case SLAVES_UP:
			if (fluorite.workspaces[fluorite.current_workspace].slaves_count < 2)
				break;
			fluorite_organise_stack(SLAVES_UP, -1);
			fluorite_redraw_windows();
			break;
		case SLAVES_DOWN:
			if (fluorite.workspaces[fluorite.current_workspace].slaves_count < 2)
				break;
			fluorite_organise_stack(SLAVES_DOWN, -1);
			fluorite_redraw_windows();
			break;
		case BIGGER_MASTER:
			if (fluorite.workspaces[fluorite.current_workspace].slaves_count > 0 && !fluorite.workspaces[fluorite.current_workspace].is_stacked)
			{
				if (fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0]->width - 25 > 10)
				{
					fluorite.workspaces[fluorite.current_workspace].master_offset += 25;
					fluorite_redraw_windows();
				}
			}
			break;
		case SMALLER_MASTER:
			if (fluorite.workspaces[fluorite.current_workspace].slaves_count > 0 && !fluorite.workspaces[fluorite.current_workspace].is_stacked)
			{
				if (fluorite.workspaces[fluorite.current_workspace].master_winframe->width - 25 > 10)
				{
					fluorite.workspaces[fluorite.current_workspace].master_offset -= 25;
					fluorite_redraw_windows();
				}
			}
			break;
		case BASE_MASTER:
			fluorite.workspaces[fluorite.current_workspace].master_offset = DEFAULT_MASTER_OFFSET;
			if (fluorite.workspaces[fluorite.current_workspace].frames_count > 1)
				fluorite_redraw_windows();
			break;
		case STACKING_TOGGLE:
			fluorite.workspaces[fluorite.current_workspace].is_stacked = !fluorite.workspaces[fluorite.current_workspace].is_stacked;
			fluorite_redraw_windows();
			break;
		case FULLSCREEN_TOGGLE:
			if (fluorite.workspaces[fluorite.current_workspace].frames_count <= 0 && fluorite.workspaces[fluorite.current_workspace].floating_count <= 0)
			{
				fluorite.workspaces[fluorite.current_workspace].is_fullscreen = 0;
				return ;
			}
			if (fluorite.workspaces[fluorite.current_workspace].is_fullscreen == 0)
			{
				Window tmp_window = XCreateSimpleWindow(fluorite.display, fluorite.root, fluorite.monitor[fluorite.current_monitor].pos_x, fluorite.monitor[fluorite.current_monitor].pos_y, fluorite.monitor[fluorite.current_monitor].width, fluorite.monitor[fluorite.current_monitor].height, 0, 0x0, 0x0);
				if (fluorite.workspaces[fluorite.current_workspace].current_focus == MASTER_FOCUS)
				{
					XReparentWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, tmp_window, RevertToPointerRoot, CurrentTime);
					fluorite.workspaces[fluorite.current_workspace].master_winframe->fullscreen_frame = tmp_window;
					XMapWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->fullscreen_frame);
					XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, 0x0);
					XMoveResizeWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, 0, 0, fluorite.monitor[fluorite.current_monitor].width, fluorite.monitor[fluorite.current_monitor].height);
					XMoveResizeWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->window, 0, 0, fluorite.monitor[fluorite.current_monitor].width, fluorite.monitor[fluorite.current_monitor].height);
					XSync(fluorite.display, True);
					XSetInputFocus(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->window, RevertToPointerRoot, CurrentTime);
				}
				else if (fluorite.workspaces[fluorite.current_workspace].current_focus == SLAVE_FOCUS)
				{
					XReparentWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0]->frame, tmp_window, RevertToPointerRoot, CurrentTime);
					fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0]->fullscreen_frame = tmp_window;
					XMapWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0]->fullscreen_frame);
					XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0]->frame, 0x0);
					XMoveResizeWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0]->frame, 0, 0, fluorite.screen_width, fluorite.screen_height);
					XMoveResizeWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0]->window, 0, 0, fluorite.screen_width, fluorite.screen_height);
					XSync(fluorite.display, True);
					XSetInputFocus(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0]->window, RevertToPointerRoot, CurrentTime);
				}
				else
				{
					Window focused;
					int revert;
					XGetInputFocus(fluorite.display, &focused, &revert);
					for (int i = 0; i < fluorite.workspaces[fluorite.current_workspace].floating_count; i++)
						if (focused == fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->window)
						{
							fluorite_change_layout(FLOATING_TOGGLE);
							fluorite_change_layout(FULLSCREEN_TOGGLE);
						}
					return ;
				}
				fluorite.workspaces[fluorite.current_workspace].is_fullscreen = 1;
			}
			else
			{
				if (fluorite.workspaces[fluorite.current_workspace].current_focus == MASTER_FOCUS)
				{
					XReparentWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, fluorite.root, RevertToPointerRoot, CurrentTime);
					XUnmapWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->fullscreen_frame);
				}
				else
				{
					XReparentWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0]->frame, fluorite.root, RevertToPointerRoot, CurrentTime);
					XUnmapWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0]->fullscreen_frame);
				}
				XSync(fluorite.display, True);
				fluorite.workspaces[fluorite.current_workspace].is_fullscreen = 0;
				fluorite_redraw_windows();
			}
			break;
		case ORGANIZER_TOGGLE:
			if (fluorite.workspaces[fluorite.current_workspace].frames_count <= 1)
			{
				fluorite.workspaces[fluorite.current_workspace].is_organising = False;
				return ;
			}
			if (fluorite.workspaces[fluorite.current_workspace].is_organising)
			{
				fluorite.workspaces[fluorite.current_workspace].is_organising = False;
				fluorite_redraw_windows();
			}
			else
			{
				fluorite.workspaces[fluorite.current_workspace].is_organising = True;
				fluorite.workspaces[fluorite.current_workspace].organizer_selected = 0;
				fluorite_redraw_organizer();
			}

			break;
		case SWAP_FOCUS:
			if (fluorite.workspaces[fluorite.current_workspace].is_fullscreen || fluorite.workspaces[fluorite.current_workspace].is_stacked || fluorite.workspaces[fluorite.current_workspace].frames_count <= 1)
				return ;
			if (fluorite.workspaces[fluorite.current_workspace].current_focus == MASTER_FOCUS)
			{
				fluorite.workspaces[fluorite.current_workspace].current_focus = SLAVE_FOCUS;
				XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0]->frame, BORDER_FOCUSED);
				XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, BORDER_UNFOCUSED);
				XSetInputFocus(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0]->window, RevertToPointerRoot, CurrentTime);
				XChangeProperty(fluorite.display, fluorite.root, XInternAtom(fluorite.display, "_NET_ACTIVE_WINDOW", False), XA_WINDOW, 32, PropModeReplace, (const unsigned char *) &fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0]->window, 1);
			}
			else
			{
				fluorite.workspaces[fluorite.current_workspace].current_focus = MASTER_FOCUS;
				XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, BORDER_FOCUSED);
				XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0]->frame, BORDER_UNFOCUSED);
				XSetInputFocus(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->window, RevertToPointerRoot, CurrentTime);
				XChangeProperty(fluorite.display, fluorite.root, XInternAtom(fluorite.display, "_NET_ACTIVE_WINDOW", False), XA_WINDOW, 32, PropModeReplace, (const unsigned char *) &fluorite.workspaces[fluorite.current_workspace].master_winframe->window, 1);
			}
			break;
		case FLOATING_TOGGLE:
			{
				Window focused;
				int revert;
				XGetInputFocus(fluorite.display, &focused, &revert);
				if (focused == fluorite.workspaces[fluorite.current_workspace].master_winframe->window && fluorite.workspaces[fluorite.current_workspace].floating_count < MAX_WINDOWS && fluorite.workspaces[fluorite.current_workspace].frames_count != 0)
				{
					fluorite.workspaces[fluorite.current_workspace].frames_count--;
					XUnmapWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame);
					bzero(fluorite.workspaces[fluorite.current_workspace].master_winframe, sizeof(WinFrames));
					XReparentWindow(fluorite.display, focused, fluorite.root, 0, 0);
					fluorite_handle_specials(focused);
					if (fluorite.workspaces[fluorite.current_workspace].slaves_count > 0)
					{
						memcpy(fluorite.workspaces[fluorite.current_workspace].master_winframe, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0], sizeof(WinFrames));
						fluorite.workspaces[fluorite.current_workspace].slaves_count--;
						fluorite_organise_stack(STACK_DEL, 0);
					}
					fluorite_redraw_windows();
				}
				else if (focused == fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0]->window && fluorite.workspaces[fluorite.current_workspace].floating_count < MAX_WINDOWS && fluorite.workspaces[fluorite.current_workspace].slaves_count != 0)
				{
					if (fluorite.workspaces[fluorite.current_workspace].slaves_count == 0)
						return ;
					fluorite.workspaces[fluorite.current_workspace].frames_count--;
					XUnmapWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0]->frame);
					bzero(fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0], sizeof(WinFrames));
					XReparentWindow(fluorite.display, focused, fluorite.root, 0, 0);
					fluorite_handle_specials(focused);
					fluorite_organise_stack(STACK_DEL, 0);
					fluorite.workspaces[fluorite.current_workspace].slaves_count--;
					fluorite_redraw_windows();
				}
				else
				{
					if (fluorite.workspaces[fluorite.current_workspace].frames_count >= 10)
						return ;
					for (int i = 0; i < fluorite.workspaces[fluorite.current_workspace].floating_count; i++)
					{
						if (focused == fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->window)
						{
							for (int j = i + 1; j < fluorite.workspaces[fluorite.current_workspace].floating_count; j++)
								memcpy(fluorite.workspaces[fluorite.current_workspace].floating_windows[j - 1], fluorite.workspaces[fluorite.current_workspace].floating_windows[j], sizeof(Floating));
							fluorite.workspaces[fluorite.current_workspace].floating_count--;
							fluorite_handle_normals(focused);
							fluorite_redraw_windows();
							break;
						}
					}
				}
			}
			break;
		case FLOATING_HIDE_SHOW:
			if (fluorite.workspaces[fluorite.current_workspace].floating_count <= 0)
				return ;
			if (!fluorite.workspaces[fluorite.current_workspace].floating_hidden)
				for (int i = 0; i < fluorite.workspaces[fluorite.current_workspace].floating_count; i++)
					XMoveWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->window, fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->pos_x, -(fluorite.screen_height * 2));
			else
				for (int i = 0; i < fluorite.workspaces[fluorite.current_workspace].floating_count; i++)
					XMoveWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->window, fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->pos_x, fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->pos_y);
			fluorite.workspaces[fluorite.current_workspace].floating_hidden = !fluorite.workspaces[fluorite.current_workspace].floating_hidden;
			break;
	}
}


void fluorite_show_new_workspace(int new_workspace)
{
	for (int i = 0; i < fluorite.monitor_count; i++)
	{
		if (fluorite.monitor[i].workspace == new_workspace)
		{
			int swap_workspaces;
			int swap_monitor;

			swap_workspaces = fluorite.current_workspace;
			swap_monitor = fluorite.current_monitor;
			fluorite.monitor[i].workspace = swap_workspaces;
			fluorite.current_monitor = i;
			fluorite_redraw_windows();
			fluorite.current_workspace = new_workspace;
			fluorite.current_monitor = swap_monitor;
			fluorite.monitor[fluorite.current_monitor].workspace = new_workspace;
			XChangeProperty(fluorite.display, fluorite.root, XInternAtom(fluorite.display, "_NET_CURRENT_DESKTOP", False), XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&fluorite.current_workspace, 1);
			return ;
		}
	}

	if (fluorite.workspaces[fluorite.current_workspace].frames_count > 0)
	{
		if (fluorite.workspaces[fluorite.current_workspace].master_winframe->frame)
			XUnmapWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame);
		for (int i = 0; i < fluorite.workspaces[fluorite.current_workspace].slaves_count; i++)
			XUnmapWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->frame);
	}
	if (!fluorite.workspaces[fluorite.current_workspace].floating_hidden)
		for (int i = 0; i < fluorite.workspaces[fluorite.current_workspace].floating_count; i++)
			XMoveWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->window, fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->pos_x, -(fluorite.screen_height * 2));
	fluorite.current_workspace = new_workspace;
	fluorite.monitor[fluorite.current_monitor].workspace = new_workspace;
	if (fluorite.workspaces[fluorite.current_workspace].frames_count > 0)
	{
		if (fluorite.workspaces[fluorite.current_workspace].master_winframe->frame)
			XMapWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame);
		for (int i = 0; i < fluorite.workspaces[fluorite.current_workspace].slaves_count; i++)
			XMapWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->frame);
	}
	if (!fluorite.workspaces[fluorite.current_workspace].floating_hidden)
		for (int i = 0; i < fluorite.workspaces[fluorite.current_workspace].floating_count; i++)
			XMoveWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->window, fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->pos_x, fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->pos_y);
	XChangeProperty(fluorite.display, fluorite.root, XInternAtom(fluorite.display, "_NET_CURRENT_DESKTOP", False), XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&fluorite.current_workspace, 1);
}

void fluorite_send_window(int new_workspace)
{
	int keep_workspace = fluorite.current_workspace;

	if (fluorite.workspaces[fluorite.current_workspace].frames_count == 0 || fluorite.workspaces[new_workspace].frames_count == MAX_WINDOWS)
		return ;

	if (fluorite.workspaces[fluorite.current_workspace].current_focus == SLAVE_FOCUS)
		fluorite_organise_stack(STACK_UP, -1);

	if (fluorite.workspaces[new_workspace].frames_count > 0)
	{
		memcpy(fluorite.workspaces[new_workspace].tmp_winframe, fluorite.workspaces[new_workspace].master_winframe, sizeof(WinFrames));
		fluorite.workspaces[new_workspace].slaves_count++;
	}
	fluorite.workspaces[new_workspace].frames_count++;
	memcpy(fluorite.workspaces[new_workspace].master_winframe, fluorite.workspaces[fluorite.current_workspace].master_winframe, sizeof(WinFrames));
	if (fluorite.workspaces[new_workspace].slaves_count > 0)
	{
		fluorite.current_workspace = new_workspace;
		fluorite_organise_stack(STACK_NEW, -1);
		fluorite.current_workspace = keep_workspace;
	}
	fluorite.workspaces[fluorite.current_workspace].frames_count--;
	XUnmapWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame);
	XSetInputFocus(fluorite.display, fluorite.root, RevertToPointerRoot, CurrentTime);
	if (fluorite.workspaces[fluorite.current_workspace].slaves_count > 0)
	{
		memcpy(fluorite.workspaces[fluorite.current_workspace].master_winframe, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0], sizeof(WinFrames));
		XSetInputFocus(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, RevertToPointerRoot, CurrentTime);
		XRaiseWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame);
		fluorite_organise_stack(STACK_DEL, 0);
		fluorite.workspaces[fluorite.current_workspace].slaves_count--;
	}

	for (int i = 0; i < fluorite.monitor_count; i++)
	{
		if (fluorite.monitor[i].workspace == new_workspace)
		{
			int keep_monitor = fluorite.current_monitor;
			fluorite.current_monitor = i;
			fluorite.current_workspace = new_workspace;
			XMapWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame);
			fluorite_redraw_windows();
			fluorite.current_monitor = keep_monitor;
			fluorite.current_workspace = keep_workspace;
		}
	}

	if (fluorite.workspaces[fluorite.current_workspace].current_focus == SLAVE_FOCUS)
		fluorite_organise_stack(STACK_DOWN, -1);

	if (FOLLOW_WINDOWS)
		fluorite_change_workspace(new_workspace, 0);
}

void fluorite_change_workspace(int new_workspace, int mode)
{
	if (fluorite.current_workspace == new_workspace)
		return;
	if (fluorite.workspaces[fluorite.current_workspace].is_fullscreen)
		fluorite_change_layout(FULLSCREEN_TOGGLE);
	if (fluorite.workspaces[fluorite.current_workspace].is_organising && mode == 0)
		fluorite_change_layout(ORGANIZER_TOGGLE);
	else if (fluorite.workspaces[fluorite.current_workspace].is_organising && mode == 1)
		return ;
	if (mode == 0)
		fluorite_show_new_workspace(new_workspace);
	else
		fluorite_send_window(new_workspace);
	fluorite_redraw_windows();
}

// Core functions
void fluorite_apply_property()
{
	XChangeProperty(fluorite.display, fluorite.root, XInternAtom(fluorite.display, "_NET_WM_NAME", False), XInternAtom(fluorite.display, "UTF8_STRING", False), 8, PropModeReplace, (const unsigned char *) "Fluorite", 8);
	XChangeProperty(fluorite.display, fluorite.root, XInternAtom(fluorite.display, "_NET_SUPPORTING_WM_CHECK", False), XA_WINDOW, 32, PropModeReplace, (const unsigned char *) &fluorite.root, 1);
	int num_work_atom = MAX_WORKSPACES;
	XChangeProperty(fluorite.display, fluorite.root, XInternAtom(fluorite.display, "_NET_NUMBER_OF_DESKTOPS", False), XA_CARDINAL, 32, PropModeReplace, (const unsigned char *)&num_work_atom, 1);
	char *workspaces;
	for (int i = 0; i < MAX_WORKSPACES; i++)
	{
		workspaces = strdup(workspace_names[i]);
		XChangeProperty(fluorite.display, fluorite.root, XInternAtom(fluorite.display, "_NET_DESKTOP_NAMES", False), XInternAtom(fluorite.display, "UTF8_STRING", False), 8, PropModeAppend, (const unsigned char *)workspaces, 2);
		XFree(workspaces);
	}
	XChangeProperty(fluorite.display, fluorite.root, XInternAtom(fluorite.display, "_NET_CURRENT_DESKTOP", False), XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&fluorite.current_workspace, 1);
	Atom supported[1];
	supported[0] = XInternAtom(fluorite.display, "_NET_ACTIVE_WINDOW", False);
	XChangeProperty(fluorite.display, fluorite.root, XInternAtom(fluorite.display, "_NET_SUPPORTED", False), XA_ATOM, 32, PropModeReplace, (unsigned char *)supported, 1);
}

void fluorite_init()
{
	XSetWindowAttributes attributes;

	fluorite.log = open("/tmp/fluorite.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
	fluorite.display = XOpenDisplay(NULL);
	if (fluorite.display == NULL)
		errx(1, "Can't open display.");

	fluorite.screen = DefaultScreen(fluorite.display);
	fluorite.root = RootWindow(fluorite.display, fluorite.screen);
	fluorite.screen_width = DisplayWidth(fluorite.display, fluorite.screen);
	fluorite.screen_height = DisplayHeight(fluorite.display, fluorite.screen);
	fluorite.current_workspace = 0;

	XRRMonitorInfo *infos;
	infos = XRRGetMonitors(fluorite.display, fluorite.root, 0, &fluorite.monitor_count);
	fluorite.monitor = (Monitor *) malloc(sizeof(Monitor) * fluorite.monitor_count);
	int non_primary_workspace = 1;
	for (int i = 0; i < fluorite.monitor_count; i++)
	{
		fluorite.monitor[i].width = infos[i].width;
		fluorite.monitor[i].height = infos[i].height;
		fluorite.monitor[i].pos_x = infos[i].x;
		fluorite.monitor[i].pos_y = infos[i].y;
		if (infos[i].primary)
		{
			fluorite.monitor[i].primary = True;
			fluorite.monitor[i].workspace = 0;
			fluorite.current_monitor = i;
		}
		else
		{
			fluorite.monitor[i].primary = False;
			fluorite.monitor[i].workspace = non_primary_workspace;
			non_primary_workspace++;
		}
	}

	fluorite.workspaces = malloc(sizeof(Workspaces) * MAX_WORKSPACES);
	for (int i = 0; i < MAX_WORKSPACES; i++)
	{
		fluorite.workspaces[i].frames_count = 0;
		fluorite.workspaces[i].slaves_count = 0;
		fluorite.workspaces[i].floating_count = 0;
		fluorite.workspaces[i].master_offset = DEFAULT_MASTER_OFFSET;
		fluorite.workspaces[i].is_stacked = False;
		fluorite.workspaces[i].is_fullscreen = False;
		fluorite.workspaces[i].floating_hidden = False;
		fluorite.workspaces[i].is_organising = False;
		fluorite.workspaces[i].current_focus = NO_FOCUS;
	}

	fluorite_apply_property();
	XFlush(fluorite.display);
	XSelectInput(fluorite.display, fluorite.root, SubstructureNotifyMask | SubstructureRedirectMask);
	XSync(fluorite.display, False);
	attributes.event_mask = SubstructureNotifyMask | SubstructureRedirectMask | StructureNotifyMask | ButtonPressMask | KeyPressMask | PointerMotionMask | PropertyChangeMask;
	XChangeWindowAttributes(fluorite.display, fluorite.root, CWEventMask, &attributes);
	XDefineCursor(fluorite.display, fluorite.root, XcursorLibraryLoadCursor(fluorite.display, "arrow"));
	XSync(fluorite.display, False);
	XSetErrorHandler(fluorite_handle_errors);

	for (int j = 0; j < MAX_WORKSPACES; j++)
	{
		fluorite.workspaces[j].master_winframe = (WinFrames *) malloc(sizeof(WinFrames));
		fluorite.workspaces[j].tmp_winframe = (WinFrames *) malloc(sizeof(WinFrames));
		fluorite.workspaces[j].slaves_winframes = (WinFrames **) malloc(sizeof(WinFrames *) * (MAX_WINDOWS + 1));
		fluorite.workspaces[j].floating_windows = (Floating **) malloc(sizeof(Floating *) * (MAX_WINDOWS + 1));
		for (int i = 0; i < MAX_WINDOWS; i++)
		{
			fluorite.workspaces[j].slaves_winframes[i] = (WinFrames *) malloc(sizeof(WinFrames));
			fluorite.workspaces[j].floating_windows[i] = (Floating *) malloc(sizeof(Floating));
		}
	}

	dwm_grabkeys();
}

void fluorite_run()
{
	XEvent ev;

	fluorite.running = 1;
	while (fluorite.running)
	{
		XNextEvent(fluorite.display, &ev);
		switch (ev.type)
		{
			case ConfigureRequest:
				fluorite_handle_configuration(ev.xconfigurerequest);
				break;
			case MapRequest:
				fluorite_handle_mapping(ev.xmaprequest);
				break;
			case UnmapNotify:
				fluorite_handle_unmapping(ev.xunmap.window);
				break;
			case EnterNotify:
				fluorite_handle_enternotify(ev);
				break;
			case ButtonPress:
				fluorite_handle_buttonpress(ev.xbutton);
				break;
			case MotionNotify:
				fluorite_handle_motions(ev.xmotion);
				break;
			case LeaveNotify:
				if (fluorite.workspaces[fluorite.current_workspace].current_focus == SLAVE_FOCUS && fluorite.workspaces[fluorite.current_workspace].frames_count > 0 && !fluorite.workspaces[fluorite.current_workspace].is_fullscreen && !fluorite.workspaces[fluorite.current_workspace].is_organising)
				{
					fluorite.workspaces[fluorite.current_workspace].current_focus = MASTER_FOCUS;
					XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0]->frame, BORDER_UNFOCUSED);
					XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, BORDER_FOCUSED);
					XSetInputFocus(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->window, RevertToPointerRoot, CurrentTime);
				}
				for (int i = 0; i < fluorite.workspaces[fluorite.current_workspace].floating_count; i++)
					XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->window, BORDER_UNFOCUSED);
				break;
			case KeyPress:
				fluorite_handle_keys(ev.xkey);
				break;
		}
	}
}

void fluorite_clean()
{
	close(fluorite.log);
	for (int i = 0; i < MAX_WORKSPACES; i++)
	{
		fluorite.current_workspace = i;
		free(fluorite.workspaces[fluorite.current_workspace].master_winframe);
		free(fluorite.workspaces[fluorite.current_workspace].tmp_winframe);
		for (int i = 0; fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]; i++)
			free(fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]);
		free(fluorite.workspaces[fluorite.current_workspace].slaves_winframes);
	}
	XCloseDisplay(fluorite.display);
}

// Core utilities
void dwm_updatenumlockmask()
{
	unsigned int i;
	int j;
	XModifierKeymap *modmap;

	numlockmask = 0;
	modmap = XGetModifierMapping(fluorite.display);
	for (i = 0; i < 8; i++)
		for (j = 0; j < modmap->max_keypermod; j++)
			if (modmap->modifiermap[i * modmap->max_keypermod + j] == XKeysymToKeycode(fluorite.display, XK_Num_Lock))
				numlockmask = (1 << i);
	XFreeModifiermap(modmap);
}

void dwm_grabkeys()
{
	// Since it works fine, why recode it ?
	dwm_updatenumlockmask();
	{
		unsigned int i, j, k;
		unsigned int modifiers[] = { 0, LockMask, numlockmask, numlockmask|LockMask };
		int start, end, skip;
		KeySym *syms;

		XUngrabKey(fluorite.display, AnyKey, AnyModifier, fluorite.root);
		XDisplayKeycodes(fluorite.display, &start, &end);
		syms = XGetKeyboardMapping(fluorite.display, start, end - start + 1, &skip);
		if (!syms)
			return;
		for (k = start; k <= (unsigned int)end; k++)
			for (i = 0; i < LENGTH(bind); i++)
				if (bind[i].key == syms[(k - start) * skip])
					for (j = 0; j < LENGTH(modifiers); j++)
						XGrabKey(fluorite.display, k,
							 bind[i].mod | modifiers[j],
							 fluorite.root, True,
							 GrabModeAsync, GrabModeAsync);
		XFree(syms);
	}
}

void fluorite_handle_configuration(XConfigureRequestEvent e)
{
	XWindowChanges changes;
	changes.x = e.x;
	changes.y = e.y;
	changes.width = e.width;
	changes.height = e.height;
	changes.border_width = e.border_width;
	changes.sibling = e.above;
	changes.stack_mode = e.detail;
	XConfigureWindow(fluorite.display, e.window, e.value_mask, &changes);
}

int fluorite_check_fixed(Window e)
{
	long msize;
	XSizeHints size;
	int maxw, maxh;
	int minw, minh;

	if (!XGetWMNormalHints(fluorite.display, e, &size, &msize))
		return False;
	if (size.flags & PMaxSize)
	{
		maxw = size.max_width;
		maxh = size.max_height;
	}
	else
		maxw = maxh = 0;
	if (size.flags & PMinSize)
	{
		minw = size.min_width;
		minh = size.min_height;
	}
	else if (size.flags & PBaseSize)
	{
		minw = size.base_width;
		minh = size.base_height;
	}
	else
		minw = minh = 0;
	if (maxw && maxh && maxw == minw && maxh == minh)
		return True;
	return False;
}

int fluorite_check_type(Window e)
{
	int di;
	unsigned long dl;
	unsigned char *p = NULL;
	Atom da, atom = None;
	XClassHint name;

	if (XGetWindowProperty(fluorite.display, e, XInternAtom(fluorite.display, "_NET_WM_WINDOW_TYPE", False), 0L, sizeof(atom), False, XA_ATOM, &da, &di, &dl, &dl, &p) == Success && p)
	{
		atom = *(Atom *)p;
		if (atom == XInternAtom(fluorite.display, "_NET_WM_WINDOW_TYPE_DIALOG", False))
		{
			XFree(p);
			return True;
		}
	}
	XFree(p);
	XGetClassHint(fluorite.display, e, &name);
	if (strlen(name.res_class) == 0)
		return False;
	for (long unsigned int i = 0; i < LENGTH(default_floating); i++)
	{
		if (strcmp(default_floating[i].wm_class, name.res_class) == 0 || strcmp(default_floating[i].wm_class, name.res_name) == 0)
			return True;
	}
	return False;
}

void fluorite_handle_specials(Window e)
{
	if (fluorite.workspaces[fluorite.current_workspace].floating_count == MAX_WINDOWS)
		return ;

	long lhints;
	XSizeHints hints;
	int	pos;

	if (fluorite.workspaces[fluorite.current_workspace].is_fullscreen)
	{
		XMapWindow(fluorite.display, e);
		XGrabButton(fluorite.display, Button1, METAKEY, e, False, ButtonPressMask | ButtonReleaseMask | ButtonMotionMask, GrabModeAsync, GrabModeAsync, None, None);
		XGrabButton(fluorite.display, Button3, METAKEY, e, False, ButtonPressMask | ButtonReleaseMask | ButtonMotionMask, GrabModeAsync, GrabModeAsync, None, None);
		XSetWindowBorderWidth(fluorite.display, e, BORDER_WIDTH);
		XSetWindowBorder(fluorite.display, e, BORDER_FOCUSED);
		XSetInputFocus(fluorite.display, e, RevertToPointerRoot, CurrentTime);
		XSync(fluorite.display, True);
		return ;
	}

	pos = fluorite.workspaces[fluorite.current_workspace].floating_count;
	if (pos >= 1 && fluorite.workspaces[fluorite.current_workspace].floating_hidden)
		fluorite_change_layout(FLOATING_HIDE_SHOW);
	fluorite.workspaces[fluorite.current_workspace].floating_count++;
	fluorite.workspaces[fluorite.current_workspace].floating_windows[pos]->window = e;
	if (XGetWMNormalHints(fluorite.display, e, &hints, &lhints))
	{
		if (hints.x < fluorite.monitor[fluorite.current_monitor].pos_x)
			hints.x += fluorite.monitor[fluorite.current_monitor].pos_x;
		if (hints.y < fluorite.monitor[fluorite.current_monitor].pos_y)
			hints.y += fluorite.monitor[fluorite.current_monitor].pos_y;
		if (hints.x < 2)
			hints.x = fluorite.monitor[fluorite.current_monitor].width / 2 - (hints.width / 2);
		if (hints.y < 2)
			hints.y = fluorite.monitor[fluorite.current_monitor].height / 2 - (hints.height / 2);
		fluorite.workspaces[fluorite.current_workspace].floating_windows[pos]->pos_x = hints.x;
		fluorite.workspaces[fluorite.current_workspace].floating_windows[pos]->pos_y = hints.y;
		fluorite.workspaces[fluorite.current_workspace].floating_windows[pos]->width = hints.width;
		fluorite.workspaces[fluorite.current_workspace].floating_windows[pos]->height = hints.height;
	}
	else
	{
		fluorite.workspaces[fluorite.current_workspace].floating_windows[pos]->width =  300;
		fluorite.workspaces[fluorite.current_workspace].floating_windows[pos]->height = 300;
		fluorite.workspaces[fluorite.current_workspace].floating_windows[pos]->pos_x = fluorite.monitor[fluorite.current_monitor].pos_x + (fluorite.monitor[fluorite.current_monitor].width / 2) - (fluorite.workspaces[fluorite.current_workspace].floating_windows[pos]->width / 2);
		fluorite.workspaces[fluorite.current_workspace].floating_windows[pos]->pos_y = fluorite.monitor[fluorite.current_monitor].pos_y + (fluorite.monitor[fluorite.current_monitor].height / 2) - (fluorite.workspaces[fluorite.current_workspace].floating_windows[pos]->height / 2);
	}
	XMoveResizeWindow(fluorite.display, e, hints.x, hints.y, hints.width, hints.height);
	XMapWindow(fluorite.display, e);
	XGrabButton(fluorite.display, Button1, METAKEY, e, False, ButtonPressMask | ButtonReleaseMask | ButtonMotionMask, GrabModeAsync, GrabModeAsync, None, None);
	XGrabButton(fluorite.display, Button3, METAKEY, e, False, ButtonPressMask | ButtonReleaseMask | ButtonMotionMask, GrabModeAsync, GrabModeAsync, None, None);
	XSetWindowBorderWidth(fluorite.display, e, BORDER_WIDTH);
	XSetWindowBorder(fluorite.display, e, BORDER_FOCUSED);
	XSetInputFocus(fluorite.display, e, RevertToPointerRoot, CurrentTime);
	XSync(fluorite.display, True);
	fluorite.workspaces[fluorite.current_workspace].current_focus = FLOAT_FOCUS;
}

void fluorite_handle_normals(Window e)
{
	if (fluorite.workspaces[fluorite.current_workspace].frames_count > 0)
	{
		memcpy(fluorite.workspaces[fluorite.current_workspace].tmp_winframe, fluorite.workspaces[fluorite.current_workspace].master_winframe, sizeof(WinFrames));
		fluorite.workspaces[fluorite.current_workspace].slaves_count++;
	}

	fluorite.workspaces[fluorite.current_workspace].frames_count++;
	fluorite.workspaces[fluorite.current_workspace].master_winframe = fluorite_create_winframe();
	fluorite.workspaces[fluorite.current_workspace].master_winframe->window = e;
	fluorite.workspaces[fluorite.current_workspace].current_focus = MASTER_FOCUS;

	XSelectInput(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, SubstructureNotifyMask | SubstructureRedirectMask | EnterWindowMask);
	XWMHints *source_hints = XGetWMHints(fluorite.display, e);
	if (source_hints)
	{
		XSetWMHints(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, source_hints);
		XFree(source_hints);
	}

	// TODO: Check if it's working
	XTextProperty name;
	XClassHint class;
	XGetWMName(fluorite.display, e, &name);
	XStoreName(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, (const char *)name.value);
	XGetClassHint(fluorite.display, e, &class);
	if (strlen(class.res_class) != 0)
		XSetClassHint(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, &class);

	XSetWindowBorder(fluorite.display, e, 0x0);
	XSetWindowBorderWidth(fluorite.display, e, 0);
	XReparentWindow(fluorite.display, e, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, 0, 0);
	XMapWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame);
	XRaiseWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame);
	XMapWindow(fluorite.display, e);
	XSync(fluorite.display, True);

	if (fluorite.workspaces[fluorite.current_workspace].slaves_count > 0)
		fluorite_organise_stack(STACK_NEW, -1);

	for (int i = 0; i < fluorite.workspaces[fluorite.current_workspace].floating_count; i++)
		XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->window, BORDER_UNFOCUSED);
	XChangeProperty(fluorite.display, fluorite.root, XInternAtom(fluorite.display, "_NET_ACTIVE_WINDOW", False), XA_WINDOW, 32, PropModeReplace, (const unsigned char *) &e, 1);
}

void fluorite_handle_mapping(XMapRequestEvent e)
{
	if (fluorite.workspaces[fluorite.current_workspace].is_organising)
		fluorite_change_layout(ORGANIZER_TOGGLE);

	if (fluorite_check_type(e.window) && AUTO_FLOATING)
	{
		fluorite_handle_specials(e.window);
		return ;
	}

	if (fluorite_check_fixed(e.window))
	{
		XMapWindow(fluorite.display, e.window);
		XRaiseWindow(fluorite.display, e.window);
		XSync(fluorite.display, True);
		return ;
	}

	if (fluorite.workspaces[fluorite.current_workspace].frames_count == MAX_WINDOWS)
		return ;

	if (fluorite.workspaces[fluorite.current_workspace].is_fullscreen)
		fluorite_change_layout(FULLSCREEN_TOGGLE);

	fluorite_handle_normals(e.window);
	fluorite_redraw_windows();
}

void fluorite_handle_keys(XKeyPressedEvent e)
{
	for (long unsigned int i = 0; i < LENGTH(bind); i++)
		if (e.keycode == XKeysymToKeycode(fluorite.display, bind[i].key) && MODMASK(bind[i].mod) == MODMASK(e.state) && bind[i].func)
				bind[i].func();
}

void fluorite_handle_enternotify(XEvent e)
{
	if (fluorite.workspaces[fluorite.current_workspace].is_fullscreen || fluorite.workspaces[fluorite.current_workspace].is_organising)
		return ;
	if (fluorite.workspaces[fluorite.current_workspace].slaves_count > 0)
	{
		if (e.xcrossing.window == fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0]->frame)
		{
			fluorite.workspaces[fluorite.current_workspace].current_focus = SLAVE_FOCUS;
			XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0]->frame, BORDER_FOCUSED);
			XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, BORDER_UNFOCUSED);
			XSetInputFocus(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0]->window, RevertToPointerRoot, CurrentTime);
			XChangeProperty(fluorite.display, fluorite.root, XInternAtom(fluorite.display, "_NET_ACTIVE_WINDOW", False), XA_WINDOW, 32, PropModeReplace, (const unsigned char *) &fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0]->window, 1);
		}
		else if (e.xcrossing.window == fluorite.workspaces[fluorite.current_workspace].master_winframe->frame)
		{
			fluorite.workspaces[fluorite.current_workspace].current_focus = MASTER_FOCUS;
			XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, BORDER_FOCUSED);
			XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0]->frame, BORDER_UNFOCUSED);
			XSetInputFocus(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->window, RevertToPointerRoot, CurrentTime);
			XChangeProperty(fluorite.display, fluorite.root, XInternAtom(fluorite.display, "_NET_ACTIVE_WINDOW", False), XA_WINDOW, 32, PropModeReplace, (const unsigned char *) &fluorite.workspaces[fluorite.current_workspace].master_winframe->window, 1);
		}
	}
	else
	{
		if (fluorite.workspaces[fluorite.current_workspace].frames_count > 0)
		{
			fluorite.workspaces[fluorite.current_workspace].current_focus = MASTER_FOCUS;
			XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, BORDER_FOCUSED);
			XSetInputFocus(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->window, RevertToPointerRoot, CurrentTime);
			XChangeProperty(fluorite.display, fluorite.root, XInternAtom(fluorite.display, "_NET_ACTIVE_WINDOW", False), XA_WINDOW, 32, PropModeReplace, (const unsigned char *) &fluorite.workspaces[fluorite.current_workspace].master_winframe->window, 1);
		}
		else
		{
			fluorite.workspaces[fluorite.current_workspace].current_focus = NO_FOCUS;
			XSetInputFocus(fluorite.display, fluorite.root, RevertToPointerRoot, CurrentTime);
			XDeleteProperty(fluorite.display, fluorite.root, XInternAtom(fluorite.display, "_NET_ACTIVE_WINDOW", False));
		}
	}
	for (int i = 0; i < fluorite.workspaces[fluorite.current_workspace].floating_count; i++)
		XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->window, BORDER_UNFOCUSED);
}

void fluorite_get_pointer_coord(int *x, int *y)
{
	int di;
	unsigned int dui;
	Window dummy;

	XQueryPointer(fluorite.display, fluorite.root, &dummy, &dummy, x, y, &di, &di, &dui);
}

void fluorite_handle_buttonpress(XButtonEvent e)
{
	int is_float = False;
	unsigned b_w, d;

	for (int i = 0; i < fluorite.workspaces[fluorite.current_workspace].floating_count; i++)
		if (e.window == fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->window)
			is_float = True;
	if (!is_float)
		return ;

	fluorite.mouse.start_pos_x = e.x_root;
	fluorite.mouse.start_pos_y = e.y_root;
	XGetGeometry(fluorite.display, e.window, &fluorite.root, &fluorite.mouse.start_frame_x, &fluorite.mouse.start_frame_y, &fluorite.mouse.start_frame_w, &fluorite.mouse.start_frame_h, &b_w, &d);
	XSetInputFocus(fluorite.display, e.window, RevertToPointerRoot, CurrentTime);
	if (fluorite.workspaces[fluorite.current_workspace].frames_count > 0)
		XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, BORDER_UNFOCUSED);
	if (fluorite.workspaces[fluorite.current_workspace].slaves_count > 0)
		XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0]->frame, BORDER_UNFOCUSED);
	for (int i = 0; i < fluorite.workspaces[fluorite.current_workspace].floating_count; i++)
		XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->window, BORDER_UNFOCUSED);
	XSetWindowBorder(fluorite.display, e.window, BORDER_FOCUSED);
	XRaiseWindow(fluorite.display, e.window);
	fluorite.workspaces[fluorite.current_workspace].current_focus = FLOAT_FOCUS;
	XChangeProperty(fluorite.display, fluorite.root, XInternAtom(fluorite.display, "_NET_ACTIVE_WINDOW", False), XA_WINDOW, 32, PropModeReplace, (const unsigned char *) &e.window, 1);
}

void fluorite_handle_motions(XMotionEvent e)
{
	int delta_pos_x, delta_pos_y;
	int drag_dest_x, drag_dest_y;
	long lhints;
	XSizeHints hints;
	xdo_t *xdo;
	int xdo_mouse_x;
	int xdo_mouse_y;
	int max_x;
	int max_y;
	int pos_x;
	int pos_y;
	int window_max_x;
	int window_max_y;
	int window_pos_x;
	int window_pos_y;


	if (XGetWMNormalHints(fluorite.display, e.window, &hints, &lhints))
	{
		delta_pos_x = e.x_root - fluorite.mouse.start_pos_x;
		delta_pos_y = e.y_root - fluorite.mouse.start_pos_y;
		if (e.state & Button1Mask)
		{
			drag_dest_x = fluorite.mouse.start_frame_x + delta_pos_x;
			drag_dest_y = fluorite.mouse.start_frame_y + delta_pos_y;
			hints.x = drag_dest_x;
			hints.y = drag_dest_y;
			for (int i = 0; i < fluorite.workspaces[fluorite.current_workspace].floating_count; i++)
			{
				if (e.window == fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->window)
				{
					max_x = fluorite.monitor[fluorite.current_monitor].pos_x + fluorite.monitor[fluorite.current_monitor].width;
					max_y = fluorite.monitor[fluorite.current_monitor].pos_y + fluorite.monitor[fluorite.current_monitor].height;
					pos_x = fluorite.monitor[fluorite.current_monitor].pos_x;
					pos_y = fluorite.monitor[fluorite.current_monitor].pos_y;
					window_max_x = fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->width;
					window_max_y = fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->height;
					window_pos_x = fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->pos_x;

					if (window_pos_x + drag_dest_x < pos_x)
						drag_dest_x = pos_x;
					if (window_max_x + drag_dest_x > max_x)
						drag_dest_x = max_x - window_max_x;
					if (drag_dest_y < pos_y)
						drag_dest_y = pos_y;
					if (window_max_y + drag_dest_y > max_y)
						drag_dest_y = max_y - window_max_y;
					XMoveWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->window, drag_dest_x, drag_dest_y);
					fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->pos_x = drag_dest_x;
					fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->pos_y = drag_dest_y;
				}
			}
		}
		else if (e.state & Button3Mask)
		{
			drag_dest_x = fluorite.mouse.start_frame_w + delta_pos_x;
			drag_dest_y = fluorite.mouse.start_frame_h + delta_pos_y;
			hints.width = drag_dest_x;
			hints.height = drag_dest_y;
			for (int i = 0; i < fluorite.workspaces[fluorite.current_workspace].floating_count; i++)
			{
				if (e.window == fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->window)
				{
					max_x = fluorite.monitor[fluorite.current_monitor].pos_x + fluorite.monitor[fluorite.current_monitor].width;
					max_y = fluorite.monitor[fluorite.current_monitor].pos_y + fluorite.monitor[fluorite.current_monitor].height;
					pos_x = fluorite.monitor[fluorite.current_monitor].pos_x;
					pos_y = fluorite.monitor[fluorite.current_monitor].pos_y;
					window_max_x = fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->width;
					window_max_y = fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->height;
					window_pos_x = fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->pos_x;
					window_pos_y = fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->pos_y;

					if (drag_dest_x + window_pos_x > max_x)
						drag_dest_x = window_max_x;
					if (drag_dest_y + window_pos_y > max_y)
						drag_dest_y = window_max_y;
					XResizeWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->window, drag_dest_x, drag_dest_y);
					fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->width = drag_dest_x;
					fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->height = drag_dest_y;
				}
			}
		}
		XRaiseWindow(fluorite.display, e.window);
		XSetInputFocus(fluorite.display, e.window, RevertToPointerRoot, CurrentTime);
	}
	xdo = xdo_new(NULL);
	xdo_get_mouse_location(xdo, &xdo_mouse_x, &xdo_mouse_y, NULL);
	xdo_free(xdo);

	for (int i = 0; i < fluorite.monitor_count; i++)
	{
		max_x = fluorite.monitor[i].pos_x + fluorite.monitor[i].width;
		max_y = fluorite.monitor[i].pos_y + fluorite.monitor[i].height;
		pos_x = fluorite.monitor[i].pos_x;
		pos_y = fluorite.monitor[i].pos_y;
		if ((xdo_mouse_x >= pos_x && xdo_mouse_x <= max_x) && (xdo_mouse_y >= pos_y && xdo_mouse_y <= max_y) && fluorite.current_monitor != i)
			fluorite_change_monitor(i);
	}
}

int fluorite_handle_errors(Display *display, XErrorEvent *e)
{
	int fd = open("/tmp/fluorite.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
	char error[1024];

	XGetErrorText(display, e->error_code, error, sizeof(error));
	dprintf(fd, "Err Code : %d\nRequest : %d\n%s\n", e->error_code, e->request_code, error);
	return e->error_code;
}

WinFrames *fluorite_create_winframe()
{
	WinFrames *new_frame;
	XVisualInfo vinfo;
	XSetWindowAttributes attribs_set;
	XMatchVisualInfo(fluorite.display, fluorite.screen, 32, TrueColor, &vinfo);
	attribs_set.background_pixel = 0;
	attribs_set.border_pixel = BORDER_FOCUSED;
	attribs_set.colormap = XCreateColormap(fluorite.display, fluorite.root, vinfo.visual, AllocNone);
	attribs_set.event_mask = StructureNotifyMask | ExposureMask;

	new_frame = malloc(sizeof(WinFrames));
	new_frame->frame = XCreateWindow(fluorite.display, fluorite.root, 
			0, 
			0,
			fluorite.screen_width - (BORDER_WIDTH * 2),
			fluorite.screen_height - (BORDER_WIDTH * 2),
			BORDER_WIDTH,
			vinfo.depth,
			InputOutput,
			vinfo.visual,
			CWBackPixel | CWBorderPixel | CWColormap | CWEventMask,
			&attribs_set
	);
	XCompositeRedirectWindow(fluorite.display, new_frame->frame, CompositeRedirectAutomatic);

	return new_frame;
}

void fluorite_organise_stack(int mode, int offset)
{
	if (fluorite.workspaces[fluorite.current_workspace].slaves_count == 0)
		return ;
	switch (mode)
	{
		case STACK_NEW:
			for (int i = fluorite.workspaces[fluorite.current_workspace].slaves_count; i >= 1; i--)
				memcpy(fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i], fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i - 1], sizeof(WinFrames));
			memcpy(fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0], fluorite.workspaces[fluorite.current_workspace].tmp_winframe, sizeof(WinFrames));
			break;
		case STACK_DEL:
			for (int i = offset + 1; i <= fluorite.workspaces[fluorite.current_workspace].slaves_count; i++)
				memcpy(fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i - 1], fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i], sizeof(WinFrames));
			break;
		case STACK_UP:
			memcpy(fluorite.workspaces[fluorite.current_workspace].tmp_winframe, fluorite.workspaces[fluorite.current_workspace].master_winframe, sizeof(WinFrames));
			memcpy(fluorite.workspaces[fluorite.current_workspace].master_winframe, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0], sizeof(WinFrames));
			fluorite_organise_stack(STACK_DEL, 0);
			memcpy(fluorite.workspaces[fluorite.current_workspace].slaves_winframes[fluorite.workspaces[fluorite.current_workspace].slaves_count - 1], fluorite.workspaces[fluorite.current_workspace].tmp_winframe, sizeof(WinFrames));
			break;
		case STACK_DOWN:
			memcpy(fluorite.workspaces[fluorite.current_workspace].tmp_winframe, fluorite.workspaces[fluorite.current_workspace].master_winframe, sizeof(WinFrames));
			memcpy(fluorite.workspaces[fluorite.current_workspace].master_winframe, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[fluorite.workspaces[fluorite.current_workspace].slaves_count - 1], sizeof(WinFrames));
			fluorite_organise_stack(STACK_NEW, -1);
			memcpy(fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0], fluorite.workspaces[fluorite.current_workspace].tmp_winframe, sizeof(WinFrames));
			break;
		case SLAVES_UP:
			memcpy(fluorite.workspaces[fluorite.current_workspace].tmp_winframe, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0], sizeof(WinFrames));
			for (int i = 0; i < fluorite.workspaces[fluorite.current_workspace].slaves_count; i++)
				memcpy(fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i], fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i + 1], sizeof(WinFrames));
			memcpy(fluorite.workspaces[fluorite.current_workspace].slaves_winframes[fluorite.workspaces[fluorite.current_workspace].slaves_count - 1], fluorite.workspaces[fluorite.current_workspace].tmp_winframe, sizeof(WinFrames));
			break;
		case SLAVES_DOWN:
			memcpy(fluorite.workspaces[fluorite.current_workspace].tmp_winframe, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[fluorite.workspaces[fluorite.current_workspace].slaves_count - 1], sizeof(WinFrames));
			fluorite_organise_stack(STACK_NEW, -1);
			break;
	}
}

void fluorite_redraw_stacking()
{
	fluorite.workspaces[fluorite.current_workspace].master_winframe->pos_x = fluorite.monitor[fluorite.current_monitor].pos_x + (GAPS * 2);
	if (fluorite.monitor[fluorite.current_monitor].primary == True)
		fluorite.workspaces[fluorite.current_workspace].master_winframe->pos_y = fluorite.monitor[fluorite.current_monitor].pos_y + (GAPS * 2) + TOPBAR_GAPS;
	else
		fluorite.workspaces[fluorite.current_workspace].master_winframe->pos_y = fluorite.monitor[fluorite.current_monitor].pos_y + (GAPS * 2);
	if (fluorite.monitor[fluorite.current_monitor].primary == True)
		fluorite.workspaces[fluorite.current_workspace].master_winframe->height = fluorite.monitor[fluorite.current_monitor].height - (BORDER_WIDTH * 2) - (GAPS * 4) - TOPBAR_GAPS - BOTTOMBAR_GAPS;
	else
		fluorite.workspaces[fluorite.current_workspace].master_winframe->height = fluorite.monitor[fluorite.current_monitor].height - (BORDER_WIDTH * 2) - (GAPS * 4);
	fluorite.workspaces[fluorite.current_workspace].master_winframe->width =  fluorite.monitor[fluorite.current_monitor].width - (BORDER_WIDTH * 2) - (GAPS * 4);
	XSetWindowBorderWidth(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, BORDER_WIDTH);
	XMoveResizeWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, fluorite.workspaces[fluorite.current_workspace].master_winframe->pos_x, fluorite.workspaces[fluorite.current_workspace].master_winframe->pos_y, fluorite.workspaces[fluorite.current_workspace].master_winframe->width, fluorite.workspaces[fluorite.current_workspace].master_winframe->height);
	XMoveResizeWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->window, 0, 0, fluorite.workspaces[fluorite.current_workspace].master_winframe->width, fluorite.workspaces[fluorite.current_workspace].master_winframe->height);
	if (fluorite.workspaces[fluorite.current_workspace].slaves_count > 0)
	{
		for (int i = fluorite.workspaces[fluorite.current_workspace].slaves_count - 1; i >= 0; i--)
		{
			fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->pos_x = fluorite.monitor[fluorite.current_monitor].pos_x + (GAPS * 2);
			if (fluorite.monitor[fluorite.current_monitor].primary == True)
				fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->pos_y = fluorite.monitor[fluorite.current_monitor].pos_y + (GAPS * 2) + TOPBAR_GAPS;
			else
				fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->pos_y = fluorite.monitor[fluorite.current_monitor].pos_y + (GAPS * 2);
			if (fluorite.monitor[fluorite.current_monitor].primary == True)
				fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->height = fluorite.monitor[fluorite.current_monitor].height - (BORDER_WIDTH * 2) - (GAPS * 4) - TOPBAR_GAPS - BOTTOMBAR_GAPS;
			else
				fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->height = fluorite.monitor[fluorite.current_monitor].height - (BORDER_WIDTH * 2) - (GAPS * 4);
			fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->width =  fluorite.monitor[fluorite.current_monitor].width - (BORDER_WIDTH * 2) - (GAPS * 4);
			XSetWindowBorderWidth(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->frame, 0);
			XMoveResizeWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->frame, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->pos_x, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->pos_y, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->width, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->height);
			XMoveResizeWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->window, 0, 0, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->width, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->height);
			XRaiseWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->frame);
		}
	}
}

void fluorite_redraw_tiling()
{
	if (fluorite.workspaces[fluorite.current_workspace].slaves_count > 0)
	{
		int position_offset;
		int size_offset;

		fluorite.workspaces[fluorite.current_workspace].master_winframe->pos_x = fluorite.monitor[fluorite.current_monitor].pos_x + (GAPS * 2);
		if (fluorite.monitor[fluorite.current_monitor].primary == True)
			fluorite.workspaces[fluorite.current_workspace].master_winframe->pos_y = fluorite.monitor[fluorite.current_monitor].pos_y + (GAPS * 2) + TOPBAR_GAPS;
		else
			fluorite.workspaces[fluorite.current_workspace].master_winframe->pos_y = fluorite.monitor[fluorite.current_monitor].pos_y + (GAPS * 2);
		fluorite.workspaces[fluorite.current_workspace].master_winframe->width = fluorite.monitor[fluorite.current_monitor].width / 2 - (BORDER_WIDTH * 2) - (GAPS * 3) + fluorite.workspaces[fluorite.current_workspace].master_offset;
		if (fluorite.monitor[fluorite.current_monitor].primary == True)
			fluorite.workspaces[fluorite.current_workspace].master_winframe->height = fluorite.monitor[fluorite.current_monitor].height - (BORDER_WIDTH * 2) - (GAPS * 4) - TOPBAR_GAPS - BOTTOMBAR_GAPS;
		else
			fluorite.workspaces[fluorite.current_workspace].master_winframe->height = fluorite.monitor[fluorite.current_monitor].height - (BORDER_WIDTH * 2) - (GAPS * 4);
		XSetWindowBorderWidth(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, BORDER_WIDTH);
		XMoveResizeWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, fluorite.workspaces[fluorite.current_workspace].master_winframe->pos_x, fluorite.workspaces[fluorite.current_workspace].master_winframe->pos_y, fluorite.workspaces[fluorite.current_workspace].master_winframe->width, fluorite.workspaces[fluorite.current_workspace].master_winframe->height);
		XMoveResizeWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->window, 0, 0, fluorite.workspaces[fluorite.current_workspace].master_winframe->width, fluorite.workspaces[fluorite.current_workspace].master_winframe->height);

		position_offset = 0;
		size_offset = STACK_OFFSET * (fluorite.workspaces[fluorite.current_workspace].slaves_count - 1) * 10;
		for (int i = fluorite.workspaces[fluorite.current_workspace].slaves_count - 1; i >= 0; i--)
		{
			fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->pos_x = fluorite.monitor[fluorite.current_monitor].width / 2 + (GAPS) + (position_offset / fluorite.workspaces[fluorite.current_workspace].slaves_count) + fluorite.workspaces[fluorite.current_workspace].master_offset;
			if (fluorite.monitor[fluorite.current_monitor].primary == True)
				fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->pos_y = fluorite.monitor[fluorite.current_monitor].pos_y + (GAPS * 2) + (position_offset / fluorite.workspaces[fluorite.current_workspace].slaves_count) + TOPBAR_GAPS;
			else
				fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->pos_y = fluorite.monitor[fluorite.current_monitor].pos_y + (GAPS * 2) + (position_offset / fluorite.workspaces[fluorite.current_workspace].slaves_count);
			fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->width =  fluorite.monitor[fluorite.current_monitor].width / 2 - (BORDER_WIDTH * 2) - (GAPS * 3) - (size_offset / fluorite.workspaces[fluorite.current_workspace].slaves_count) - fluorite.workspaces[fluorite.current_workspace].master_offset;
			if (fluorite.monitor[fluorite.current_monitor].primary == True)
				fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->height = fluorite.monitor[fluorite.current_monitor].height - (BORDER_WIDTH * 2) - (GAPS * 4) - (size_offset / fluorite.workspaces[fluorite.current_workspace].slaves_count) - TOPBAR_GAPS - BOTTOMBAR_GAPS;
			else
				fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->height = fluorite.monitor[fluorite.current_monitor].height - (BORDER_WIDTH * 2) - (GAPS * 4) - (size_offset / fluorite.workspaces[fluorite.current_workspace].slaves_count);
			XSetWindowBorderWidth(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->frame, BORDER_WIDTH);
			XMoveResizeWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->frame, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->pos_x, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->pos_y, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->width, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->height);
			XMoveResizeWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->window, 0, 0, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->width, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->height);
			XRaiseWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->frame);
			position_offset += STACK_OFFSET * 10;
			if (i == 0)
				XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->frame, BORDER_UNFOCUSED);
			else
				XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[i]->frame, BORDER_INACTIVE);
		}
	}
}

void fluorite_redraw_windows()
{
	if (fluorite.workspaces[fluorite.current_workspace].frames_count == 0 || fluorite.workspaces[fluorite.current_workspace].is_fullscreen)
		return;

	if (fluorite.workspaces[fluorite.current_workspace].is_stacked || fluorite.workspaces[fluorite.current_workspace].frames_count == 1)
		fluorite_redraw_stacking();
	else
		fluorite_redraw_tiling();
	XRaiseWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame);
	XSetInputFocus(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->window, RevertToPointerRoot, CurrentTime);
	XSetWindowBorder(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->frame, BORDER_FOCUSED);
	fluorite.workspaces[fluorite.current_workspace].current_focus = MASTER_FOCUS;
	for (int i = 0; i < fluorite.workspaces[fluorite.current_workspace].floating_count; i++)
		XRaiseWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->window);
}

void fluorite_handle_unmapping(Window e)
{
	int keep_workspace = fluorite.current_workspace;
	int closed = False;

	for (int i = 0; i < MAX_WORKSPACES; i++)
	{
		if (fluorite.workspaces[i].floating_count == 0)
			continue;
		fluorite.current_workspace = i;
		for (int i = 0; i < fluorite.workspaces[fluorite.current_workspace].floating_count; i++)
			if (fluorite.workspaces[fluorite.current_workspace].floating_windows[i]->window == e)
			{
				for (int j = i + 1; j < fluorite.workspaces[fluorite.current_workspace].floating_count; j++)
					memcpy(fluorite.workspaces[fluorite.current_workspace].floating_windows[j - 1], fluorite.workspaces[fluorite.current_workspace].floating_windows[j], sizeof(Floating));
				fluorite.workspaces[fluorite.current_workspace].floating_count--;
				XSetInputFocus(fluorite.display, fluorite.root, RevertToPointerRoot, CurrentTime);
				fluorite.current_workspace = keep_workspace;
				return ;
			}
	}

	for (int i = 0; i < MAX_WORKSPACES; i++)
	{
		if (fluorite.workspaces[i].frames_count <= 0)
			continue;
		fluorite.current_workspace = i;
		if (fluorite.workspaces[fluorite.current_workspace].master_winframe->window == e)
		{
			if (fluorite.workspaces[fluorite.current_workspace].is_fullscreen && fluorite.workspaces[fluorite.current_workspace].current_focus == MASTER_FOCUS)
			{
				XReparentWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->fullscreen_frame, fluorite.root, 0, 0);
				fluorite.workspaces[fluorite.current_workspace].is_fullscreen = 0;
			}
			else
			{
				XReparentWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].master_winframe->window, fluorite.root, 0, 0);
				for (int j = 0; j < fluorite.monitor_count; j++)
					if (fluorite.monitor[j].workspace == i)
						XUnmapWindow(fluorite.display, fluorite.workspaces[fluorite.monitor[j].workspace].master_winframe->frame);
			}
			fluorite.workspaces[fluorite.current_workspace].frames_count--;
			if (fluorite.workspaces[fluorite.current_workspace].slaves_count > 0)
			{
				memcpy(fluorite.workspaces[fluorite.current_workspace].master_winframe, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0], sizeof(WinFrames));
				fluorite.workspaces[fluorite.current_workspace].slaves_count--;
				fluorite_organise_stack(STACK_DEL, 0);
				for (int j = 0; j < fluorite.monitor_count; j++)
				{
					int keep_monitor;
					if (fluorite.monitor[j].workspace == i)
					{
						keep_monitor = fluorite.current_monitor;
						fluorite.current_workspace = i;
						fluorite.current_monitor = j;
						fluorite_redraw_windows();
						fluorite.current_monitor = keep_monitor;
						fluorite.workspaces[fluorite.current_workspace].current_focus = MASTER_FOCUS;
					}
				}
				break;
			}
			closed = True;
		}
		else
		{
			if (fluorite.workspaces[i].slaves_count <= 0)
				continue;
			int stack_offset = 0;

			while (stack_offset < fluorite.workspaces[fluorite.current_workspace].slaves_count)
			{
				if (fluorite.workspaces[fluorite.current_workspace].slaves_winframes[stack_offset]->window == e)
				{
					if (fluorite.workspaces[fluorite.current_workspace].is_fullscreen && stack_offset == 0 && fluorite.workspaces[fluorite.current_workspace].current_focus == SLAVE_FOCUS)
					{
						XReparentWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[0]->fullscreen_frame, fluorite.root, 0, 0);
						fluorite.workspaces[fluorite.current_workspace].is_fullscreen = 0;
					}
					else
					{
						XReparentWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[stack_offset]->window, fluorite.root, 0, 0);
						if (keep_workspace == i)
							XUnmapWindow(fluorite.display, fluorite.workspaces[fluorite.current_workspace].slaves_winframes[stack_offset]->frame);
					}
					fluorite.workspaces[fluorite.current_workspace].frames_count--;
					fluorite.workspaces[fluorite.current_workspace].slaves_count--;
					closed = True;
					fluorite_organise_stack(STACK_DEL, stack_offset);
					if (keep_workspace == i && closed)
					{
						if (!fluorite.workspaces[fluorite.current_workspace].is_fullscreen)
							fluorite.workspaces[fluorite.current_workspace].current_focus = MASTER_FOCUS;
						fluorite_redraw_windows();
					}
					break;
				}
				stack_offset++;
			}
		}
	}
	fluorite.current_workspace = keep_workspace;
	if (fluorite.workspaces[fluorite.current_workspace].frames_count == 0)
		XDeleteProperty(fluorite.display, fluorite.root, XInternAtom(fluorite.display, "_NET_ACTIVE_WINDOW", False));
}

int main(void)
{
	fluorite_init();
	fluorite_run();
	fluorite_clean();
	return 0;
}
