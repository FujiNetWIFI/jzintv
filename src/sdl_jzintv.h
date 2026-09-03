/* Include the appropriate SDL headers based on which version we're building. */
#ifndef SDL_JZINTV_H_
#   define SDL_JZINTV_H_ 1
#   if !defined(USE_SDL)
#       error "sdl_jzintv.h must be #included after config.h"
#   endif
#   if USE_SDL == 0
#       error "sdl_jzintv.h included in a non-SDL dependent source file."
#   elif USE_SDL == 1
#       include <SDL/SDL.h>
#       include <SDL/SDL_audio.h>
#       include <SDL/SDL_events.h>
#       include <SDL/SDL_error.h>
#       if defined(__EMSCRIPTEN__)
#           include <SDL/SDL_mixer.h>
#       endif
#       include <SDL/SDL_thread.h>
#   elif USE_SDL == 2
#       include <SDL2/SDL.h>
#       include <SDL2/SDL_audio.h>
#       include <SDL2/SDL_events.h>
#       include <SDL2/SDL_error.h>
#       include <SDL2/SDL_thread.h>
#       include <SDL2/SDL_syswm.h>
#   else /* No SDL or unknown SDL */
        /* We should only set USE_SDL on files in SDL1_OBJS and SDL2_OBJS. */
#       error "Unknown SDL version."
#   endif 
#endif /* SDL_JZINTV_H_ */
