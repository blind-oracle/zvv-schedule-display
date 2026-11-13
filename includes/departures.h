// Departure board helpers for zvv-schedule.yaml
//
// Ported from https://github.com/sschueller/vbz-fahrgastinformation (MIT, (c) 2022 Stefan Schüller):
// the layout, VBZ line colours and destination clean-up follow lib/Display/src/Display.cpp.
// Data now comes from https://transport.opendata.ch/v1/stationboard (JSON) instead of TRIAS.
//
// This file is included from main.cpp after "esphome.h", so it must not reference YAML ids
// (they are declared later). Everything is passed in as arguments.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace zvv {

using esphome::Color;
using esphome::display::Display;
using esphome::display::TextAlign;

// ---------------------------------------------------------------------------
// Layout constants (pixel positions taken from the original Display.cpp)
// ---------------------------------------------------------------------------
static constexpr int ROW_PITCH = 13;      // original: position * 13
static constexpr int MAX_ROWS = 5;
static constexpr int LINE_BOX_W = 24;     // fillRect(0, y, 24, 11)
static constexpr int LINE_BOX_H = 11;
static constexpr int DEST_X = 27;
static constexpr int DEST_MAX_W = 74;     // up to the time column (no low-floor icon any more)
static constexpr int TIME_RIGHT_X = 123;  // original: cursor = 123 - advance
static constexpr int TRAM_ICON_X = 116;   // original: 108 + 16 - 8
static constexpr int TICK_X = 125;
static constexpr int LATE_MIN = 3;        // original lateMinCutoff
static constexpr size_t MAX_KEEP = 15;

static const char *const TRAM_ICON = "\xEE\x80\x80";  // U+E000 in vbzfont.bdf
static const char *const TICK_LIVE = "`";
static const char *const TICK_SCHEDULED = "'";

inline Color text_color() { return Color(255, 255, 255); }  // original "vbzYellow" is white
inline Color error_color() { return Color(255, 0, 0); }

// ---------------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------------
struct Departure {
  std::string label;     // "11", "S5", "IR36"
  std::string category;  // "T", "B", "S", ...
  std::string dest;      // cleaned, font-safe UTF-8
  std::string platform;
  int64_t sched_ts{0};   // unix seconds
  int delay_min{0};
  bool live{false};

  int64_t est_ts() const { return sched_ts + static_cast<int64_t>(delay_min) * 60; }
  bool late() const { return live && delay_min >= LATE_MIN; }
};

struct BoardState {
  std::vector<Departure> departures;
  int64_t last_ok_ts{0};  // unix time of the last successful fetch
  std::string last_error;
};

inline BoardState &board() {
  static BoardState s;
  return s;
}

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------
inline std::string trim(const std::string &s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos)
    return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

inline bool starts_with(const std::string &s, const std::string &p) {
  return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

inline void replace_all(std::string &s, const std::string &from, const std::string &to) {
  if (from.empty())
    return;
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
}

inline std::string to_upper_ascii(std::string s) {
  for (auto &c : s)
    if (c >= 'a' && c <= 'z')
      c = static_cast<char>(c - 'a' + 'A');
  return s;
}

// Characters that exist in fonts/vbzfont.bdf (besides ä ö ü and the icons).
inline bool font_has_ascii(char c) {
  static const char *const SUPPORTED =
      " !'()*+,-./0123456789:<>?ABCDEFGHIJKLMNOPQRSTUVWXYZ`abcdefghijklmnopqrstuvwxyz";
  return c != '\0' && std::strchr(SUPPORTED, c) != nullptr;
}

// Replace characters the font cannot render (otherwise ESPHome draws a box).
inline std::string font_safe(const std::string &in) {
  struct Map {
    const char *from;
    const char *to;
  };
  static const Map MAP[] = {
      {"Ä", "A"},  {"Ö", "O"},  {"Ü", "U"},  {"ß", "ss"}, {"é", "e"},  {"è", "e"},  {"ê", "e"},
      {"ë", "e"},  {"É", "E"},  {"È", "E"},  {"à", "a"},  {"â", "a"},  {"á", "a"},  {"À", "A"},
      {"ô", "o"},  {"ó", "o"},  {"ò", "o"},  {"î", "i"},  {"ï", "i"},  {"í", "i"},  {"ì", "i"},
      {"û", "u"},  {"ù", "u"},  {"ú", "u"},  {"ç", "c"},  {"Ç", "C"},  {"ñ", "n"},  {"’", "'"},
      {"–", "-"},  {"—", "-"},  {"&", "+"},  {";", ","},  {"\"", ""}, {"_", "-"},  {"=", "-"},
  };
  std::string s = in;
  for (const auto &m : MAP)
    replace_all(s, m.from, m.to);

  // Keep supported ASCII and the three umlauts; drop everything else.
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      if (font_has_ascii(static_cast<char>(c)))
        out += static_cast<char>(c);
      i++;
      continue;
    }
    size_t len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
    std::string seq = s.substr(i, len);
    if (seq == "ä" || seq == "ö" || seq == "ü")
      out += seq;
    i += len;
  }
  return out;
}

// "Zürich, Hardturm" -> "Hardturm"; "Zürich Tiefenbrunnen, Bahnhof" -> "Tiefenbrunnen";
// "Zürich Altstetten, Bahnhof N" -> "Altstetten N"; "Zürich HB" stays "Zürich HB".
inline std::string clean_destination(const std::string &raw) {
  std::string s = trim(raw);
  // Drop ", Bahnhof" only as a whole word (keep "Zürich, Bahnhofstrasse/HB").
  static const std::string BHF = ", Bahnhof";
  for (size_t pos = s.find(BHF); pos != std::string::npos; pos = s.find(BHF, pos)) {
    const size_t end = pos + BHF.size();
    if (end == s.size() || s[end] == ' ' || s[end] == ',')
      s.erase(pos, BHF.size());
    else
      pos = end;
  }
  if (starts_with(s, "Bahnhof "))  // original firmware removed "Bahnhof "
    s = s.substr(8);

  static const char *const CITIES[] = {"Zürich", "Winterthur"};
  for (const char *city : CITIES) {
    const std::string comma = std::string(city) + ", ";
    const std::string space = std::string(city) + " ";
    if (starts_with(s, comma)) {
      s = s.substr(comma.size());
      break;
    }
    if (starts_with(s, space) && s.size() - space.size() > 3) {
      s = s.substr(space.size());
      break;
    }
  }
  return font_safe(trim(s));
}

inline void pop_utf8_char(std::string &s) {
  if (s.empty())
    return;
  size_t i = s.size() - 1;
  while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80)
    i--;
  s.erase(i);
}

inline int text_width(Display &it, esphome::font::Font *font, const std::string &s) {
  if (s.empty())
    return 0;
  int x1, y1, w, h;
  it.get_text_bounds(0, 0, s.c_str(), font, TextAlign::TOP_LEFT, &x1, &y1, &w, &h);
  return w;
}

// Same idea as Display::cropDestination: cut characters until it fits, then add ".".
inline std::string crop_to_width(Display &it, esphome::font::Font *font, std::string s, int max_w) {
  if (text_width(it, font, s) <= max_w)
    return s;
  const int dot_w = text_width(it, font, ".");
  while (!s.empty() && text_width(it, font, s) + dot_w > max_w)
    pop_utf8_char(s);
  s = trim(s);
  if (!s.empty() && (s.back() == ',' || s.back() == '-' || s.back() == '/'))
    s.pop_back();
  return s + ".";
}

// Greedy word wrap for status / error screens.
inline std::vector<std::string> wrap_text(Display &it, esphome::font::Font *font, const std::string &text, int max_w) {
  std::vector<std::string> lines;
  std::string current;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t end = text.find_first_of(" \n", pos);
    if (end == std::string::npos)
      end = text.size();
    std::string word = text.substr(pos, end - pos);
    std::string candidate = current.empty() ? word : current + " " + word;
    if (!current.empty() && text_width(it, font, candidate) > max_w) {
      lines.push_back(current);
      current = word;
    } else {
      current = candidate;
    }
    if (end < text.size() && text[end] == '\n') {
      lines.push_back(current);
      current.clear();
    }
    pos = end + 1;
  }
  if (!current.empty())
    lines.push_back(current);
  return lines;
}

// ---------------------------------------------------------------------------
// VBZ line colours (Display::getVbzBackgroundColor / getVbzFontColor).
// The original keyed these on the TRIAS LineRef "...:9100X:..."; here it is the tram number.
// ---------------------------------------------------------------------------
inline bool is_local_category(const std::string &cat) {
  static const char *const LOCAL[] = {"T", "B", "BUS", "TRO", "NFT", "NFB", "NFO", "TX", "BAT", "FUN", "M", "PB"};
  const std::string up = to_upper_ascii(cat);
  for (const char *c : LOCAL)
    if (up == c)
      return true;
  return false;
}

inline std::string line_label(const std::string &category, const std::string &number) {
  if (number.empty())
    return category;
  if (category.empty() || is_local_category(category))
    return number;
  return category + number;  // "S5", "IR36"
}

inline Color line_bg(const Departure &d) {
  if (d.category != "T")
    return Color(0, 0, 0);
  const int n = std::atoi(d.label.c_str());
  switch (n) {
    case 2: return Color(229, 0, 0);
    case 3: return Color(0, 138, 41);
    case 4: return Color(14, 37, 110);
    case 5: return Color(116, 69, 30);
    case 6: return Color(204, 128, 52);
    case 7: return Color(0, 0, 0);
    case 8: return Color(137, 183, 0);
    case 9: return Color(15, 38, 113);
    case 10: return Color(228, 29, 113);
    case 11: return Color(0, 138, 41);
    case 13: return Color(255, 194, 0);
    case 14: return Color(0, 140, 199);
    case 15: return Color(228, 0, 0);
    case 17: return Color(144, 29, 77);
    default: return Color(0, 0, 0);
  }
}

inline Color line_fg(const Departure &d) {
  if (d.category == "T") {
    const int n = std::atoi(d.label.c_str());
    if (n == 8 || n == 13)
      return Color(0, 0, 0);
  }
  return Color(255, 255, 255);
}

// ---------------------------------------------------------------------------
// transport.opendata.ch parsing
// ---------------------------------------------------------------------------
inline bool platform_matches(const std::string &platform, const std::string &filter) {
  const std::string f = trim(filter);
  if (f.empty())
    return true;
  const std::string p = to_upper_ascii(trim(platform));
  size_t pos = 0;
  while (pos <= f.size()) {
    size_t end = f.find(',', pos);
    if (end == std::string::npos)
      end = f.size();
    if (to_upper_ascii(trim(f.substr(pos, end - pos))) == p)
      return true;
    pos = end + 1;
  }
  return false;
}

// Fills board().departures. Returns false (and sets last_error) if the document is unusable.
inline bool parse_stationboard(JsonObject root, const std::string &platform_filter, int64_t now) {
  JsonArray items = root["stationboard"].as<JsonArray>();
  if (items.isNull()) {
    board().last_error = "Unexpected API response";
    return false;
  }
  std::vector<Departure> out;
  for (JsonObject j : items) {
    JsonObject stop = j["stop"];
    Departure d;
    d.category = j["category"] | "";
    d.label = line_label(d.category, j["number"] | "");
    d.dest = clean_destination(j["to"] | "");
    d.platform = stop["platform"] | "";
    d.sched_ts = stop["departureTimestamp"] | static_cast<int64_t>(0);
    d.delay_min = stop["delay"] | 0;
    d.live = !stop["prognosis"]["departure"].isNull();
    if (d.sched_ts <= 0 || !platform_matches(d.platform, platform_filter))
      continue;
    out.push_back(std::move(d));
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const Departure &a, const Departure &b) { return a.est_ts() < b.est_ts(); });
  if (out.size() > MAX_KEEP)
    out.resize(MAX_KEEP);
  board().departures = std::move(out);
  board().last_ok_ts = now;
  board().last_error.clear();
  return true;
}

inline std::string stationboard_url(const std::string &station_id, size_t limit) {
  std::string url = "http://transport.opendata.ch/v1/stationboard?id=" + trim(station_id) +
                    "&limit=" + std::to_string(limit);
  static const char *const FIELDS[] = {
      "stationboard/category",          "stationboard/number",
      "stationboard/to",                "stationboard/stop/platform",
      "stationboard/stop/departureTimestamp", "stationboard/stop/delay",
      "stationboard/stop/prognosis/departure",
  };
  for (const char *f : FIELDS)
    url += std::string("&fields%5B%5D=") + f;
  return url;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
struct Row {
  const Departure *dep;
  int minutes;  // 0 => tram icon
};

inline std::vector<Row> visible_rows(int64_t now, size_t max_rows = MAX_ROWS) {
  std::vector<Row> rows;
  for (const auto &d : board().departures) {
    if (rows.size() >= max_rows)
      break;
    const int64_t secs = d.est_ts() - now;
    if (secs < 0)
      continue;  // already gone
    const int minutes = secs <= 60 ? 0 : static_cast<int>(secs / 60);
    rows.push_back({&d, minutes});
  }
  return rows;
}

// A short string that changes whenever the rendered board would change.
inline std::string board_signature(const std::vector<Row> &rows, bool stale) {
  std::string sig = stale ? "S|" : "F|";
  char buf[24];
  for (const auto &r : rows) {
    snprintf(buf, sizeof(buf), "%d|%d|%d;", r.minutes, r.dep->live ? 1 : 0, r.dep->late() ? 1 : 0);
    sig += r.dep->label + "|" + r.dep->dest + "|" + buf;
  }
  return sig;
}

inline void draw_board(Display &it, esphome::font::Font *font, const std::vector<Row> &rows, bool stale) {
  for (size_t i = 0; i < rows.size(); i++) {
    const Departure &d = *rows[i].dep;
    const int y = static_cast<int>(i) * ROW_PITCH;

    // Line number box
    it.filled_rectangle(0, y, LINE_BOX_W, LINE_BOX_H, line_bg(d));
    it.print(LINE_BOX_W / 2, y, font, line_fg(d), TextAlign::TOP_CENTER, d.label.c_str());

    // Destination
    const std::string dest = crop_to_width(it, font, d.dest, DEST_MAX_W);
    it.print(DEST_X, y, font, text_color(), TextAlign::TOP_LEFT, dest.c_str());

    // Time to departure
    if (rows[i].minutes <= 0) {
      it.print(TRAM_ICON_X, y, font, text_color(), TextAlign::TOP_LEFT, TRAM_ICON);
    } else {
      char buf[8];
      snprintf(buf, sizeof(buf), d.late() ? ">%d" : "%d", std::min(rows[i].minutes, 999));
      it.print(TIME_RIGHT_X, y, font, text_color(), TextAlign::TOP_RIGHT, buf);
      it.print(TICK_X, y, font, text_color(), TextAlign::TOP_LEFT, d.live ? TICK_LIVE : TICK_SCHEDULED);
    }
  }
  if (stale)  // data is old: small red marker in the bottom-right corner
    it.filled_rectangle(124, 62, 4, 2, error_color());
}

inline void draw_message(Display &it, esphome::font::Font *font, const std::string &text, Color color,
                         size_t max_lines = 5) {
  const auto lines = wrap_text(it, font, font_safe(text), it.get_width());
  for (size_t i = 0; i < lines.size() && i < max_lines; i++)
    it.print(0, static_cast<int>(i) * 12, font, color, TextAlign::TOP_LEFT, lines[i].c_str());
}

// ---------------------------------------------------------------------------
// Clock band ("Show clock" switch): replaces departure rows 4 and 5 (y 39..63).
// Left: HH:MM in fonts/vbzfont-big.bdf (the VBZ digits scaled 2x, 12x18 px).
// Right: weekday over "13 Nov 26" in the normal font, on the row 4 / row 5 grid,
// right-aligned to the panel edge. The widest weekday ("Wednesday", 64 px) still
// leaves a gap of a few pixels next to the time.
// ---------------------------------------------------------------------------
static constexpr size_t CLOCK_DEPARTURE_ROWS = 3;   // departure / message lines kept above the band
static constexpr int CLOCK_TIME_X = -2;             // the 2x digits have a 2 px left bearing: ink starts at x 0
static constexpr int CLOCK_TIME_Y = 40;             // digits cover y 42..59, centred on the two text lines
static constexpr int CLOCK_LINE1_Y = 3 * ROW_PITCH;  // 39, same as departure row 4
static constexpr int CLOCK_LINE2_Y = 4 * ROW_PITCH;  // 52, same as departure row 5
static constexpr int CLOCK_DATE_RIGHT_X = 128;       // TOP_RIGHT anchor: the last digit's ink ends at x 127
// Every English weekday ends in "y", whose tail sticks out 1 px past its advance. Anchoring 1 px
// further left keeps it on the panel and flush with the date.
static constexpr int CLOCK_WEEKDAY_RIGHT_X = 127;

struct ClockInfo {
  int hour, minute;
  int weekday;  // 0 = Sunday (ESPTime::day_of_week - 1, struct tm::tm_wday)
  int day, month, year;
};

inline const char *weekday_name(int weekday) {
  static const char *const NAMES[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  return (weekday >= 0 && weekday < 7) ? NAMES[weekday] : "";
}

inline std::string clock_time_str(const ClockInfo &c) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", c.hour % 100, c.minute % 100);
  return buf;
}

// "13 Nov 26"
inline std::string clock_date_str(const ClockInfo &c) {
  static const char *const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const char *month = (c.month >= 1 && c.month <= 12) ? MONTHS[c.month - 1] : "";
  char buf[16];
  snprintf(buf, sizeof(buf), "%d %s %02d", c.day % 100, month, ((c.year % 100) + 100) % 100);
  return buf;
}

// Changes once a minute (the weekday follows from the date).
inline std::string clock_signature(const ClockInfo &c) { return clock_time_str(c) + "|" + clock_date_str(c); }

inline void draw_clock(Display &it, esphome::font::Font *big, esphome::font::Font *small, const ClockInfo &c) {
  it.print(CLOCK_TIME_X, CLOCK_TIME_Y, big, text_color(), TextAlign::TOP_LEFT, clock_time_str(c).c_str());
  it.print(CLOCK_WEEKDAY_RIGHT_X, CLOCK_LINE1_Y, small, text_color(), TextAlign::TOP_RIGHT, weekday_name(c.weekday));
  it.print(CLOCK_DATE_RIGHT_X, CLOCK_LINE2_Y, small, text_color(), TextAlign::TOP_RIGHT, clock_date_str(c).c_str());
}

// Red square with a white Swiss cross (like the original 34x34 splash bitmap).
inline void draw_splash(Display &it, esphome::font::Font *font) {
  const int s = 34, x0 = (128 - s) / 2, y0 = 2;
  it.filled_rectangle(x0, y0, s, s, Color(255, 0, 0));
  it.filled_rectangle(x0 + 14, y0 + 6, 6, 22, Color(255, 255, 255));
  it.filled_rectangle(x0 + 6, y0 + 14, 22, 6, Color(255, 255, 255));
  it.print(64, 40, font, text_color(), TextAlign::TOP_CENTER, "opendata.ch");
}

// Colour bars + half markers to verify pin colour order and the E address line.
inline void draw_test_pattern(Display &it, esphome::font::Font *font) {
  it.filled_rectangle(0, 0, 42, 32, Color(255, 0, 0));
  it.filled_rectangle(43, 0, 42, 32, Color(0, 255, 0));
  it.filled_rectangle(86, 0, 42, 32, Color(0, 0, 255));
  it.print(2, 1, font, Color(255, 255, 255), TextAlign::TOP_LEFT, "R");
  it.print(45, 1, font, Color(255, 255, 255), TextAlign::TOP_LEFT, "G");
  it.print(88, 1, font, Color(255, 255, 255), TextAlign::TOP_LEFT, "B");
  it.print(0, 34, font, Color(255, 255, 255), TextAlign::TOP_LEFT, "bottom half");
  it.rectangle(0, 0, 128, 64, Color(255, 255, 255));
}

// ---------------------------------------------------------------------------
// Brightness
// ---------------------------------------------------------------------------
// Logarithmic lux -> brightness mapping: lux_lo maps to b_min, lux_hi to b_max, gamma shapes the curve.
inline int compute_brightness(float lux, float b_min, float b_max, float lux_lo, float lux_hi, float gamma) {
  if (std::isnan(lux))
    return static_cast<int>(b_max);
  lux = std::max(lux, 0.0f);
  lux_lo = std::max(lux_lo, 0.0f);
  lux_hi = std::max(lux_hi, lux_lo + 1.0f);
  const float lo = std::log10(lux_lo + 1.0f), hi = std::log10(lux_hi + 1.0f);
  float t = (std::log10(lux + 1.0f) - lo) / (hi - lo);
  t = std::min(1.0f, std::max(0.0f, t));
  t = std::pow(t, std::max(gamma, 0.05f));
  const float b = b_min + (b_max - b_min) * t;
  return static_cast<int>(std::lround(std::min(255.0f, std::max(0.0f, b))));
}

// True if minute-of-day `now_m` falls inside [start_m, end_m), wrapping past midnight.
inline bool in_window(int now_m, int start_m, int end_m) {
  if (start_m == end_m)
    return false;
  if (start_m < end_m)
    return now_m >= start_m && now_m < end_m;
  return now_m >= start_m || now_m < end_m;
}

}  // namespace zvv
