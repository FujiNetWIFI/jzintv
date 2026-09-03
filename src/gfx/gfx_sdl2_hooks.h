/* ======================================================================== */
/*  Hooks for platform-specifc SDL2 workarounds and tweaks.                 */
/* ======================================================================== */
#ifndef GFX_SDL2_HOOKS_H_
#define GFX_SDL2_HOOKS_H_

#if PLAT_MACOS
extern bool gfx_set_srgb_colorspace_metal(void *metal_layer);
#define gfx_set_srgb_colorspace(r) \
    gfx_set_srgb_colorspace_metal(SDL_RenderGetMetalLayer(r))
#else
# define gfx_set_srgb_colorspace(r) ((void)r, false)
#endif

#if defined(X11_TRANSIENT_WORKAROUND)
extern void gfx_set_window_bordered_x11(SDL_Window *w, SDL_bool b);
# define gfx_set_window_bordered(w,b) \
    gfx_set_window_bordered_x11(w,b)
#else
# define gfx_set_window_bordered(w,b) SDL_SetWindowBordered(w,b)
#endif

#endif /* GFX_SDL2_HOOKS_H_ */
