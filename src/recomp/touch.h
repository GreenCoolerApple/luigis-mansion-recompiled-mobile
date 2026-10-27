#pragma once
#include <SDL3/SDL_events.h>

namespace recomp::touch {

void init();
bool handleEvent(const SDL_Event& ev);
void draw(bool menuOpen);
bool active();
bool available();
void shutdown();

}
