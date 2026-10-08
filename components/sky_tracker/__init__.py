"""Sky Tracker: the C++ behind the sky map (an ESPHome external component).

The YAML drives everything through lambdas that call into the sat:: namespace, so this
component only has to make the headers part of the build:

* every header is included into main.cpp, in dependency order, so the lambdas can see it;
* sky_extra.cpp is compiled on its own (ESPHome builds every .cpp in a component folder);
* the web UI's logo links to this project's GitHub page: ESPHome hardcodes it to
  esphome.io/web-api inside the gzipped page it bundles (web_server, version 3, local: true),
  and that page can't load a script of ours. After ESPHome copies its sources into the build
  folder, the copy of the page is unpacked, the link swapped and the page packed again;
* C++ is built with -mtext-section-literals. With all of this inlined into main.cpp, a
  function's literal pool ended up more than 256 KB away from its code and the link failed
  ("dangerous relocation: l32r: literal target out of range"). esphome: build_flags only
  passes -D and -W options, so the flag is added here, for C++ only.
"""
import gzip
import logging
import re

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import writer
from esphome.core import CORE

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


_LOG = logging.getLogger(__name__)
_WEBUI_OLD = b'href="https://esphome.io/web-api" id="logo"'
_WEBUI_NEW = b'href="https://github.com/akcoder/sky-tracker" id="logo" target="_blank" rel="noopener"'


def _patch_webui():
    """The build folder's copy of the web UI page: its logo links to the GitHub page. A later
    ESPHome that words the page differently is left as it is (a warning, the build goes on)."""
    path = CORE.relative_src_path("esphome", "components", "web_server", "server_index_v3.h")
    if not path.is_file():
        return
    src = path.read_text(encoding="utf-8")
    m = re.search(r"(INDEX_GZ\[\] PROGMEM = \{)(.*?)(\};)", src, re.S)
    if not m:
        return
    page = gzip.decompress(bytes(int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]{2}", m.group(2))))
    if _WEBUI_NEW in page:
        return
    if _WEBUI_OLD not in page:
        _LOG.warning("sky_tracker: the web UI's logo link was not found; it is left as it is")
        return
    out = gzip.compress(page.replace(_WEBUI_OLD, _WEBUI_NEW), 9, mtime=0)
    rows = ["    " + ", ".join("0x%02x" % b for b in out[i : i + 19]) for i in range(0, len(out), 19)]
    path.write_text(src[: m.start(2)] + "\n" + ",\n".join(rows) + "\n" + src[m.end(2) :], encoding="utf-8")
    _LOG.info("sky_tracker: the web UI's logo now links to the GitHub page")


if not getattr(writer.copy_src_tree, "_sky_tracker", False):
    _copy_src_tree = writer.copy_src_tree

    def _copy_src_tree_and_patch(*args, **kwargs):
        result = _copy_src_tree(*args, **kwargs)
        _patch_webui()
        return result

    _copy_src_tree_and_patch._sky_tracker = True
    writer.copy_src_tree = _copy_src_tree_and_patch


async def to_code(config):
    cg.add_cxx_build_flag("-mtext-section-literals")
    for header in HEADERS:
        cg.add_global(cg.RawStatement(f'#include "esphome/components/sky_tracker/{header}"'))
