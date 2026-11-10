// Off-screen frame for the HUB75 panel (device only, not used by tools/host_preview.cpp).
//
// With double_buffer off, every HUB75 drawing call writes straight into the DMA buffer the
// panel is scanning out, so "clear, then redraw" shows a black or half-drawn frame for a
// moment. A second 64 KB DMA buffer doesn't fit next to Wi-Fi on a WROOM-32, so instead each
// frame is drawn here (RGB565, 16 KB) and copied to the panel in one draw_pixels_at() call:
// every panel pixel goes straight from its old to its new value, never through black.
#pragma once

#include <algorithm>
#include <cstdint>

#include "esphome/components/display/display.h"

namespace zvv {

class Canvas : public esphome::display::Display {
 public:
  static constexpr int WIDTH = 128;
  static constexpr int HEIGHT = 64;

  void draw_pixel_at(int x, int y, esphome::Color color) override {
    if (x < 0 || y < 0 || x >= WIDTH || y >= HEIGHT || this->is_point_clipped(x, y))
      return;
    buf_[y * WIDTH + x] = esphome::display::ColorUtil::color_to_565(color);
  }

  void fill(esphome::Color color) override {
    std::fill(std::begin(buf_), std::end(buf_), esphome::display::ColorUtil::color_to_565(color));
  }

  // Copy the whole frame to the panel (native little-endian RGB565, tightly packed).
  void present(esphome::display::Display &dst) {
    dst.draw_pixels_at(0, 0, WIDTH, HEIGHT, reinterpret_cast<const uint8_t *>(buf_),
                       esphome::display::COLOR_ORDER_RGB, esphome::display::COLOR_BITNESS_565, false);
  }

  void update() override {}
  esphome::display::DisplayType get_display_type() override { return esphome::display::DISPLAY_TYPE_COLOR; }

 protected:
  int get_width_internal() override { return WIDTH; }
  int get_height_internal() override { return HEIGHT; }

  uint16_t buf_[WIDTH * HEIGHT]{};
};

}  // namespace zvv
