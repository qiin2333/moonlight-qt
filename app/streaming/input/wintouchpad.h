#pragma once

#ifdef HAVE_WINDOWS_RAW_TOUCHPAD

#include "SDL_compat.h"

#include <memory>

class SdlInputHandler;

class WindowsTouchpadInput
{
public:
    explicit WindowsTouchpadInput(SdlInputHandler* inputHandler);
    ~WindowsTouchpadInput();

    bool initialize(SDL_Window* window);

private:
    class Impl;
    std::unique_ptr<Impl> m_Impl;

    static void SDLCALL messageHook(void* userdata, void* hwnd,
                                    unsigned int message, Uint64 wParam, Sint64 lParam);
};

#endif
