// Host-side preview of the departure board: renders includes/departures.h with the real BDF
// font into a 128x64 ASCII frame, using live JSON from transport.opendata.ch.
//
// The esphome::display / esphome::font stubs below replicate the text measuring and alignment
// rules of ESPHome's display.cpp / font.cpp, so pixel positions match the device.
//
// Build (needs ArduinoJson 7 headers, e.g. from the ESPHome build directory):
//   c++ -std=c++17 -I <path-to-ArduinoJson/src> tools/host_preview.cpp -o /tmp/zvv-preview
// Run:
//   curl -s "$(/tmp/zvv-preview --url 8576193)" | /tmp/zvv-preview fonts/vbzfont.bdf [platform-filter]
//   ... | /tmp/zvv-preview fonts/vbzfont.bdf [platform-filter] --via "Schlyfi" --clock fonts/vbzfont-big.bdf
//   ... | /tmp/zvv-preview fonts/vbzfont.bdf --color Amber   (non-white text prints as 'o')
//   /tmp/zvv-preview --selftest fonts/vbzfont.bdf [fonts/vbzfont-big.bdf]

#include <ArduinoJson.h>

#include <cassert>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace esphome {

struct Color {
  uint8_t r{0}, g{0}, b{0};
  Color() = default;
  Color(uint8_t r, uint8_t g, uint8_t b) : r(r), g(g), b(b) {}
  static const Color BLACK;
  bool operator==(const Color &o) const { return r == o.r && g == o.g && b == o.b; }
};
const Color Color::BLACK{0, 0, 0};

namespace font {
struct Glyph {
  int advance, offset_x, offset_y, width, height;
  std::vector<std::vector<bool>> rows;
};

class Font {
 public:
  explicit Font(const std::string &bdf_path) { load(bdf_path); }
  std::map<uint32_t, Glyph> glyphs;
  int ascent{0}, descent{0}, missing{0};
  int height() const { return ascent + descent; }
  const Glyph *find(uint32_t cp) const {
    auto it = glyphs.find(cp);
    return it == glyphs.end() ? nullptr : &it->second;
  }
  int fallback_advance() const { return glyphs.empty() ? 0 : glyphs.begin()->second.advance; }

 private:
  void load(const std::string &path) {
    std::ifstream in(path);
    if (!in) {
      std::cerr << "cannot open " << path << "\n";
      std::exit(1);
    }
    std::string line;
    uint32_t enc = 0;
    Glyph g{};
    int bbx_w = 0, bbx_h = 0, bbx_x = 0, bbx_y = 0;
    bool in_bitmap = false;
    while (std::getline(in, line)) {
      std::istringstream ss(line);
      std::string key;
      ss >> key;
      if (key == "FONT_ASCENT") ss >> ascent;
      else if (key == "FONT_DESCENT") ss >> descent;
      else if (key == "ENCODING") ss >> enc;
      else if (key == "DWIDTH") ss >> g.advance;
      else if (key == "BBX") ss >> bbx_w >> bbx_h >> bbx_x >> bbx_y;
      else if (key == "BITMAP") {
        in_bitmap = true;
        g.rows.clear();
      } else if (key == "ENDCHAR") {
        in_bitmap = false;
        g.width = bbx_w;
        g.height = bbx_h;
        g.offset_x = bbx_x;
        g.offset_y = ascent - (bbx_h + bbx_y);  // ESPHome: ascender - bitmap_top
        glyphs[enc] = g;
        g = Glyph{};
      } else if (in_bitmap) {
        const unsigned long v = std::stoul(key, nullptr, 16);
        const int bits = static_cast<int>(key.size()) * 4;
        std::vector<bool> row(bbx_w);
        for (int c = 0; c < bbx_w; c++)
          row[c] = (v >> (bits - 1 - c)) & 1;
        g.rows.push_back(row);
      }
    }
  }
};
}  // namespace font

namespace display {
enum class TextAlign : uint8_t {
  TOP = 0x00,
  CENTER_VERTICAL = 0x01,
  BASELINE = 0x02,
  BOTTOM = 0x04,
  LEFT = 0x00,
  CENTER_HORIZONTAL = 0x08,
  RIGHT = 0x10,
  TOP_LEFT = TOP | LEFT,
  TOP_CENTER = TOP | CENTER_HORIZONTAL,
  TOP_RIGHT = TOP | RIGHT,
};

inline uint32_t next_cp(const char *&s) {
  const auto c = static_cast<unsigned char>(*s);
  if (!c)
    return 0;
  if (c < 0x80) {
    s += 1;
    return c;
  }
  if (c < 0xE0) {
    uint32_t cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
    s += 2;
    return cp;
  }
  if (c < 0xF0) {
    uint32_t cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    s += 3;
    return cp;
  }
  uint32_t cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
  s += 4;
  return cp;
}

class Display {
 public:
  static constexpr int W = 128, H = 64;
  Color px[H][W];

  int get_width() const { return W; }
  void fill(Color c) {
    for (auto &row : px)
      for (auto &p : row)
        p = c;
  }
  void draw_pixel_at(int x, int y, Color c) {
    if (x >= 0 && x < W && y >= 0 && y < H)
      px[y][x] = c;
  }
  void filled_rectangle(int x, int y, int w, int h, Color c) {
    for (int yy = y; yy < y + h; yy++)
      for (int xx = x; xx < x + w; xx++)
        draw_pixel_at(xx, yy, c);
  }
  void rectangle(int x, int y, int w, int h, Color c) {
    for (int xx = x; xx < x + w; xx++) {
      draw_pixel_at(xx, y, c);
      draw_pixel_at(xx, y + h - 1, c);
    }
    for (int yy = y; yy < y + h; yy++) {
      draw_pixel_at(x, yy, c);
      draw_pixel_at(x + w - 1, yy, c);
    }
  }

  // font.cpp Font::measure
  static void measure(font::Font *f, const char *s, int *width, int *x_offset) {
    int min_x = 0, x = 0;
    bool has = false;
    while (*s) {
      const uint32_t cp = next_cp(s);
      const auto *g = f->find(cp);
      if (!g) {
        x += f->fallback_advance();
        continue;
      }
      min_x = has ? std::min(min_x, x + g->offset_x) : g->offset_x;
      x += g->advance;
      has = true;
    }
    *x_offset = min_x;
    *width = x - min_x;
  }

  // display.cpp Display::get_text_bounds
  void get_text_bounds(int x, int y, const char *text, font::Font *f, TextAlign align, int *x1, int *y1, int *width,
                       int *height) {
    int x_offset;
    measure(f, text, width, &x_offset);
    *height = f->height();
    switch (static_cast<TextAlign>(static_cast<int>(align) & 0x18)) {
      case TextAlign::RIGHT: *x1 = x - *width - x_offset; break;
      case TextAlign::CENTER_HORIZONTAL: *x1 = x - (*width + x_offset) / 2; break;
      default: *x1 = x; break;
    }
    *y1 = y;
  }

  void print(int x, int y, font::Font *f, Color color, TextAlign align, const char *text) {
    int x1, y1, w, h;
    get_text_bounds(x, y, text, f, align, &x1, &y1, &w, &h);
    int x_at = x1;
    while (*text) {
      const uint32_t cp = next_cp(text);
      const auto *g = f->find(cp);
      if (!g) {
        f->missing++;
        std::cerr << "missing glyph U+" << std::hex << cp << std::dec << "\n";
        rectangle(x_at, y1, f->fallback_advance(), f->height(), color);
        x_at += f->fallback_advance();
        continue;
      }
      for (int r = 0; r < g->height; r++)
        for (int c = 0; c < g->width; c++)
          if (g->rows[r][c])
            draw_pixel_at(x_at + g->offset_x + c, y1 + g->offset_y + r, color);
      x_at += g->advance;
    }
  }
};
}  // namespace display
}  // namespace esphome

#include "../includes/departures.h"

static void dump(const esphome::display::Display &d) {
  std::cout << '+' << std::string(128, '-') << "+\n";
  for (int y = 0; y < 64; y++) {
    std::cout << '|';
    for (int x = 0; x < 128; x++) {
      const auto &p = d.px[y][x];
      char ch = ' ';
      if (p.r == 255 && p.g == 255 && p.b == 255) ch = '#';
      else if (p.r > 200 && p.g < 50 && p.b < 50) ch = 'r';
      else if (p.r || p.g || p.b) ch = 'o';
      std::cout << ch;
    }
    std::cout << "| " << y << "\n";
  }
  std::cout << '+' << std::string(128, '-') << "+\n";
}

// True if nothing is lit in the outermost column on the right, and (optionally) on the left and
// in the top and bottom rows. Text keeps 1 px off the panel edges. Line boxes are allowed at x 0
// (left = false), and the 5-row board's dots and tails at y 0 / 63 (top_bottom = false).
static bool edges_clear(const esphome::display::Display &d, const char *what, bool left = true,
                        bool top_bottom = true) {
  using esphome::display::Display;
  auto lit = [&](int x, int y) { return !(d.px[y][x] == esphome::Color::BLACK); };
  std::string bad;
  for (int y = 0; y < Display::H; y++) {
    if (lit(Display::W - 1, y)) bad += " x127,y" + std::to_string(y);
    if (left && lit(0, y)) bad += " x0,y" + std::to_string(y);
  }
  for (int x = 0; top_bottom && x < Display::W; x++) {
    if (lit(x, 0)) bad += " y0,x" + std::to_string(x);
    if (lit(x, Display::H - 1)) bad += " y63,x" + std::to_string(x);
  }
  if (!bad.empty())
    std::cerr << what << ": lit on the panel edge:" << bad.substr(0, 120) << "\n";
  return bad.empty();
}

static int selftest(esphome::display::Display &it, esphome::font::Font *font, esphome::font::Font *big) {
  using namespace zvv;
  struct Case {
    const char *in, *out;
  };
  const Case dest_cases[] = {
      {"Zürich, Hardturm", "Hardturm"},
      {"Zürich Tiefenbrunnen, Bahnhof", "Tiefenbrunnen"},
      {"Zürich Altstetten, Bahnhof N", "Altstetten N"},
      {"Zürich, Bahnhofstrasse/HB", "Bahnhofstrasse/HB"},
      {"Zürich HB", "Zürich HB"},
      {"Zürich Flughafen", "Flughafen"},
      {"Winterthur", "Winterthur"},
      {"Küsnacht ZH, Allmend", "Küsnacht ZH, Allmend"},
      {"Genève, Gare Cornavin", "Geneve, Gare Cornavin"},
      {"Zürich, Escher-Wyss-Platz", "Escher-Wyss-Platz"},
      {"Ägerisee & Co; \"x\"", "Agerisee + Co, x"},
  };
  int fails = 0;
  for (const auto &c : dest_cases) {
    const auto got = clean_destination(c.in);
    if (got != c.out) {
      std::cerr << "clean_destination(" << c.in << ") = '" << got << "', expected '" << c.out << "'\n";
      fails++;
    }
  }
  auto expect = [&](bool cond, const char *what) {
    if (!cond) {
      std::cerr << "FAILED: " << what << "\n";
      fails++;
    }
  };
  expect(line_label("T", "11") == "11", "tram label");
  expect(line_label("B", "912") == "912", "bus label");
  expect(line_label("S", "5") == "S5", "S-Bahn label");
  expect(line_label("IR", "36") == "IR36", "IR label");
  expect(platform_matches("C", ""), "empty filter matches");
  expect(platform_matches("C", "c, D"), "filter case/space");
  expect(!platform_matches("E", "C,D"), "filter rejects");
  expect(!platform_matches("", "C"), "missing platform rejected by filter");
  expect(compute_brightness(0, 12, 255, 1, 500, 1) == 12, "dark -> min");
  expect(compute_brightness(1, 12, 255, 1, 500, 1) == 12, "lux_low -> min");
  expect(compute_brightness(500, 12, 255, 1, 500, 1) == 255, "lux_high -> max");
  expect(compute_brightness(100000, 12, 255, 1, 500, 1) == 255, "very bright -> max");
  const int mid = compute_brightness(22, 12, 255, 1, 500, 1);
  expect(mid > 100 && mid < 160, "log midpoint");
  expect(compute_brightness(22, 12, 255, 1, 500, 2) < mid, "gamma > 1 is dimmer");
  expect(compute_brightness(NAN, 12, 200, 1, 500, 1) == 200, "NaN -> max");
  expect(in_window(23 * 60 + 30, 23 * 60, 6 * 60), "window wraps (late)");
  expect(in_window(5 * 60, 23 * 60, 6 * 60), "window wraps (early)");
  expect(!in_window(12 * 60, 23 * 60, 6 * 60), "outside wrapped window");
  expect(text_color("White") == esphome::Color(255, 255, 255), "text colour White");
  expect(text_color("Amber") == esphome::Color(255, 176, 0), "text colour Amber");
  expect(text_color("") == esphome::Color(255, 255, 255), "no text colour -> white");
  expect(text_color("bogus") == esphome::Color(255, 255, 255), "unknown text colour -> white");
  for (const auto &a : TEXT_COLORS) {
    int same = 0;
    for (const auto &b : TEXT_COLORS)
      same += std::string(a.name) == b.name;
    expect(same == 1, "text colour names unique");
    expect(!(text_color(a.name) == error_color()), "text colour differs from the error colour");
  }
  expect(in_window(13 * 60, 12 * 60, 14 * 60), "plain window");
  expect(!in_window(14 * 60, 12 * 60, 14 * 60), "window end exclusive");
  expect(!in_window(600, 600, 600), "empty window");

  expect(wrap_text(it, font, "No connection to transport.opendata.ch", MSG_MAX_W) ==
             std::vector<std::string>({"No connection to", "transport.", "opendata.ch"}),
         "overlong word wraps after a dot");
  expect(wrap_text(it, font, "a b\nc", MSG_MAX_W) == std::vector<std::string>({"a b", "c"}), "plain wrap unchanged");

  // Message wrapping: lines ending in N / y (1 px past their advance) and dots on the first line.
  it.fill(esphome::Color::BLACK);
  draw_message(it, font, "jiy NNNN yyyy NNNN yyyy NNNN yyyy NNNN yyyy NNNN yyyy NNNN yyyy NNNN yyyy NNNN",
               text_color("White"));
  expect(edges_clear(it, "wrapped message"), "wrapped message clear of all edges");

  // Direction: Via stop filter and the Directions summary.
  expect(fold("ZÜRICH, Äsch É") == "zürich, äsch é", "fold ASCII + umlauts");
  expect(split_terms(" a, ,b ,") == std::vector<std::string>({"a", "b"}), "split_terms");
  {
    std::string s = "aü";
    utf8_truncate(s, 2);
    expect(s == "a", "utf8_truncate drops a split character");
  }
  {
    // Bus 31 at Drusbergstrasse (no platforms, two directions) plus a tram with a platform.
    // passList[0] is the departure stop itself.
    const int64_t t0 = 1794600000;
    const std::string json = R"({"stationboard":[
      {"category":"B","number":"31","to":"Zürich, Hermetschloo",
       "stop":{"platform":null,"departureTimestamp":)" + std::to_string(t0 + 300) + R"(,"delay":0,"prognosis":{"departure":null}},
       "passList":[{"station":{"name":null}},{"station":{"name":"Zürich, Schlyfi"}},{"station":{"name":"Zürich, Kapfstrasse"}},{"station":{"name":"Zürich, Hermetschloo"}}]},
      {"category":"B","number":"31","to":"Zürich, Kienastenwies",
       "stop":{"platform":null,"departureTimestamp":)" + std::to_string(t0 + 400) + R"(,"delay":0,"prognosis":{"departure":null}},
       "passList":[{"station":{"name":"Zürich, Drusbergstrasse"}},{"station":{"name":"Zürich, Waserstrasse"}},{"station":{"name":"Zürich, Kienastenwies"}}]},
      {"category":"T","number":"6","to":"Zürich, Bahnhofplatz/HB",
       "stop":{"platform":"A","departureTimestamp":)" + std::to_string(t0 + 500) + R"(,"delay":0,"prognosis":{"departure":null}},
       "passList":[{"station":{"name":null}},{"station":{"name":"Zürich, Haldenegg"}},{"station":{"name":"Zürich, Central"}},{"station":{"name":"Zürich, Bahnhofplatz/HB"}}]},
      {"category":"B","number":"31","to":"Zürich, Herdernstrasse",
       "stop":{"platform":null,"departureTimestamp":)" + std::to_string(t0 + 600) + R"(,"delay":0,"prognosis":{"departure":null}},
       "passList":[{"station":{"name":null}},{"station":{"name":"Zürich, Schlyfi"}},{"station":{"name":"Zürich, Herdernstrasse"}}]}
    ]})";
    JsonDocument doc;
    expect(!deserializeJson(doc, json), "test stationboard JSON parses");
    const std::string all_dirs =
        "Schlyfi: 31 > Hermetschloo; Herdernstrasse | Waserstrasse: 31 > Kienastenwies | [A] Haldenegg: 6 > Bahnhofplatz/HB";
    auto run = [&](const char *platform, const char *via) {
      parse_stationboard(doc.as<JsonObject>(), platform, via, t0);
      std::string dests;
      for (const auto &d : board().departures)
        dests += (dests.empty() ? "" : ",") + d.dest;
      if (board().directions != all_dirs) {
        std::cerr << "directions = '" << board().directions << "'\n";
        fails++;
      }
      return dests;
    };
    auto expect_dests = [&](const char *platform, const char *via, const std::string &want) {
      const std::string got = run(platform, via);
      if (got != want) {
        std::cerr << "filter platform='" << platform << "' via='" << via << "': got '" << got << "', expected '" << want
                  << "'\n";
        fails++;
      }
    };
    expect_dests("", "", "Hermetschloo,Kienastenwies,Bahnhofplatz/HB,Herdernstrasse");
    expect_dests("", "schlyfi", "Hermetschloo,Herdernstrasse");
    expect_dests("", "Drusbergstrasse", "");  // the departure stop itself never matches
    expect_dests("", "foo, CENTRAL ,", "Bahnhofplatz/HB");  // terms are comma-separated
    expect_dests("", "waser,ÜRICH X", "Kienastenwies");
    expect_dests("", "ÜRICH", "Hermetschloo,Kienastenwies,Bahnhofplatz/HB,Herdernstrasse");  // Ü folds to ü
    expect_dests("", "Kienastenwies", "Kienastenwies");
    expect_dests("A", "", "Bahnhofplatz/HB");
    expect_dests("A", "Schlyfi", "");
    expect_dests("", " , ", "Hermetschloo,Kienastenwies,Bahnhofplatz/HB,Herdernstrasse");

    // Ticks and minutes keep off the right edge (line boxes sit at x 0 on purpose).
    // First departure 30 s away (tram icon), second live (thick tick), third delayed (">N").
    run("", "");
    board().departures[1].live = true;
    board().departures[2].live = true;
    board().departures[2].delay_min = 5;
    it.fill(esphome::Color::BLACK);
    draw_board(it, font, visible_rows(t0 + 270), false, text_color("White"));
    expect(edges_clear(it, "board", false, false), "board clear of the right edge");
  }
  {
    std::vector<DirectionGroup> many;
    for (int i = 0; i < 30; i++)
      add_direction(many, "", "Zürich Stop " + std::to_string(i), std::to_string(i), "Destination");
    const std::string s = directions_summary(many);
    expect(s.size() <= 255 && s.size() >= 250 && s.compare(s.size() - 3, 3, "...") == 0, "directions cut to 255");
  }

  const ClockInfo wed{9, 5, 3, 11, 11, 2026};
  expect(clock_time_str(wed) == "09:05", "clock time");
  expect(clock_date_str(wed) == "11 Nov 26", "clock date");
  expect(clock_date_str({0, 0, 0, 5, 1, 2027}) == "5 Jan 27", "clock date, single-digit day");
  expect(std::string(weekday_name(3)) == "Wednesday", "weekday name");
  expect(std::string(weekday_name(0)) == "Sunday", "weekday 0 = Sunday");
  expect(clock_signature(wed) != clock_signature({9, 6, 3, 11, 11, 2026}), "clock signature changes every minute");
  if (big) {
    // Ink (lit pixel) columns [left, right) of a string printed at x, from the glyph bitmaps.
    // Unlike text_width() this includes glyph parts that stick out past their advance.
    auto ink = [&](esphome::font::Font *f, const char *s, int x, TextAlign align) {
      int x1, y1, w, h;
      it.get_text_bounds(x, 0, s, f, align, &x1, &y1, &w, &h);
      int left = 1 << 20, right = -(1 << 20);
      for (const char *p = s; *p;) {
        const auto *g = f->find(esphome::display::next_cp(p));
        if (g && g->width) {
          left = std::min(left, x1 + g->offset_x);
          right = std::max(right, x1 + g->offset_x + g->width);
        }
        x1 += g ? g->advance : f->fallback_advance();
      }
      return std::make_pair(left, right);
    };
    int time_right = 0;  // widest possible time
    for (const char *t : {"00:00", "08:08", "23:58"})
      time_right = std::max(time_right, ink(big, t, CLOCK_TIME_X, TextAlign::TOP_LEFT).second);
    expect(ink(big, "08:08", CLOCK_TIME_X, TextAlign::TOP_LEFT).first == 1, "time starts 1 px off the left edge");
    for (int m = 1; m <= 12; m++) {
      const std::string s = clock_date_str({0, 0, 0, 28, m, 2026});
      const auto date = ink(font, s.c_str(), CLOCK_DATE_RIGHT_X, TextAlign::TOP_RIGHT);
      if (date.first < time_right + 2 || date.second != 127) {
        std::cerr << s << ": ink x " << date.first << ".." << date.second - 1 << ", time ends at x "
                  << time_right - 1 << " (need a 2 px gap and ink ending at x 126)\n";
        fails++;
      }
    }
    for (int d = 0; d < 7; d++) {
      const auto wd = ink(font, weekday_name(d), CLOCK_WEEKDAY_RIGHT_X, TextAlign::TOP_RIGHT);
      if (wd.first < time_right + 2 || wd.second != 127) {
        std::cerr << weekday_name(d) << ": ink x " << wd.first << ".." << wd.second - 1 << ", time ends at x "
                  << time_right - 1 << " (need a 2 px gap and ink ending at x 126)\n";
        fails++;
      }
    }
    // Widest weekday over a date with a "p" tail, under three message lines with dots on top.
    it.fill(esphome::Color::BLACK);
    draw_message(it, font, "jiy NNNN yyyy NNNN yyyy NNNN yyyy NNNN yyyy NNNN", text_color("White"),
                 CLOCK_DEPARTURE_ROWS);
    draw_clock(it, big, font, {23, 58, 3, 28, 9, 2026}, text_color("White"));
    expect(edges_clear(it, "clock + message"), "clock and message clear of all edges");
  }
  std::cout << (fails ? "selftest FAILED" : "selftest OK") << " (" << fails << " failures)\n";
  return fails ? 1 : 0;
}

int main(int argc, char **argv) {
  if (argc >= 3 && std::string(argv[1]) == "--url") {
    std::cout << zvv::stationboard_url(argv[2], 15) << "\n";
    return 0;
  }
  if (argc >= 3 && std::string(argv[1]) == "--selftest") {
    esphome::font::Font font(argv[2]);
    std::unique_ptr<esphome::font::Font> big;
    if (argc >= 4)
      big = std::make_unique<esphome::font::Font>(argv[3]);
    esphome::display::Display it;
    int rc = selftest(it, &font, big.get());
    // Render the static screens too; none of them may touch a panel edge.
    int edge_fails = 0;
    it.fill(esphome::Color::BLACK);
    const esphome::Color white = zvv::text_color("White");
    zvv::draw_splash(it, &font, white);
    dump(it);
    edge_fails += !edges_clear(it, "splash");
    it.fill(esphome::Color::BLACK);
    zvv::draw_message(it, &font, "connect to:\nzvv-schedule\npwd: zvv-setup\n192.168.4.1", white);
    dump(it);
    edge_fails += !edges_clear(it, "AP message");
    it.fill(esphome::Color::BLACK);
    zvv::draw_message(it, &font, "No connection to transport.opendata.ch", zvv::error_color());
    dump(it);
    edge_fails += !edges_clear(it, "error message");
    if (big) {
      it.fill(esphome::Color::BLACK);
      zvv::draw_message(it, &font, "keine Abfahrten", white, zvv::CLOCK_DEPARTURE_ROWS);
      zvv::draw_clock(it, big.get(), &font, {9, 5, 3, 11, 11, 2026}, white);
      dump(it);
      edge_fails += !edges_clear(it, "clock screen");
    }
    if (edge_fails) {
      std::cout << "edge check FAILED on " << edge_fails << " screen(s)\n";
      rc = 1;
    }
    return rc || font.missing || (big && big->missing);
  }
  std::vector<std::string> args;
  std::unique_ptr<esphome::font::Font> big;  // --clock: draw the clock band like the "Show clock" switch
  std::string via;                           // --via: like the "Via stop" text entity
  std::string color = "White";               // --color: like the "Text color" select
  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "--clock" && i + 1 < argc)
      big = std::make_unique<esphome::font::Font>(argv[++i]);
    else if (std::string(argv[i]) == "--via" && i + 1 < argc)
      via = argv[++i];
    else if (std::string(argv[i]) == "--color" && i + 1 < argc)
      color = argv[++i];
    else
      args.push_back(argv[i]);
  }
  if (args.empty()) {
    std::cerr << "usage: zvv-preview <vbzfont.bdf> [platform-filter] [--via <stops>] [--clock <vbzfont-big.bdf>]"
                 " [--color <name>] < stationboard.json\n";
    return 1;
  }
  esphome::font::Font font(args[0]);
  const std::string filter = args.size() >= 2 ? args[1] : "";
  std::stringstream buf;
  buf << std::cin.rdbuf();
  std::cout << "response: " << buf.str().size() << " bytes\n";
  JsonDocument doc;
  if (deserializeJson(doc, buf.str())) {
    std::cerr << "invalid JSON\n";
    return 1;
  }
  const int64_t now = std::time(nullptr);
  if (!zvv::parse_stationboard(doc.as<JsonObject>(), filter, via, now)) {
    std::cerr << zvv::board().last_error << "\n";
    return 1;
  }
  std::cout << "directions: " << zvv::board().directions << "\n";
  for (const auto &d : zvv::board().departures)
    std::cout << d.label << " [" << d.category << "] " << d.dest << " pl=" << d.platform << " +" << d.delay_min
              << (d.live ? " live" : " sched") << " in " << (d.est_ts() - now) << "s\n";
  esphome::display::Display it;
  it.fill(esphome::Color::BLACK);
  const auto rows = zvv::visible_rows(now, big ? zvv::CLOCK_DEPARTURE_ROWS : zvv::MAX_ROWS);
  const esphome::Color fg = zvv::text_color(color);
  zvv::draw_board(it, &font, rows, false, fg);
  std::string sig = zvv::board_signature(rows, false);
  if (big) {
    const std::time_t t = now;
    const std::tm *lt = std::localtime(&t);
    const zvv::ClockInfo ci{lt->tm_hour, lt->tm_min, lt->tm_wday, lt->tm_mday, lt->tm_mon + 1, lt->tm_year + 1900};
    zvv::draw_clock(it, big.get(), &font, ci, fg);
    sig += "|" + zvv::clock_signature(ci);
  }
  sig += "|" + color;
  dump(it);
  std::cout << "signature: " << sig << "\n";
  return (font.missing || (big && big->missing)) ? 2 : 0;
}
