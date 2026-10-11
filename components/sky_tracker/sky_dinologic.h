#pragma once
// The walking dinosaur, the part that needs no screen: a random walk (which way, how high up, how
// fast, where it stops, what it says), where the dinosaur is and what it is doing at a moment, and
// when the next walk is. tools/dino_art.py is its Python twin (the render tool and the tests agree).
//
// A walk: the dinosaur enters off screen, walks, stops somewhere in the middle, stands and roars (a
// speech bubble with a random word, the mouth opening and closing) for 2-3 s, then walks on and leaves.
// Its speed is never steady: base * (1 + amp cos(2 pi t / period + phase)), always forward, so it
// hurries and dawdles; the legs step by distance, so they hurry and dawdle with it.
#include <cmath>
#include <cstdint>

namespace sat {
namespace dino {

inline constexpr int SW = 144, SH = 96;            // the sprite
inline constexpr int SCREEN_W = 480, SCREEN_H = 480;
inline constexpr int LANE_MIN = 60, LANE_MAX = 340;     // the top edge of the sprite, so it never leaves the screen
inline constexpr int STOP_MIN = 150, STOP_MAX = 330;    // where it stops (its left edge, walking right)
inline constexpr double STEP_PX = 9.75;      // one of the four walk poses per this much distance (8 a second at 78 px/s)
inline constexpr uint32_t ROAR_DELAY_MS = 300, ROAR_STEP_MS = 250;
inline constexpr uint32_t GAP_MIN_MS = 3u * 60000u, GAP_MAX_MS = 12u * 60000u;   // between walks

enum Pose : uint8_t { WALK0 = 0, WALK1, WALK2, WALK3, STAND, ROAR_HALF, ROAR_OPEN, N_POSES };

inline constexpr int N_WORDS = 10;
inline const char *const WORDS[N_WORDS] = {"Rawr!", "RAWR!", "Grrr!", "Roarrr!", "Raaawr!",
                                           "Rawr rawr!", "Chomp!", "Grrrawr!", "Rrraaah!", "Rarrr!"};

// xorshift32: the same numbers on the device and in the render tool.
struct Rng {
  uint32_t s;
  explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
  double next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return (s >> 8) / 16777216.0;
  }
};

struct Walk {
  bool flip = false;          // true: right to left
  int lane = 0;               // the sprite's top edge
  double speed = 78.0;        // px/s
  int stop_x = 0;             // its left edge when it stops (walking right)
  uint32_t pause_ms = 2000;
  int word = 0;
  double amp = 0.3, period = 2000.0, phase = 0.0;   // how its speed varies: depth, ms, where in the cycle
  uint32_t t_stop = 0, t_end = 0;

  // pixels walked after tw_ms of walking (the pause does not count)
  double dist(uint32_t tw_ms) const {
    const double w = 2 * M_PI / period;
    return speed * (tw_ms / 1000.0 + amp / w * (std::sin(w * tw_ms + phase) - std::sin(phase)) / 1000.0);
  }
  uint32_t time_for(double px) const {
    uint32_t t = 0;
    while (dist(t) < px) t++;
    return t;
  }
};

inline Walk make_walk(uint32_t seed) {
  Rng r(seed);
  Walk w;
  w.flip = r.next() < 0.5;
  w.lane = LANE_MIN + (int) (r.next() * (LANE_MAX - LANE_MIN + 1));
  w.speed = 78.0 * (0.7 + 0.6 * r.next());
  w.stop_x = STOP_MIN + (int) (r.next() * (STOP_MAX - STOP_MIN + 1));
  w.pause_ms = 2000 + (uint32_t) (r.next() * 1000);
  w.word = (int) (r.next() * N_WORDS) % N_WORDS;
  w.amp = 0.25 + 0.25 * r.next();
  w.period = 1200.0 + 1800.0 * r.next();
  w.phase = 2 * M_PI * r.next();
  w.t_stop = w.time_for(w.stop_x + SW);
  w.t_end = w.t_stop + w.pause_ms + (w.time_for(SCREEN_W + SW) - w.t_stop);
  return w;
}

struct Frame {
  int x = 0, y = 0;           // top left of the sprite on screen
  Pose pose = WALK0;
  bool bubble = false;
};

inline Frame at(const Walk &w, uint32_t t_ms) {
  Frame f;
  uint32_t tw;
  bool roar = false;
  if (t_ms < w.t_stop) {
    tw = t_ms;
  } else if (t_ms < w.t_stop + w.pause_ms) {
    tw = w.t_stop;
    const uint32_t u = t_ms - w.t_stop;
    f.bubble = u >= ROAR_DELAY_MS;
    static const Pose ROAR[4] = {STAND, ROAR_HALF, ROAR_OPEN, ROAR_HALF};
    f.pose = f.bubble ? ROAR[((u - ROAR_DELAY_MS) / ROAR_STEP_MS) % 4] : STAND;
    roar = true;
  } else {
    tw = t_ms - w.pause_ms;
  }
  const double d = w.dist(tw);
  if (!roar) f.pose = (Pose) ((long) (d / STEP_PX) % 4);
  double x = -SW + d;
  if (w.flip) x = SCREEN_W - x - SW;
  f.x = (int) std::floor(x + 0.5);
  f.y = w.lane;
  return f;
}

inline bool finished(const Walk &w, uint32_t t_ms) { return t_ms >= w.t_end; }

inline uint32_t next_gap_ms(uint32_t seed) {
  Rng r(seed);
  return GAP_MIN_MS + (uint32_t) (r.next() * (GAP_MAX_MS - GAP_MIN_MS));
}

}  // namespace dino
}  // namespace sat
