/*
 * Message queues related functions
 *
 * Copyright 1993, 1994 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"
#include "wine/port.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef HAVE_SYS_TIME_H
# include <sys/time.h>
#endif
#include <sys/types.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "message.h"
#include "winerror.h"
#include "ntstatus.h"
#include "wine/server.h"
#include "controls.h"
#include "dde.h"
#include "message.h"
#include "user_private.h"
#include "win.h"
#include "winpos.h"
#include "winproc.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(msg);
WINE_DECLARE_DEBUG_CHANNEL(key);

#define WM_NCMOUSEFIRST         WM_NCMOUSEMOVE
#define WM_NCMOUSELAST          WM_NCMBUTTONDBLCLK


/***********************************************************************
 *           is_keyboard_message
 */
inline static BOOL is_keyboard_message( UINT message )
{
    return (message >= WM_KEYFIRST && message <= WM_KEYLAST);
}


/***********************************************************************
 *           is_mouse_message
 */
inline static BOOL is_mouse_message( UINT message )
{
    return ((message >= WM_NCMOUSEFIRST && message <= WM_NCMOUSELAST) ||
            (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST));
}


/***********************************************************************
 *           check_message_filter
 */
inline static BOOL check_message_filter( const MSG *msg, HWND hwnd, UINT first, UINT last )
{
    if (hwnd)
    {
        if (msg->hwnd != hwnd && !IsChild( hwnd, msg->hwnd )) return FALSE;
    }
    if (first || last)
    {
       return (msg->message >= first && msg->message <= last);
    }
    return TRUE;
}


/***********************************************************************
 *           process_sent_messages
 *
 * Process all pending sent messages.
 */
inline static void process_sent_messages(void)
{
    MSG msg;
    MSG_peek_message( &msg, 0, 0, 0, GET_MSG_REMOVE | GET_MSG_SENT_ONLY );
}


/***********************************************************************
 *           queue_hardware_message
 *
 * store a hardware message in the thread queue
 */
#if 0
static void queue_hardware_message( MSG *msg, ULONG_PTR extra_info )
{
    SERVER_START_REQ( send_message )
    {
        req->type   = MSG_HARDWARE;
        req->id     = GetWindowThreadProcessId( msg->hwnd, NULL );
        req->win    = msg->hwnd;
        req->msg    = msg->message;
        req->wparam = msg->wParam;
        req->lparam = msg->lParam;
        req->x      = msg->pt.x;
        req->y      = msg->pt.y;
        req->time   = msg->time;
        req->info   = extra_info;
        req->timeout = 0;
        wine_server_call( req );
    }
    SERVER_END_REQ;
}
#endif


/***********************************************************************
 *           MSG_SendParentNotify
 *
 * Send a WM_PARENTNOTIFY to all ancestors of the given window, unless
 * the window has the WS_EX_NOPARENTNOTIFY style.
 */
static void MSG_SendParentNotify( HWND hwnd, WORD event, WORD idChild, POINT pt )
{
    /* pt has to be in the client coordinates of the parent window */
    MapWindowPoints( 0, hwnd, &pt, 1 );
    for (;;)
    {
        HWND parent;

        if (!(GetWindowLongA( hwnd, GWL_STYLE ) & WS_CHILD)) break;
        if (GetWindowLongA( hwnd, GWL_EXSTYLE ) & WS_EX_NOPARENTNOTIFY) break;
        if (!(parent = GetParent(hwnd))) break;
        MapWindowPoints( hwnd, parent, &pt, 1 );
        hwnd = parent;
        SendMessageA( hwnd, WM_PARENTNOTIFY,
                      MAKEWPARAM( event, idChild ), MAKELPARAM( pt.x, pt.y ) );
    }
}


#if 0  /* this is broken for now, will require proper support in the server */

/***********************************************************************
 *          MSG_JournalPlayBackMsg
 *
 * Get an EVENTMSG struct via call JOURNALPLAYBACK hook function
 */
void MSG_JournalPlayBackMsg(void)
{
    EVENTMSG tmpMsg;
    MSG msg;
    LRESULT wtime;
    int keyDown,i;

    wtime=HOOK_CallHooks( WH_JOURNALPLAYBACK, HC_GETNEXT, 0, (LPARAM)&tmpMsg, TRUE );
    /*  TRACE(msg,"Playback wait time =%ld\n",wtime); */
    if (wtime<=0)
    {
        wtime=0;
        msg.message = tmpMsg.message;
        msg.hwnd    = tmpMsg.hwnd;
        msg.time    = tmpMsg.time;
        if ((tmpMsg.message >= WM_KEYFIRST) && (tmpMsg.message <= WM_KEYLAST))
        {
            msg.wParam  = tmpMsg.paramL & 0xFF;
            msg.lParam  = MAKELONG(tmpMsg.paramH&0x7ffff,tmpMsg.paramL>>8);
            if (tmpMsg.message == WM_KEYDOWN || tmpMsg.message == WM_SYSKEYDOWN)
            {
                for (keyDown=i=0; i<256 && !keyDown; i++)
                    if (InputKeyStateTable[i] & 0x80)
                        keyDown++;
                if (!keyDown)
                    msg.lParam |= 0x40000000;
                InputKeyStateTable[msg.wParam] |= 0x80;
                AsyncKeyStateTable[msg.wParam] |= 0x80;
            }
            else                                       /* WM_KEYUP, WM_SYSKEYUP */
            {
                msg.lParam |= 0xC0000000;
                InputKeyStateTable[msg.wParam] &= ~0x80;
            }
            if (InputKeyStateTable[VK_MENU] & 0x80)
                msg.lParam |= 0x20000000;
            if (tmpMsg.paramH & 0x8000)              /*special_key bit*/
                msg.lParam |= 0x01000000;

            msg.pt.x = msg.pt.y = 0;
            queue_hardware_message( &msg, 0 );
        }
        else if ((tmpMsg.message>= WM_MOUSEFIRST) && (tmpMsg.message <= WM_MOUSELAST))
        {
            switch (tmpMsg.message)
            {
            case WM_LBUTTONDOWN:
                InputKeyStateTable[VK_LBUTTON] |= 0x80;
                AsyncKeyStateTable[VK_LBUTTON] |= 0x80;
                break;
            case WM_LBUTTONUP:
                InputKeyStateTable[VK_LBUTTON] &= ~0x80;
                break;
            case WM_MBUTTONDOWN:
                InputKeyStateTable[VK_MBUTTON] |= 0x80;
                AsyncKeyStateTable[VK_MBUTTON] |= 0x80;
                break;
            case WM_MBUTTONUP:
                InputKeyStateTable[VK_MBUTTON] &= ~0x80;
                break;
            case WM_RBUTTONDOWN:
                InputKeyStateTable[VK_RBUTTON] |= 0x80;
                AsyncKeyStateTable[VK_RBUTTON] |= 0x80;
                break;
            case WM_RBUTTONUP:
                InputKeyStateTable[VK_RBUTTON] &= ~0x80;
                break;
            }
            SetCursorPos(tmpMsg.paramL,tmpMsg.paramH);
            msg.lParam=MAKELONG(tmpMsg.paramL,tmpMsg.paramH);
            msg.wParam=0;
            if (InputKeyStateTable[VK_LBUTTON] & 0x80) msg.wParam |= MK_LBUTTON;
            if (InputKeyStateTable[VK_MBUTTON] & 0x80) msg.wParam |= MK_MBUTTON;
            if (InputKeyStateTable[VK_RBUTTON] & 0x80) msg.wParam |= MK_RBUTTON;

            msg.pt.x = tmpMsg.paramL;
            msg.pt.y = tmpMsg.paramH;
            queue_hardware_message( &msg, 0 );
        }
        HOOK_CallHooks( WH_JOURNALPLAYBACK, HC_SKIP, 0, (LPARAM)&tmpMsg, TRUE );
    }
    else
    {
        if( tmpMsg.message == WM_QUEUESYNC ) HOOK_CallHooks( WH_CBT, HCBT_QS, 0, 0, TRUE );
    }
}
#endif


/***********************************************************************
 *          process_raw_keyboard_message
 *
 * returns TRUE if the contents of 'msg' should be passed to the application
 */
static BOOL process_raw_keyboard_message( MSG *msg, HWND hwnd_filter, UINT first, UINT last )
{
    EVENTMSG event;

    event.message = msg->message;
    event.hwnd    = msg->hwnd;
    event.time    = msg->time;
    event.paramL  = (msg->wParam & 0xFF) | (HIWORD(msg->lParam) << 8);
    event.paramH  = msg->lParam & 0x7FFF;
    if (HIWORD(msg->lParam) & 0x0100) event.paramH |= 0x8000; /* special_key - bit */
    HOOK_CallHooks( WH_JOURNALRECORD, HC_ACTION, 0, (LPARAM)&event, TRUE );
    return check_message_filter( msg, hwnd_filter, first, last );
}


/***********************************************************************
 *          process_cooked_keyboard_message
 *
 * returns TRUE if the contents of 'msg' should be passed to the application
 */
static BOOL process_cooked_keyboard_message( MSG *msg, BOOL remove )
{
    if (remove)
    {
        /* Handle F1 key by sending out WM_HELP message */
        if ((msg->message == WM_KEYUP) &&
            (msg->wParam == VK_F1) &&
            (msg->hwnd != GetDesktopWindow()) &&
            !MENU_IsMenuActive())
        {
            HELPINFO hi;
            hi.cbSize = sizeof(HELPINFO);
            hi.iContextType = HELPINFO_WINDOW;
            hi.iCtrlId = GetWindowLongPtrA( msg->hwnd, GWLP_ID );
            hi.hItemHandle = msg->hwnd;
            hi.dwContextId = GetWindowContextHelpId( msg->hwnd );
            hi.MousePos = msg->pt;
            SendMessageA(msg->hwnd, WM_HELP, 0, (LPARAM)&hi);
        }
    }

    if (HOOK_CallHooks( WH_KEYBOARD, remove ? HC_ACTION : HC_NOREMOVE,
                        LOWORD(msg->wParam), msg->lParam, TRUE ))
    {
        /* skip this message */
        HOOK_CallHooks( WH_CBT, HCBT_KEYSKIPPED, LOWORD(msg->wParam), msg->lParam, TRUE );
        return FALSE;
    }
    return TRUE;
}


/***********************************************************************
 *          process_raw_mouse_message
 */
static BOOL process_raw_mouse_message( MSG *msg, HWND hwnd_filter, UINT first,
                                       UINT last, BOOL remove )
{
    static MSG clk_msg;

    POINT pt;
    UINT message;
    INT hittest;
    EVENTMSG event;
    GUITHREADINFO info;
    HWND hWndScope = msg->hwnd;

    /* find the window to dispatch this mouse message to */

    hittest = HTCLIENT;
    GetGUIThreadInfo( GetCurrentThreadId(), &info );
    if (!(msg->hwnd = info.hwndCapture))
    {
        /* If no capture HWND, find window which contains the mouse position.
         * Also find the position of the cursor hot spot (hittest) */
        if (!IsWindow(hWndScope)) hWndScope = 0;
        if (!(msg->hwnd = WINPOS_WindowFromPoint( hWndScope, msg->pt, &hittest )))
            msg->hwnd = GetDesktopWindow();
    }

    event.message = msg->message;
    event.time    = msg->time;
    event.hwnd    = msg->hwnd;
    event.paramL  = msg->pt.x;
    event.paramH  = msg->pt.y;
    HOOK_CallHooks( WH_JOURNALRECORD, HC_ACTION, 0, (LPARAM)&event, TRUE );

    if (hwnd_filter)
    {
        if (msg->hwnd != hwnd_filter && !IsChild( hwnd_filter, msg->hwnd )) return FALSE;
    }

    pt = msg->pt;
    message = msg->message;
    /* Note: windows has no concept of a non-client wheel message */
    if (message != WM_MOUSEWHEEL)
    {
        if (hittest != HTCLIENT)
        {
            message += WM_NCMOUSEMOVE - WM_MOUSEMOVE;
            msg->wParam = hittest;
        }
        else
        {
            /* coordinates don't get translated while tracking a menu */
            /* FIXME: should differentiate popups and top-level menus */
            if (!(info.flags & GUI_INMENUMODE))
                ScreenToClient( msg->hwnd, &pt );
        }
    }
    msg->lParam = MAKELONG( pt.x, pt.y );

    /* translate double clicks */

    if ((msg->message == WM_LBUTTONDOWN) ||
        (msg->message == WM_RBUTTONDOWN) ||
        (msg->message == WM_MBUTTONDOWN))
    {
        /* translate double clicks -
	 * note that ...MOUSEMOVEs can slip in between
	 * ...BUTTONDOWN and ...BUTTONDBLCLK messages */

        if ((info.flags & (GUI_INMENUMODE|GUI_INMOVESIZE)) ||
            hittest != HTCLIENT ||
            (GetClassLongA( msg->hwnd, GCL_STYLE ) & CS_DBLCLKS))
        {
           if ((msg->message == clk_msg.message) &&
               (msg->hwnd == clk_msg.hwnd) &&
               (msg->time - clk_msg.time < GetDoubleClickTime()) &&
               (abs(msg->pt.x - clk_msg.pt.x) < GetSystemMetrics(SM_CXDOUBLECLK)/2) &&
               (abs(msg->pt.y - clk_msg.pt.y) < GetSystemMetrics(SM_CYDOUBLECLK)/2))
           {
               message += (WM_LBUTTONDBLCLK - WM_LBUTTONDOWN);
               msg->message = 0;  /* to clear the double click conditions */
           }
        }
        if (first || last)
        {
            if (message < first || message > last) remove = FALSE;
        }
        /* update static double click conditions */
        if (remove) clk_msg = *msg;
    }

    msg->message = message;
    if (first || last) return (message >= first && message <= last);
    return TRUE;
}


/***********************************************************************
 *          process_cooked_mouse_message
 *
 * returns TRUE if the contents of 'msg' should be passed to the application
 */
static BOOL process_cooked_mouse_message( MSG *msg, ULONG_PTR extra_info, BOOL remove )
{
    MOUSEHOOKSTRUCT hook;
    INT hittest = HTCLIENT;
    UINT raw_message = msg->message;
    BOOL eatMsg;

    if (msg->message >= WM_NCMOUSEFIRST && msg->message <= WM_NCMOUSELAST)
    {
        raw_message += WM_MOUSEFIRST - WM_NCMOUSEFIRST;
        hittest = msg->wParam;
    }
    if (raw_message == WM_LBUTTONDBLCLK ||
        raw_message == WM_RBUTTONDBLCLK ||
        raw_message == WM_MBUTTONDBLCLK)
    {
        raw_message += WM_LBUTTONDOWN - WM_LBUTTONDBLCLK;
    }

    hook.pt           = msg->pt;
    hook.hwnd         = msg->hwnd;
    hook.wHitTestCode = hittest;
    hook.dwExtraInfo  = extra_info;
    if (HOOK_CallHooks( WH_MOUSE, remove ? HC_ACTION : HC_NOREMOVE,
                        msg->message, (LPARAM)&hook, TRUE ))
    {
        hook.pt           = msg->pt;
        hook.hwnd         = msg->hwnd;
        hook.wHitTestCode = hittest;
        hook.dwExtraInfo  = extra_info;
        HOOK_CallHooks( WH_CBT, HCBT_CLICKSKIPPED, msg->message, (LPARAM)&hook, TRUE );
        return FALSE;
    }

    if ((hittest == HTERROR) || (hittest == HTNOWHERE))
    {
        SendMessageA( msg->hwnd, WM_SETCURSOR, (WPARAM)msg->hwnd,
		      MAKELONG( hittest, raw_message ));
        return FALSE;
    }

    if (!remove || GetCapture()) return TRUE;

    eatMsg = FALSE;

    if ((raw_message == WM_LBUTTONDOWN) ||
        (raw_message == WM_RBUTTONDOWN) ||
        (raw_message == WM_MBUTTONDOWN))
    {
        /* Send the WM_PARENTNOTIFY,
         * note that even for double/nonclient clicks
         * notification message is still WM_L/M/RBUTTONDOWN.
         */
        MSG_SendParentNotify( msg->hwnd, raw_message, 0, msg->pt );

        /* Activate the window if needed */

        if (msg->hwnd != GetActiveWindow())
        {
            HWND hwndTop = msg->hwnd;
            while (hwndTop)
            {
                if ((GetWindowLongW( hwndTop, GWL_STYLE ) & (WS_POPUP|WS_CHILD)) != WS_CHILD) break;
                hwndTop = GetParent( hwndTop );
            }

            if (hwndTop && hwndTop != GetDesktopWindow())
            {
                LONG ret = SendMessageA( msg->hwnd, WM_MOUSEACTIVATE, (WPARAM)hwndTop,
                                         MAKELONG( hittest, raw_message ) );
                switch(ret)
                {
                case MA_NOACTIVATEANDEAT:
                    eatMsg = TRUE;
                    /* fall through */
                case MA_NOACTIVATE:
                    break;
                case MA_ACTIVATEANDEAT:
                    eatMsg = TRUE;
                    /* fall through */
                case MA_ACTIVATE:
                case 0:
                    if (!FOCUS_MouseActivate( hwndTop )) eatMsg = TRUE;
                    break;
                default:
                    WARN( "unknown WM_MOUSEACTIVATE code %ld\n", ret );
                    break;
                }
            }
        }
    }

    /* send the WM_SETCURSOR message */

    /* Windows sends the normal mouse message as the message parameter
       in the WM_SETCURSOR message even if it's non-client mouse message */
    SendMessageA( msg->hwnd, WM_SETCURSOR, (WPARAM)msg->hwnd,
		  MAKELONG( hittest, raw_message ));

    return !eatMsg;
}


/***********************************************************************
 *          MSG_process_raw_hardware_message
 *
 * returns TRUE if the contents of 'msg' should be passed to the application
 */
BOOL MSG_process_raw_hardware_message( MSG *msg, ULONG_PTR extra_info, HWND hwnd_filter,
                                       UINT first, UINT last, BOOL remove )
{
    if (is_keyboard_message( msg->message ))
        return process_raw_keyboard_message( msg, hwnd_filter, first, last );

    if (is_mouse_message( msg->message ))
        return process_raw_mouse_message( msg, hwnd_filter, first, last, remove );

    ERR( "unknown message type %x\n", msg->message );
    return FALSE;
}


/***********************************************************************
 *          MSG_process_cooked_hardware_message
 *
 * returns TRUE if the contents of 'msg' should be passed to the application
 */
BOOL MSG_process_cooked_hardware_message( MSG *msg, ULONG_PTR extra_info, BOOL remove )
{
    if (is_keyboard_message( msg->message ))
        return process_cooked_keyboard_message( msg, remove );

    if (is_mouse_message( msg->message ))
        return process_cooked_mouse_message( msg, extra_info, remove );

    ERR( "unknown message type %x\n", msg->message );
    return FALSE;
}


/***********************************************************************
 *		WaitMessage (USER.112) Suspend thread pending messages
 *		WaitMessage (USER32.@) Suspend thread pending messages
 *
 * WaitMessage() suspends a thread until events appear in the thread's
 * queue.
 */
BOOL WINAPI WaitMessage(void)
{
    return (MsgWaitForMultipleObjectsEx( 0, NULL, INFINITE, QS_ALLINPUT, 0 ) != WAIT_FAILED);
}


/***********************************************************************
 *		MsgWaitForMultipleObjectsEx   (USER32.@)
 */
DWORD WINAPI MsgWaitForMultipleObjectsEx( DWORD count, CONST HANDLE *pHandles,
                                          DWORD timeout, DWORD mask, DWORD flags )
{
    HANDLE handles[MAXIMUM_WAIT_OBJECTS];
    DWORD i, ret, lock;
    MESSAGEQUEUE *msgQueue;

    if (count > MAXIMUM_WAIT_OBJECTS-1)
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return WAIT_FAILED;
    }

    if (!(msgQueue = QUEUE_Current())) return WAIT_FAILED;

    /* set the queue mask */
    SERVER_START_REQ( set_queue_mask )
    {
        req->wake_mask    = (flags & MWMO_INPUTAVAILABLE) ? mask : 0;
        req->changed_mask = mask;
        req->skip_wait    = 0;
        wine_server_call( req );
    }
    SERVER_END_REQ;

    /* Add the thread event to the handle list */
    for (i = 0; i < count; i++) handles[i] = pHandles[i];
    handles[count] = msgQueue->server_queue;

    ReleaseThunkLock( &lock );
    if (USER_Driver.pMsgWaitForMultipleObjectsEx)
    {
        ret = USER_Driver.pMsgWaitForMultipleObjectsEx( count+1, handles, timeout, mask, flags );
        if (ret == count+1) ret = count; /* pretend the msg queue is ready */
    }
    else
        ret = WaitForMultipleObjectsEx( count+1, handles, flags & MWMO_WAITALL,
                                        timeout, flags & MWMO_ALERTABLE );
    if (lock) RestoreThunkLock( lock );
    return ret;
}


/***********************************************************************
 *		MsgWaitForMultipleObjects (USER32.@)
 */
DWORD WINAPI MsgWaitForMultipleObjects( DWORD count, CONST HANDLE *handles,
                                        BOOL wait_all, DWORD timeout, DWORD mask )
{
    return MsgWaitForMultipleObjectsEx( count, handles, timeout, mask,
                                        wait_all ? MWMO_WAITALL : 0 );
}


/***********************************************************************
 *		WaitForInputIdle (USER32.@)
 */
DWORD WINAPI WaitForInputIdle( HANDLE hProcess, DWORD dwTimeOut )
{
    DWORD start_time, elapsed, ret;
    HANDLE idle_event = (HANDLE)-1;

    SERVER_START_REQ( wait_input_idle )
    {
        req->handle = hProcess;
        req->timeout = dwTimeOut;
        if (!(ret = wine_server_call_err( req ))) idle_event = reply->event;
    }
    SERVER_END_REQ;
    if (ret) return WAIT_FAILED;  /* error */
    if (!idle_event) return 0;  /* no event to wait on */

    start_time = GetTickCount();
    elapsed = 0;

    TRACE("waiting for %p\n", idle_event );
    do
    {
        ret = MsgWaitForMultipleObjects ( 1, &idle_event, FALSE, dwTimeOut - elapsed, QS_SENDMESSAGE );
        switch (ret)
        {
        case WAIT_OBJECT_0+1:
            process_sent_messages();
            break;
        case WAIT_TIMEOUT:
        case WAIT_FAILED:
            TRACE("timeout or error\n");
            return ret;
        default:
            TRACE("finished\n");
            return 0;
        }
        if (dwTimeOut != INFINITE)
        {
            elapsed = GetTickCount() - start_time;
            if (elapsed > dwTimeOut)
                break;
        }
    }
    while (1);

    return WAIT_TIMEOUT;
}


/***********************************************************************
 *		UserYield (USER.332)
 */
void WINAPI UserYield16(void)
{
   DWORD count;

    /* Handle sent messages */
    process_sent_messages();

    /* Yield */
    ReleaseThunkLock(&count);
    if (count)
    {
        RestoreThunkLock(count);
        /* Handle sent messages again */
        process_sent_messages();
    }
}


/***********************************************************************
 *		RegisterWindowMessage (USER.118)
 *		RegisterWindowMessageA (USER32.@)
 */
WORD WINAPI RegisterWindowMessageA( LPCSTR str )
{
    TRACE("%s\n", str );
    return GlobalAddAtomA( str );
}


/***********************************************************************
 *		RegisterWindowMessageW (USER32.@)
 */
WORD WINAPI RegisterWindowMessageW( LPCWSTR str )
{
    TRACE("%p\n", str );
    return GlobalAddAtomW( str );
}


/***********************************************************************
 *		BroadcastSystemMessage  (USER32.@)
 *		BroadcastSystemMessageA (USER32.@)
 */
LONG WINAPI BroadcastSystemMessageA(
	DWORD dwFlags,LPDWORD recipients,UINT uMessage,WPARAM wParam,
	LPARAM lParam )
{
    if ((*recipients & BSM_APPLICATIONS)||
        (*recipients == BSM_ALLCOMPONENTS))
    {
        FIXME("(%08lx,%08lx,%08x,%08x,%08lx): semi-stub!\n",
              dwFlags,*recipients,uMessage,wParam,lParam);
        PostMessageA(HWND_BROADCAST,uMessage,wParam,lParam);
        return 1;
    }
    else
    {
        FIXME("(%08lx,%08lx,%08x,%08x,%08lx): stub!\n",
              dwFlags,*recipients,uMessage,wParam,lParam);
        return -1;
    }
}

/***********************************************************************
 *		BroadcastSystemMessageW (USER32.@)
 */
LONG WINAPI BroadcastSystemMessageW(
	DWORD dwFlags,LPDWORD recipients,UINT uMessage,WPARAM wParam,
	LPARAM lParam )
{
    if ((*recipients & BSM_APPLICATIONS)||
        (*recipients == BSM_ALLCOMPONENTS))
    {
        FIXME("(%08lx,%08lx,%08x,%08x,%08lx): semi-stub!\n",
              dwFlags,*recipients,uMessage,wParam,lParam);
        PostMessageW(HWND_BROADCAST,uMessage,wParam,lParam);
        return 1;
    }
    else
    {
        FIXME("(%08lx,%08lx,%08x,%08x,%08lx): stub!\n",
              dwFlags,*recipients,uMessage,wParam,lParam);
        return -1;
    }
}
