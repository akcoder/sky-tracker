# Sky Tracker

ESPHome + LVGL 9.5 firmware for a Guition ESP32-4848S040 (ESP32-S3, 480x480 RGB panel, GT911 touch). Everything is C++ headers in
`components/sky_tracker/`, wired by `sky-tracker.yaml`. The shared board setup, update card, JPEG/PNG decoders, About text and
Sun maths come from the `tracker_common` component (pinned tag, `tracker_common_ref` in the YAML), shared with
`../f1-tracker`. `docs/requirements.md` is the spec (requirement ids like UI-59b, PERF-19; the changelog is its first lines).

## Build, test, flash

- Venv is `./.venv` (`source .venv/bin/activate`); keep tool venvs in the project dir.
- Host tests (LVGL on the desktop, no device): `tools/host_test/build.sh run` must end `ALL CHECKS PASSED`. It writes screen
  renders to `tools/host_test/out/renders/*.ppm`. It needs `../tracker-common` checked out next to this repo (no cloning).
- Device build: `python3 tools/mkbl.py` first (writes `build-local.yaml`, the YAML with the component taken from `./components`),
  then `esphome compile build-local.yaml` and `esphome upload build-local.yaml --device 192.168.0.217`. A stale
  `build-local.yaml` silently flashes old YAML; regenerate it after every change to `sky-tracker.yaml`. `esphome upload` after a
  failed compile uploads the previous binary: check "Successfully compiled" first.
- Drive and measure the unit without a finger or the serial port: `tools/dbg.py [--all] script.txt` (API action `debug_cmd`, log
  stream). Commands: `perf reset state list close stack` (`stack`: the main task's stack high-water mark; it is 8 KB and a peak near it aborts the firmware), `cdtest` (the launch countdown now, then the launch animation at T-0), `open sun|moon|earth|region|uaf|planet N`, `page sky|list|settings|about|debug`,
  `card moon|sun`, `card image`, `reentry`, `dino`. Examples in `tools/dbg_*.txt`.
- Release (only when asked): bump `fw_version` in the YAML, mark the changelog line `(released)`, commit and push,
  `gh release create X.Y.Z --notes-file ...`. The Firmware workflow then publishes the manifest the units check.

## Rules of this project

- **Tell the user before anything resets the unit** (OTA flash, opening the serial port, the Reboot button). Opening or closing the
  serial port resets the board; a reset within ~60 s of a flash rolls the firmware back, so wait over 60 s before measuring.
- Don't release, delete releases, or edit `../tracker-common` / `../f1-tracker` unprompted. Coordinate with the f1-tracker session
  by `SendMessage`; shared-code changes go through tracker-common, never a private copy. The dinosaur (`sky_dino*.h`) is a
  private copy from f1-tracker by decision: it never goes into tracker-common.
- **Renders (`docs/renders`): only regenerate and commit the ones whose subject changed.** After `tools/host_test/make_comps.sh`
  (it redraws everything on live data) `git checkout --` the rest, **only under `docs/renders/`** (a `git status docs` loop once reverted uncommitted edits to `docs/requirements.md`), and delete stray new files (`r19_prog_*`). A version bump, a
  date, live data, or one small element shared by many renders (a strip icon) is not a reason. Check `git diff --name-only
  docs/renders` before every commit. Name the planet in `r10_planet<N>_<name>`.
- Update `docs/requirements.md` (the requirement and the `4.6.xx (unreleased)` changelog line) with every user-visible change.
- Commit messages end with the Co-Authored-By line from the session's attribution reminder.

## Hardware and LVGL facts that cost time

- Internal RAM is about 20 kB free and PSRAM 1-2 MB: allocate persistent buffers once at start-up (FAIL-12/13), never grow a
  vector later. ESPHome routes plain `new` to internal RAM only; use the PSRAM allocators (`pvector`, `heap_caps_malloc`).
- LVGL draws the top layer (`lv_layer_top`) over the active page without culling, so a full-screen overlay still redraws the page
  under it: hide the page's children while an overlay is open (never hide a screen object itself: it crashes).
- This build has `LV_DRAW_SW_SUPPORT_RGB565A8 = 0` and no A8: a scaled RGB565 image draws nothing. Nothing is scaled on the
  device; pictures are prescaled (`tools/refine_photos.py`, `gen_outer_photos.py`, `gen_planet_icons.py`) or decoded straight to size.
- An `lv_image` is sized to its content: a child sticking out of it widens it and re-centres the picture. Give it a fixed size.
- Top-row buttons start at y=10 (the panel's edge hides the top few pixels); touch targets are small, so give buttons and switches
  `lv_obj_set_ext_click_area`.
- `sky_extra.cpp` is its own translation unit (`SKY_IMPL`): main.cpp is near the Xtensa `l32r` literal-pool limit, so new
  code goes in a header compiled there (declarations when `SKY_IMPL` is unset, definitions when set; see BUILD-7).
- The logger is at INFO (about 45 boot lines); ~600 lines through the blocking serial port delayed the loading screen by 1.5 s.
- The clock reads 1970 until SNTP answers: anything computed from the time (shower peaks, stars, planets) must wait for
  `clock_valid()`. A web icon name must exist in Iconify's `mdi` set or the web UI shows nothing.
- The web page's icon and header patches, the update card, the board package: `tracker_common`. Ours to keep: memory and network
  settings, the NET-14 install-when-quiet hook, no BLE.
