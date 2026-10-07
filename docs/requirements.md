# Sky Tracker — Design Requirements (rev 4.5)

## Changes in rev 4.5
- 4.6.26 (queued): after a minute untouched, any open screen closes and the map returns (UI-72). At power-on the boot screen shows the logo and "Loading" before the slow start-up, then the Wi-Fi status (BOOT-2). The screen comes up ~2.7 s sooner: the animation pictures are drawn after the display starts, 10 ms at a time, and the boot launch plays when they are ready (UI-69n). A tap on an alert's icon opens the alert too (the picture icon took the tap). The splashdown's sea rises into view, and each splashdown lands in its own spot with its own sway and chute drift (UI-69m).
- 4.6.25 (installed): pictures work again (the Sun, Moon, Earth and planet photos failed in 4.6.22-4.6.24: the orbital lists were reserved at their caps before the picture buffers and left 0.2 MB of PSRAM; the buffers are now taken first and the lists sized to what they hold, FAIL-12b). A splashdown animation: a capsule on four parachutes comes down to the ocean, splashes and bobs (UI-69m), with a Play Splashdown Animation button and a Splashdowns alert switch (UI-41h). The details card of a launch, splashdown or docking alert shows the country's flag after its title (UI-54d). The whole header left of the counts is the alert's tap zone (UI-41d). The launch card's distance follows Miles.
- 4.6.24 (installed): the settings header names the open tab (DISPLAY, LOCATION, CELESTIAL, SATELLITES, ALERTS) instead of SETTINGS (UI-16a). Settings icons clear their labels: on Display and Location the icon column is at x 140 (was 128), on Celestial, Satellites and Alerts the left icons at 130 and switches at 156 (were 122 and 150). Every progress bar uses the same colours (UI-71; the picture loader's was orange). A Comets alert switch (UI-41g): the visible-comet alert follows it instead of Sky events. A Lunar alert switch (Settings > Alerts and Home Assistant): the full Moon, the Moon near a planet or bright star, and lunar eclipses now follow it instead of Sky events (UI-41f). "Launches" reads "Launch".
- 4.6.23 (installed): Settings tabs are icons only, with a fifth, Alerts (bell-ring): Aurora, Planets and Sky events moved there from Celestial, plus new Space station, Launch and Docking alert switches (UI-41e). A Space stations switch on the Satellites tab shows or hides the ISS and Tiangong (UI-52b). Each new switch is also a Home Assistant switch.
- 4.6.22 (installed over USB to end a 4.6.21 boot loop): fixes a crash (abort) when satellite lists downloaded while layers were switched: downloads freed and reallocated lists of up to 1 MB until PSRAM was in pieces too small for the next one. The lists are now reserved once at start and refilled in place (FAIL-12). Crash records survive (their flash sector moved off the launch list's, which overwrote them) and the Last Crash sensor carries the backtrace (FAIL-10).
- 4.6.21 (installed): alerts change once a minute (were every 6 s); no aurora alert for a faint one (Kp under 3.5) unless NOAA's nowcast says likely (UI-38b); a changed status line is drawn just after the panel's scan has passed it, so it no longer tears (PERF-14).
- 4.6.20 (installed): a rocket launches across the boot screen (UI-69k). Settings gains a Satellites tab (LEO cone, LEO, Starlink, MEO, GEO, Debris, Sat Trails moved from Celestial), tabs with the icon above the text (UI-16a). A tap on an alert opens its details (UI-41d): the card of its object (ISS or Tiangong overhead, planet, comet, meteor shower, Moon), or a details card for launches, space events, passes to come, aurora, solar wind, alignments, eclipses and seasons. Alert icons centred on the whole text, one or two lines (UI-41c mode 2). The internet update screen's bar and percentage move during the download (UI-68b); installs started from the web page or HA show that screen too.
- 4.6.19 (installed): the ground cloud fades from 15% of the climb and is gone by 35% (was 25% / 70%), so the animation doesn't bog down early (UI-69j).
- 4.6.18: fixes a boot loop. The rocket and capsule pictures are drawn at boot (~5 s, 4x4 supersampled, code from PSRAM) without feeding the task watchdog; the device reset 13 s into every boot, so 4.6.17 never got past setup (display dark, safe mode). The drawing loops now feed it each row (FAIL-11).
- 4.6.17 (boot loop, withdrawn): the launch and undocking smoke is one full-screen object that draws its discs itself (UI-69i), instead of ~40 objects resized and moved each frame. 4.6.16's log: 4.6 fps, ~4.7 ms per lv_obj_set_size and ~4.8 ms per set_pos even with the 32 KB cache; a changed disc now only invalidates its old and new areas.
- 4.6.16: 32 KB instruction cache (PERF-13). 4.6.15's probes: a cache-resident CPU loop ran at the same speed during the launch animation as before it (5.4 ms), but a PSRAM read cost ~0.7-1 us per cache line and one smoke update's lv_obj_set_size ~5 ms: LVGL's code, run from PSRAM (PERF-10), keeps missing the 16 KB instruction cache while the panel scanout holds the bus.
- 4.6.15: the planet picture is always the photo (Photo | Drawn removed; the drawing stays only as a fallback with no photo); a "Tonight's phase" switch at the lower right of Mercury, Venus and Mars turns the phase shading on or off (UI-61g). Alert icons are centred on the first line of their text, measured from the fonts' glyphs (UI-41c; they sat 2 px above the text box, high on some glyphs and low on others). 4.6.14's profile: frame() ~126 ms a call, ~12 ms per smoke update; the log now splits a smoke update into its LVGL calls and times a fixed CPU loop and a PSRAM read before and during the animation (UI-69h), to tell a slow LVGL path from a starved core or PSRAM bus. The rocket's angle is only set again after 1.5 degrees.
- 4.6.14 (installed): 4.6.13's frame profile of the launch animation: 3.7 fps; per frame the animation's own code took 95 ms (max 334), the refresh 87 ms more (flush only 5 ms), and the rest of the loop 97 ms, which includes the 40 ms fallback timer running the animation again. The profile now splits frame() into the rocket image, the puffs and the ground cloud and counts smoke updates and fallback frames; the fallback waits 150 ms; hidden smoke discs start at size 0 off screen (LVGL's default square at 0,0 was redrawn every time one was hidden again, over the title and alert text) and are not hidden twice.
- 4.6.13 (installed): planet photos with a phase (Venus as a crescent) no longer show a thin unlit arc around the dark side: the photo's soft rim reached past the disc the shading found, and was left bright (UI-61f). The Wi-Fi status page names the network it is trying (saved in flash, NET-2c; the public build showed its placeholder "your Wi-Fi network"). ISS undocking animation (UI-69f) and a Play Undocking Animation button. Each animation now logs where a frame's time goes (UI-69g): the opaque smoke of 4.6.12 did not help, still 3.4-3.6 fps, and the same with the Milky Way and stars switched off, so the map is not the cost. A long mission name in the animation banner wrapped to two lines and spilled over the border: the banner now grows upward to hold two lines (dots after that).
- 4.6.12 (released): the unit's own frame log on 4.6.11 showed 3-5 fps during the launch animation (longest gap 600+ ms), worst while the ground cloud shows: translucent smoke makes LVGL blend against the PSRAM side (PERF-9). Smoke is now opaque discs, their colour pre-mixed with the background under each one (UI-69e); the soft smoke images of 4.6.11 are gone.
- 4.6.11 (installed): the launch animation was still jerky on the unit. Smoke is now soft round images made at boot (a plain image blend, no anti-aliased circle masks), the ground cloud has 6 billows (was 12) and the trail 32 puffs (was 48); host render time per frame down about 30%, the largest frame's redraw down 37%. Firmware update checks are logged: the manifest URL at DEBUG and the answer at INFO (NET-13a; ESPHome's own update check logged nothing). The unit logs the frame rate after each animation ("animation: N frames in T ms (F fps), longest gap G ms") to tune from.
- 4.6.10 (installed): the launch's ground cloud is 12 round billows rolling out sideways from the pad, the inner ones warm-lit, then drifting and thinning (UI-69; was three widening rounded bars). Upgrade Check button in the web UI and HA (UI-68b). Satellite and sky updates pause while the launch animation plays (UI-69b). The rocket's flame flickers through five shapes (length, width, lean) with a white-hot core (UI-69c; was two lengths swapped every 80 ms). README animation re-recorded at 30 fps.
- 4.6.9 (installed): smoother launch animation (UI-69a): positions are set at the start of every display refresh (16 ms) instead of on a 33 ms timer that beat against it, and the smoke puffs grow and fade in 0.18 s steps so only a few change per frame (host test: 6% of the screen redrawn per frame, was 36% with whole-screen frames when the 32 dirty-area limit overflowed).
- 4.6.8 (installed): every HTTP request is logged with its URL at DEBUG ("launches: GET https://ll.thespacedevs.com/..."), and the URL is added to each failure line (NET-13).
- 4.6.7 (installed): releases carry the manifest and the .ota/.factory .bin files with checksums, no longer the 33 MB .elf (also left off GitHub Pages); the .elf stays in the build's artifact for 90 days, for decoding crashes. The Settings button Updates is now Upgrade Check (206,440 150x34; the error line beside it is 182 wide). Time zone picker at the top of Settings > Display, also a Time Zone select in HA and the web page (UI-70). Launch alerts from 3 days ahead, the next three (UI-54; was: a nearby launch from 24 h, any launch in its last hour). Space event alerts: dockings, undockings, spacecraft releases, EVAs from Launch Library 2's events, within 3 days, exact times only, up to two (UI-54c).
- From 4.6.6 on, Dan's unit is updated only through GitHub releases: a change is committed, a release is published, the Firmware workflow builds the public sky-tracker.yaml, and the unit installs it from the manifest (the update icon, or HA's update entity). The unit then runs the public build: Wi-Fi from flash (saved by the dev build), no API encryption (HA's ESPHome entry is confirmed once without a key), name sky-tracker-9cad68 (MAC suffix), time zone from Home Assistant (2026.3+; UTC until HA connects). The ESPHome Builder is no longer used to install.
- 4.6.6 (installed): the update progress screen says just "Keep the power on" ("it restarts by itself" removed). An older release is never offered (UI-68). A newer one no longer opens a prompt over the screen: an amber download icon appears beside the gear, and tapping it shows the prompt (UI-68a). ESPHome's update entity calls any version difference "available", so 4.6.5 offered the GitHub 4.6.3, it was accepted, and the public build (no Wi-Fi, no API key) replaced the dev build.
- 4.6.5 (installed): all mbedTLS memory from PSRAM (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC). 4.6.4's malloc route still left the update check failing with -0x7F00 (UI-68).
- 4.6.4 (installed): mbedTLS through malloc (CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC), meant to let the update check's TLS buffers come from PSRAM; it still failed (fixed in 4.6.5). Launch animation at T-0 (UI-69, Launch Animation switch, Play Launch Animation button). The launch list is kept in flash so reboots don't empty it or use up Launch Library's free calls (UI-54a); RocketLaunch.Live fills in when Launch Library refuses (UI-54b). The public build's improv_serial id is improv_usb ("improv" clashed with a C++ namespace and failed the GitHub build).
- 4.6.3 (installed): the microSD card is checked once at boot, before the panel is set up, instead of every 5 s (UI-66): the checks were being read by the panel's init interface (same two pins) and turned the screen red. The first internet update check waits 3 min after boot (UI-68): at boot it ran out of memory for a second TLS connection.
- 4.6.2 (installed, test): microSD checks switched off; the screen stays normal, which confirmed the cause.
- 4.6.1 (installed): held the panel CS (39) high during card checks; the screen still turned red.
- 4.6.0 (installed): the C++ is the sky_tracker external component (repo github.com/akcoder/sky-tracker); internet updates (update entity on GitHub Pages, http_request OTA, Settings > Updates, an offer when the hourly check finds a release, UI-68); Made for ESPHome (ids everywhere, project name/version, improv, dashboard_import in the public build); includes the 4.5.38 changes below.
- 4.5.38 (installed as part of 4.6.0): a detailed logo, 200 px, with "Sky Tracker" on the boot page (UI-65, NET-2b). About page from Settings: version, "by Dan Morphis", build date, device details and data credits (UI-67). Firmware updates from a microSD card: put sky_tracker_firmware_<version>.bin on the card; the device asks Update / Not now and installs it (UI-66). The card runs in SD 1-bit mode so the GPS keeps GPIO42. Icons on every row of the Display and Location settings tabs, as on Celestial (UI-16a). New code lives in sky_extra.cpp and C++ is built with -mtext-section-literals (BUILD-7): main.cpp had outgrown l32r's reach to its literal pool.
- 4.5.37 (installed): the Sun picture is GOES-19 SUVI 30.4 nm again (chosen from a side-by-side of SUVI's six bands, SOHO EIT's four, SDO and LASCO): NOAA's frame list, the 1280 px PNG box-filtered to 360 px, a new frame every 4 minutes. Dark frames (GOES in Earth's shadow around the equinoxes) are skipped by size or darkness and the last good frame stays, captioned "GOES in Earth's shadow"; < steps back through the list's last hour (UI-59).
- 4.5.36 (installed): Wi-Fi and lwIP buffers back in PSRAM (4.5.35 had moved them to internal RAM, which left ~20 KB free and the Earth download failed with "Failed to allocate memory"); the TCP window stays 16 KB (was 32 KB), halving the download bursts on the PSRAM bus (PERF-12).
- 4.5.35 (installed): display glitching. Wi-Fi and lwIP buffers moved to internal RAM and the TCP window cut to 16 KB, on the theory that downloads through PSRAM starve the panel's refills. Removed two heap walks added in 4.5.2x (picture buffers at boot, and on out of memory) that broke FAIL-9.
- 4.5.34 (installed): the C++ sources (sat_*.h, sgp4.h, sky_*.h) and the old rev-2 backups moved from the ESPHome config folder into esphome/sky-tracker/; the yaml's includes name them there (ESPHome still copies each to src/ by its bare name, so the build is unchanged). Only sky-tracker.yaml stays in the root.
- 4.5.33 (installed): fixed the Moon's < ("could not reach NASA") and the Sun's "image list: HTTP -1": when a server had closed the kept connection in the seconds since the last download, the next request on it read as status -1, which was not recognised as a dropped connection. Any reply without a valid status now counts as one: the connection is replaced and the request sent again at once (NET-12).
- 4.5.32 (installed): fixed the Sun "out of memory" and the display glitching that came with it. Each live picture kept three 253 KB buffers of its own (the download's, the one showing, the one before), so after the Moon, Earth and Alaska with their earlier frames PSRAM was down to ~50 KB. Now one work buffer serves every download and older frame, and one "picture before" buffer is shared by the live pictures (the latest of each is still kept): ~1.3 MB less at worst (UI-59e).
- 4.5.31 (installed): the Sun picture comes from SOHO's EIT at 30.4 nm (ESA/NASA, at L1, so never in Earth's shadow; ~70 KB JPEG, a new frame every few hours) instead of GOES SUVI; < fetches earlier frames from SOHO's daily folders (UI-59). Planet photos are stored at display size and decoded in the loop the moment the view opens: no job, no queue, no "Preparing" message (UI-61e). The Earth tab opens on the globe (UI-62a). Fixed "not a readable JPEG" on the Earth and Alaska pictures: a kept connection reused after a 404 misread the next reply; the connection is now dropped after any error reply, and a reply that is not a JPEG is retried once on a new connection (NET-12).
- 4.5.30 (installed): planet photos are built into the firmware (sky_photos.h, ~150 KB of JPEG): never downloaded, available offline; the flash photo store is gone (UI-61d). The Sun picture skips SUVI's dark frames (the satellite in Earth's shadow around the equinoxes, ~30 KB instead of ~1.1 MB) for the newest good one, captioned "GOES in Earth's shadow" (UI-59). < steps back a frame at a time on the Sun, Moon, Earth and region pictures, fetching the one before when it isn't held (Sun: NOAA's list; Moon: the hour before; Earth/region: 10 minutes before); > returns to the latest; the buttons are always there (UI-59d). The Earth and region pictures use NOAA's scan-time frame names, so their times are the scan times. One kept-alive HTTPS connection is reused while requests go to the same host (NET-12).
- 4.5.29 (installed): the download log splits connect time (DNS, TCP, TLS) from body speed. First readings: connect 2-9 s, body 46-60 KB/s (before 4.5.28: 15-27 KB/s overall) (NET-11).
- 4.5.28 (installed): faster downloads: a 32 KB TCP receive window (was 5.7 KB, which capped a download at ~57 KB/s over the ~100 ms round trip from Alaska), lwIP/Wi-Fi buffers allowed in PSRAM, 8 KB HTTP reads; a failed connect (e.g. a lost DNS answer) is retried once after a second; downloads of 50 KB or more log KB, seconds, KB/s and the time spent decoding (NET-11). NASA SDO was checked as a smaller Sun source but its "latest" images are 8 days old (an SDO outage), so the Sun stays on GOES SUVI.
- 4.5.27 (installed): rocket launches load again: Launch Library 2.3.0 replies nest 11 levels deep and ArduinoJson stops at 10 by default, so every reply was "unreadable"; the limit is now 24 (comets too) and the parse error is logged (UI-54).
- 4.5.26 (installed): fix "out of memory" on the Earth/region picture after a planet photo: the planets had their own 777 KB decode buffer and a 253 KB temporary picture, so after Mercury and the Moon no 777 KB block was left for Alaska. One decode buffer now serves all pictures, the planet decodes straight into place, and both big buffers are taken at start-up (PSRAM free and largest block logged; an out-of-memory picture logs both) (PERF).
- 4.5.25 (installed): planet photos are kept in flash after their first download: they open at once, offline and after a restart, and are never downloaded again (UI-61c). Two compile warnings fixed (the Sun picture URL buffer, the picture button).
- 4.5.24 (installed): the Milky Way drawn faintly under the stars, brighter where it is thicker and fading toward the horizon (UI-64). Comets: the few brightest (magnitude 6.5 or brighter, from JPL, checked daily) on the map with a head, a name and a tail pointing away from the Sun; a card (brightness, distances, perihelion, whether it returns) with Find; a list row; and a Sky Event alert when one is magnitude 6 or brighter, 10° up in a dark sky (UI-63). Switches Show Milky Way and Show Comets (Celestial tab and web). Sun picture URL buffer widened (a compile warning).
- 4.5.23: every duration reads the same way, units spaced: "45s", "12m", "1h 31m", "2d 4h" (UI-17a). Sun, Moon, Earth and region keep the picture before the latest; < and > at the bottom flip between them (UI-59c). The planets are NASA/ESA photos with tonight's phase shaded on (Mercury, Venus, Mars) and Jupiter's moon strip below; drawn as before when the photo cannot be had (UI-61a). A Photo | Drawn switch under the planet (like Globe | region) picks either; the choice is kept (UI-61b). Debug page headers are the size of the SKY TRACKER title (mono16, UI-27).
- 4.5.22: the picture button reads just "Image" (Find's size) and is on the planet cards too. The picture screen has tabs along the top (Sun, Moon, Earth, and the planet when opened from its card) with Close top right (UI-59b). New: the Earth from NOAA GOES-18 or GOES-19 GeoColor, whichever is nearer (UI-62), and the planets drawn on the device: phase, Saturn's rings at their tilt, Jupiter's belts and its four big moons where they are now (UI-61). The Earth tab has Globe / region (NOAA's sector for the observer, Alaska here; UI-62a). Pictures float on the page: black sky becomes the page colour and a soft round edge replaces the square (UI-62b).
- 4.5.21: "Moon image" on the Moon card: NASA Dial-A-Moon for the current hour (phase, libration, size, tilt; south-up in the southern hemisphere), decoded on the device by the ROM's TJpgDec (UI-60). Both pictures show download progress: a bar and "46% - 520 of 1124 KB" until the first picture, then "updating 26%" in the caption; stages "Finding the latest picture..." and "Unpacking the picture..." (UI-59a).
- 4.5.20: NOAA OVATION aurora nowcast (downloaded only after dark: every 75 min when quiet, every 15 min when aurora is in play, at once when a solar wind warning starts; UI-58a): the chance of visible aurora overhead and the most within 800 km (with its direction), a Sun-card line, a debug line, HA sensors Aurora Chance and Aurora Chance In View; after dark it can raise the aurora notice to "Aurora likely now - 35% overhead, 60% low N" (UI-58). "Sun image" on the Sun card opens a full-screen picture of the Sun from NOAA GOES SUVI (30.4 nm), decoded on the device from the 1280 px PNG with a box filter to 360 px (UI-59).
- 4.5.19: "Find" on every details card opens a pointing screen: an arrow that turns with the compass (or relative to the map's top without one), "Turn right 40°", "Look up 30°" (UI-57); 12 deep-sky objects on the star map with cards (UI-56); rocket launches from Launch Library 2: alerts for any launch in its last hour and for launches within 1,500 km a day ahead, list rows, a Next Launch sensor (UI-54); solar wind (Bz, speed) from NOAA's real-time feed: an early aurora warning, a Sun-card line, two sensors (UI-55); the ISS and Tiangong elements from SatNOGS while CelesTrak refuses (DATA-13). Full-Moon/Moon-conjunction/lunar-eclipse alerts use the full-Moon icon (4.5.15 had the new-Moon glyph by mistake).
- 4.5.18: debug screen in two pages at 16 px (UI-27): 1 System (IP, Wi-Fi, versions, uptime, memory, orbital data, reset and last crash), 2 GPS & compass (two newest NMEA lines) with a Calibrate compass button (disabled without a compass). Buttons, not page taps: Close top right on both pages, < Prev bottom left, Next > bottom right.
- 4.5.17: the debug page shows the IP address (UI-27).
- 4.5.16: compass calibration from the display: a Compass row on the Location tab (greyed out without a compass) opens a guided calibration screen, a ring of 36 sectors that fill as the display turns, the percentage, and a pace hint (Turn a little faster / Good pace / Slow down); it ends by itself once every sector is seen after a full turn (60 s at most). The web/HA Calibrate Compass button opens the same guide (HW-9b).
- 4.5.15: sky events, all worked out for the configured latitude/longitude: meteor showers with the radiant on the map and a shower card (UI-47); full-Moon names, Blue Moons and supermoons (UI-48); the Moon near a planet or bright star (UI-49); 20 named bright stars with tap cards (UI-50); lunar and solar eclipses seen from here, with alerts and a line on the Moon card (UI-51); Tiangong marker, passes, card, list row and alerts (UI-52); time scrub, a long press on the map (UI-53). New Sky Event Alerts switch (Celestial tab, web Celestial section) covers UI-47..51 and the solstice/equinox alerts. New sky_events.h (fitted precise Moon, eclipse geometry, tables). Location-specific wording removed from star and constellation facts.
- 4.5.14: solstice and equinox alerts, from 3 days before until the end of the day: "Winter solstice Dec 21 - shortest day of the year" (UI-41b).
- 4.5.13: Sun/Moon cards keep the searched times, not text: the card is written on every refresh, so units, clock format and live values are always current (UI-36c). Day length reads "Daylight 11h 58m, losing 6 min/day" (seconds near the solstices) in place of "Day 11h58m, 5m45s shorter".
- 4.5.12: Sun and Moon cards follow the Miles and 24-hour switches at once; their lines were cached for 10 min without regard to either (UI-36).
- 4.5.11: every text font (mono12/14/15/16/18/24) is anti-aliased (bpp: 4); ESPHome's default of 1 bit per pixel left the thin Roboto Mono strokes jagged on the panel. Host renders use Montserrat and did not show it (VER).
- 4.5.10: the Sun's distance from the Earth: on the Sun card ("Distance 1.003 AU (150.0 million km)", or mi) and in the list's HEIGHT column ("1.00 AU") (UI-36b).
- 4.5.9: constellation cards show the constellation's own stick figure (40 px, north up, east left) in place of the generic star (UI-46a; new generated sky_cfig.h, ~2.9 KB); GPS Satellites and GPS Altitude report state_class measurement again, so Home Assistant keeps their long-term statistics (NET-9).
- 4.5.8: the list's CL column reads STR for the Sun and LUN for the Moon (UI-40b).
- 4.5.7: the ISS pass line no longer sticks on "No ISS pass above 10° in 4 days": an empty pass list is searched again every 10 min once the ISS elements and the clock are in (DATA-6a). The first search could run before either was there and was never repeated.
- 4.5.6: switches Aurora Alerts and Planet Alerts (Celestial tab and web Celestial section, with icons; default on) (UI-41a); conjunction and parade alerts only within 3 days of the event (UI-41); planets keep their name tags when low, so Mars and Venus near the horizon are labelled (UI-40); icons on every web System entity (NET-9).
- 4.5.5: every details-card picture is 40 px, the Moon picture's size, in the card's top right corner: type icons (mdi_card40), the planet pictures (40 px set in sky_picons.h), the constellation star (UI-30).
- 4.5.4: alerts in one font (mono15, 1 pt over the old mono14) and 3 px lower (UI-41); details-card icons at the card's far right (UI-30); firmware version in the lower right of the Wi-Fi screen (NET-2b).
- 4.5.3: list directions carry a degree sign ("WNW 300°") (UI-10); tapping a constellation name opens a card with what the name means, its story, brightest star and a sight to find (UI-46).
- 4.5.2: the list shows each planet's own picture; web page has a Celestial section (Show Stars & Constellations, Stars Only After Dusk, Show Motion Trails, Show Planets) after Satellites; two compile warnings from 4.5.1 fixed (NET-6, UI-40b).
- 4.5.1: planets in the list (UI-40b); the Sun and Moon cards tell the Roman/Greek story of their names (UI-36a).
- 4.5.0: planets on the map with pictures and name tags, tap for a planet card (distance, light time, magnitude, the myth behind the name) (UI-40, UI-40a); status-line alerts in rotation: visible ISS pass, aurora, visible planets, upcoming conjunctions and planet parades (UI-41); Starlink trains (UI-42); markers below 10° dimmed and untagged (UI-43); Auto Brightness (UI-44); Night Mode, red after dusk (UI-45); launch dates on satellite and ISS cards (UI-24b); crash capture with a Last Crash sensor and debug-page lines (FAIL-10); settings in three tabs, Display, Location, Celestial, with icons for Planets, Sat Trails, Stars and After dusk (UI-16a, UI-33). One-off downloads after flashing: the Starlink list (its record gained a launch key) and the SATCAT list (for launch dates).

## Changes in rev 4.4
- 4.4.1: settings text light again: the tab panels and coordinate boxes set text_color (LVGL obj containers otherwise take the theme's dark grey) (UI-16a).
- 4.4.0: settings page in two tabs, General (cog: heading, coordinates, time format, brightness, miles) and Celestial (weather-night: LEO cone slider, layer switches) (UI-16a). New LEO Cone setting, 15-90° in 5° steps, on the Celestial tab and as the LEO Cone entity; applied on Save and restored at boot (UI-39).

## Changes in rev 4.3
- 4.3.12: Starlink cone 80° (above 10° elevation), marker pool 200 (DATA-3, UI-14). 90° was tried in 4.3.9–4.3.11: from 61.6°N about 380 Starlink are above the horizon, most of them in a dense band at the rim (the outer 10° of elevation covers more sky than everything inside it, and the 53° shells crowd the southern horizon). A 400-marker pool crashed at boot and the device rolled back; 200 runs cleanly. Pool creation now feeds the watchdog and logs its time; a short pool is logged.
- 4.3.8: the web page header logo (top left) is a Sky Tracker badge: sky disc, dotted orbit, satellite, Sun (NET-10a).
- 4.3.7: the tab icon now sticks: the web app replaced it with its house at start-up (NET-10).
- 4.3.6: the web page's browser-tab icon is a yellow sun (NET-10).
- 4.3.5: a layer switched on after boot (MEO, GEO, Starlink, LEO/debris) is drawn from the flash cache at once; a stale list refreshes on the next 5-minute element pass instead of holding the display for downloads (DATA-10). Many markers appearing together cause one sky redraw, not dozens (PERF-11). Firmware Version reads "ESPHome 2026.9.0 · Sky Tracker 4.3.5" (NET-9).
- 4.3.4: web page sections ordered Location & Sky, Satellites, Display, ISS, System (OTA form last); aurora notice in mono14 (NET-6, UI-38).
- 4.3.3: Debris is its own layer: it shows whenever its switch is on, even with LEO/MEO off (before, rocket bodies were filtered out with LEO) (UI-35).
- 4.3.2: black page background (#000000); sky disc a deep, slightly desaturated blue (#0B1220); colours blended against the sky follow SKY_BG (UI-11).
- 4.3.1: upgrade panel title and progress centred as a pair (UI-12).
- Everything listed under 4.2 after the USB flash (Sun/Moon cards, aurora, WMM declination, icons, flags, debris switch, larger text) ships as 4.3.0. From here fw_version is bumped on every install (patch for fixes, minor for feature batches) (BUILD).
- Details card: type icon centred vertically on the title line, with the country code and flag (UI-30).

## Changes in rev 4.2
- Flash layout: two 5.75 MB app slots plus a 4 MB `skydata` data partition (subtype 0x40). Needs one USB flash; settings (NVS) reset once (DATA-10).
- Orbital data is cached in flash and reused after a reboot, so a restart does not re-download from CelesTrak (DATA-10).
- Orbit-class switches LEO (Starlink is a sub-switch of LEO), MEO, GEO. Defaults: LEO on, MEO on, GEO off. GNSS and GEO lists are downloaded only while their switch is on (DATA-11, UI-28).
- MEO satellites drawn as square dots with a G/E/R/C constellation tag; GEO as small dim dots without labels; a class column (CL) on the list page (UI-28).
- Rocket bodies and debris (" R/B", " DEB") drawn as a trash-can icon in place of the dot and label (UI-28).
- HEO shown on the details card; HEO counts as MEO for the switches (UI-28).
- Coordinates kept to 3 decimals (about 110 m); HA observer step 0.001 (UI-16).
- Distances in miles or km: switch "Distances in Miles" (settings "Miles", default km) (UI-29).
- Settings: the Starlink switch is editable only while LEO is on (greyed otherwise) (UI-16a).
- Details card and list show an icon for the object type (UI-30).
- Map: the ISS is a space-station icon instead of the orange dot (UI-5); the Moon is drawn as its real phase, lit side toward the Sun, replacing the shadow-disc approximation (UI-6).
- Map: every satellite is an icon (type icon, orbit-class colour); GNSS satellites carry a small flag (USA/EU/Russia/China) in place of G/E/R/C; Starlink as dim uplink icons; GEO stays as small dots (UI-31).
- Layer counts in a column on the right edge, icon + number, one row per layer that is on (UI-32).
- A CelesTrak 403 hold (2 h) is saved to flash and survives a reboot (DATA-12).
- Settings: icons beside the LEO, Starlink, MEO and GEO switches (UI-33).
- Debris switch ("Show Orbital Debris" in HA, "Debris" in settings, default on) hides rocket bodies and debris (UI-35).
- With the details card up, any touch (sky, header, gear) only closes it (UI-24).
- Owner flag after the ISO country code on the details card; map GNSS flags now come from the same flag set (UI-34).
- Job requests merge while one of the same kind is waiting: no "job queue full" bursts during long downloads (ARCH-4).
- Sun and Moon near the horizon are pulled in so they sit wholly inside the horizon ring (no flat cut at N/E/S/W) (UI-6).
- Defaults now match Dan's settings (2026-09-25): 12 h clock, miles, MEO off, debris off, stars any time; observer 61.581, -149.439 (BUILD).
- Magnetic declination is computed for the location from WMM2025 (HA: read-only "Magnetic Declination" sensor); the fixed setting is gone (HW-9a).
- Tap the Sun or Moon (or the almanac Moon, lower right) for a card; the body's path across the sky is drawn with hour marks (UI-36).
- Aurora: NOAA planetary Kp (every 3 h) on the Sun card with an outlook, and an "Aurora Kp" sensor in HA (UI-37).
- Starlink cone widened to 70° (above 20° elevation); marker pool 96 (DATA-3, UI-14).
- Aurora notice on the status line (aurora icon, soft green) plus a faint glow on the map's northern horizon when aurora is forecast for tonight's dark hours; plain words instead of Kp (UI-38).
- Fixed: "debris off" was lost at boot (the data task started with the default); counts on the right are larger (UI-32, UI-35).
- Details card text 2 pt larger (mono14 body, mono18 title; card 352 px wide); status line 3 px lower so the aurora notice clears the title; debug page shows the age of the orbital data (UI-24, UI-27, UI-38).
- Build warnings fixed (dangling else in sgp4.h, NMEA copy in sky_sensors.h, and -Wall/-Wextra host findings).

## Changes in rev 4.1
- Optional GPS (ATGM336H, NMEA 9600 baud, RX only on GPIO42 (module pin 35)) supplies latitude/longitude and locks them (HW-8).
- User-facing text says "orbital data" instead of "orbit data"/"orbits"; the HA sensor is now "Orbital Data Age".
- The boot / Wi-Fi status page lists whether a GPS (and its fix) and a compass (and its chip) are connected (NET-2b).
- Fixed: layers switched off were still downloaded at boot (net_init copied a Config with both layers on) (UI-22).
- GPS diagnostics and baud auto-detect (HW-8a).
- Optional magnetometer (GY-271: QMC5883L/QMC5883P/HMC5883L, auto-detected on its own I2C bus, GPIO1 SDA / GPIO2 SCL) supplies the map heading and locks it; calibration, offset, declination and reverse controls (HW-9).

## Changes in rev 4
- Orbits computed on the board: CelesTrak orbital elements are downloaded twice a day and SGP4 runs on the ESP32. N2YO and its API key are gone (DATA, MOTION).
- Tap a satellite, Starlink or the ISS for a details card: az/el, height, range, speed, sunlit, orbit class/period/inclination, NORAD and international IDs. Tapping the ISS also draws its next pass across the sky (UI-24).
- The header countdown is replaced by a status line with clear, coloured messages (download failures, HTTP 403, stale data) (UI-25).
- Motion trails behind moving objects, switchable ("Show Motion Trails") (UI-26).
- The list page is a real table: Name, Country, Az, El, Height (UI-6).
- Diagnostic entities: uptime, Wi-Fi signal, IP, network, firmware version, free RAM/PSRAM, orbit data age, objects tracked, data status (NET-9).
- Backlight is now a 1-100 % number in HA and on the web page, not a 0-255 light (HW-6).
- Leftovers removed: N2YO code/entity/secret, boot diag lambda, per-fetch countdown.

## Changes in rev 3.5
- Star map: stars, constellation figures and names under the satellites, optionally only after dusk (UI-21).
- Satellites and Starlink can be switched off; a hidden layer makes no API calls and has no countdown (UI-22).
- Four new switches on the settings page and in HA; the settings keyboard appears only while a text field is being edited (UI-16).
- LVGL draw buffer moved to internal RAM (12.5%) to relieve the PSRAM bus (PERF-4).
- Diagnostic "Reboot" button in HA and the web UI; icons on every entity (NET-4).
- Clock font has "-" (no empty boxes before time sync); OTA panel title glyphs fixed and the panel text centred (UI-12).
- Seven major constellation figures drawn more prominently (UI-21).
- Fallback AP after 30 s with no reboot timers (NET-2a); a Wi-Fi status page that the device boots into, shown until Wi-Fi connects (NET-2b).
- Observer position shown top centre, between the title and the clock (UI-23).
- Boot diagnostic no longer walks the heap: heap_caps_get_largest_free_block() runs with interrupts off and glitched the panel (FAIL-9).

## Changes in rev 3.4
- Satellite name tags show the owner's country code, from the CelesTrak SATCAT (UI-18).
- Where satellites or their tags overlap, only the lowest satellite keeps its tag (UI-19).
- Sun rise/set bottom-left; Moon rise/set, phase name and a phase picture bottom-right (UI-20).
- The N2YO key is shown in plain text, not masked (NET-3).

## Changes in rev 3.3
- Brightness is one HA entity (the Backlight light). Web-page firmware upload is back on (HW-6, NET-1).
- The N2YO API key is editable from HA and the web UI (NET-3).
- Web server on port 80 (web OTA off). Open fallback AP named with the MAC suffix, plus captive portal (NET-1, NET-2).
- Starlink markers now move: the layer is fetched whole-sky so each Starlink has a velocity by the time it enters the cone (DATA-3, UI-4).
- Brightness is now a HA number (HW-6), and the settings text fields have readable text (UI-16).
- Fixed the solid black screen after uploads: the panel gets a software reset at boot (HW-7). buffer_size 50% stays but was not the cause (PERF-4, FAIL-1).
- There is a settings page behind a gear button (UI-16). It sets heading, latitude and longitude (decimal or DMS), time format, and brightness.
- The observer location can be changed at runtime (CTX-2, ARCH-10). Time format is new (UI-17). Backlight minimum is new (HW-6).
- Page flipping moved from the touchscreen to LVGL, so the gear gets its own taps (UI-2).

## Changes in rev 3.2
- Code and read-only data run from PSRAM, with 64-byte data cache lines (PERF-10). This fixes the remaining jitter and shift.
- The map can be rotated to any heading from Home Assistant (UI-15).

## Changes in rev 3.1
- Fixes the panel jumping on every update (FAIL-8): LVGL's dirty-area buffer is raised to 128 (PERF-8), and the map is drawn without transparency or shadows (PERF-9).

## Changes since rev 2
- PERF-6 now covers marker labels and colours only; HUD text may change on the tick (new PERF-6a).
- One geometry path. N2YO's ISS az/el is a logged cross-check, not a drawing input (DATA-5, MOTION-6).
- Arc parameters travel in the published snapshot. The loop never reads task history (ARCH-7, ARCH-5).
- UI-13 restyles only the layer that received fresh data. ISS data never restyles satellites.
- Marker pools are capped: 40 satellites, 48 Starlink (UI-14). Extra satellites are counted but not drawn.
- MOTION-5 decay is tied to the 2 s tick. Visible steps are now accepted (MOTION-7).
- UI-10 explains how lines are allocated and truncated. DATA-6 says when radiopasses is used.
- DATA-2 radius defaults to 90. The UI-8 countdown steps by 2 s by design.
- New: fetching and motion pause during OTA (BUILD-6). HA sensors read only loop-owned data (ARCH-9).
- ARCH-8 now requires heap_caps allocation for big containers, because malloc never reaches PSRAM on this build.
- MOTION-3 now documents the start-up catch-up that follows from it.

## 0. Context
CTX-1  Device: Guition ESP32-4848S040 (ESP32-S3, 16MB flash, 8MB octal PSRAM,
       480x480 ST7701S RGB panel, GT911 touch), ESPHome 2026.9 + ESP-IDF, LVGL 9.5.
CTX-2  Purpose: an always-on sky map showing satellites, the ISS, the Sun and the Moon
       for one observer. The default is Wasilla, AK: 61.581, -149.439, 100 m. The
       location can be changed on the settings page or from HA (UI-16).
CTX-3  Files: /homeassistant/esphome/sky-tracker.yaml (config) in the root; every C++
       source in /homeassistant/esphome/sky-tracker/ (sat_tracker.h: scheduling and LVGL
       drawing; sat_net.h: fetch task; sky_*.h, sgp4.h), listed in esphome: includes:.

## 1. Architecture (ARCH)
ARCH-1 HTTP and JSON work MUST run on a dedicated FreeRTOS task (sat_net.h),
       never on the main loop.
ARCH-2 The task MUST use esp_http_client directly with the certificate bundle,
       NOT ESPHome's http_request component, which assumes the loop thread.
ARCH-3 The `http_request:` block MUST stay in the config even though it issues no
       requests. It is what pulls in esp_http_client, esp-tls, the certificate
       bundle and ArduinoJson.
ARCH-4 Work reaches the task as jobs on a queue (ABOVE, STARLINK, ISS, PASS).
       Queueing MUST return immediately. A full queue is logged, and the job is
       retried on the next tick.
ARCH-5 The task parses into staging copies. It swaps them into a pending slot
       under a mutex. The main loop collects them with drain() on its tick. drain()
       MUST NOT block: if the mutex is busy, it tries again on the next tick.
       drain() reports which layers (SAT, STARLINK, ISS, PASS) are fresh.
ARCH-6 LVGL MUST only ever be touched from the main loop. The task MUST NOT call
       LVGL, any ESPHome component, or sensor-publishing code. The one exception is
       ESP_LOGx logging, which ESPHome buffers safely from other tasks.
ARCH-7 Dead-reckoning history (each satellite's last sighting) belongs to the
       task alone, and only the task reads or writes it. Every published satellite
       record carries its own motion: unit position vector at the fix, fix time,
       rotation axis, angular rate, altitude rate, and a has-velocity flag.
ARCH-8 The task's stack is 16 KB in internal RAM. Response buffers, JSON documents,
       record lists, dead-reckoning history and marker pools live in PSRAM. ESPHome
       builds with CONFIG_SPIRAM_USE_CAPS_ALLOC, so plain malloc/new NEVER reaches
       PSRAM: these MUST use heap_caps allocation (PsramAlloc in sat_net.h).
ARCH-9 Home Assistant sensors read only loop-owned copies, never the pending slot.
ARCH-10 The observer location is written only by the loop, under the mutex. The task
       copies the configuration at the start of each job. Dead-reckoning history
       holds sub-satellite points, which do not depend on the observer, so a move
       keeps it. A move re-projects every marker at once (a snap) and refetches
       satellites, the ISS and passes immediately. Starlink keeps its 45 s offset.

## 2. Hardware constraints (HW)
HW-1   Panel pins, timings and 180° rotation come from the mipi_rgb model
       GUITION-4848S040. Rotation MUST be expressed as transform.mirror_x/mirror_y
       false, NEVER as `rotation:` (it is rejected when LVGL is attached).
HW-2   Touch is GT911 on the same I2C bus (SDA 19, SCL 45). A tap MUST flip pages,
       so touch orientation doesn't matter and MUST NOT be transformed.
HW-3   The backlight is LEDC on GPIO38, exposed to Home Assistant. It uses restore mode
       RESTORE_AND_ON, so it always boots on and keeps its brightness.
HW-6   The backlight output has min_power = `backlight_min` (default 0.10), with
       zero_means_zero. Every brightness from 1% up maps to at least that duty, so
       neither the slider nor HA can dim the panel to invisible. Only an explicit
       OFF turns it off. Raise the value if the lowest setting is still too dark.
       Brightness has ONE HA/web entity: the "Backlight" number, 1-100 % (slider).
       The light behind it is internal, because the web UI showed its brightness as
       0-255 (rev 4). There is no off from HA. The light's on_state keeps the
       number and the settings-page slider in step; both ignore unchanged values,
       so they can't loop.
HW-8   GPS (optional): UART RX on `gps_rx_pin` (GPIO42, module pin 35), 9600 baud.
       Not RXD0/GPIO44: the USB-serial chip drives that line and drowns a GPS out, pull-up so a missing module reads idle. "Present" = NMEA
       characters in the last 10 s. A fix = location < 5 s old, >= 4 satellites,
       HDOP < 5. After the first fix the location is locked: the observer follows
       the GPS (rounded to 4 decimals; saved only on moves of 0.0001° for the first
       fix and 0.001° after), settings-page fields are disabled, and HA/web edits are
       undone within 1 s. Before a fix, or once the module goes quiet, manual entry
       works. "Location Source" text sensor; GPS Satellites/Altitude diagnostics.
HW-8b (Removed) A one-off pin finder watched GPIO 43/44/42/41/40/1/2/39/47/48 with
       edge interrupts (VSYNC as control) and located the GPS on GPIO42.
HW-9   Compass (optional): on I2C bus_b (GPIO1 SDA, GPIO2 SCL, 100 kHz; the relay-3/2
       pins, so again only without relays), probed at boot and every 30 s while
       absent: QMC5883L 0x0D (reg 0x0D = 0xFF), QMC5883P 0x2C (reg 0 = 0x80),
       HMC5883L 0x1E ("H43"). ESPHome's qmc5883l component covers only the first. Read at 5 Hz on the main loop,
       smoothed as a unit vector. Heading = atan2(Y, X) (negated with "Compass
       Reverse") + magnetic declination (computed from WMM2025 for the location,
       HW-9a; read-only "Magnetic Declination" sensor in HA) +
       "Compass Offset". Published to Map Heading with publish_state (no flash
       writes) when it moves >= 1°. While present, heading edits are rejected.
       "Calibrate Compass" records X/Y min/max for 30 s (hard-iron centre and
       soft-iron scale, saved in globals); the status line shows a countdown.
       Mounting: level, chip up, X arrow toward where the map top should face,
       away from the board, speaker and steel. 10 bus errors in a row = unplugged.
       Lambdas call esphome::millis(): the gps component pulls in TinyGPS++, whose
       own millis() makes the bare name ambiguous.
HW-8a  GPS diagnostics: a log line every 10 s (not sent to HA) and the debug page:
       RX pin level, edges/s (GPIO any-edge interrupt counted for 1 s in every 5,
       only until the first valid NMEA sentence), bytes/s, baud, NMEA checksums
       passed/failed. If the line is busy but no sentence passes for 5 s, the UART
       steps through 9600, 115200, 38400, 57600, 4800, 19200 baud.
HW-4   GPIO19/20/45 warnings are expected (panel and touch wiring) and MUST be ignored.
HW-7   The ST7701 MUST get a software reset (SWRESET 0x01, then 120 ms) before ESPHome
       initialises it. The board has no panel reset pin, and ESPHome's init
       sequence has no SWRESET, so after a warm reboot (every OTA install) the
       panel sometimes kept its old state and stayed solid black until a power
       cycle. sat::panel_soft_reset() bit-bangs the 9-bit 3-wire command on CS 39,
       SCK 48, MOSI 47 from an on_boot at priority 1100, before the SPI bus (1000).
HW-5   pclk_frequency MUST stay at the model default (12 MHz). Lowering it to
       10 MHz produced a blank screen.
HW-9a  Magnetic declination is computed on the device from the World Magnetic Model
       WMM2025 (NOAA/NCEI + BGS, public domain; coefficients in sky_wmm.h, valid
       2025.0-2030.0), degree/order 12, for the observer's latitude, longitude and
       altitude and the current decimal year: at boot, whenever the location changes
       (GPS or settings), and daily. Checked against pygeomag to 1e-8° (Wasilla
       2026.73: 14.17° E, dip 74.29°). The same model's dipole terms give the
       geomagnetic latitude used by the aurora outlook (UI-37). HA gets it as the
       read-only diagnostic sensor "Magnetic Declination"; the old editable number
       and the mag_declination substitution are gone. Replace the coefficient table
       with WMM2030 when it is published (late 2029).

## 3. Data (DATA)
DATA-1 Orbital elements come from CelesTrak GP data (CSV/OMM, no key):
       GROUP=visual (the ~150 brightest satellites; the `sat_group` substitution),
       CATNR=25544 (ISS) and GROUP=starlink (~11k objects, streamed and parsed line
       by line, never held whole). Owner codes come from the CelesTrak SATCAT.
DATA-2 Elements are refreshed every 12 h, and only while Wi-Fi is up (NET-8). A
       failed download is retried after 10 min (2 h after an HTTP 403: CelesTrak refuses repeat downloads within 2 h and may block clients that keep retrying). The
       previous elements stay in use meanwhile.
DATA-3 "Orbital data age" is the time since the oldest group in use was downloaded
       (not the element epochs, which vary by object). Over 3 days is flagged on
       the status line and in "Data Status" (UI-25); positions keep being computed.
DATA-4 Hidden layers (UI-22) are neither downloaded nor propagated.
DATA-9 Element sets whose epoch is over 10 days old at download (decayed or lost
       objects) are dropped rather than propagated.
DATA-5 One data pull per day is enough for position and velocity: SGP4 gives both,
       and LEO errors grow to only a few km per day of element age.
DATA-7 Sun and Moon MUST be computed on-device (no network), using low-precision
       Astronomical Almanac formulae. Refraction is applied, and the Moon is
       corrected for topocentric parallax. Sunlit state is computed on-device from
       the Sun vector and a cylindrical Earth shadow.
DATA-10 Flash cache in the `skydata` partition: region A (ISS, visual, owners) at
       0x000000, B (Starlink) at 0x100000, C (GNSS) at 0x300000, D (GEO) at
       0x340000. Each region has a 4 KB header (magic, version, length, CRC32,
       download time) written last, so a torn write is never loaded. A group is
       saved right after a successful download and loaded at boot; its download
       time drives the 12 h refresh, so a reboot does not re-download.
       Regions are loaded at boot only for layers that are on. A layer switched on
       later is filled from its region at the start of the next element job (before
       any download, even inside a 403 hold); that job then ends, so the positions
       job draws it within about a second. A stale list refreshes on the following
       element pass (ELEM_PERIOD, 5 min). An empty region is re-read at most once a
       minute.
DATA-11 GNSS groups (gps-ops, galileo, glo-ops, beidou; de-duplicated, capped at
       200) are downloaded only while MEO is on; the geo group (capped at 740) only
       while GEO is on. GEO positions are recomputed every 5 min.
DATA-12 A 403 hold (next_try after an HTTP 403) is written to one sector of the
       skydata partition (0x0F0000, magic + CRC) and read back at boot, so a reboot
       inside the 2 h window does not retry.
DATA-8 A response larger than its buffer MUST be dropped with a warning. It MUST
       NEVER be parsed half-way.

DATA-6a ISS passes are computed on the device (next 4 days, peaks of 10° and up, 3 kept),
       again when the first has ended or the location changes, and every 10 min while
       the list is empty (once ISS elements and a valid clock exist).

## 4. Display (UI)
UI-1   Page 1 is a polar sky map: zenith at the centre, horizon on the outer ring,
       the map heading (UI-15) up and 90° clockwise from it on the right (north up,
       east right at heading 0), with rings at 30° and 60°.
UI-2   Page 2 is a text list. A tap anywhere except the gear switches between pages 1
       and 2. This is an LVGL on_click on each page, not on the touchscreen. The sky
       disc is not clickable, so taps fall through to the page.
UI-3   Satellite markers are coloured by orbit altitude (LEO < 2000 km near-white,
       MEO teal, GEO ≥ 35000 km amber) and labelled with their name. With the MDI
       icon font they are 14 px type icons (UI-31); without it, 7 px dots.
UI-4   Starlink markers are dim satellite-uplink icons (UI-31; 4 px dim blue dots
       without the icon font), drawn BEHIND satellites and NEVER labelled on the map.
       The cone is `starlink_radius` degrees from overhead (80: above 10° elevation). 90 (the whole sky) is not used: the horizon band holds more Starlink than the rest of the sky combined. Their names appear only in the list. Markers are bound
       to Starlinks that are in the cone now or will enter it within 90 s
       (extrapolated), and are shown only while inside it. The header count, the
       list and the HA sensor count only those in the cone.
UI-5   The ISS is a 20 px MDI space-station icon in orange (16 px orange dot with a white
       border if the icon font is missing), always on top.
UI-6   The Sun (20 px, with an opaque 30 px halo) and Moon (20 px phase picture, lit side toward the Sun) MUST sit above the sky disc and
       BELOW every satellite marker. The Moon is an ARGB canvas rendered like the
       UI-20 phase picture, its terminator turned so the lit side faces the Sun's
       projected position; redrawn when the lit fraction moves 0.4% or that
       direction turns 3°.
       This stacking order MUST be reasserted on each refresh, not left to creation order.
UI-7   Anything below the horizon MUST be hidden, not clamped to the rim.
UI-8   Header: title, satellite counts, clock. Under the title sits a two-line
       countdown, "Satellites {n}s" / "Starlink {n}s" ("Starlink off" when
       disabled). It updates on the 2 s tick, so it steps by 2 by design.
UI-9   Footer: the ISS direction, height and sunlit state, then the next pass with
       its time, countdown, peak height and direction.
UI-10  The list page MUST fit 20 lines, allocated in this order: header, ISS, Sun,
       Moon (4 lines); then the STARLINK section if the layer has any members
       (a section line, up to 8 rows, and "+ N more" if needed, so 10 lines at
       most); satellites, sorted by elevation, get the lines left over. If they
       don't fit, their last line becomes "+ N more". The list sorts a copy.
UI-11  Palette (4.3.2): pages are black (#000000, PAGE_BG); the sky disc is a deep,
       slightly desaturated blue (#0B1220, SKY_BG); cards and panels #101B3D. Dim
       colours (Starlink icons, moon edge, arcs, trails) are mixed against SKY_BG.
       The sky colour is duplicated in the drawing code as SKY_BG and MUST be kept
       in step with sky_box bg_color in the YAML. The panel is IPS: black shows as
       dark grey with some edge glow, which the blue disc helps hide.
UI-12  During a firmware upload the screen MUST show an UPGRADING panel with the
       percentage, then "rebooting". If the upload fails, the panel MUST say so.
       The UPGRADING title (label CENTER, y -14) and the status line (y +18) are
       centred in the box as a pair: the ink of both lines is centred within 0.5 px
       (4.3.1). The status has an explicit text colour: without one it took the
       theme's dark grey and the percentage was invisible on the panel. mono24 carries every
       uppercase letter and "-" so the title and the "--:--" clock never show
       missing-glyph boxes.
UI-13  A refresh restyles one layer in one pass, and only on a tick where drain()
       reported that layer fresh: SAT restyles satellites, STARLINK restyles
       Starlink, ISS restyles the ISS. No layer is ever restyled on a tick
       without fresh data. Spreading a layer's restyle across ticks has been tried
       and removed.
UI-15  The map heading (the compass direction at the top) is a Home Assistant number
       "Map Heading", 0-359°, restored across reboots, with the default set by the
       `map_heading` substitution. The compass letters rotate with it. A change
       redraws every marker once, snapping without the MOTION-5 offset. The list page
       always shows true azimuths.
UI-16  A light-grey gear (LV_SYMBOL_SETTINGS, 46 px target) in the lower right of
       page 1 opens the settings page. That page is skipped by tap-to-flip and has
       Cancel and Save, plus these fields:
       - heading, a whole number 0-359;
       - latitude and longitude, as decimal degrees (4 decimals) or as DMS (degrees,
         minutes, seconds, and an N/S or E/W button); a Dec/DMS switch picks the
         format, and it is remembered. Decimal minutes with empty seconds are
         accepted (GPS style). Values are stored to 4 decimals;
       - time format, 12h or 24h;
       - brightness, a slider 1-100 that applies live. Cancel restores the
         brightness the page opened with.
       Text fields MUST set text_color explicitly: LVGL's theme default is dark grey,
       which can't be read on the navy background. A numeric keyboard edits
       whichever field was tapped last. Save validates
       every field, outlines bad ones in red with a message, and writes the values
       to the HA entities Map Heading, Observer Latitude, Observer Longitude and
       24-hour Clock, all restored across reboots.
UI-17  Clock and pass times are HH:MM in 24-hour mode, or h:MM AM/PM in 12-hour mode
       (default: 12-hour).
UI-17a Durations, everywhere (ages, countdowns, uptime, light time, day length, alerts),
       via sat::fmt_dur: under a minute "45s", under an hour "12m", under a day "1h 31m",
       else "2d 4h". Units always spaced; no zero padding; one format throughout.
UI-18  Satellite name tags end with the owner's country as an ISO 3166-1 alpha-3
       code ("NOAA 19 USA"), mapped from the CelesTrak SATCAT OWNER field. Owners
       with no country (ESA, ISS, ...) keep CelesTrak's code; an unknown owner shows
       no code. The fetch task loads GROUP=visual in one request at most once a day,
       then looks up at most 3 other catalogue numbers per satellites fetch; a new
       code appears on the next fetch. Answers are cached for the boot, unknowns are
       not asked again, and a failed lookup pauses lookups for 5 minutes.
UI-19  Name tags are placed lowest elevation first. A tag is hidden when its dot
       overlaps a lower satellite's dot, or when the tag would cover a tag already
       placed. The check runs every tick.
UI-20  Sun and Moon almanac, in the two bottom corners outside the disc (mono12).
       Bottom-left, y 412: "Sunrise|Sunset h:mm" (Sun yellow). Bottom-right,
       right-aligned 10 px from the edge: a 32 px phase picture at y 360, then
       "Moonrise|Moonset h:mm" (Moon white, y 396) and the phase name with percent
       lit (dim, y 412). The picture sits above the text because the gear (UI-16)
       and footer take the space below. Each line shows whichever event comes
       next; the event is the refracted centre crossing -0.27°, found in 20-minute
       steps over 24 h and refined by bisection. "up all day" is shown when a body
       stays up; a body that stays down gets an empty line (no "down all day").
       Phase names: New (<3% lit), Waxing/Waning Crescent, First/Last Quarter
       (47-53%), Waxing/Waning Gibbous, Full (>97%). Search results are refreshed
       every 15 min, when an event passes, and when the observer moves. Times follow
       UI-17.
       Phase picture: an ARGB8888 LVGL canvas, 4x4 supersampled (smooth limb), lit
       side C_MOON with slight limb darkening, dark side 0x2A3350 (earthshine). The
       lit side is on the right when waxing in the northern hemisphere and mirrored
       in the southern. It is redrawn only when the lit fraction changes by 0.4% or
       the side flips; its size comes from the canvas buffer, not the object, so the
       first draw works before layout.
UI-14  Marker pools are fixed at start-up: 72 satellite markers with labels (LEO and
       MEO) and 200 Starlink markers (80° cone; a 400 pool crashed at boot). Satellites beyond a pool are counted and listed but
       not drawn, and this is logged once per fetch.

UI-21  Star map (sky_stars.h, generated from d3-celestial 0.7.35 data, BSD-3,
       J2000): 288 stars to magnitude 3.5 plus 484 fainter stars that constellation
       figures pass through, 150 figure polylines and 89 names; about 8 KB of flash.
       - Drawn by the sky disc's own LV_EVENT_DRAW_MAIN callback, after its
         background and before its children, straight into LVGL's draw buffer and
         culled to the area being redrawn. No star image: a 270 KB PSRAM canvas was
         tried first and its read-back on every marker redraw starved the panel
         (garbage lines, vsync shift). Positions (about 13 KB, internal RAM) are
         recomputed every 120 s (~1 px of sky rotation) and at once when the
         heading, observer or switches change.
       - Seven major figures (Ursa Major, Ursa Minor, Cassiopeia, Orion, Cygnus,
         Leo, Taurus; sky::FIG_MAJOR) are drawn on top in a brighter blue at 2 px
         with round ends; all others stay faint 1 px lines.
       - Stars: 5/4/3/2 px for magnitude <0.5/<1.5/<2.5/<=3.5, 1 px for fainter
         figure stars, dimmer with magnitude. Figures: 1 px lines in a faint blue;
         a segment crossing the horizon is cut at the horizon.
       - Names of rank 1-2 constellations (46), in dim text, centred on the
         catalogue label point; dropped below 5° elevation, outside the disc, on the
         compass letters or on a higher-rank name. Every tick a name is hidden where
         it would cover a satellite dot or shown satellite tag (UI-19).
       - HA switches "Show Stars & Constellations" (default on) and "Stars Only After
         Dusk" (default off): with the latter on, the layer is shown only while the
         Sun is below -6° (end of civil twilight). Both are also on the settings page.
       - Positions are J2000 without precession: up to ~0.4° off (under 1 px).
UI-22  HA switches "Show LEO" and "Show Starlink" (default on), "Show MEO" and "Show
       GEO" (default off), and "Show Orbital Debris" (default off), also on the
       settings page. Off: the layer's markers are freed, its /above fetch is not
       propagated or downloaded, results already in flight are dropped, and its
       count lines disappear.
       On: fetched immediately. The ISS and passes are unaffected.
       Settings page: four switches in two rows (y 290 and 328); the status line
       moves to y 370. The number keyboard is hidden until a text field is tapped,
       and hides again on OK. Switch changes apply on Save, like the other fields.

UI-23  Header alignment: the clock (mono24) is at y 1 so its digit tops (8 px into
       the line) sit level with the tops of the "SKY TRACKER" capitals (mono16 at
       y 4, 5 px into the line); the counts below it moved up to y 31.
       The observer's position is shown top centre (mono16, dim, TOP_MID y 4),
       between "SKY TRACKER" and the clock: "61.5814°N  149.4394°W", or
       "61°34'53\"N  149°26'21\"W" when the settings page is in DMS mode. It updates
       when the observer or the format changes.

UI-24b Launch date on satellite cards ("Launched 3 Jan 2024"), from LAUNCH_DATE in
       the same SATCAT records as the owner codes (UI-18), kept per NORAD number and
       in the region-A cache (the OwnerRec padding). A cache without dates asks for
       the bulk list once. The ISS card shows 20 Nov 1998 (Zarya).
UI-24  Tap for details: tapping within 22 px of a satellite, Starlink or the ISS
       (ISS preferred) rings it and opens a card on the far half of the disc:
       name + country, az/el + compass, height, range, speed, sunlit, orbit class
       (LEO/MEO/GEO/HEO), period, inclination, NORAD and international IDs. The
       card follows live values and closes on any touch (the card, the sky, the
       header or the gear: that touch does nothing else), when the
       object sets, or after 90 s. Tapping the ISS also draws its next pass as an
       arc (24 points) with the peak marked.
UI-25  Header status line (replaces the countdown), coloured OK/warn/bad:
       "Waiting for the time...", "Downloading orbital data...", "Orbital data updated X ago",
       "Orbital data is X old; positions may drift", "Can't reach CelesTrak (...)",
       "CelesTrak refused the ... download (HTTP 403)" (retried after 2 h, CelesTrak's repeat-download limit; other failures after 10 min), each failure followed by
       "No orbital data yet" or "Using orbital data X old" and "retrying in Y".
       On screen, the all-is-well "... updated X ago" line clears 5 minutes after the
       download (warnings and errors stay); HA's Data Status keeps the full text.
       With only the ISS loaded it reads "ISS orbital data ...".
UI-26  Motion trails: each moving marker leaves 3 fading segments covering the
       last 30 s. HA/settings switch "Show Motion Trails" (default on).

UI-16a Settings tabs: under the header (the open tab's name, Cancel, Save) five icon-only tab buttons,
       91x44 at y 52 (x 4, 99, 194, 289, 384): Display (MDI monitor), Location (map-marker),
       Celestial (weather-night), Satellites (satellite-variant), Alerts (bell-ring); the
       active tab is 0x2D5BD0, the others 0x1A2547. Each tab is a panel (y 104, 336 high);
       only one is shown. Satellites: LEO cone (angle-acute icon), LEO | Starlink, MEO | GEO, Debris |
       Sat Trails, Stations. Celestial: Stars | After dusk, Planets | Comets, Milky Way.
       Alerts: Aurora | Planets, Sky events | Stations, Launch | Dockings, Lunar | Comets.
       The page always opens on Display; switching tabs hides the keyboard. Cancel
       and Save cover all tabs.
       Every obj container on the page (panels, coordinate boxes, tab rows) MUST set
       text_color: the theme gives obj containers dark-grey text, which children inherit.
       Display: Time format, Brightness, Auto bright (UI-44), Miles, Night mode (UI-45)
       (switches at x 232, hints in mono12).
       Location: Heading, Coordinates (Dec/DMS), Latitude, Longitude.
       Celestial: LEO cone slider (UI-39), then rows 44 px apart: LEO | Starlink,
       MEO | GEO, Debris | Sat Trails, Stars | After dusk, Planets. UI-33 icons beside
       LEO (satellite-variant), Starlink (satellite-uplink), MEO (crosshairs-gps), GEO
       (earth), Debris (trash-can), Sat Trails (star-shooting), Stars
       (star-four-points), After dusk (weather-sunset-down); Planets has Saturn's
       20 px picture (MDI 7.4 has no planet glyph; added from C++).
       Starlink (switch, label, icon) is disabled and dimmed while the LEO switch is off.
       Status text under the panels (y 446). Firmware version (fw_version
       substitution, also the ESPHome project version) in mono12, lower right.
UI-27  Debug page: hold the gear 1 s (LVGL long_press_time 1000 ms; a short tap still
       opens Settings via on_short_click). Refreshed once a second while showing,
       mono12: GPS pin/baud/bytes, state, fix quality, satellites used/in view, HDOP,
       position, altitude, age, UTC date/time, RMC status, speed, course, NMEA
       passed/failed, the last 6 raw sentences (62 chars); compass chip/address,
       calibration state, raw X/Y/Z, calibrated X/Y and |B|, centre/scale, magnetic
       heading; uptime, free internal RAM, Wi-Fi RSSI. Tap anywhere to return.
       (4.5.23: page headers in mono16, the same size as the SKY TRACKER title.)
       NMEA is read by sat::hw::nmea_feed (a 50 ms interval drains the UART) instead
       of ESPHome's gps component, so the raw text is available; GPS Satellites and
       GPS Altitude are template sensors.

UI-28  Orbit classes: LEO period < 128 min; HEO eccentricity > 0.25 (treated as
       MEO by the switches); GEO 0.9–1.1 rev/day with eccentricity < 0.1; MEO the
       rest. MEO markers are squares tagged G (GPS), E (Galileo), R (GLONASS) or
       C (BeiDou). GEO objects are 3×3 px dim dots drawn on the overlay (no LVGL
       objects, no labels); tapping one opens the card. Objects named " R/B" or
       " DEB" (or starting "DEB") show only an MDI trash-can glyph (U+F0A79, 14 px),
       centred on the position, never removed by label decluttering. The list has a
       CL column; GEO rows come after LEO/MEO, at most 4, then "+ n GEO".
UI-31  Map icons (when the MDI icon font is present): satellite markers are 14 px
       transparent boxes holding the UI-30 icon in the orbit colour; labels sit
       beside the 14 px box. GNSS labels are a 12x8 ARGB flag image (G USA, E EU,
       R Russia, C China) instead of the letter. Starlink markers are dim
       satellite-uplink icons. GEO stays as 3x3 overlay dots: from 61°N a few
       hundred GEO objects sit low along the south, and icons there would merge.
UI-32  Counts: rows SAT (satellite-variant), SL (satellite-uplink), GEO (earth), only
       for layers that are on, down the right edge under the clock, 26 px apart, 20 px
       icons and mono16 numbers;
       icon in a fixed column, number right-aligned. Text fallback without the
       icon font.
UI-33  Settings icons (mdi_card20), just left of each switch: LEO satellite-variant
       (white), Starlink satellite-uplink (blue; dimmed with its switch while LEO is
       off), MEO crosshairs-gps (teal), GEO earth (gold). Sat Trails, Miles, Stars
       and After dusk have none.
UI-34  Details card flag: 16x12 ARGB flag (flag-icons 7.5.0, MIT, rasterised into
       sky_flags.h, 78 countries keyed by ISO alpha-3) placed just after the ISO code at
       the end of the title; ESA gets the EU flag; none for other agencies (ISS, ...).
UI-35  Debris switch: a layer of its own. R/B and DEB objects from the visual list are
       shown exactly when it is on, whatever the LEO/MEO switches say (GEO debris also
       needs GEO, since it comes with that list). The visual list is downloaded and
       computed while LEO, MEO or Debris is on. Settings row 3 left, trash icon.
UI-36  Sun / Moon cards: tapping the Sun or Moon disc (or the almanac Moon picture)
       opens a card; any touch closes it (UI-24). Sun: az/el; rise and set; solar noon and
       peak elevation; day length and change from yesterday; civil dawn/dusk; golden
       hour (Sun 6° down to the horizon, this evening or tomorrow morning); dark window
       (Sun below -12°); aurora line (UI-37). Moon: az/el; phase name, % lit, age since
       new Moon; rise/set today; next full and new Moon dates; distance (km/mi) with
       "(supermoon)" when full and closer than 360,000 km; 40 px phase picture top right.
       Times from sampled crossings refined by bisection (~1 s); phases from the Moon-Sun
       elongation (~10 min vs USNO). Heavy parts cached 10 min. The current pass above the
       horizon (or the next one) is drawn as an arc with dots at whole local hours.
UI-37  Aurora: NOAA SWPC noaa-planetary-k-index-forecast.json (object or legacy array
       form), fetched every 3 h (retry 30 min), independent of CelesTrak holds. Outlook:
       visible arcs at 69.5 - 2 Kp degrees geomagnetic (a few degrees poleward of the
       oval's faint edge, 66.5 - 2 Kp; dipole from WMM2025); overhead if <= maglat - 2,
       low in north if <= maglat + 2, else unlikely (Wasilla: low N from Kp ~2.5,
       overhead from ~4.5); "sky too bright" if the Sun stays above -12°.
UI-40  Planets (sky_planets.h): Mercury, Venus, Mars, Jupiter, Saturn from JPL's
       approximate Keplerian elements (Standish, 1800-2050), Earth-Moon barycentre
       for the Earth, precessed to the equinox of date; magnitudes from the
       Explanatory Supplement phase laws, Saturn with its ring tilt. Checked against
       PyEphem: within 0.08° and 0.2 mag. Drawn above the horizon as 14 px pictures
       (sky_picons.h, generated) with a name tag in the planet's tint (kept below
       LOW_EL, unlike satellites: UI-43), above the Sun and Moon and below every satellite. Positions
       every 30 s and on view changes. Switch "Show Planets" (default on; Celestial
       tab) hides the layer and its alerts.
UI-40b List: planets above the horizon, highest first, after the Moon: the planet's
       14 px picture (drawn in the table's draw-task callback over a hidden placeholder
       glyph), name in the planet's tint, CL "PL" (Sun "STR", Moon "LUN"), direction, elevation, distance in AU.
UI-36c Sun/Moon card data: body_info_update keeps the searched times and distances (rise,
       set, noon, day length and its change, twilight, golden hour, dark window, Moon
       phase times) for 10 min, about 1000 Sun/Moon positions per Sun rebuild; body_text
       writes the card from them on every refresh. Day change: "gaining/losing N min/day",
       under 90 s in seconds, under 1 s "no change today".
UI-36b Sun distance: |Earth-Moon barycentre| from the UI-40 elements (planets::sun_dist_au),
       on the Sun card after Noon and in the list's last column.
UI-36a The Sun and Moon cards end with the myth behind the name (Sol/Helios,
       Luna/Selene), like the planet cards.
UI-40a Planet card (tap within ~15 px of a planet): Az/El, distance (AU and million
       km or mi), light time, magnitude ("visible now" when UI-41 says so), and the
       story of the name (Roman god, Greek counterpart, one fact); the 20 px picture
       at the right of the title. The card body wraps at the card width.
UI-41  Status-line alerts, when the status line would be all-is-well or empty
       (warnings and errors keep it), one at a time for 6 s each, in this order:
       ISS visible pass (from 30 min before a visible pass until it ends: "ISS
       visible 21:14 - NW to SE, 64° up", then "ISS passing now - look SE, 45° up");
       aurora (UI-38); each visible planet ("Jupiter visible - ENE, 12° up": at least
       10° up, magnitude 1.0 or brighter, Sun below -6°, or -3° for magnitude -3 and
       brighter); upcoming alignments within 60 days: a conjunction (two planets
       within 2°, both at least 5° up, magnitude 1.5 or brighter, Sun below -6°;
       "Mars & Jupiter 1.2° apart - Nov 16 morning") and a planet parade (4 or more
       such planets up at once; "Planet parade - 4 planets Nov 12 morning"). The
       alignment look-ahead samples every half hour, one day per tick, hourly. Icons:
       MDI glyphs (space-station, aurora) or 20 px pictures (planets; the alignment
       icon), text in mono15 (every alert the same font) in the alert's colour, 3 px
       below the status line's position; icons 2 px above the text. Alignment
       alerts show only within 3 days of the event (ALIGN_ALERT_S); the scan
       still looks 60 days ahead.
UI-41d A tap on the header's left part (400 x 56 from the top left: icon, text and around them,
       clear of the counts and the sky disc) while an alert shows opens that alert's details: its object's
       card where there is one (the ISS or Tiangong overhead, a planet, comet, meteor shower,
       the Moon for full Moon and Moon-near alerts), else a details card (K_INFO) built from
       the live data, in the alert's colour and icon, no Find button: launch (mission, local
       date and time, countdown, site, status, distance and direction), space event (type,
       name, time, at the ISS), an ISS/Tiangong pass to come (rise, peak, set; its arc drawn),
       aurora and solar wind (Kp, NOAA nowcast, Bz), conjunction and parade, eclipse (times,
       totality, magnitude), season. With no alert the tap goes on to the page.
UI-38b No aurora alert for a faint aurora (Kp under 3.5) unless the OVATION nowcast says
       likely; the glow on the map is unchanged. Alerts rotate every 60 s (ALERT_ROTATE_S).
UI-41e Alert kinds with their own switch (Settings > Alerts and Home Assistant), all on by
       default: Space Station Alerts (ISS and Tiangong visible passes), Launch Alerts (UI-54),
       Docking Alerts (UI-54c dockings, undockings, EVAs). Launches and events no longer
       follow Sky Event Alerts.
UI-41f Lunar Alerts (on by default): the full Moon (UI-48), the Moon near a planet or bright
       star (UI-49) and lunar eclipses (UI-51). Sky Event Alerts keeps solar eclipses, meteor
       shower peaks (UI-47) and their radiant on the map, and solstices and equinoxes (UI-41b).
UI-41g Comet Alerts (on by default): a comet bright enough to see (UI-63). The Comets map layer
       (Celestial) must be on too: a comet not on the map raises no alert.
UI-72  After 60 s untouched (lv_display_get_inactive_time), checked every 2 s: Settings closes as
       Cancel (nothing saved, brightness restored); the list, debug and About pages go back to
       the map; the picture viewer, Find, the time scrubber and a details card close; an "Update
       available" or microSD offer closes as Not now. Never the boot / Wi-Fi page, an animation,
       an install or a microSD flash in progress.
UI-71  Every progress bar (picture downloads, internet and microSD updates) uses C_BAR 0x2D5BD0 on
       C_BAR_BG 0x1A2547.
UI-41h Splashdown Alerts (on by default): Launch Library "Spacecraft Landing" events whose name
       says splashdown; other events stay with Docking Alerts. Their alert has the parachute icon.
UI-54d The details card of a launch, splashdown or docking alert shows the country's 16x12 flag
       after its title, as satellite cards do (UI-34); the status line shows none: a launch's pad country (Kourou: ESA, Baikonur: Russia; RocketLaunch.Live by the
       country's name), an event's from the spacecraft in its name (Dragon, Cygnus: USA; Soyuz,
       Progress: Russia; Shenzhou, Tianzhou: China; HTV: Japan).
UI-41c Alert icon placement: after the text is set, the icon (glyph or picture) is placed so
       its ink is centred on the text, from the first line's cap top to the last line's
       baseline, from the fonts' glyph metrics (lv_font_get_glyph_dsc).
UI-41b Solstices and equinoxes (next_season: the Sun's apparent ecliptic longitude at a
       multiple of 90°, low-precision Sun and Newton steps; within 15 min of USNO) in the
       alert rotation from 3 days before until the end of that local day, Sun glyph in
       C_SUN: "Winter solstice Dec 21 - shortest day of the year", "Autumn equinox
       tomorrow - day and night nearly equal"; names swap south of the equator. No switch.
UI-47  Meteor showers (ev::SHOWERS: Quadrantids, Lyrids, Eta Aquariids, Perseids, Draconids,
       Orionids, Leonids, Geminids, Ursids; IMO peak solar longitude precessed to date,
       ZHR, radiant, active span, parent). Alert from 3 days before the peak to the end
       of the peak's local day: "Geminids peak Dec 14 evening - up to 150/hr, dark sky"
       (Moon: under 35% "dark sky", under 70% "some moonlight", else "bright Moon").
       From 3 days before to 1 day after the peak the radiant is drawn on the map (a
       small burst and the name, C_METEOR) when above the horizon; a tap opens its card
       (radiant Az/El, peak, rate, active dates, Moon at the peak, parent).
UI-48  Full Moon, on its day and the day before: "Hunter's Moon tonight - full at 13:40"
       / "Beaver Moon tomorrow"; northern monthly names, Harvest Moon (nearest the
       September equinox), Hunter's Moon (the next), Blue Moon (second in a calendar
       month), supermoon (under 360,000 km). South of the equator: "Full Moon".
UI-49  Moon near a planet (magnitude 1.5 or brighter) or Aldebaran, Regulus, Spica,
       Antares, Pollux: searched every 10 min over the next 18 h, hourly, dark sky (Sun
       below -6°) with both at least 5° up; within 5°: "Moon near Jupiter tonight - 2°
       apart", or "Moon 2° from Jupiter now".
UI-50  Named bright stars (ev::BRIGHT, 20): a tap within ~15 px of one (stars shown)
       opens a card: Az/El, magnitude, distance in light years, constellation, one fact.
UI-51  Eclipses seen from here, the next three within 3 years, found one syzygy every 2 s
       (restarted daily and on a location change). Precise Moon: a series fitted to
       PyEphem (ELP) positions, rms 0.002°. Lunar: Meeus shadow radii (1.02
       enlargement), type and magnitude, contacts by bisection, kept when the Moon is up
       during the umbral phase (penumbral: magnitude 0.7 and over). Solar: topocentric
       Sun-Moon separation against the radii, only the part with the Sun up; partial,
       annular or total here. Checked against NASA dates and PyEphem magnitudes (within
       0.005). Alert from 3 days before: "Total lunar eclipse tomorrow 07:16-08:28" (the
       total phase when there is one), "Partial solar eclipse Jun 12 10:12, 23%"; "...
       now - until 08:28" during. The Moon card ends "Next eclipse Dec 31 '28: total lunar".
UI-52  Tiangong (CSS Tianhe, NORAD 48274; Wentian and Mengtian dropped from the lists):
       elements fetched with the ISS (not cached; a failure other than 403/no network is
       retried hourly without a status error), position with the ISS job, passes with
       the ISS passes. Red space-station marker under the ISS, card (launched 29 Apr 2021,
       next pass, China flag), pass arc, list row while up, visible-pass alerts like the
       ISS. Its 41.5° orbit keeps it at or below the horizon north of about 61°.
UI-52b Show Space Stations (Settings > Satellites and Home Assistant, on by default): off, the
       ISS and Tiangong markers, their list rows and an open station card go; passes,
       the footer and alerts are unaffected (alerts have UI-41e).
UI-53  Time scrub: a long press on the map opens a panel over the footer (label, slider
       0-24 h in 10-minute steps, "Now"). Stars, constellations, planets, Sun, Moon and
       radiants are drawn at the chosen time; satellites, trails, trains and GEO dots
       are hidden; alerts, cards for the Sun/Moon and night mode stay on the present.
       "Now" or 90 s untouched closes it.
UI-54  Launches: Launch Library 2 (ll.thespacedevs.com 2.3.0, upcoming, 8, every 3 h; free
       tier 15 requests/hour), parsed with an ArduinoJson filter from a 400 KB PSRAM buffer:
       name, time (NET), status, pad latitude/longitude and location, rocket. Alerts (Sky
       events switch): a launch within 1,500 km from 24 h before ("Minotaur IV from
       Pacific Spaceport Complex in 5h 00m - look SSW"), any launch in its last hour
       ("Falcon 9 launches in 40 min - Cape Canaveral SFS"); flown ones are skipped. The
       list shows the next two in 48 h (rocket, bearing to the site, T-countdown). HA text
       sensor Next Launch. From 4.6.7 the alerts cover the next three launches within 3 days
       (Sky event switch): a launch within 1,500 km says which way to look ("Minotaur IV from
       Pacific Spaceport Complex in 2d 5h - look SSW"), others "Falcon 9 launches in 1d 3h -
       Vandenberg SFB".
UI-54a The launch list is kept in flash (skydata, one sector at 0x0F1000: magic, count,
       CRC, save time, up to 8 records) after every good download, and loaded when the net
       task starts: a reboot shows the launches at once, and the next download waits until
       the saved list is 3 h old. A night of reboots had used up the free tier ("launches:
       HTTP 429") and left the list empty while NROL-97 was 23 minutes out.
UI-54b Backup source: when Launch Library fails (HTTP 429, or no answer), RocketLaunch.Live's
       free feed (fdo.rocketlaunch.live/json/launches/next/5, no key). Names become
       "<vehicle> | <mission>"; time from t0, else win_open (both "2026-10-05T08:17Z"), else
       sort_date (status TBD); result 1/0/2+ = Success/Failure/Partial F, otherwise Go when
       the time is set. The feed has no pad coordinates, so known sites are looked up by
       location name (Cape Canaveral, Kennedy, Vandenberg, Starbase, Wallops, Pacific
       Spaceport/Kodiak, Mahia, Baikonur, Jiuquan, ...) for the nearby alert and bearing.
       Credited on the About page and in the README.
UI-54c Space events: Launch Library 2 events/upcoming (limit 10, list mode) every 6 h, kept in
       flash (skydata sector 0x0F2000, as UI-54a): name, type, date, date precision. Alerts
       (Sky event switch, space-station icon, 0x8FD3FF) for events with a time known to the
       hour or better within 3 days, up to two: "SpaceX Crew-12 Crew Dragon Undocking in
       2d 1h", "... now" for 10 min after.
UI-70  Time zone: Settings > Display, first row (above Time format; the other rows moved down
       42 px): "Time zone", earth icon, a dropdown (190,8 260x36, mono16, list up to 300 px).
       Choices: Auto (from HA), Hawaii, Alaska, Pacific, Mountain, Arizona, Central, Eastern,
       Atlantic, Newfoundland, UTC, London, Central Europe, Eastern Europe, Moscow, India,
       China, Japan, Sydney, Auckland. Saved with Save (Cancel leaves it). The template select
       Time Zone (restored across reboots) holds the choice and is in HA and the web page.
       sky_tz.h keeps pre-parsed POSIX rules for each (US: M3.2.0 / M11.1.0; EU: last Sunday
       of March / October; Sydney, Auckland: southern dates) and sets ESPHome's global zone.
       Auto leaves Home Assistant's zone (sent with every time sync, HA 2026.3+); a chosen
       zone is put back within 0.5 s after each sync, and going back to Auto restores HA's.
UI-55  Solar wind: NOAA SWPC json/rtsw rtsw_mag_1m and rtsw_wind_1m (newest first, a day
       each): the first 15 samples from the active spacecraft are read and the download
       is cut short; 15-minute means of Bz (GSM), Bt, speed, density, every 10 min. Early
       warning (Aurora alert switch, Sun below -12°, no aurora notice yet): Bz <= -10 nT,
       or <= -5 nT with 450 km/s or more. Sun card line, debug page line, HA sensors Solar
       Wind Bz and Solar Wind Speed.
UI-56  Deep-sky objects (ev::DSO, 12: M31, M33, M45, M42, Double Cluster, M44, M13, M81,
       M51, M57, M27, M8): a small ring and the catalogue name with the star layer; a tap
       opens a card (type, magnitude, distance, constellation, one fact).
UI-57  Find: a button at the bottom right of every details card opens a full-screen
       pointer (LVGL top layer): Close top right; an arrow turned by the object's azimuth
       minus the heading (the compass at 5 Hz, true north; without one the map heading),
       green within 8°; "Turn left/right N°" or "Straight ahead"; "Look up N°", "Straight
       up", or "Below the horizon". Targets are recomputed each update (satellites from
       their markers).
UI-58  Aurora chance: NOAA SWPC json/ovation_aurora_latest.json (OVATION, 1-degree grid,
       [lon, lat, %] ordered by longitude; ~0.9 MB streamed, cut short once past the
       observer's longitudes, so ~60% is read). Schedule: UI-58a. "Here" is interpolated between the four cells around
       the observer; "in view" is the most within 800 km (ties: the nearest), with its
       bearing. Shown when under 2 h old: Sun card "Aurora chance 12% overhead, 40% low N"
       (the second part only when it adds 3 points or more), debug line, HA sensors
       Aurora Chance and Aurora Chance In View (%). While dark (Sun below -12°) the
       nowcast can raise the aurora notice (in view >= 10%: possible; overhead >= 30% or
       in view >= 50%: likely), never lower the Kp outlook; the alert then reads
       "Aurora likely now - <chance>".
UI-58a OVATION schedule (net task, checked on each element pass, 5 min): never while
       the Sun is above -6° (sensors unknown, card line hidden). After dark: "active"
       when a solar wind warning is on (UI-55 rule, net::wind_warns), or Kp in the
       interval under way or the next reaches the gate ((67.5 - geomagnetic lat) / 2,
       where the outlook reaches "low in north"; 2.6 at 62.4°), or the last nowcast
       had >= 5% overhead or >= 10% in view; active: every 15 min, quiet: every 75 min;
       a quiet-to-active change downloads at once (the wind warning leads the aurora by
       30-60 min). A nowcast counts for 2 h. Typical quiet winter month ~250 MB.
UI-59  The Sun now: "Sun image" (bottom left of the Sun card; Find stays bottom right)
       opens a full-screen picture: title, Close top right, 360 px image, captions
       ("GOES-19 SUVI, 30.4 nm ultraviolet" / "Taken 19:04, 12m ago"). Source: the last
       frame in products/animations/suvi-primary-304.json (its file name gives the time
       and satellite), else images/.../304/latest.png. The net task (JOB_SUNIMG) streams
       the 1280 px RGBA PNG (~1.1 MB) through sky_png.h: ROM tinfl inflate, row filters,
       the middle 3/4 (the disc fills the view, the burnt-in label drops out) box-averaged
       into RGB565 in PSRAM; the loop copies it under the mutex. Requested when the Sun
       card opens and while the picture is open, at most every 10 min (90 s after a
       failure); an unchanged frame is not downloaded again. "Downloading..." until the
       first picture; errors in plain words.
UI-59a Picture progress: http_stream reports bytes read and Content-Length (net::img_bytes,
       img_total, with img_stage LIST / DOWNLOAD / DECODE and img_stage_kind) for the
       picture job only. Until the first picture: message, "46%  -  520 of 1124 KB" and a
       300x10 bar (orange on navy) under it; with a picture showing, the caption ends
       "- updating 26%". Without a Content-Length only KB are shown.
UI-60  The Moon now: "Moon image" (bottom left of the Moon card; the same button as the
       Sun's, relabelled) opens the picture screen with "The Moon now". Source: NASA SVS
       Dial-A-Moon, api/dialamoon/<UTC hour> (~2 KB JSON: image.url, su_image.url when the
       observer is south of the equator, phase, age, distance), then the 730 px baseline
       JPEG (~110 KB, into a 256 KB PSRAM buffer). A rendering from LRO maps for that hour
       as seen from Earth's centre (no camera photographs the Moon continuously). Decoded
       by the ROM's TJpgDec (sky_jpg.h, host tests: ChaN's later release from LVGL) at
       full size, averaged 2x2 to 365 px and cropped to 360. Captions: "94% lit, 17.7
       days old, 372,667 km away" / "NASA Dial-A-Moon for 20:00, north up". Same request
       rules as the Sun (10 min, 90 s after a failure); the same hour is not fetched twice.
UI-59b Picture tabs: Sun (80), Moon (80), Earth (84) and, from a planet's card, the planet
       (100) along the top at y 4, 36 high; the one showing is navy with orange text; Close
       stays at 376,4. A tab switches in place (the planet tab stays).
UI-59c History: when a newer Sun, Moon, Earth or region frame arrives (different obs
       time), the one showing moves to a second PSRAM buffer (live.img_prev_px). Round
       48x48 < (66,354) and > (366,354) buttons flip between them; the caption ends
       " - the one before" on the older one. Hidden until there is a picture before.
UI-59d Earlier pictures: the < and > buttons are always shown on the Sun, Moon, Earth and
       region pictures. < shows the picture before; when it isn't held (or from an earlier one
       already), JOB_BACK fetches the frame older than the one showing into the work buffer
       (Sun: the newest non-dark frame older than it in NOAA's list, 16 frames ~1 h; Moon:
       Dial-A-Moon for the hour before; Earth/region: the frame 10 min earlier, up to 40 min
       on 404s), which becomes live.img_prev. Caption tail " - earlier picture", " - getting an
       earlier one 45%", " - none earlier". > returns to the latest. Earth/region frames are
       named by scan start (.../GEOCOLOR/YYYYDDDHHMM_GOES18-ABI-ak-GEOCOLOR-500x500.jpg); the
       newest is found stepping back from the current 10 minutes (a frame appears ~10-15 min
       after its scan); a frame already showing is not fetched again.
UI-59e Picture memory: one IMG_PX^2 RGB565 work buffer (img_work_buf) for every picture and
       older frame; img_work_owner (kind, WORK_BACK+kind, or -1) marks its pixels until the loop
       copies them, and the task waits for it to be free (up to 3 s) before decoding. The
       latest picture of each live kind is kept (live.img_px); one "picture before" buffer is
       shared (prev_buf(k) moves it to the kind asking and forgets the other's earlier one).
       Worst case 4 + 1 + 1 picture buffers (~1.5 MB) instead of 3 per kind.
UI-61  Planets drawn (sky_pview.h), redrawn each minute: phase angle and bright-limb
       position angle from the Sun; pole position angle and the Earth's planetocentric
       latitude from the IAU pole directions (Saturn's ring tilt B); flattening; Lambert
       shading with limb darkening, 2x2 samples per pixel. Jupiter: belts by latitude and a
       strip below with the planet and Io, Europa, Ganymede, Callisto at their true offsets
       (Meeus ch. 44; hidden when behind the disc, dark when crossing it), lettered I E G
       C, with E and W at the ends. Saturn: C, B, A rings and the Cassini division, the far
       half hidden by the disc. Mars: polar caps. North up, east left. Checked against
       PyEphem: lit fraction 0.0001, size 0.1%, ring tilt 0.2 deg, moons 0.15 Jupiter radii.
UI-61a Planet photos (Wikimedia Commons, User-Agent "SkyTracker/4.5 (ESPHome display)",
       fetched once per planet, 90 s after a failure): Mercury NASA MESSENGER, Venus NASA
       Mariner 10, Mars ESA Rosetta, Jupiter NASA Hubble, Saturn NASA Cassini. Box-filtered
       to 360 wide (Jupiter 260 at the top, its moon strip below; Saturn letterboxed).
       Each minute: the disc is found by brightness and tonight's terminator shaded on
       (Mercury, Venus, Mars; same Lambert law as UI-61), black lifted to the page colour.
       The credit sits top left of the picture (64,52, dim): "NASA Mariner 10", Saturn
       "NASA Cassini, 2004" (its ring tilt then). No photo: drawn (UI-61), credit reads
       "Photo unavailable: drawn".
UI-61c Photo store: the 192 KB of the skydata partition between region A and the hold
       sector (0x0C0000-0x0F0000). A directory sector (magic "SKYP", per planet: offset,
       length, CRC32 and an FNV hash of the Wikimedia path; its own CRC; written last) and
       the JPEGs appended after it on 4 KB boundaries (~165 KB for all five). planet_image()
       reads the saved copy first and downloads only when there is none (or its path
       changed); a download that decodes is saved. When the next photo will not fit, the
       store restarts empty with that one.
UI-61d Built-in photos: sky_photos.h holds the five Wikimedia Commons JPEGs (Mercury 54 KB,
       Venus 16, Mars 31, Jupiter 41, Saturn 11) as byte arrays with their credits. It was
       generated in the browser straight into the Builder; the copy beside the sources here
       is a HOST TEST STUB (synthetic pictures) and must never be uploaded. planet_image()
       decodes from flash; JOB_PLANETIMG runs without a network. (Replaces UI-61c.)
UI-61e Display-size photos: sky_photos.h stores each photo scaled in the browser to its place
       in the square (Mercury, Venus, Mars 360x360; Jupiter 260x260 at the top; Saturn
       360x184), baseline JPEG q0.92, ~92 KB in all, with w and h. planet_photo_ready() decodes
       it 1:1 (skyjpg::decode_copy, its own work area) straight into planet_src on the loop
       when the view opens (a few tens of ms); the last one decoded is kept.
UI-61f Photo phase shading covers the whole image: pixels outside the disc found by the
       brightness threshold are shaded as the limb in their direction (unit vector, no depth),
       so the photo's soft edge and glow on the night side go dark with it; the lit side keeps
       its glow.
UI-61b (removed in 4.6.15: Photo | Drawn buttons; see UI-61g.)
UI-61g "Tonight's phase": an LVGL switch (56x28, theme look as on the settings page) at
       408,442 with its label to the left, on Mercury, Venus and Mars only; on (default) the
       photo is shaded for tonight's phase (UI-61a/f), off it shows the whole lit disc. Kept
       between opens (sat::planet_phase), redrawn at once. The drawn planet (UI-61) is shown
       only when no photo can be had.
UI-62  The Earth: NOAA STAR CDN GOES18 or GOES19 ABI/FD/GEOCOLOR/678x678.jpg (~450 KB,
       every 10 min), the satellite nearer the observer's longitude (137.0 W / 75.2 W).
       The Last-Modified header dates it; an unchanged frame is dropped after the headers.
       TJpgDec output is box-filtered 678 -> 360 (sky_jpg.h decode_fit, 777 KB PSRAM sums).
       Captions: "GOES-18 GeoColor: day in colour, night lights" / "Taken 12:50, 8m ago".
UI-62a Region: NOAA STAR GeoColor sectors (net::region_for: ak 500 px; hi, pnw, psw, nr, sr,
       umv, smv, cgl, ne, se, pr 600 px), the first box holding the observer; none: the
       globe only. Label strip cropped (top 96.5%, centred square). Under the Earth picture
       Globe | <region> buttons (128x32 at y 442) replace the second caption line; the
       choice is remembered (region first). Captions "Alaska: GOES-18 12:50, 8m ago".
UI-62b Round mask (net::round_mask) after decoding: pixels brightened to at least the page
       colour 0x070B18, then smoothstep to it between r0 and r1 from the centre: Sun
       158-178 (prominences kept), Moon 166-180, globe 176-180 (the corner label goes);
       regions keep their square. Planets are drawn on the same colour.
UI-63  Comets (sky_comets.h). Data: JPL SBDB query API (ssd-api.jpl.nasa.gov/sbdb_query.api,
       fields full_name,e,q,tp,om,w,i,M1,K1, sb-kind=c, sb-cdata {"AND":["q|LT|4","M1|DF",
       "tp|RG|<now-300 d>|<now+500 d>"]}; ~40 rows, ~4 KB; values are strings), once a day
       (an hour after a failure), into live.comets (at most 80). Positions: two-body motion in
       universal variables from perihelion (Stumpff functions, Laguerre-Conway), J2000 ecliptic
       to RA/Dec of date as UI-40; checked against PyEphem: 0.016 deg (NEOWISE 2020,
       Tsuchinshan-ATLAS 2024, 12P 2024, the 38 current rows). Magnitude M1 + 5 log D +
       K1 log r. Every 30 s the MAX_COMETS (4) brightest at magnitude 6.5 or brighter are
       drawn while up: a 6 px head (C_COMET #A8F0E0, soft glow), the name tag on the side away
       from the tail (the discoverer's name; a survey name such as PANSTARRS or ATLAS keeps
       the designation, "C/2025 R3"), and a tail drawn by the disc under every marker: toward
       the point 0.1 AU further from the Sun, 12-44 px by brightness, foreshortened by its
       apparent length against side-on, five fading segments 4 to 2 px wide. Tap: card
       (title the full name; Az/El, "visible now"; magnitude with naked eye / binoculars /
       telescope; distance from the Sun and from us; perihelion date and distance; period or
       "One pass"), Find. List: after the planets, meteor icon, "CM", "mag 5.2". Alert (Sky
       Event Alerts): magnitude 6 or brighter, 10 deg up, Sun below -9 deg: "Comet Lemmon
       visible - mag 5.2, WNW 20 deg up". Switch Show Comets (default on). MDI has no comet
       glyph: mdi:meteor stands in.
UI-64  Milky Way (sky_mw.h from gen_mw.py: d3-celestial mw.json's five outlines filled by
       even-odd crossings along meridians, summed, Gaussian 0.8 deg, 0..255 on a 1-degree
       RA/Dec grid, 64.8 KB of flash). Each 2x2 block of the disc keeps its hour angle and
       Dec (PSRAM, recomputed when the heading or location changes) and a horizon fade (a
       quarter at 0 deg, full from 15 deg); each star period (UI-21) the blocks are shaded
       from the grid (bilinear) at RA = LST - h into 5 shades of C_MW #B4C4F0 over SKY_BG
       (18/30/42/56/72 of 255) and kept as runs per 2-px band (~300); the disc draws them first,
       under the aurora glow, figures and stars. Shown with the star layer; switch Show Milky
       Way (default on). Settings, Celestial tab: a row at y 328, Milky Way (blur-radial)
       left, Comets (meteor) right; the panel is 380 high.
UI-16b Row icons (4.5.38): every row of the Display and Location tabs has an MDI icon
       (mdi_card20) at x 128, 2 px above its label, as on the Celestial tab: Time format
       clock-outline, Brightness brightness-6 and Auto bright brightness-auto (#FFD54A),
       Miles map-marker-distance, Night mode weather-night (#FF6B6B); Heading compass-rose,
       Coordinates crosshairs-gps, Latitude latitude, Longitude longitude (#7EE0B0), Compass
       compass-outline (#A8D8FF elsewhere).
UI-65  Logo (sky_logo.svg, a detailed redraw of the web page's header logo, NET-10a: a
       deep-blue sky disc with azimuth rings, stars and a faint Milky Way band, a dotted
       orbit, a satellite with gold body and blue panels, and a glowing Sun, in a blue rim).
       tools/gen_logo.py renders it to 200x200 RGB565A8, raw-deflates and base64s it into
       sky_logo.h (~20 KB); sky_about.h inflates it once into PSRAM (120 KB) with the ROM's
       tinfl and box-filters smaller copies (LVGL's own scaling garbles RGB565A8). Shown at
       200 px on the boot page and 112 px on About.
UI-66  Firmware from a microSD card (sky_sdfw.h). The TF slot is wired for SPI with CS on
       GPIO42, which carries the GPS (HW-8), so the card runs in SD 1-bit mode: CLK 48,
       CMD 47, DAT0 41; DAT3 (= GPIO42) is never driven, so the GPS keeps working. CLK 48
       and CMD 47 are also the panel's 3-wire init lines: card traffic while the panel runs
       turns the whole screen red (4.6.0/4.6.1, which probed every 5 s; holding the panel CS
       high did not stop it). So the card is looked at once per boot, in on_boot priority
       1100 before the panel's software reset and init (boot_probe): mount FAT (fatfs, sdmmc,
       esp_driver_sdmmc and wear_levelling re-included in the IDF build, long names on,
       VFS directories on), find sky_tracker_firmware_<anything>.bin in the root (any case;
       the highest <anything> by version order), check the image header (0xE9, ESP32-S3,
       app descriptor of the same project, not the running build's ELF SHA-256), unmount.
       Then a modal over everything (lv_layer_top, screen dimmed): "Firmware update",
       "On the card: <ver>", "Installed: <ver>", the file name; Not now / Update. Not now
       (or Close after a failure) holds until the next boot. Update (JOB_SDFLASH): esp_ota_begin for the file's
       size, 4 KB reads written to the other OTA slot, esp_ota_end (checks the image),
       boot slot switched; "Updating firmware" with a bar and "45%  -  1.0 of 2.2 MB",
       "Keep the card in and the power on"; then "Update installed / Restarting with
       <ver>" and a restart 1.5 s later (the restart also re-inits the panel after the
       copy's card traffic, which may tint the progress screen red). Failures say why ("The card could not be read",
       "The file is damaged", ...) and keep the running firmware.
UI-67  About page (Settings > About, a button lower right in place of the version; Close
       at 376,4 returns to Settings): the logo (112 px), "Sky Tracker", "Version <ver>",
       "by Dan Morphis", "ESPHome <ver>, built <date>"; THIS DEVICE (name and IP, Wi-Fi
       and signal, MAC, uptime as "3h 12m", refreshed each second while shown); DATA FROM
       (CelesTrak, NOAA SWPC, NOAA GOES-19/18, NASA SVS, NASA JPL, The Space Devs, NASA
       via Wikimedia); a footer naming the microSD update file.
UI-68  Internet updates (sky_update.h): ESPHome's http_request update entity reads
       manifest.json from GitHub Pages (built by the Firmware workflow for each release);
       first check 3 min after boot, then hourly. Settings > Upgrade Check checks now: "Checking",
       "Up to date" / "Update available" (Not now, Update), download progress, or "Couldn't
       check". A version found by the hourly check shows the update icon (UI-68a). Only a NEWER version counts (numeric compare, "4.6.10" > "4.6.9"):
       ESPHome reports any difference as available, an older release included.
UI-68a The hourly check never opens anything by itself. While a newer version is waiting
       (and nothing is installing), an update icon (LV_SYMBOL_DOWNLOAD, montserrat_28,
       0xFFB547) shows on the map page beside the gear (BOTTOM_RIGHT -50,-2, 46x46); tapping
       it opens the Update available prompt. Not now closes the prompt; the icon stays until
       the update is installed. Settings > Upgrade Check still checks and shows the result. http_request OTA installs it. All mbedTLS memory comes from PSRAM
       (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC): from internal RAM the check's TLS session never
       fit beside sat_net's kept one (4.6.4 tried malloc, which was not enough).
UI-68b Upgrade Check button (web UI System group, weight 2, and HA, config category): runs the
       Firmware update entity's check quietly (no prompt on the display); the result shows on
       the web Firmware row and as the update icon beside the gear (UI-68a).
UI-69  Launch animation (sky_rocket.h): at a launch's T-0 (status not flown/scrubbed, within
       2 min after NET) a cartoon rocket lifts off at 140,400 with a pad cloud, climbs on
       x = 140 + 380 u^1.6, y = 400 - 470 u + 110 u^2 (u 0..1 over 4.5 s, rotated along the
       path, flame flickering), leaves a trail of up to 32 fading smoke puffs (the ground cloud: 6 round billows,
       PAD_BILLOWS, bursting out sideways from the pad, easing out, drifting outward and
       thinning by u = 0.75) and exits past
       the upper right. Banner 360x46 at 60,424 (0x0E1836, 2 px 0x2D5BD0 border, radius 10)
       (taller, growing upward, for a name that wraps to two lines; dots after two)
       with the mission ("Falcon 9 · Starlink Group 12-7", mono18, white), text centred
       both ways (dropped a third of the descent so caps sit in the middle). Once per launch
       (name + NET); waits while a full-screen view or prompt is open; a tap skips it. On
       lv_layer_top, deleted when done. Launch Animation switch (on by default) and Play
       Launch Animation button in HA and the web page.
UI-69a Launch animation smoothness: frame() runs on the display's LV_EVENT_REFR_START (the
       moved objects re-arm the next refresh; a 40 ms timer only covers a gap with no
       refresh). Each puff and the pad cloud change only when their age crosses a 0.04 DUR
       step, so the number of dirty areas per frame stays well under LVGL's 32 (past that
       a frame redraws the whole screen).
UI-69e Smoke is drawn opaque (PERF-9): each puff and billow is a round lv_obj, bg_opa COVER,
       coloured mix(smoke colour, background, strength), the background being SKY_BG when its
       centre is inside the sky disc (centre 240,242, radius 184) and PAGE_BG outside. Fading
       is a colour step toward the background. (UI-69d's translucent soft images, 4.6.11, ran at
       3-5 fps on the unit.) After each animation the frame count, rate and longest gap between
       refreshes are logged (INFO, tag rocket).
UI-69f Undocking animation: at the time of a Launch Library event whose type contains
       "Undocking" and whose location is the International Space Station (EventRec.iss, from
       the list's "location" string; exact times only, within 2 min after), once per event: the
       station from above as opaque rectangles (ISS_SHAPE: truss, eight solar arrays, two
       radiators, the module stack with two nodes, the Russian segment and its arrays) and a
       capsule image (26x40, trunk, heat shield, white body, docking adapter) nose-down at the
       forward port (240,176). At 1 s the hooks let go: four thruster puffs, then a pair every
       0.5 s for 2 s (opaque discs fading toward the sky colour, hidden below strength 60); the
       capsule backs away up and slightly right (4u + 9u^2 px, u in s since release) and leaves
       the screen; 6 s in all. Same banner, tap to skip, Launch Animation switch and frame log.
       Play Undocking Animation button (web Display group, weight 8, and HA): the next ISS
       undocking's name, or "Undocking from the ISS".
UI-69g Frame profile, logged at INFO after each animation: per refresh, average and maximum of
       the refresh itself (LV_EVENT_REFR_START to REFR_READY) and its flush part (FLUSH_START to
       FLUSH_FINISH, with the number of flushed chunks), the main-loop time between refreshes,
       the animation's own code, and the invalidated pixels.
UI-69b While the launch animation plays, sat::tick() returns at once (no propagation, marker
       moves, sky layers or data drain); the first tick after it ends redraws for the
       current time.
UI-69c Flame flicker: FLAMES = 5 pre-drawn flames (length 0.8..1.3, width 0.92..1.10, tip
       lean -0.06..+0.05 body widths; outer orange, inner yellow, white core), shown in the
       irregular FLAME_SEQ order, one every 45 ms.
UI-69i The smoke (ground cloud, then the trail or thruster puffs) is one non-clickable 480x480
       object on the top layer, above the ISS and under the rocket and banner, drawing its
       discs in LV_EVENT_DRAW_MAIN (opaque, pre-mixed colours, PERF-9). A disc that changes
       invalidates its old and new areas only; discs outside the area being redrawn are skipped.
UI-69j The ground cloud fades from 15% of the climb (PAD_FADE) and is gone at 35% (PAD_END).
UI-68b The internet install (http_request OTA) downloads and writes in one blocking call on
       the main loop, so no LVGL timer runs: its on_begin/on_progress/on_end/on_error hooks
       update the "Updating firmware" screen (bar and percentage every 2%) and call
       lv_refr_now(). An install started from the web page or HA opens that screen too.
UI-69k At the end of setup, with Launch Animation on, the launch plays once over the boot
       (Wi-Fi status) screen: no banner, and the smoke translucent (true colours at the
       puff's strength) so the logo and text show through as it thins; on the map the smoke
       stays opaque and pre-mixed (PERF-9). A switch to the map mid-flight lets it finish.
UI-69m Splashdown (7 s, with Launch Animation on, at a splashdown's time, or the Play Splashdown
       Animation button): an ocean band across the bottom of the sky disc (drawn once, clipped to
       the disc), a capsule on four orange-and-white mains falling steadily with a slow sway, a
       burst of spray at 4.2 s, the chutes released downwind, the capsule bobbing with the water
       line over its base. The sea rises into view over the first 1.3 s (its clip grows up from
       the bottom; a surface line follows the disc's chord). Each splashdown has its own landing
       spot (within 60 px of the centre), sway (3-12 px, 0.9-2 rad/s, random phase) and chute
       drift direction. Pictures 4x4 supersampled (UI-69n).
BOOT-2 The boot screen (wifi_page: logo, "Loading") is drawn with lv_refr_now() at the start of
       the start-up lambda, before the slow work (marker pools, cache, data task); LVGL draws
       nothing on its own until setup ends. The title reads "Loading" until start-up is done,
       then "Connecting to Wi-Fi" (and the usual Wi-Fi status).
UI-69n The animation pictures (5 rocket flames, two capsules, the chutes, the sea) are drawn after
       setup, by a 25 ms LVGL timer spending up to 10 ms a call, so the display starts without
       them (drawn in setup they kept it dark ~2.7 s). No scene plays until they are ready; a boot
       launch asked for meanwhile plays then, if within 30 s of the request.
UI-69h Load probes in the animation log: a 100,000-step integer loop and a 64 KB PSRAM read
       (one byte per cache line), timed before the animation and 2 s into it; and each smoke
       update's LVGL calls (size, position, colour, show) averaged.
UI-41a Switches Aurora Alerts (mdi:aurora), Planet Alerts (mdi:orbit) and Sky Event Alerts
       (mdi:weather-night; UI-41b, UI-47..51, 4.5.15), default on,
       in the Celestial settings tab (with the aurora glyph and alignment picture)
       and the web Celestial section. Aurora Alerts off drops the aurora alert and
       the horizon glow; Planet Alerts off drops visible-planet and alignment
       alerts (the planets stay on the map). sat::set_alerts(aurora, planet).
UI-42  Starlink trains: 4 or more Starlink from one launch (launch key from the
       designator, YY*1000 + launch number, kept in SlElem/SatRec) in view, each
       within 40 px of the next along the line, found on each Starlink restyle. A
       faint line joins the cars (drawn with the overlays) and a "Starlink train"
       tag sits beside the first; at most two trains.
UI-43  Horizon: markers below 10° elevation are drawn at 55 % over the sky colour
       and lose their name tags (debris icons dim too); the colour is re-applied only
       when a marker crosses 10°.
UI-44  Auto Brightness (switch, default off; Display tab "Auto bright"): the
       Backlight value / settings slider is the daytime level (bright_day, restored);
       every 30 s the backlight is set to it x factor, factor 1 with the Sun above
       +5°, falling linearly to 0.25 at -8°, 3 s transition. Paused while the settings
       page is open (the slider applies live) and while the light is off.
UI-45  Night Mode (switch, default off; Display tab): after civil dusk (Sun below
       -6°) the whole screen is drawn in red: a LV_EVENT_FLUSH_START display callback
       converts each flushed RGB565 area to its luma in the red channel. Switching on
       or off invalidates the screen.
UI-39  LEO cone: the Starlink layer is drawn, counted and listed only within this
       many degrees of overhead (elevation >= 90 - cone). 15-90 in 5° steps; default
       80 (the starlink_radius substitution). Set from the Celestial tab's slider
       (readout snapped to 5°, applied on Save) or the LEO Cone number entity (web
       Satellites group); restored at boot. A change restyles the layer at once from
       the whole-sky list (DATA-3) and starts a fresh scan. At wide cones the
       200-marker pool (UI-14) keeps the highest; the rest are counted and logged.
       Other LEO satellites are not limited by the cone.
UI-46  Constellation cards: a tap on a shown constellation name tag (6 px margin), when
       no satellite, Sun, Moon or planet is nearer, opens a card: Az/El of the name
       position, what the name means and its story, the brightest star, one thing to
       look for (sky_lore.h, all 46 rank 1-2 names), star-four-points icon; title in
       0xB8C4F0. No ring; the card sits on the half away from the tag.
UI-46a Constellation card picture: the figure from sky_cfig.h (gen_cfig.py from the same
       d3-celestial lines as the map; gnomonic about the figure's centre, north up, east
       left, fitted to 34 of 40 px), drawn into the card's 40 px canvas (shared with the
       Moon picture, which is redrawn when it comes back): lines #7F96D7, stars white,
       1/3/4 px by magnitude (fainter than 3.2 / 1.5-3.2 / brighter than 1.5).
UI-38  Aurora notice: when tonight's dark window (Sun below -12°, starting within 12 h)
       has the most Kp giving "low in north" or "overhead", the status line (only when
       it would be all-is-well or empty; warnings and errors keep it) shows the MDI
       aurora icon and "Aurora possible|likely after|until <time> - <word>" in 0x7EE0B0
       (mono14, 1 px higher than the status line; the status line's font otherwise),
       word by Kp: <3.5 faint, <4.5 moderate, <6 strong, else very strong. While dark,
       a glow hugs the northern horizon (true north, follows the map heading): 18 arc
       bands fading inward and to the sides, reach 32/48 px, peak opacity 48/64
       (possible/likely). Re-evaluated every 10 min and on each new Kp download. The
       Sun card's aurora line uses the same words ("Aurora strong, Kp 5: overhead likely").
UI-29  Units: heights, ranges and speeds in km or miles (switch "Distances in
       Miles", default on since 4.3.0), on the card and the list; applied immediately.
UI-30  Object-type icons (MDI): space-station (ISS), trash-can (R/B, DEB),
       satellite-uplink (Starlink), crosshairs-gps (GNSS: G/E/R/C tag), earth (GEO),
       satellite-variant (anything else); the list also has weather-sunny (Sun) and
       moon-waning-crescent (Moon). Details card: 20 px, top right, in the title
       colour. List: a first 24 px column, 14 px icons in the row colour (the
       table's draw callback swaps the font for column 0); NAME narrows to 128 px.

## 5. Motion (MOTION)
MOTION-1 The ISS and visual satellites are propagated with full SGP4 (Vallado,
         trimmed from python-sgp4 2.27, MIT; deep-space terms included). Checked
         against skyfield: ≤0.02°.
MOTION-2 Starlink uses a light model: SGP4's secular rates (mean motion, node and
         perigee drift) from sgp4init on a Kepler orbit. Error ≤0.6° vs skyfield,
         cheap enough for thousands of objects.
MOTION-3 Positions are recomputed every 5 s (ISS), 10 s (satellites) and 30 s
         (Starlink) on the network task. Each fix holds two positions 10 s apart;
         between fixes markers are dead-reckoned along that arc (≤0.07° error).
MOTION-4 A marker MUST stay bound to one satellite id for as long as that
         satellite is above the horizon. Marker assignment MUST NOT depend on
         list order.
MOTION-5 Propagation continues offline; only element downloads need the network.
MOTION-6 Az/el MUST be computed on-device (TEME → ECEF via GMST → ENU, WGS84
         observer) for every object; the ISS and ordinary satellites share one path.
MOTION-7 Markers move once per 2 s tick; tweening is a non-goal (PERF-1).
MOTION-8 Next ISS passes are searched on-device: 4 days ahead at 60 s steps, rise
         and set by bisection, peak by golden-section search; passes peaking
         below 10° are skipped. A pass is "visible" when the ISS is sunlit and the
         Sun is below -6°.

## 6. Performance and stability (PERF)
PERF-1 The RGB panel is scanned continuously out of PSRAM. Any change that adds
       sustained PSRAM traffic or long main-loop work risks the panel losing sync
       (picture shifts), or LVGL failing to paint (blank, backlit screen).
PERF-2 CONFIG_LCD_RGB_RESTART_IN_VSYNC MUST be enabled (it re-syncs DMA every vblank).
PERF-12 Wi-Fi and lwIP buffers MUST come from PSRAM (CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP):
       in internal RAM they leave ~20 KB free and HTTPS downloads fail. The TCP window
       (NET-11) is 16 KB, so a download's bursts on the PSRAM bus the panel refills from
       stay small.
BUILD-7 C++ is compiled with -mtext-section-literals (local external component
       sky-tracker/components/sky_build: esphome build_flags passes only -D/-W flags): with
       every header inlined into main.cpp, a function's literal pool ended up more than
       256 KB from its code ("dangerous relocation: l32r: literal target out of range").
       New features go in their own translation unit (sky_extra.cpp, headers with SKY_IMPL).
PERF-3 CONFIG_LCD_RGB_ISR_IRAM_SAFE MUST NOT be enabled on this build: IRAM is
       already ~100% used, and the firmware rolls back at boot. PERF-10 now covers
       shifting during uploads instead.
PERF-4 buffer_size is 12.5% (57.6 KB), which ESPHome places in internal RAM (it
       tries internal first at 25% or less and falls back to PSRAM). LVGL then
       blends in SRAM and only finished pixels cross to the PSRAM framebuffer,
       leaving the PSRAM bus to the panel's bounce-buffer refills. Measured after
       the change: draw buffer internal, 127 KB internal free (largest block 57 KB).
       (50% was used before, when the black screen was wrongly blamed on buffer
       placement; HW-7 was the real cause.) A one-off "diag" log line 20 s after
       boot reports LVGL state, the buffer's location and free heap.
PERF-5 Marker positions and hidden flags MUST only be pushed to LVGL when they
       actually changed (rounded pixel position, visibility).
PERF-6 Map marker label text and marker colours MUST be rewritten only on fresh data
       for their layer, never on the motion tick.
PERF-6a HUD text (clock, countdowns, footer) MAY be set on the tick, but only
       when the formatted string differs from what is shown.
PERF-8 LV_INV_BUF_SIZE MUST be 128 (a build flag in the YAML). LVGL's default of 32
       dirty areas overflows once ~16 markers move in a tick, and LVGL then redraws
       the WHOLE screen through PSRAM, which made the panel jump every 2 s. With 128,
       a tick flushes ~15-22k px instead of up to 230k (host benchmark).
PERF-9 Nothing on the sky map may use opacity below COVER or shadows. Blending reads
       the PSRAM buffer back for every pixel. Pre-mix colours over SKY_BG instead
       (mix() in sat_tracker.h); the Sun's glow is an opaque halo disc.
PERF-10 CONFIG_SPIRAM_XIP_FROM_PSRAM and CONFIG_ESP32S3_DATA_CACHE_LINE_64B MUST be
       enabled. Flash and PSRAM share the MSPI bus: flash instruction fetches starved
       the bounce-buffer refills (jitter), and flash writes stalled the cache (shift
       during uploads). This is Espressif's recommended setup for a PSRAM framebuffer
       with bounce buffers, and it makes CONFIG_LCD_RGB_ISR_IRAM_SAFE (PERF-3) unnecessary.
PERF-13 CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB MUST be enabled. Code runs from PSRAM
       (PERF-10), which the panel scanout keeps busy, so an instruction-cache miss costs
       ~1 us; with 16 KB LVGL calls took milliseconds each. Costs 16 KB of internal RAM.
PERF-14 Status-line changes are drawn behind the scan: the panel's vsync interrupt (an
       esp_lcd RGB panel callback; ESPHome's mipi_rgb registers none) times each frame, and
       when the status text changes draw_hud waits (at most one frame) until the scan and its
       bounce-buffer refill are past the line, then calls lv_refr_now(). With one PSRAM
       framebuffer, a write across the rows being scanned out shows as a one-frame tear.
PERF-11 When a restyle binds more than 8 new markers (a layer switched on), the whole
       sky is invalidated first. Otherwise each marker adds its own small dirty
       area and LVGL redraws the stars and constellation lines under every one of
       them (MEO on: a 1.9 s LVGL stall); inside one full-sky area they fold into
       one redraw.
PERF-7 With fetching off the loop, "took a long time" warnings SHOULD be under
       ~100 ms. Anything in the seconds means work has leaked back onto the loop.

## 6a. Network (NET)
NET-1  ESPHome's web server runs on port 80 (version 3, with the UI bundled locally so
       it works without internet, e.g. on the fallback AP). Firmware upload from the
       web page is ON, as asked. The web UI has no login, so anyone on the LAN can
       change entities or flash firmware; add `auth:` to the web_server block if
       that matters. The Builder's encrypted native OTA (BUILD-1) still works.
NET-2  Fallback AP "Sky Tracker - XXXX", where XXXX is the last 4 hex digits of the
       MAC, with no password. The captive portal serves Wi-Fi setup while the AP is
       up. The name is set at boot (on_boot priority 1100, before Wi-Fi), because
       ESPHome's ap ssid must be a literal. ESPHome's captive portal can take a
       firmware upload while the AP is up, so an open AP allows that during a Wi-Fi
       outage. This was accepted as asked.
NET-2a If Wi-Fi does not connect, the fallback AP starts after 30 s (ap_timeout;
       ESPHome default 90 s). Nothing reboots the device away from it: wifi
       reboot_timeout 0 s, and api reboot_timeout 0 s (the default 15 min "no HA
       client" reboot would drop anyone in the middle of configuring).
NET-2b Wi-Fi status page (the boot page): a centred column (380 px wide) of the logo
       (UI-65, 200 px), "Sky Tracker" (Roboto Light 34, white), a status line (Roboto 22,
       white), a body line (Roboto 22, 0xB0B0B0) and the add-on sensor lines (mono16), on
       black; the firmware version lower right (4.5.38: the logo and name replace the MDI
       wifi icon).
       - It is the first LVGL page, so it is what the device boots into; the map is
         not shown until Wi-Fi connects. Page flips skip it.
       - Connecting: "Connecting to Wi-Fi", and the saved network name (substitution
         wifi_name from the wifi_ssid secret).
       - Fallback AP up: "Set up Wi-Fi", "Join 'Sky Tracker - XXXX'\nfrom your phone".
       - Checked every 2 s. On connect it fades to the map. After 10 s without
         Wi-Fi it comes back, but never over the settings page.
NET-7  Time: SNTP (pool.ntp.org) plus Home Assistant time as a second source.
       SNTP starts at boot, before Wi-Fi, so its first requests fail and lwIP backs
       off (15 s doubling to 150 s); after a slow Wi-Fi join the clock could stay
       unset for minutes. wifi on_connect calls sat::sntp_kick()
       (esp_sntp_restart) to ask again at once, and HA sets the clock as soon as it
       connects.
NET-8  No network calls until Wi-Fi is up, and none while it is down.
       - Fetches: the loop writes net::link_up from network_up() every tick; while
         it is false nothing is scheduled, and the fetch task drops any job still in
         its queue. On reconnect every layer refreshes at once (Starlink keeps its
         45 s offset).
       - SNTP: stopped at boot (on_boot) until the first Wi-Fi connect and again on
         every disconnect; wifi on_connect starts it or asks again (NET-7).
NET-3  (Removed in rev 4: no API key is needed. CelesTrak is fetched over HTTPS
       with the ESP-IDF certificate bundle.)
NET-5  api max_connections is 8 (ESPHome default 5). HA holds one, and every open
       log viewer (Builder, CLI) holds another. When the limit is hit, a new client is
       dropped right after the Noise hello, which the client misreports as
       "Try enabling encryption on the device" (EncryptionHelloAPIError). The
       encryption key is not the problem.
NET-6  The web page groups entities into sections (web_server v3 sorting_groups;
       Home Assistant is unaffected), in this order: Location & Sky (Latitude,
       Longitude, Map Heading, compass, Magnetic Declination, Aurora Kp),
       Satellites (LEO Cone, Show LEO/Starlink/MEO/GEO/Orbital Debris and the
       counts), Celestial (Show Stars & Constellations, Stars Only After Dusk, Show
       Motion Trails, Show Planets), Display (Backlight, Auto Brightness, 24-hour
       Clock, Distances in Miles, Night Mode), ISS (Elevation,
       Azimuth, Overhead, Next Pass), System (Reboot and the NET-9 diagnostics).
       ESPHome's OTA upload form always comes last.
HW-9b  Guided compass calibration (sky_sensors.h): each fresh sample during a run is
       measured about a circle fitted to the samples so far (algebraic least squares,
       about the first sample), giving the 10° sectors seen, the total turn and the
       turning rate (smoothed). Done at 35 sectors, a full turn and 40 samples, or 60 s;
       the result (min/max centre and scale, as before) is saved at once. Screen on the
       LVGL top layer; pace: over 50°/s "Slow down", under 8°/s "Turn a little faster".
       The Location-tab button is disabled while no compass is detected.
DATA-13 SatNOGS fallback: while CelesTrak refuses (a failed pass or its 2 h hold), the ISS
       and Tiangong elements come from db.satnogs.org (TLE from Space-Track; tle_to_omm, at
       most hourly); the status line adds "ISS via SatNOGS" and passes are recomputed.
NET-9  Diagnostic entities (entity_category diagnostic; web group System):
       Uptime, Wi-Fi Signal, IP Address, Wi-Fi Network, Firmware Version
       (a template: "ESPHome <version> · Sky Tracker <fw_version>", no hash or
       build timestamp),
       Free Internal RAM, Free PSRAM (heap_caps_get_free_size only, never a heap
       walk: FAIL-9), Orbital Data Age (h), Satellites Tracked, Starlink Tracked,
       Data Status (the UI-25 text). Every entity carries an MDI icon. GPS Satellites
       and GPS Altitude carry state_class measurement (HA long-term statistics).
NET-10 Browser-tab icon: a sun (disc and eight rays, C_SUN #FFD54A, inline SVG).
       The bundled v3 page has an empty icon and no option for one (js_include is
       not loaded by it), so sky_web.h answers GET "/" first: at boot (on_boot 600,
       before web_server's setup) ESPHome's own INDEX_GZ is inflated into PSRAM
       with the ROM miniz, the icon link is replaced, and the page is served
       uncompressed (~78 KB). The app's firstUpdated() sets the first
       link[rel~='icon'] to its house icon, so that selector is renamed in place to
       link[rel~='none'] (same length). An ESPHome update brings its new page along. Any
       failure leaves ESPHome's handler serving its page; while the captive portal
       is active it keeps "/".
NET-10a Header logo (top left, the esp-logo element, 52x40): a 40x40 badge, dark
       sky disc (#0B1220, rim #3B4C70) with a dotted cyan orbit (#18BCF2), a
       satellite on it, three stars and a small Sun (#FFD54A). Swapped into the
       same served page as NET-10 (the app's fixed SVG string); if it isn't found,
       ESPHome's logo stays.
NET-4  Entities carry icons: ISS Azimuth mdi:sun-compass, ISS Elevation
       mdi:angle-acute, ISS Overhead mdi:space-station (ESPHome sends one static icon;
       per-state icons are set in HA), Backlight mdi:brightness-percent, Next ISS Pass
       mdi:calendar-clock, Satellites/Starlink Overhead mdi:counter. A restart button
       "Reboot" (entity_category diagnostic) is exposed to HA and the web UI.NET-11 Download speed: CONFIG_LWIP_TCP_WND_DEFAULT 32768 and TCP_RECVMBOX_SIZE 32 (a
       download is limited to window / round trip), CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP (the
       buffers may live in PSRAM; internal RAM is ~70 KB free), HTTP buffer and reads 8 KB.
       A failed esp_http_client_open is retried once after 1 s. Every download of 50 KB or
       more logs "<what>: N KB in S s (R KB/s; D s of it decoding)".
NET-12 Connection reuse: one kept-alive esp_http_client, reused (esp_http_client_set_url)
       while requests go to the same host; closed on a request to another host, any error, a
       non-200 whose body can't be flushed, or a body left unread. A reused connection the
       server has closed is replaced at once; a fresh connect that fails is retried twice,
       a second apart (NET-11). The download log says "reused connection" or "connect".
NET-2c The Wi-Fi status page's second line is the SSID being tried (the station entry ESPHome
       selected: credentials saved in flash, or built in), refreshed while it shows; the
       substitution wifi_name only if none is known.
NET-13 Each request's URL goes to the log at DEBUG when it starts ("<what>: GET <url>"), and the
       failure lines (client init, connect failed, HTTP <code>, read failed) end with the URL,
       so any download can be tried by hand from a browser.
NET-13a Firmware update checks go through sat::upd (boot +3 min, hourly interval, Settings, the
       Upgrade Check button; the entity's own polling is off) and log "firmware: GET <manifest>"
       at DEBUG; each new answer logs "firmware: manifest has X, installed Y (newer, offered |
       nothing newer)" at INFO. The install's downloads are logged by ESPHome ("Connecting to").

## 7. Build and deployment (BUILD)
BUILD-1 OTA MUST use `encryption:` (reusing the API key), not a password.
BUILD-2 Installs go over Wi-Fi from the ESPHome Builder. The first flash after
        switching OTA auth must be over USB.
BUILD-3 Every change MUST pass `esphome config` before it is installed.
BUILD-4 A failed boot rolls back automatically, so a bad build costs one reboot.
BUILD-5 Changing sdkconfig options forces a full rebuild (several minutes).
BUILD-6 During an OTA upload, the loop stops queueing jobs and moving markers, and
        the task drops queued jobs. Both resume if the upload fails.

## 8. Known failure modes (FAIL)
FAIL-1 Solid black but backlit screen, with the diag line showing LVGL healthy ←
       the panel missed its init after a warm reboot (HW-7). A power cycle recovers
       it. Check that the priority-1100 on_boot reset is still in the YAML.
FAIL-2 Markers leaping across the map ← the marker-to-satellite binding changed;
       see MOTION-4.
FAIL-3 Picture shifted and staying shifted ← panel DMA desync; see PERF-2/PERF-3.
FAIL-4 "Secret not defined" ← the key was put in HA's secrets.yaml instead of the
       ESPHome folder's.
FAIL-5 Missing name tags ← more satellites than the pool (UI-14). The log says so.
FAIL-6 "'X' is not a member of 'sat'" after editing sat_tracker.h ← a region
       splice removed a neighbouring function. Compile the header on a host first.
FAIL-8 Panel jumps or tears when markers update ← too many dirty areas made LVGL
       redraw the full screen; see PERF-8/PERF-9. Check the build flag survived.
FAIL-7 Duplicate globals across sat_tracker.h and sat_net.h ← schedule and
       countdown targets live in sat_tracker.h only; sat_net.h keeps just
       clock_now() and the shared records.

FAIL-9 Anything that walks a heap (heap_caps_get_largest_free_block,
       heap_caps_get_info, heap_caps_check_integrity) holds the heap lock with
       interrupts off for milliseconds on the 6 MB PSRAM heap. The RGB panel's
       bounce-buffer ISR misses its deadline and the picture glitches or shifts.
       MUST NOT be called at runtime; heap_caps_get_free_size() is a counter read and
       is fine.
FAIL-11 Any loop that runs during setup for more than a moment (marker pools, the launch
       animation's rocket and capsule pictures) MUST call ui_feed_wdt() as it goes. Setup runs
       in the loop task; past the task watchdog's limit the device resets before the display
       comes up, every boot.
FAIL-12 PSRAM is never asked for a large block after start-up. The four element lists are
       reserved at their caps when the data task starts (satellites 400, GNSS 200, GEO 740,
       Starlink 13,000: ~2.6 MB, while PSRAM is in one piece), the flash cache is read
       straight into them, and a download clears and refills its list in place (the data
       task alone owns them); a failed download reloads the list from the cache. Rows past a
       list's capacity are dropped, never reallocated. The per-job position lists (2 s and
       30 s jobs) are persistent at full size, so their swaps with pending and the UI's live
       copies stop allocating. room_for() guards any growth: only with a free block that size
       plus 32 KB, else skipped. PsramAlloc aborts on failure: in 4.6.21 rebuilding the GEO
       list (and the Starlink scan's list) aborted once downloads had cut PSRAM into pieces
       of 60-240 KB with 2.5 MB free. Checked with TEST-1: 79 layer switches over 8 rounds
       of every list downloading each minute, no abort (4.6.21: abort within 9-69).
FAIL-12b The picture buffers (JPEG 768 KB, sums 777 KB) are taken first, before the orbital
       lists. The lists are reserved at what the cache holds plus a margin (satellites +48,
       GNSS +24, GEO +48, Starlink +800) and only for layers that are on, and grow by a quarter
       (one_more/push_room) only while PSRAM has the block; the per-job position lists grow to
       the most they have needed. 4.6.22-4.6.24 reserved every list at its cap (~3.4 MB with
       the job lists) ahead of the picture buffers: 0.2 MB was left and every picture failed.
FAIL-10 Crash capture (sky_diag.h): ESPHome's crash handler keeps the last panic in
       no-init RAM, logs it at boot and to the first API client, then clears it. At
       on_boot 600 (before the API) the record is replayed through a logger callback,
       saved to the skydata sector 0x0F3000 (magic, CRC, firmware version) and shown
       on the debug page (reset reason, then up to 8 lines) and as the "Last Crash"
       text sensor (reason / PC / backtrace PCs, decodable with the release's .elf from
       the build artifact) until the next crash replaces it. Until 4.6.22 it used
       0x0F1000, the launch list's sector (UI-54a), which overwrote it: every record
       was lost at the next launch-list save.

## 9. Verification (VER)
VER-3  Design comps live in docs/comps (PNG/GIF), rendered by tools/host_test/make_comps.sh:
       the settings tabs by render_settings.py (the page's LVGL calls lifted from ESPHome's
       generated main.cpp, LVGL built with ESPHome's lv_conf.h, fonts rebuilt from the YAML),
       the boot launch (BOOT_GIF=1) and the test suite's screens (docs/comps/renders), drawn on
       the cached CelesTrak lists (RENDER_REAL=1; fake data where CelesTrak refuses). The checks
       run separately on the test data.
       Pictures are the real ones (tools/host_test/fetch_images.py: GOES SUVI, Dial-A-Moon
       at the test's hour, GOES GeoColor, Wikimedia planet photos, JPL comets), cached and
       not committed; without the cache gen_fixtures.py draws stand-ins.
TEST-1 Download stress: tools/host_test/fetch_celestrak.py caches the CelesTrak files (at most
       one fetch per file per 2 h); tools/host_test/celestrak_server.py serves them on the
       firmware's paths. A debug build sets net::celestrak_base to that server (no CelesTrak
       403 hold is loaded then), net::ELEM_REFRESH_S = 60 and ELEM_PERIOD = 30, so every
       list downloads each minute while layers are switched. Release builds keep
       celestrak.org, 12 h and 300 s.
VER-1  Orbital and astronomical maths MUST be checked against an independent
       implementation (e.g. pyephem) or a simulated orbit before installing.
VER-2  Drawing code SHOULD be compiled and exercised against real LVGL on a host;
       sat_net.h SHOULD be syntax-checked against stubbed ESPHome/IDF headers.
       A full `esphome compile` with the matching ESPHome version satisfies both.
VER-3  After every install, the device log MUST show a successful boot, a
       satellite count, and no rollback message.
VER-4  Display faults MUST be confirmed by eye on the panel; logs cannot see them.

## 10. Non-goals
NG-1   (Dropped in rev 4: orbits are now propagated on the device.)
NG-2   No historical logging or charting on the device.
NG-3   Touch input is limited to page switching, tap-for-details (UI-24) and the
       settings page.
NG-4   No animation between motion ticks (MOTION-7).
