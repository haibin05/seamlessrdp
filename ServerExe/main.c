/* -*- c-basic-offset: 8 -*-
   rdesktop: A Remote Desktop Protocol client.
   Seamless windows - Remote server executable

   Based on code copyright (C) 2004-2005 Martin Wickett

   Copyright (C) Peter Åstrand <astrand@cendio.se> 2005-2006
   Copyright (C) Pierre Ossman <ossman@cendio.se> 2006

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include <windows.h>
#include <stdio.h>
#include <wtsapi32.h>
#include <cchannel.h>

#include "vchannel.h"

#include "resource.h"

#define APP_NAME "SeamlessRDP Shell"

#ifndef WM_WTSSESSION_CHANGE
#define WM_WTSSESSION_CHANGE 0x02B1
#endif
#ifndef WTS_REMOTE_CONNECT
#define WTS_REMOTE_CONNECT 0x3
#endif

/* Global data */
static HINSTANCE g_instance;
static HWND g_hwnd;

typedef void (*set_hooks_proc_t) ();
typedef void (*remove_hooks_proc_t) ();
typedef int (*get_instance_count_proc_t) ();

typedef void (*move_window_proc_t) (HWND hwnd, int x, int y, int width, int height);

static move_window_proc_t g_move_window_fn = NULL;

static void
message(const char *text)
{
	MessageBox(GetDesktopWindow(), text, "SeamlessRDP Shell", MB_OK);
}

static char *
get_token(char **s)
{
	char *comma, *head;
	head = *s;

	if (!head)
		return NULL;

	comma = strchr(head, ',');
	if (comma)
	{
		*comma = '\0';
		*s = comma + 1;
	}
	else
	{
		*s = NULL;
	}

	return head;
}

static BOOL CALLBACK
enum_cb(HWND hwnd, LPARAM lparam)
{
	RECT rect;
	unsigned short title[150];
	LONG styles;
	int state;
	HWND parent;

	styles = GetWindowLong(hwnd, GWL_STYLE);

	if (!(styles & WS_VISIBLE))
		return TRUE;

	if (styles & WS_POPUP)
		parent = (HWND) GetWindowLong(hwnd, GWL_HWNDPARENT);
	else
		parent = NULL;

	vchannel_write("CREATE,0x%p,0x%p,0x%x", hwnd, parent, 0);

	if (!GetWindowRect(hwnd, &rect))
	{
		debug("GetWindowRect failed!");
		return TRUE;
	}

	vchannel_write("POSITION,0x%p,%d,%d,%d,%d,0x%x",
		       hwnd,
		       rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, 0);

	GetWindowTextW(hwnd, title, sizeof(title) / sizeof(*title));

	vchannel_write("TITLE,0x%x,%s,0x%x", hwnd, vchannel_strfilter_unicode(title), 0);

	if (styles & WS_MAXIMIZE)
		state = 2;
	else if (styles & WS_MINIMIZE)
		state = 1;
	else
		state = 0;

	vchannel_write("STATE,0x%p,0x%x,0x%x", hwnd, state, 0);

	return TRUE;
}

static void
do_sync(void)
{
	vchannel_block();

	vchannel_write("SYNCBEGIN,0x0");

	EnumWindows(enum_cb, 0);

	vchannel_write("SYNCEND,0x0");

	vchannel_unblock();
}

static void
do_state(HWND hwnd, int state)
{
	if (state == 0)
		ShowWindow(hwnd, SW_RESTORE);
	else if (state == 1)
		ShowWindow(hwnd, SW_MINIMIZE);
	else if (state == 2)
		ShowWindow(hwnd, SW_MAXIMIZE);
	else
		debug("Invalid state %d sent.", state);
}

static void
do_position(HWND hwnd, int x, int y, int width, int height)
{
	g_move_window_fn(hwnd, x, y, width, height);
}

static void
do_zchange(HWND hwnd, HWND behind)
{
	if (behind == NULL)
		behind = HWND_TOP;
	SetWindowPos(hwnd, behind, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
}

static void
process_cmds(void)
{
	char line[VCHANNEL_MAX_LINE];
	int size;

	char *p, *tok1, *tok2, *tok3, *tok4, *tok5, *tok6, *tok7, *tok8;

	while ((size = vchannel_read(line, sizeof(line))) >= 0)
	{
		p = line;

		tok1 = get_token(&p);
		tok2 = get_token(&p);
		tok3 = get_token(&p);
		tok4 = get_token(&p);
		tok5 = get_token(&p);
		tok6 = get_token(&p);
		tok7 = get_token(&p);
		tok8 = get_token(&p);

		if (strcmp(tok1, "SYNC") == 0)
			do_sync();
		else if (strcmp(tok1, "STATE") == 0)
			do_state((HWND) strtoul(tok2, NULL, 0), strtol(tok3, NULL, 0));
		else if (strcmp(tok1, "POSITION") == 0)
			do_position((HWND) strtoul(tok2, NULL, 0), strtol(tok3, NULL, 0),
				    strtol(tok4, NULL, 0), strtol(tok5, NULL, 0), strtol(tok6, NULL,
											 0));
		else if (strcmp(tok1, "ZCHANGE") == 0)
			do_zchange((HWND) strtoul(tok2, NULL, 0), (HWND) strtoul(tok3, NULL, 0));
	}
}

static LRESULT CALLBACK
wndproc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_WTSSESSION_CHANGE)
	{
		if (wparam == WTS_REMOTE_CONNECT)
		{
			/* These get reset on each reconnect */
			SystemParametersInfo(SPI_SETDRAGFULLWINDOWS, TRUE, NULL, 0);
			SystemParametersInfo(SPI_SETSCREENSAVEACTIVE, FALSE, NULL, 0);
			vchannel_write("HELLO,0x%08x", 1);
		}
	}

	return DefWindowProc(hwnd, message, wparam, lparam);
}

static BOOL
register_class(void)
{
	WNDCLASSEX wcx;

	memset(&wcx, 0, sizeof(wcx));

	wcx.cbSize = sizeof(wcx);
	wcx.lpfnWndProc = wndproc;
	wcx.hInstance = g_instance;
	wcx.lpszClassName = "SeamlessClass";

	return RegisterClassEx(&wcx);
}

static BOOL
create_wnd(void)
{
	if (!register_class())
		return FALSE;

	g_hwnd = CreateWindow("SeamlessClass",
			      "Seamless Window",
			      WS_OVERLAPPEDWINDOW,
			      CW_USEDEFAULT,
			      CW_USEDEFAULT,
			      CW_USEDEFAULT,
			      CW_USEDEFAULT, (HWND) NULL, (HMENU) NULL, g_instance, (LPVOID) NULL);

	if (!g_hwnd)
		return FALSE;

	return TRUE;
}

int WINAPI
WinMain(HINSTANCE instance, HINSTANCE prev_instance, LPSTR cmdline, int cmdshow)
{
	HMODULE hookdll;

	set_hooks_proc_t set_hooks_fn;
	remove_hooks_proc_t remove_hooks_fn;
	get_instance_count_proc_t instance_count_fn;

	g_instance = instance;

	hookdll = LoadLibrary("seamlessrdp.dll");
	if (!hookdll)
	{
		message("Could not load hook DLL. Unable to continue.");
		return -1;
	}

	set_hooks_fn = (set_hooks_proc_t) GetProcAddress(hookdll, "SetHooks");
	remove_hooks_fn = (remove_hooks_proc_t) GetProcAddress(hookdll, "RemoveHooks");
	instance_count_fn = (get_instance_count_proc_t) GetProcAddress(hookdll, "GetInstanceCount");
	g_move_window_fn = (move_window_proc_t) GetProcAddress(hookdll, "SafeMoveWindow");

	if (!set_hooks_fn || !remove_hooks_fn || !instance_count_fn || !g_move_window_fn)
	{
		FreeLibrary(hookdll);
		message("Hook DLL doesn't contain the correct functions. Unable to continue.");
		return -1;
	}

	/* Check if the DLL is already loaded */
	if (instance_count_fn() != 1)
	{
		FreeLibrary(hookdll);
		message("Another running instance of Seamless RDP detected.");
		return -1;
	}

	if (!create_wnd())
	{
		FreeLibrary(hookdll);
		message("Couldn't create a window to catch events.");
		return -1;
	}

	WTSRegisterSessionNotification(g_hwnd, NOTIFY_FOR_THIS_SESSION);

	vchannel_open();

	vchannel_write("HELLO,0x%08x", 0);

	set_hooks_fn();

	/* Since we don't see the entire desktop we must resize windows
	   immediatly. */
	SystemParametersInfo(SPI_SETDRAGFULLWINDOWS, TRUE, NULL, 0);

	/* Disable screen saver since we cannot catch its windows. */
	SystemParametersInfo(SPI_SETSCREENSAVEACTIVE, FALSE, NULL, 0);

	if (strlen(cmdline) == 0)
	{
		message("No command line specified.");
		return -1;
	}
	else
	{
		BOOL result;
		DWORD exitcode;
		PROCESS_INFORMATION proc_info;
		STARTUPINFO startup_info;
		MSG msg;

		memset(&startup_info, 0, sizeof(STARTUPINFO));
		startup_info.cb = sizeof(STARTUPINFO);

		result = CreateProcess(NULL, cmdline, NULL, NULL, FALSE, 0,
				       NULL, NULL, &startup_info, &proc_info);

		if (result)
		{
			do
			{
				while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
				{
					TranslateMessage(&msg);
					DispatchMessage(&msg);
				}
				process_cmds();
				Sleep(100);
				GetExitCodeProcess(proc_info.hProcess, &exitcode);
			}
			while (exitcode == STILL_ACTIVE);

			// Release handles
			CloseHandle(proc_info.hProcess);
			CloseHandle(proc_info.hThread);
		}
		else
		{
			// CreateProcess failed.
			char msg[256];
			_snprintf(msg, sizeof(msg),
				  "Unable to launch the requested application:\n%s", cmdline);
			message(msg);
		}
	}

	WTSUnRegisterSessionNotification(g_hwnd);

	remove_hooks_fn();

	FreeLibrary(hookdll);

	vchannel_close();

	return 1;
}
