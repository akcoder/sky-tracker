"""Sky Tracker: the C++ behind the sky map (an ESPHome external component).

The YAML drives everything through lambdas that call into the sat:: namespace, so this
component only has to make the headers part of the build:

* every header is included into main.cpp, in dependency order, so the lambdas can see it;
* sky_extra.cpp is compiled on its own (ESPHome builds every .cpp in a component folder);
* C++ is built with -mtext-section-literals. With all of this inlined into main.cpp, a
  function's literal pool ended up more than 256 KB away from its code and the link failed
  ("dangerous relocation: l32r: literal target out of range"). esphome: build_flags only
  passes -D and -W options, so the flag is added here, for C++ only.
"""
import esphome.codegen as cg
import esphome.config_validation as cv

CODEOWNERS = ["@akcoder"]
DEPENDENCIES = ["lvgl", "http_request"]

CONFIG_SCHEMA = cv.Schema({})

HEADERS = [
    "sky_math.h", "sky_png.h", "sky_jpg.h", "sgp4.h", "sat_net.h", "sky_stars.h",
    "sky_flags.h", "sky_wmm.h", "sat_tracker.h", "sky_sensors.h", "sky_web.h",
    "sky_planets.h", "sky_pview.h", "sky_picons.h", "sky_diag.h", "sky_lore.h",
    "sky_cfig.h", "sky_events.h", "sky_comets.h", "sky_mw.h", "sky_photos.h",
    "sky_logo.h", "sky_about.h", "sky_sdfw.h", "sky_update.h", "sky_rocket.h", "sky_tz.h",
]


async def to_code(config):
    cg.add_cxx_build_flag("-mtext-section-literals")
    for header in HEADERS:
        cg.add_global(cg.RawStatement(f'#include "esphome/components/sky_tracker/{header}"'))
