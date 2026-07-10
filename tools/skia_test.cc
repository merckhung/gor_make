#include <iostream>
#include <vector>
#include <cstdarg>
#include <cstdio>
#include <SDL2/SDL.h>

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkFont.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkSurface.h"

void SkDebugf(const char format[], ...) {
  va_list args;
  va_start(args, format);
  vprintf(format, args);
  va_end(args);
}

int main(int argc, char** argv) {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::cerr << "SDL_Init Error: " << SDL_GetError() << std::endl;
    return 1;
  }

  int width = 800;
  int height = 600;
  SDL_Window* window = SDL_CreateWindow(
      "Skia + SDL2 Test", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      width, height, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  if (!window) {
    std::cerr << "SDL_CreateWindow Error: " << SDL_GetError() << std::endl;
    SDL_Quit();
    return 1;
  }

  SDL_Surface* window_surface = SDL_GetWindowSurface(window);

  SkImageInfo info = SkImageInfo::MakeN32Premul(width, height);
  sk_sp<SkSurface> sk_surface = SkSurfaces::WrapPixels(info, window_surface->pixels, window_surface->pitch);
  if (!sk_surface) {
    std::cerr << "Failed to create Skia surface" << std::endl;
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  SkCanvas* canvas = sk_surface->getCanvas();
  canvas->clear(SK_ColorWHITE);

  SkPaint paint;
  paint.setColor(SK_ColorBLUE);
  paint.setAntiAlias(true);
  canvas->drawCircle(400, 300, 100, paint);

  SDL_UpdateWindowSurface(window);

  std::cout << "Skia + SDL2 initialized successfully!" << std::endl;

  bool running = true;
  SDL_Event event;
  int frames = 0;
  while (running && frames < 10) {
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) running = false;
    }
    SDL_Delay(16);
    frames++;
  }

  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
