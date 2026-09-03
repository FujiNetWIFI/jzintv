/* ======================================================================== */
/*  SDL2 Bug Workaround for X11 and non-Motif-compatible window managers.   */
/*                                                                          */
/*  SDL2's X11 backend uses the _MOTIF_WM_HINTS property to communicate     */
/*  whether a window has a border or not.  If that property isn't present,  */
/*  then it falls back to calling X11_XSetWindowTransientForHint to set     */
/*  the transient property.  The issue is that it ignores the border        */
/*  setting, and blindly sets this hint regardless of whether we want a     */
/*  border.                                                                 */
/*                                                                          */
/*  I believe a correct workaround is to unset this property for windows    */
/*  that want a border, *if* we happen to see the property set.             */
/* ======================================================================== */
#include "config.h"
#include "sdl_jzintv.h"
#include "gfx/gfx_sdl2_hooks.h"

#include <X11/Xutil.h>

#define PRINT_FLAG(flags,flag) \
    jzp_printf("gfx:  %-40s %4s %08X\n", #flag, \
                ((flags) & (flag)) != 0 ? "ON" : "off", \
                (int)((flags) & (flag)));

LOCAL void gfx_print_window_flags(SDL_Window *const window)
{
    uint32_t flags = SDL_GetWindowFlags(window);
    PRINT_FLAG(flags, SDL_WINDOW_FULLSCREEN);
    PRINT_FLAG(flags, SDL_WINDOW_FULLSCREEN_DESKTOP);
    PRINT_FLAG(flags, SDL_WINDOW_OPENGL);
    PRINT_FLAG(flags, SDL_WINDOW_VULKAN);
    PRINT_FLAG(flags, SDL_WINDOW_SHOWN);
    PRINT_FLAG(flags, SDL_WINDOW_HIDDEN);
    PRINT_FLAG(flags, SDL_WINDOW_BORDERLESS);
    PRINT_FLAG(flags, SDL_WINDOW_RESIZABLE);
    PRINT_FLAG(flags, SDL_WINDOW_MINIMIZED);
    PRINT_FLAG(flags, SDL_WINDOW_MAXIMIZED);
    PRINT_FLAG(flags, SDL_WINDOW_INPUT_GRABBED);
    PRINT_FLAG(flags, SDL_WINDOW_INPUT_FOCUS);
    PRINT_FLAG(flags, SDL_WINDOW_MOUSE_FOCUS);
    PRINT_FLAG(flags, SDL_WINDOW_FOREIGN);
#if SDL_VERSION_ATLEAST(2, 0, 1)
    PRINT_FLAG(flags, SDL_WINDOW_ALLOW_HIGHDPI);
#endif
#if SDL_VERSION_ATLEAST(2, 0, 4)
    PRINT_FLAG(flags, SDL_WINDOW_MOUSE_CAPTURE);
#endif
#if SDL_VERSION_ATLEAST(2, 0, 5)
    PRINT_FLAG(flags, SDL_WINDOW_ALWAYS_ON_TOP);
    PRINT_FLAG(flags, SDL_WINDOW_SKIP_TASKBAR);
    PRINT_FLAG(flags, SDL_WINDOW_UTILITY);
    PRINT_FLAG(flags, SDL_WINDOW_TOOLTIP);
    PRINT_FLAG(flags, SDL_WINDOW_POPUP_MENU);
#endif
}

void gfx_set_window_bordered_x11
(
    SDL_Window *const window,
    const SDL_bool    bordered
)
{
    jzp_printf("gfx:  gfx_set_window_bordered(%p, %d)\n",
               (void*)window, bordered);

    jzp_printf("gfx:  Window flags before:\n");
    gfx_print_window_flags(window);

    /* First, let SDL do its thing. */
    SDL_SetWindowBordered(window, bordered);

    jzp_printf("gfx:  Window flags after:\n");
    gfx_print_window_flags(window);

    /* If we were turning border off, just return.  Nothing to fix. */
    if (bordered == SDL_FALSE)
    {
        jzp_printf("gfx:  Bordered == False, so exiting.\n");
        return;
    }

    /* Try to get window manager info.  If it fails, just leave. */
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info))
    {
        jzp_printf("gfx:  SDL_GetWindowWMInfo failed: %s\n", SDL_GetError());
        return;
    }

    /* success */
    const char *subsystem = "an unknown system!";
    switch(info.subsystem) {
      case SDL_SYSWM_UNKNOWN:   break;
      case SDL_SYSWM_WINDOWS:   subsystem = "Microsoft Windows(TM)";  break;
      case SDL_SYSWM_X11:       subsystem = "X Window System";        break;
#if SDL_VERSION_ATLEAST(2, 0, 3)
      case SDL_SYSWM_WINRT:     subsystem = "WinRT";                  break;
#endif
      case SDL_SYSWM_DIRECTFB:  subsystem = "DirectFB";               break;
      case SDL_SYSWM_COCOA:     subsystem = "Apple OS X";             break;
      case SDL_SYSWM_UIKIT:     subsystem = "UIKit";                  break;
#if SDL_VERSION_ATLEAST(2, 0, 2)
      case SDL_SYSWM_WAYLAND:   subsystem = "Wayland";                break;
      case SDL_SYSWM_MIR:       subsystem = "Mir";                    break;
#endif
#if SDL_VERSION_ATLEAST(2, 0, 4)
      case SDL_SYSWM_ANDROID:   subsystem = "Android";                break;
#endif
#if SDL_VERSION_ATLEAST(2, 0, 5)
      case SDL_SYSWM_VIVANTE:   subsystem = "Vivante";                break;
#endif
      case SDL_SYSWM_OS2:       subsystem = "OS/2";                   break;
#if SDL_VERSION_ATLEAST(2, 0, 12)
      case SDL_SYSWM_HAIKU:     subsystem = "Haiku";                  break;
#endif
      default: break;
    }

    jzp_printf("gfx:  SDL version %d.%d.%d, subsystem %s (%d)\n",
               (int)info.version.major, (int)info.version.minor,
               (int)info.version.patch, subsystem, (int)info.subsystem);

    /* If this isn't X11, just leave. */
    if (info.subsystem != SDL_SYSWM_X11)
    {
        jzp_printf("gfx:  Subsystem is not SDL_SYSWM_X11\n");
        return;
    }

    Window   xwindow = info.info.x11.window;
    Display *display = info.info.x11.display;

    /* Set this to "#if 1" to try more conservative version. */
#if 0
    /* Do we have a WM_TRANSIENT_FOR hint? */
    Atom transient_hint =
        XInternAtom(display, "WM_TRANSIENT_FOR",
                    /* only_if_exists = */ True);

    /* No hint?  Nothing to fix, so just leave. */
    if (transient_hint == None)
    {
        jzp_printf("gfx:  transient_hint == None\n");
        return;
    }

    /* Otherwise, buh-bye! */
    jzp_printf(
        "gfx:  WM_TRANSIENT_FOR hint found.  Deleting hint.\n");
    int ret = XDeleteProperty(display, xwindow, transient_hint);
#else
    /* Buh-byte transient hint, if any. */
    jzp_printf(
        "gfx:  Proactively deleting any WM_TRANSIENT_FOR hint.\n");
    int ret = XDeleteProperty(display, xwindow,
        XInternAtom(display, "WM_TRANSIENT_FOR",
                    /* only_if_exists = */ False));
#endif
    jzp_printf("gfx:  XDeleteProperty returned %d: %s\n", ret,
                ret == 0         ? "Success"
              : ret == BadAtom   ? "BadAtom"
              : ret == BadWindow ? "BadWindow"
              :                    "unknown");

    ret = XFlush(display);
    jzp_printf("gfx:  XFlush returned %d\n", ret);
}
