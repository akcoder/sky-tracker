// UI-65..70 in their own translation unit: the logo, the About text, the microSD firmware
// update, the internet update prompt, the launch animation and the time zone picker. main.cpp
// (every header inlined into it) is close to the reach of Xtensa l32r to its literal pool, so
// new code goes here (see BUILD-7).
#define SKY_IMPL
#include "esphome.h"
#include "sat_tracker.h"
#include "sky_logo.h"
#include "sky_about.h"
#include "sky_sdfw.h"
#include "sky_update.h"
#include "sky_rocket.h"
#include "sky_tz.h"
