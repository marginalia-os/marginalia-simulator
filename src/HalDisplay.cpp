#include "HalDisplay.h"

#include <GfxRenderer.h>
#include <SDL2/SDL.h>

#include <atomic>
#include <cstring>
#include <iostream>

static SDL_Window* window = nullptr;
static SDL_Renderer* sdl_renderer = nullptr;
static SDL_Texture* texture = nullptr;

// Pixel buffer written by the render task, read by the main thread for SDL_RenderPresent.
// On macOS, SDL calls must happen on the main thread.
static uint32_t pixelBuf[HalDisplay::DISPLAY_WIDTH * HalDisplay::DISPLAY_HEIGHT];
static std::atomic<bool> pendingPresent{false};
// Written by HalGPIO::update() (which owns SDL event polling); read by shouldQuit().
std::atomic<bool> quitRequested{false};

static GfxRenderer::Orientation currentOrientation = GfxRenderer::Portrait;

void HalDisplay::setSimulatorOrientation(int o) { currentOrientation = static_cast<GfxRenderer::Orientation>(o); }

HalDisplay::HalDisplay() {}
HalDisplay::~HalDisplay() {}

// X4: 4.3" panel, 800×480 buffer — display at 0.5× (400×240 window in landscape).
// X3: 3.7" panel, 792×528 buffer — display at 0.5× (396×264 window in landscape).
// Both use the same 0.5× scale; the different buffer dimensions already produce the
// correctly proportioned window for each device.
#if defined(SIMULATOR_DEVICE_X3)
static constexpr double WINDOW_SCALE = 0.5;
static constexpr const char* WINDOW_TITLE = "Simulator - XTEINK X3";
#else
static constexpr double WINDOW_SCALE = 0.5;
static constexpr const char* WINDOW_TITLE = "Simulator - XTEINK X4";
#endif

void HalDisplay::begin() {
  if (SDL_Init(SDL_INIT_VIDEO) < 0) {
    std::cerr << "SDL could not initialize! SDL_Error: " << SDL_GetError() << std::endl;
    return;
  }

  bool isPortrait =
      (currentOrientation == GfxRenderer::Portrait || currentOrientation == GfxRenderer::PortraitInverted);
  int winW = static_cast<int>(isPortrait ? DISPLAY_HEIGHT * WINDOW_SCALE : DISPLAY_WIDTH * WINDOW_SCALE);
  int winH = static_cast<int>(isPortrait ? DISPLAY_WIDTH * WINDOW_SCALE : DISPLAY_HEIGHT * WINDOW_SCALE);

  // SDL_WINDOW_ALLOW_HIGHDPI lets the renderer use full Retina/HiDPI pixels on macOS
  // so we get crisp 1:1 rendering instead of a blurry upscale.
  window = SDL_CreateWindow(WINDOW_TITLE, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, winW, winH,
                            SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
  sdl_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

  // Keep all rendering logic in logical (winW×winH) coordinates; SDL maps to drawable pixels.
  SDL_RenderSetLogicalSize(sdl_renderer, winW, winH);

  // Linear filtering: Bayer-dithered pixels average to correct gray at scaled sizes
  // rather than showing harsh black/white patterns.
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
  texture = SDL_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, DISPLAY_WIDTH,
                              DISPLAY_HEIGHT);
}

void HalDisplay::clearScreen(uint8_t color) const { memset(getFrameBuffer(), color, BUFFER_SIZE); }

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool) const {
  uint8_t* fb = getFrameBuffer();
  const uint16_t imageWidthBytes = w / 8;
  for (uint16_t row = 0; row < h; row++) {
    const uint16_t destY = y + row;
    if (destY >= DISPLAY_HEIGHT) break;
    const uint16_t destOffset = destY * DISPLAY_WIDTH_BYTES + (x / 8);
    const uint16_t srcOffset = row * imageWidthBytes;
    for (uint16_t col = 0; col < imageWidthBytes; col++) {
      if ((x / 8 + col) >= DISPLAY_WIDTH_BYTES) break;
      fb[destOffset + col] = imageData[srcOffset + col];
    }
  }
}

void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      bool) const {
  uint8_t* fb = getFrameBuffer();
  const uint16_t imageWidthBytes = w / 8;
  for (uint16_t row = 0; row < h; row++) {
    const uint16_t destY = y + row;
    if (destY >= DISPLAY_HEIGHT) break;
    const uint16_t destOffset = destY * DISPLAY_WIDTH_BYTES + (x / 8);
    const uint16_t srcOffset = row * imageWidthBytes;
    for (uint16_t col = 0; col < imageWidthBytes; col++) {
      if ((x / 8 + col) >= DISPLAY_WIDTH_BYTES) break;
      fb[destOffset + col] &= imageData[srcOffset + col];
    }
  }
}

void HalDisplay::displayBuffer(RefreshMode mode, bool turnOffScreen) { refreshDisplay(mode, turnOffScreen); }

// Called from the render task (background thread): convert framebuffer to pixels and flag for present.
void HalDisplay::refreshDisplay(RefreshMode /*mode*/, bool /*turnOffScreen*/) {
  const uint8_t* fb = getFrameBuffer();
  for (int y = 0; y < DISPLAY_HEIGHT; y++) {
    for (int x = 0; x < DISPLAY_WIDTH; x++) {
      int byteIdx = (y * DISPLAY_WIDTH + x) / 8;
      int bitIdx = 7 - (x % 8);
      bool isWhite = (fb[byteIdx] & (1 << bitIdx)) != 0;
      pixelBuf[y * DISPLAY_WIDTH + x] = isWhite ? 0xFFFFFFFF : 0xFF000000;
    }
  }
  pendingPresent.store(true);
}

// Called from the main thread (simulator_main.cpp) to push pixels to SDL.
void HalDisplay::presentIfNeeded() {
  if (!pendingPresent.load()) return;
  pendingPresent.store(false);

  if (!texture || !sdl_renderer) return;

  SDL_UpdateTexture(texture, nullptr, pixelBuf, DISPLAY_WIDTH * sizeof(uint32_t));
  SDL_RenderClear(sdl_renderer);

  // For portrait modes the texture (landscape 800×480) must be rotated to fill the
  // portrait window.  SDL_RenderCopyEx rotates around the centre of dst, so dst must
  // be landscape-oriented and offset so its centre coincides with the window centre.
  // Portrait stores content rotated 90° CCW → undo with +90° CW.
  // PortraitInverted stores content rotated 90° CW → undo with -90°.
  const int scaledW = static_cast<int>(DISPLAY_WIDTH * WINDOW_SCALE);
  const int scaledH = static_cast<int>(DISPLAY_HEIGHT * WINDOW_SCALE);

  switch (currentOrientation) {
    case GfxRenderer::Portrait: {
      // Texture is landscape (scaledW×scaledH); rotate 90° CW to fill the portrait window.
      // dst must be landscape-oriented and centred in the portrait window so that
      // after rotation the result fills the window exactly.
      SDL_Rect dst = {-(scaledW - scaledH) / 2, (scaledW - scaledH) / 2, scaledW, scaledH};
      SDL_RenderCopyEx(sdl_renderer, texture, nullptr, &dst, 90.0, nullptr, SDL_FLIP_NONE);
      break;
    }
    case GfxRenderer::PortraitInverted: {
      SDL_Rect dst = {-(scaledW - scaledH) / 2, (scaledW - scaledH) / 2, scaledW, scaledH};
      SDL_RenderCopyEx(sdl_renderer, texture, nullptr, &dst, -90.0, nullptr, SDL_FLIP_NONE);
      break;
    }
    default: {
      SDL_Rect dst = {0, 0, scaledW, scaledH};
      SDL_RenderCopy(sdl_renderer, texture, nullptr, &dst);
      break;
    }
  }

  SDL_RenderPresent(sdl_renderer);
}

bool HalDisplay::shouldQuit() const { return quitRequested.load(); }

void HalDisplay::deepSleep() {}

uint8_t* HalDisplay::getFrameBuffer() const {
  static uint8_t buffer[HalDisplay::BUFFER_SIZE];
  return buffer;
}

void HalDisplay::copyGrayscaleBuffers(const uint8_t*, const uint8_t*) {}
void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t*) {}
void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t*) {}
void HalDisplay::cleanupGrayscaleBuffers(const uint8_t*) {}
void HalDisplay::displayGrayBuffer(bool) {}

uint16_t HalDisplay::getDisplayWidth() const { return DISPLAY_WIDTH; }
uint16_t HalDisplay::getDisplayHeight() const { return DISPLAY_HEIGHT; }
uint16_t HalDisplay::getDisplayWidthBytes() const { return DISPLAY_WIDTH_BYTES; }
uint32_t HalDisplay::getBufferSize() const { return BUFFER_SIZE; }

HalDisplay display;
