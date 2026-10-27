#pragma once
// The walking dinosaur, the part that needs no screen: a random walk (which way, how high up, how
// fast, where it stops, what it says), where the dinosaur is and what it is doing at a moment, and
// when the next walk is. tools/dino_art.py is its Python twin (the render tool and the tests agree).
//
// A walk: the dinosaur enters off screen, walks, stops somewhere in the middle, stands and roars (a
// speech bubble with a random word, the mouth opening and closing) for 2-3 s, then walks on and leaves.
// Three in four of its walks are hunting walks (UI-85c): it walks to a Sun, Moon or planet on the map, stops with its
// snout at it, says it is hungry (one of N_HUNGRY lines), chews (the object is gone at the middle of the chewing)
// and says it was tasty (one of N_TASTY); then it walks on.
// Its speed is never steady: base * (1 + amp cos(2 pi t / period + phase)), always forward, so it
// hurries and dawdles; the legs step by distance, so they hurry and dawdle with it.
#include <algorithm>
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

enum Pose : uint8_t { WALK0 = 0, WALK1, WALK2, WALK3, STAND, ROAR_HALF, ROAR_OPEN, DIP_HALF, DIP_OPEN, N_POSES };  // DIP: the head down at the food, the mouth half open / open

inline constexpr int N_WORDS = 10;
inline const char *const WORDS[N_WORDS] = {"Rawr!", "RAWR!", "Grrr!", "Roarrr!", "Raaawr!",
                                           "Rawr rawr!", "Chomp!", "Grrrawr!", "Rrraaah!", "Rarrr!"};

// Lines for a hunting walk: "%s" is the object's name ("The Sun", "The Moon", "Mars"). Few characters: the bubble
// is one line of the 16 px font and must fit the screen with the name in.
inline constexpr int N_HUNGRY = 30, N_TASTY = 30;
inline const char *const HUNGRY[N_HUNGRY] = {
    "I'm so hungry!", "Starving here!", "Hungry, hungry...", "My tummy rumbles.", "Time for a snack!",
    "Is it lunchtime?", "I could eat a star.", "Dinner time?", "No food all day!", "Ooh, a snack!",
    "Is that %s?", "%s looks yummy.", "I'm famished!", "Rawr... so hungry.", "Snack attack!",
    "That %s... mmm.", "Mind if I nibble?", "I smell %s.", "My belly is empty.", "Feed me, please!",
    "%s! My lunch!", "What a tasty view.", "I'm a bit peckish.", "Something smells good", "Gimme %s!",
    "So... very... hungry", "Grumble grumble...", "Ooh, %s. Dessert?", "Time to chomp!", "Breakfast at last?"};
inline const char *const TASTY[N_TASTY] = {
    "Tasty!", "So tasty!", "%s was tasty!", "Yum! Delicious!", "Mmm, scrumptious!",
    "Needs more salt.", "Crunchy! Love it.", "Best snack ever!", "%s tastes great!", "Burp! Excuse me.",
    "Delicious!", "Yummy in my tummy!", "That hit the spot.", "Tastes like stardust.", "Mmm, cosmic!",
    "More, please!", "A little spicy.", "Chef's kiss!", "Not bad at all.", "Sweet, like candy.",
    "%s: five stars!", "Ahh, much better.", "Rawr! Yummy!", "Tangy! I like it.", "Gone in one bite!",
    "Satisfying!", "Tastes like cheese.", "Mmm, delish!", "I'd eat it again.", "Worth the walk!"};

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
  bool eat = false;           // a hunting walk: it stops at the target (stop_x, lane) and eats it
  int hungry = 0, tasty = 0;  // which lines (HUNGRY, TASTY)

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

inline constexpr uint32_t HUNGRY_MS = 1900, CHEW_MS = 1800, TASTY_MS = 1900, EATEN_AT_MS = 800;  // EATEN_AT: into the chewing
inline constexpr int SNOUT_REACH = 26;     // how far the sprite's edge is from the snout with the head down: the snout reaches the object's rim
inline constexpr int MOUTH_Y = 42;         // where the mouth is on the sprite with the head down: the object's centre is level with it

struct Target {   // what a hunting walk is for: the object's centre and radius on the screen
  int x = 0, y = 0, r = 8;
};

inline Walk make_walk(uint32_t seed, const Target *tg = nullptr) {
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
  w.hungry = (int) (r.next() * N_HUNGRY) % N_HUNGRY;
  w.tasty = (int) (r.next() * N_TASTY) % N_TASTY;
  if (tg) {   // hunting: from the side the target is on (the dinosaur stands between it and the near edge), snout at it
    w.eat = true;
    w.flip = tg->x < SCREEN_W / 2;
    w.lane = std::max(0, std::min(SCREEN_H - SH, tg->y - MOUTH_Y));
    const int left = w.flip ? tg->x + tg->r - SNOUT_REACH : tg->x - tg->r - SW + SNOUT_REACH;   // the sprite's left edge on screen
    w.stop_x = w.flip ? SCREEN_W - SW - left : left;
    w.pause_ms = ROAR_DELAY_MS + HUNGRY_MS + CHEW_MS + TASTY_MS + 200;
  }
  w.t_stop = w.time_for(w.stop_x + SW);
  w.t_end = w.t_stop + w.pause_ms + (w.time_for(SCREEN_W + SW) - w.t_stop);
  return w;
}

struct Frame {
  int x = 0, y = 0;           // top left of the sprite on screen
  Pose pose = WALK0;
  int bubble = 0;             // 0 none; 1 the roar (or the hungry line); 2 the tasty line
  bool eaten = false;         // a hunting walk: the target is gone
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
    if (w.eat) {
      const uint32_t chew0 = ROAR_DELAY_MS + HUNGRY_MS, chew1 = chew0 + CHEW_MS;
      f.pose = STAND;
      if (u >= ROAR_DELAY_MS && u < chew0) {
        f.bubble = 1;
      } else if (u >= chew0 && u < chew1) {
        f.pose = ((u - chew0) / 200) % 2 ? DIP_HALF : DIP_OPEN;     // chewing: the head down at the target
        f.eaten = u >= chew0 + EATEN_AT_MS;
      } else if (u >= chew1) {
        f.bubble = 2;
        f.eaten = true;
      }
    } else {
      f.bubble = u >= ROAR_DELAY_MS ? 1 : 0;
      static const Pose ROAR[4] = {STAND, ROAR_HALF, ROAR_OPEN, ROAR_HALF};
      f.pose = f.bubble ? ROAR[((u - ROAR_DELAY_MS) / ROAR_STEP_MS) % 4] : STAND;
    }
    roar = true;
  } else {
    tw = t_ms - w.pause_ms;
    f.eaten = w.eat;
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
