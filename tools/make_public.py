#!/usr/bin/env python3
"""Turn the development config (sky-tracker.dev.yaml: the one Dan builds, with his Wi-Fi and
API key as secrets and the component read from the local folder) into the public
sky-tracker.yaml that meets ESPHome's "Made for ESPHome" rules:

* no secrets: Wi-Fi comes from Improv (USB serial) or the fallback access point, the API and
  the ESPHome OTA have no keys until the device is adopted;
* the component comes from GitHub, and dashboard_import points back at this file;
* blocks marked DEV-ONLY BEGIN/END (and single lines marked DEV-ONLY) are dropped;
* each device gets its own name (MAC suffix) and takes its time zone from Home Assistant.

Usage: python3 tools/make_public.py sky-tracker.dev.yaml sky-tracker.yaml
"""
import re
import sys

REPO = "akcoder/sky-tracker"

src, dst = sys.argv[1], sys.argv[2]
s = open(src, encoding="utf-8").read()


def must(old, new, count=1):
    global s
    if old not in s:
        sys.exit(f"make_public: not found: {old[:60]!r}")
    s = s.replace(old, new, count)


# DEV-ONLY blocks and lines
s = re.sub(r"\n[ \t]*# DEV-ONLY BEGIN.*?# DEV-ONLY END[^\n]*", "", s, flags=re.S)
s = re.sub(r"\n[^\n]*# DEV-ONLY[^\n]*", "", s)

must("  wifi_name: !secret wifi_ssid", '  wifi_name: "your Wi-Fi network"')
must("  ssid: !secret wifi_ssid\n  password: !secret wifi_password\n", "")
s = re.sub(r"\n  encryption:\n    key: !secret api_encryption_key", "", s)
s = re.sub(r"\n    encryption:[^\n]*", "", s)  # the ESPHome OTA reused the API key
s = re.sub(r"\n    timezone: America/Anchorage", "", s)
if "!secret" in s:
    sys.exit("make_public: a !secret is left")

must("      type: local                  # PUBLIC: github://akcoder/sky-tracker@main\n      path: sky-tracker/components\n",
     f"      type: git\n      url: https://github.com/{REPO}\n      ref: main\n")
must("esphome:\n  name: ${name}\n",
     "esphome:\n  name: ${name}\n  name_add_mac_suffix: true       # every unit gets its own name\n")
must("\nlogger:\n", f"""
# Made for ESPHome: adopt into your own ESPHome Builder from this file, and set up Wi-Fi
# over USB with Improv (https://www.improv-wifi.com) or from the fallback access point.
dashboard_import:
  package_import_url: github://{REPO}/sky-tracker.yaml@main
  import_full_config: true

improv_serial:
  id: improv

logger:
""")
open(dst, "w", encoding="utf-8").write(s)
print(f"wrote {dst}")
