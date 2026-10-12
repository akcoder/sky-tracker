#!/usr/bin/env python3
"""build-local.yaml: sky-tracker.yaml with the sky_tracker component taken from ./components instead of GitHub, so a
build uses the working tree. Run from the repo root before every `esphome compile/upload build-local.yaml` (it is not
tracked, and goes stale the moment sky-tracker.yaml changes)."""
import re
y = open("sky-tracker.yaml").read()
y = re.sub(r"  - source:\n      type: git\n      url: https://github.com/akcoder/sky-tracker\n      ref: main\n",
           "  - source:\n      type: local\n      path: components\n", y)
open("build-local.yaml", "w").write(y)
