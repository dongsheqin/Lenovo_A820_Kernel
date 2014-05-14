/*
    SDL - Simple DirectMedia Layer
    Copyright (C) 1997-2006 Sam Lantinga

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

    Sam Lantinga
    slouken@libsdl.org
*/
#include "SDL_config.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Make sure XBUTTON stuff is defined that isn't in older Platform SDKs... */
#ifndef WM_XBUTTONDOWN
#define WM_XBUTTONDOWN 0x020B
#endif
#ifndef WM_XBUTTONUP
#define WM_XBUTTONUP 0x020C
#endif
#ifndef GET_XBUTTON_WPARAM
#define GET_XBUTTON_WPARAM(w) (HIWORD(w))
#endif

#include "SDL_events.h"
#include "SDL_video.h"
#include "SDL_syswm.h"
#include "../SDL_sysvideo.h"
#include "../../events/SDL_sysevents.h"
#include "../../events/SDL_events_c.h"
#include "SDL_lowvideo.h"
#include "SDL_syswm_c.h"
#include "SDL_main.h"
#include "SDL_loadso.h"

#ifdef WMMSG_DEBUG
#include "wmmsg.h"
#endif

#ifdef _WIN32_WCE
#include "../gapi/SDL_gapivideo.h"

#define IsZoomed(HWND) 1
#define NO_GETKEYBOARDSTATE
#if _WIN32_WCE < 420
#define NO_CHANGEDISPLAYSETTINGS
#endif
#endif

/* The window we use for everything... */
#ifdef _WIN32_WCE
LPWSTR SDL_Appname = NULL;
#else
LPSTR SDL_Appname = NULL;
#endif
Uint32 SDL_Appstyle = 0;
HINSTANCE SDL_Instance = NULL;
HWND SDL_Window = NULL;
RECT SDL_bounds = {0, 0, 0, 0};
int SDL_windowX = 0;
int SDL_windowY = 0;
int SDL_resizing = 0;
int mouse_relative = 0;
int posted = 0;
#ifndef NO_CHANGEDISPLAYSETTINGS
DEVMODE SDL_desktop_mode;
DEVMODE SDL_fullscreen_mode;
#endif
WORD *gamma_saved = NULL;


/* Functions called by the message processing function */
LONG (*HandleMessage)(_THIS, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)=NULL;
void (*WIN_Activate)(_THIS, BOOL active, BOOL iconic);
void (*WIN_RealizePalette)(_THIS);
void (*WIN_PaletteChanged)(_THIS, HWND window);
void (*WIN_WinPAINT)(_THIS, HDC hdc);
extern void DIB_SwapGamma(_THIS);

#ifndef NO_GETKEYBOARDSTATE
/* Variables and support functions for SDL_ToUnicode() */
static int codepage;
static int Is9xME();
static int GetCodePage();
static int WINAPI ToUnicode9xME(UINT vkey, UINT scancode, BYTE *keystate, LPWSTR wchars, int wsize, UINT flags);

ToUnicodeFN SDL_ToUnicode = ToUnicode9xME;
#endif /* !NO_GETKEYBOARDSTATE */


#if defined(_WIN32_WCE)

// dynamically load aygshell dll because we want SDL to work on HPC and be300
HINSTANCE aygshell = NULL;
BOOL (WINAPI *SHFullScreen)(HWND hwndRequester, DWORD dwState) = 0;

#define SHFS_SHOWTASKBAR            0x0001
#define SHFS_HIDETASKBAR            0x0002
#define SHFS_SHOWSIPBUTTON          0x0004
#define SHFS_HIDESIPBUTTON          0x0008
#define SHFS_SHOWSTARTICON          0x0010
#define SHFS_HIDESTARTICON          0x0020

static void LoadAygshell(void)
{
	if( !aygshell )
		 aygshell = SDL_LoadObject("aygshell.dll");
	if( (aygshell != 0) && (SHFullScreen == 0) )
	{
		SHFullScreen = (int (WINAPI *)(struct HWND__ *,unsigned long)) SDL_LoadFunction(aygshell, "SHFullScreen");
	}
}

/* for gapi landscape mode */
static void GapiTransform(SDL_ScreenOrientation rotate, char hires, Sint16 *x, Sint16 *y) {
	Sint16 rotatedX;
	Sint16 rotatedY;

	if (hires) {
		*x = *x * 2;
		*y = *y * 2;
	}

	switch(rotate) {
		case SDL_ORIENTATION_UP:
			{
/* this code needs testing on a real device!
   So it will be enabled later */
/*
#ifdef _WIN32_WCE
#if _WIN32_WCE >= 420
				// test device orientation
				// FIXME: do not check every mouse message
				DEVMODE settings;
				SDL_memset(&settings, 0, sizeof(DEVMODE));
				settings.dmSize = sizeof(DEVMODE);
				settings.dmFields = DM_DISPLAYORIENTATION;
				ChangeDisplaySettingsEx(NULL, &settings, NULL, CDS_TEST, NULL);
				if( settings.dmOrientation == DMDO_90 )
				{
					rotatedX = SDL_VideoSurface->h - *x;
					rotatedY = *y;
					*x = rotatedX;
					*y = rotatedY;
				}
#endif
#endif */
			}
			break;
		case SDL_ORIENTATION_RIGHT:
			if (!SDL_VideoSurface)
				break;
			rotatedX = SDL_VideoSurface->w - *y;
			rotatedY = *x;
			*x = rotatedX;
			*y = rotatedY;
			break;
		case SDL_ORIENTATION_LEFT:
			if (!SDL_VideoSurface)
				break;
			rotatedX = *y;
			rotatedY = SDL_VideoSurface->h - *x;
			*x = rotatedX;
			*y = rotatedY;
			break;
	}
}

#endif

/* JC 14 Mar 2006
   This is used all over the place, in the windib driver and in the dx5 driver
   So we may as well stick it here instead of having multiple copies scattered
   about
*/
void WIN_FlushMessageQueue()
{
	MSG  msg;
	while ( PeekMessage(&msg, NULL, 0, 0, PM_REMOVE) ) {
		if ( msg.message == WM_QUIT ) break;
		TranslateMessage( &msg );
		DispatchMessage( &msg );
	}
}

static void SDL_RestoreGameMode(void)
{
#ifdef _WIN32_WCE
	SDL_VideoDevice *this = current_video;
	if(SDL_strcmp(this->name, "gapi") == 0)
	{
		if( this->hidden->suspended )
		{
			this->hidden->suspended = 0;
		}
	}
#else
	ShowWindow(SDL_Window, SW_RESTORE);
#endif

#ifndef NO_CHANGEDISPLAYSETTINGS
#ifndef _WIN32_WCE
	ChangeDisplaySettings(&SDL_fullscreen_mode, CDS_FULLSCREEN);
#endif
#endif /* NO_CHANGEDISPLAYSETTINGS */
}
static void SDL_RestoreDesktopMode(void)
{

#ifdef _WIN32_WCE
	SDL_VideoDevice *this = current_video;
	if(SDL_strcmp(this->name, "gapi") == 0)
	{
		if( !this->hidden->suspended )
		{
			this->hidden->suspended = 1;
		}
	}
#else
	/* WinCE does not have a taskbar, so minimizing is not convenient */
	ShowWindow(SDL_Window, SW_MINIMIZE);
#endif

#ifndef NO_CHANGEDISPLAYSETTINGS
#ifndef _WIN32_WCE
	ChangeDisplaySettings(NULL, 0);
#endif
#endif /* NO_CHANGEDISPLAYSETTINGS */
}

#ifdef WM_MOUSELEAVE
/* 
   Special code to handle mouse leave events - this sucks...
   http://support.microsoft.com/support/kb/articles/q183/1/07.asp

   TrackMouseEvent() is only available on Win98 and WinNT.
   _TrackMouseEvent() is available on Win95, but isn't yet in the mingw32
   development environment, and only works on systems that have had IE 3.0
   or newer installed on them (which is not the case with the base Win95).
   Therefore, we implement our own version of _TrackMouseEvent() which
   uses our own implementation if TrackMouseEvent() is not available.
*/
static BOOL (WINAPI *_TrackMouseEvent)(TRACKMOUSEEVENT *ptme) = NULL;

static VOID CALLBACK
TrackMouseTimerProc(HWND hWnd, UINT uMsg, UINT idEvent, DWORD dwTime)
{
	RECT rect;
	POINT pt;

	GetClientRect(hWnd, &rect);
	MapWindowPoints(hWnd, NULL, (LPPOINT)&rect, 2);
	GetCursorPos(&pt);
	if ( !PtInRect(&rect, pt) || (WindowFromPoint(pt) != hWnd) ) {
		if ( !KillTimer(hWnd, idEvent) ) {
			/* Error killing the timer! */
		}
		PostMessage(hWnd, WM_MOUSELEAVE, 0, 0);
	}
}
static BOOL WINAPI WIN_TrackMouseEvent(TRACKMOUSEEVENT *ptme)
{
	if ( ptme->dwFlags == TME_LEAVE ) {
		return SetTimer(ptme->hwndTrack, ptme->dwFlags, 100,
		                (TIMERPROC)TrackMouseTimerProc) != 0;
	}
	return FALSE;
}
#endif /* WM_MOUSELEAVE */

/* Function to retrieve the current keyboard modifiers */
static void WIN_GetKeyboardState(void)
{
#ifndef NO_GETKEYBOARDSTATE
	SDLMod state;
	BYTE keyboard[256];
	Uint8 *kstate = SDL_GetKeyState(NULL);

	state = KMOD_NONE;
	if ( GetKeyboardState(keyboard) ) {
		if ( keyboard[VK_LSHIFT] & 0x80) {
			state |= KMOD_LSHIFT;
			kstate[SDLK_LSHIFT] = SDL_PRESSED;
		}
		if ( keyboard[VK_RSHIFT] & 0x80) {
			state |= KMOD_RSHIFT;
			kstate[SDLK_RSHIFT] = SDL_PRESSED;
		}
		if ( keyboard[VK_LCONTROL] & 0x80) {
			state |= KMOD_LCTRL;
			kstate[SDLK_LCTRL] = SDL_PRESSED;
		}
		if ( keyboard[VK_RCONTROL] & 0x80) {
			state |= KMOD_RCTRL;
			kstate[SDLK_RCTRL] = SDL_PRESSED;
		}
		if ( keyboard[VK_LMENU] & 0x80) {
			state |= KMOD_LALT;
			kstate[SDLK_LALT] = SDL_PRESSED;
		}
		if ( keyboard[VK_RMENU] & 0x80) {
			state |= KMOD_RALT;
			kstate[SDLK_RALT] = SDL_PRESSED;
		}
		if ( keyboard[VK_NUMLOCK] & 0x01) {
			state |= KMOD_NUM;
			kstate[SDLK_NUMLOCK] = SDL_PRESSED;
		}
		if ( keyboard[VK_CAPITAL] & 0x01) {
			state |= KMOD_CAPS;
			kstate[SDLK_CAPSLOCK] = SDL_PRESSED;
		}
	}
	SDL_SetModState(state);
#endif /* !NO_GETKEYBOARDSTATE */
}

/* The main Win32 event handler
DJM: This is no longer static as (DX5/DIB)_CreateWindow needs it
*/
LRESULT CALLBACK WinMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	SDL_VideoDevice *this = current_video;
	static int mouse_pressed = 0;
	static int in_window = 0;
#ifdef WMMSG_DEBUG
	fprintf(stderr, "Received windows message:  ");
	if ( msg > MAX_WMMSG ) {
		fprintf(stderr, "%d", msg);
	} else {
		fprintf(stderr, "%s", wmtab[msg]);
	}
	fprintf(stderr, " -- 0x%X, 0x%X\n", wParam, lParam);
#endif
	switch (msg) {

		case WM_ACTIVATE: {
			SDL_VideoDevice *this = current_video;
			BOOL active, minimized;
			Uint8 appstate;

			minimized = HIWORD(wParam);
			active = (LOWORD(wParam) != WA_INACTIVE) && !minimized;
			if ( active ) {
				/* Gain the following states */
				appstate = SDL_APPACTIVE|SDL_APPINPUTFOCUS;
				if ( this->input_grab != SDL_GRAB_OFF ) {
					WIN_GrabInput(this, SDL_GRAB_ON);
				}
				if ( !(SDL_GetAppState()&SDL_APPINPUTFOCUS) ) {
					if ( ! DDRAW_FULLSCREEN() ) {
						DIB_SwapGamma(this);
					}
					if ( WINDIB_FULLSCREEN() ) {
						SDL_RestoreGameMode();
					}
				}
#if defined(_WIN32_WCE)
				if ( WINDIB_FULLSCREEN() ) {
					LoadAygshell();
					if( SHFullScreen )
						SHFullScreen(SDL_Window, SHFS_HIDESTARTICON|SHFS_HIDETASKBAR|SHFS_HIDESIPBUTTON);
					else
						ShowWindow(FindWindow(TEXT("HHTaskBar"),NULL),SW_HIDE);
				}
#endif
				posted = SDL_PrivateAppActive(1, appstate);
				WIN_GetKeyboardState();
			} else {
				/* Lose the following states */
				appstate = SDL_APPINPUTFOCUS;
				if ( minimized ) {
					appstate |= SDL_APPACTIVE;
				}
				if ( this->input_grab != SDL_GRAB_OFF ) {
					WIN_GrabInput(this, SDL_GRAB_OFF);
				}
				if ( SDL_GetAppState() & SDL_APPINPUTFOCUS ) {
					if ( ! DDRAW_FULLSCREEN() ) {
						DIB_SwapGamma(this);
					}
					if ( WINDIB_FULLSCREEN() ) {
						SDL_RestoreDesktopMode();
#if defined(_WIN32_WCE)
						LoadAygshell();
						if( SHFullScreen ) 
							SHFullScreen(SDL_Window, SHFS_SHOWSTARTICON|SHFS_SHOWTASKBAR|SHFS_SHOWSIPBUTTON);
						else
							ShowWindow(FindWindow(TEXT("HHTaskBar"),NULL),SW_SHOW);
#endif
					}
				}
				posted = SDL_PrivateAppActive(0, appstate);
			}
			WIN_Activate(this, active, minimized);
			return(0);
		}
		break;

		case WM_MOUSEMOVE: {
			
			/* Mouse is handled by DirectInput when fullscreen */
			if ( SDL_VideoSurface && ! DINPUT_FULLSCREEN() ) {
				Sint16 x, y;

				/* mouse has entered the window */
				if ( ! in_window ) {
#ifdef WM_MOUSELEAVE
					TRACKMOUSEEVENT tme;

					tme.cbSize = sizeof(tme);
					tme.dwFlags = TME_LEAVE;
					tme.hwndTrack = SDL_Window;
					_TrackMouseEvent(&tme);
#endif /* WM_MOUSELEAVE */
					in_window = TRUE;

					posted = SDL_PrivateAppActive(1, SDL_APPMOUSEFOCUS);
				}

				/* mouse has moved within the window */
				x = LOWORD(lParam);
				y = HIWORD(lParam);
				if ( mouse_relative ) {
					POINT center;
					center.x = (SDL_VideoSurface->w/2);
					center.y = (SDL_VideoSurface->h/2);
					x -= (Sint16)center.x;
					y -= (Sint16)center.y;
					if ( x || y ) {
						ClientToScreen(SDL_Window, &center);
						SetCursorPos(center.x, center.y);
						posted = SDL_PrivateMouseMotion(0, 1, x, y);
					}
				} else {
#ifdef _WIN32_WCE
					if (SDL_VideoSurface)
						GapiTransform(this->hidden->userOrientation, this->hidden->hiresFix, &x, &y);
#endif
					posted = SDL_PrivateMouseMotion(0, 0, x, y);
				}
			}
		}
		return(0);

#ifdef WM_MOUSELEAVE
		case WM_MOUSELEAVE: {

			/* Mouse is handled by DirectInput when fullscreen */
			if ( SDL_VideoSurface && ! DINPUT_FULLSCREEN() ) {
				/* mouse has left the window */
				/* or */
				/* Elvis has left the building! */
				posted = SDL_PrivateAppActive(0, SDL_APPMOUSEFOCUS);
			}
			in_window = FALSE;
		}
		return(0);
#endif /* WM_MOUSELEAVE */

		case WM_LBUTTONDOWN:
		case WM_LBUTTONUP:
		case WM_MBUTTONDOWN:
		case WM_MBUTTONUP:
		case WM_RBUTTONDOWN:
		case WM_RBUTTONUP:
		case WM_XBUTTONDOWN:
		case WM_XBUTTONUP: {
			/* Mouse is handled by DirectInput when fullscreen */
			if ( SDL_VideoSurface && ! DINPUT_FULLSCREEN() ) {
				WORD xbuttonval = 0;
				Sint16 x, y;
				Uint8 button, state;

				/* DJM:
				   We want the SDL window to take focus so that
				   it acts like a normal windows "component"
				   (e.g. gains keyboard focus on a mouse click).
				 */
				SetFocus(SDL_Window);

				/* Figure out which button to use */
				switch (msg) {
					case WM_LBUTTONDOWN:
						button = SDL_BUTTON_LEFT;
						state = SDL_PRESSED;
						break;
					case WM_LBUTTONUP:
						button = SDL_BUTTON_LEFT;
						state = SDL_RELEASED;
						break;
					case WM_MBUTTONDOWN:
						button = SDL_BUTTON_MIDDLE;
						state = SDL_PRESSED;
						break;
					case WM_MBUTTONUP:
						button = SDL_BUTTON_MIDDLE;
						state = SDL_RELEASED;
						break;
					case WM_RBUTTONDOWN:
						button = SDL_BUTTON_RIGHT;
						state = SDL_PRESSED;
						break;
					case WM_RBUTTONUP:
						button = SDL_BUTTON_RIGHT;
						state = SDL_RELEASED;
						break;
					case WM_XBUTTONDOWN:
						xbuttonval = GET_XBUTTON_WPARAM(wParam);
						button = SDL_BUTTON_WHEELDOWN + xbuttonval;
						state = SDL_PRESSED;
						break;
					case WM_XBUTTONUP:
						xbuttonval = GET_XBUTTON_WPARAM(wParam);
						button = SDL_BUTTON_WHEELDOWN + xbuttonval;
						state = SDL_RELEASED;
						break;
					default:
						/* Eh? Unknown button? */
						return(0);
				}
				if ( state == SDL_PRESSED ) {
					/* Grab mouse so we get up events */
					if ( ++mouse_pressed > 0 ) {
						SetCapture(hwnd);
					}
				} else {
					/* Release mouse after all up events */
					if ( --mouse_pressed <= 0 ) {
						ReleaseCapture();
						mouse_pressed = 0;
					}
				}
				if ( mouse_relative ) {
				/*	RJR: March 28, 2000
					report internal mouse position if in relative mode */
					x = 0; y = 0;
				} else {
					x = (Sint16)LOWORD(lParam);
					y = (Sint16)HIWORD(lParam);
#ifdef _WIN32_WCE
					if (SDL_VideoSurface)
						GapiTransform(this->hidden->userOrientation, this->hidden->hiresFix, &x, &y);
#endif
				}
				posted = SDL_PrivateMouseButton(
							state, button, x, y);

				/*
				 * MSDN says:
				 *  "Unlike the WM_LBUTTONUP, WM_MBUTTONUP, and WM_RBUTTONUP
				 *   messages, an application should return TRUE from [an 
				 *   XBUTTON message] if it processes it. Doing so will allow
				 *   software that simulates this message on Microsoft Windows
				 *   systems earlier than Windows 2000 to determine whether
				 *   the window procedure processed the message or called
				 *   DefWindowProc to process it.
				 */
				if (xbuttonval > 0)
					return(TRUE);
			}
		}
		return(0);


#if (_WIN32_WINNT >= 0x0400) || (_WIN32_WINDOWS > 0x0400)
		case WM_MOUSEWHEEL: 
			if ( SDL_VideoSurface && ! DINPUT_FULLSCREEN() ) {
				int move = (short)HIWORD(wParam);
				if ( move ) {
					Uint8 button;
					if ( move > 0 )
						button = SDL_BUTTON_WHEELUP;
					else
						button = SDL_BUTTON_WHEELDOWN;
					posted = SDL_PrivateMouseButton(
						SDL_PRESSED, button, 0, 0);
					posted |= SDL_PrivateMouseButton(
						SDL_RELEASED, button, 0, 0);
				}
			}
			return(0);
#endif

#ifdef WM_GETMINMAXINFO
		/* This message is sent as a way for us to "check" the values
		 * of a position change.  If we don't like it, we can adjust
		 * the values before they are changed.
		 */
		case WM_GETMINMAXINFO: {
			MINMAXINFO *info;
			RECT        size;
			int x, y;
			int style;
			int width;
			int height;

			/* We don't want to clobber an internal resize */
			if ( SDL_resizing )
				return(0);

			/* We allow resizing with the SDL_RESIZABLE flag */
			if ( SDL_PublicSurface &&
				(SDL_PublicSurface->flags & SDL_RESIZABLE) ) {
				return(0);
			}

			/* Get the current position of our window */
			GetWindowRect(SDL_Window, &size);
			x = size.left;
			y = size.top;

			/* Calculate current width and height of our window */
			size.top = 0;
			size.left = 0;
			if ( SDL_PublicSurface != NULL ) {
				size.bottom = SDL_PublicSurface->h;
				size.right = SDL_PublicSurface->w;
			} else {
				size.bottom = 0;
				size.right = 0;
			}

			/* DJM - according to the docs for GetMenu(), the
			   return value is undefined if hwnd is a child window.
			   Aparently it's too difficult for MS to check
			   inside their function, so I have to do it here.
          		 */
         		style = GetWindowLong(hwnd, GWL_STYLE);
         		AdjustWindowRect(
				&size,
				style,
            			style & WS_CHILDWINDOW ? FALSE
						       : GetMenu(hwnd) != NULL);

			width = size.right - size.left;
			height = size.bottom - size.top;

			/* Fix our size to the current size */
			info = (MINMAXINFO *)lParam;
			info->ptMaxSize.x = width;
			info->ptMaxSize.y = height;
			info->ptMaxPosition.x = x;
			info->ptMaxPosition.y = y;
			info->ptMinTrackSize.x = width;
			info->ptMinTrackSize.y = height;
			info->ptMaxTrackSize.x = width;
			info->ptMaxTrackSize.y = height;
		}
		return(0);
#endif /* WM_GETMINMAXINFO */

		case WM_WINDOWPOSCHANGED: {
			SDL_VideoDevice *this = current_video;
			int w, h;

			GetClientRect(SDL_Window, &SDL_bounds);
			ClientToScreen(SDL_Window, (LPPOINT)&SDL_bounds);
			ClientToScreen(SDL_Window, (LPPOINT)&SDL_bounds+1);
			if ( !SDL_resizing && !IsZoomed(SDL_Window) &&
			     SDL_PublicSurface &&
				!(SDL_PublicSurface->flags & SDL_FULLSCREEN) ) {
				SDL_windowX = SDL_bounds.left;
				SDL_windowY = SDL_bounds.top;
			}
			w = SDL_bounds.right-SDL_bounds.left;
			h = SDL_bounds.bottom-SDL_bounds.top;
			if ( this->input_grab != SDL_GRAB_OFF ) {
				ClipCursor(&SDL_bounds);
			}
			if ( SDL_PublicSurface && 
				(SDL_PublicSurface->flags & SDL_RESIZABLE) ) {
				SDL_PrivateResize(w, h);
			}
		}
		break;

		/* We need to set the cursor */
		case WM_SETCURSOR: {
			Uint16 hittest;

			hittest = LOWORD(lParam);
			if ( hittest == HTCLIENT ) {
				SetCursor(SDL_hcursor);
				return(TRUE);
			}
		}
		break;

		/* We are about to get palette focus! */
		case WM_QUERYNEWPALETTE: {
			WIN_RealizePalette(current_video);
			return(TRUE);
		}
		break;

		/* Another application changed the palette */
		case WM_PALETTECHANGED: {
			WIN_PaletteChanged(current_video, (HWND)wParam);
		}
		break;

		/* We were occluded, refresh our display */
		case WM_PAINT: {
			HDC hdc;
			PAINTSTRUCT ps;

			hdc = BeginPaint(SDL_Window, &ps);
			if ( current_video->screen &&
			     !(current_video->screen->flags & SDL_OPENGL) ) {
				WIN_WinPAINT(current_video, hdc);
			}
			EndPaint(SDL_Window, &ps);
		}
		return(0);

		/* DJM: Send an expose event in this case */
		case WM_ERASEBKGND: {
			posted = SDL_PrivateExpose();
		}
		return(0);

		case WM_CLOSE: {
			if ( (posted = SDL_PrivateQuit()) )
				PostQuitMessage(0);
		}
		return(0);

		case WM_DESTROY: {
			PostQuitMessage(0);
		}
		return(0);

#ifndef NO_GETKEYBOARDSTATE
		case WM_INPUTLANGCHANGE: {
			codepage = GetCodePage();
		}
		return(TRUE);
#endif

		default: {
			/* Special handling by the video driver */
			if (HandleMessage) {
				return(HandleMessage(current_video,
			                     hwnd, msg, wParam, lParam));
			}
		}
		break;
	}
	return(DefWindowProc(hwnd, msg, wParam, lParam));
}

/* Allow the application handle to be stored and retrieved later */
static void *SDL_handle = NULL;

void SDL_SetModuleHandle(void *handle)
{
	SDL_handle = handle;
}
void *SDL_GetModuleHandle(void)
{
	void *handle;

	if ( SDL_handle ) {
		handle = SDL_handle;
	} else {
		handle = GetModuleHandle(NULL);
	}
	return(handle);
}

/* This allows the SDL_WINDOWID hack */
BOOL SDL_windowid = FALSE;

static int app_registered = 0;

/* Register the class for this application -- exported for winmain.c */
int SDL_RegisterApp(char *name, Uint32 style, void *hInst)
{
	WNDCLASS class;
#ifdef WM_MOUSELEAVE
	HMODULE handle;
#endif

	/* Only do this once... */
	if ( app_registered ) {
		++app_registered;
		return(0);
	}

#ifndef CS_BYTEALIGNCLIENT
#define CS_BYTEALIGNCLIENT	0
#endif
	if ( ! name && ! SDL_Appname ) {
		name = "SDL_app";
		SDL_Appstyle = CS_BYTEALIGNCLIENT;
		SDL_Instance = hInst ? hInst : SDL_GetModuleHandle();
	}

	if ( name ) {
#ifdef _WIN32_WCE
		/* WinCE uses the UNICODE version */
		SDL_Appname = SDL_iconv_utf8_ucs2(name);
#else
		SDL_Appname = SDL_iconv_utf8_locale(name);
#endif /* _WIN32_WCE */
		SDL_Appstyle = style;
		SDL_Instance = hInst ? hInst : SDL_GetModuleHandle();
	}

	/* Register the application class */
	class.hCursor		= NULL;
	class.hIcon		= LoadImage(SDL_Instance, SDL_Appname,
				            IMAGE_ICON,
	                                    0, 0, LR_DEFAULTCOLOR);
	class.lpszMenuName	= NULL;
	class.lpszClassName	= SDL_Appname;
	class.hbrBackground	= NULL;
	class.hInstance		= SDL_Instance;
	class.style		= SDL_Appstyle;
#if SDL_VIDEO_OPENGL
	class.style		|= CS_OWNDC;
#endif
	class.lpfnWndProc	= WinMessage;
	class.cbWndExtra	= 0;
	class.cbClsExtra	= 0;
	if ( ! RegisterClass(&class) ) {
		SDL_SetError("Couldn't register application class");
		return(-1);
	}

#ifdef WM_MOUSELEAVE
	/* Get the version of TrackMouseEvent() we use */
	_TrackMouseEvent = NULL;
	handle = GetModuleHandle("USER32.DLL");
	if ( handle ) {
		_TrackMouseEvent = (BOOL (WINAPI *)(TRACKMOUSEEVENT *))GetProcAddress(handle, "TrackMouseEvent");
	}
	if ( _TrackMouseEvent == NULL ) {
		_TrackMouseEvent = WIN_TrackMouseEvent;
	}
#endif /* WM_MOUSELEAVE */

#ifndef NO_GETKEYBOARDSTATE
	/* Initialise variables for SDL_ToUnicode() */
	codepage = GetCodePage();
	SDL_ToUnicode = Is9xME() ? ToUnicode9xME : ToUnicode;
#endif

	app_registered = 1;
	return(0);
}

/* Unregisters the windowclass registered in SDL_RegisterApp above. */
void SDL_UnregisterApp()
{
	WNDCLASS class;

	/* SDL_RegisterApp might not have been called before */
	if ( !app_registered ) {
		return;
	}
	--app_registered;
	if ( app_registered == 0 ) {
		/* Check for any registered window classes. */
		if ( GetClassInfo(SDL_Instance, SDL_Appname, &class) ) {
			UnregisterClass(SDL_Appname, SDL_Instance);
		}
		SDL_free(SDL_Appname);
		SDL_Appname = NULL;
	}
}

#ifndef NO_GETKEYBOARDSTATE
/* JFP: Implementation of ToUnicode() that works on 9x/ME/2K/XP */

static int Is9xME()
{
	OSVERSIONINFO   info;

	SDL_memset(&info, 0, sizeof(info));
	info.dwOSVersionInfoSize = sizeof(info);
	if (!GetVersionEx(&info)) {
		return 0;
	}
	return (info.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS);
}

static int GetCodePage()
{
	char	buff[8];
	int	lcid = MAKELCID(LOWORD(GetKeyboardLayout(0)), SORT_DEFAULT);
	int	cp = GetACP();

	if (GetLocaleInfo(lcid, LOCALE_IDEFAULTANSICODEPAGE, buff, sizeof(buff))) {
		cp = SDL_atoi(buff);
	}
	return cp;
}

static int WINAPI ToUnicode9xME(UINT vkey, UINT scancode, PBYTE keystate, LPWSTR wchars, int wsize, UINT flags)
{
	BYTE	chars[2];

	if (ToAsciiEx(vkey, scancode, keystate, (WORD*)chars, 0, GetKeyboardLayout(0)) == 1) {
		return MultiByteToWideChar(codepage, 0, chars, 1, wchars, wsize);
	}
	return 0;
}

#endif /* !NO_GETKEYBOARDSTATE */
