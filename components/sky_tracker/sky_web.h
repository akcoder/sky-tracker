#pragma once
// NET-10: what the web page shows of this project: the browser-tab icon and the header badge.
// The page patching itself (inflating ESPHome's page, swapping the icon and the badge, the logo's
// link to the GitHub page) is tracker-common's tracker::web, shared with f1-tracker.
namespace skyweb {

// A sun in the map's Sun colour (C_SUN 0xFFD54A): disc and eight rays, escaped for a data: URI.
static const char FAVICON_SVG[] =
    "%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'%3E"
    "%3Ccircle cx='16' cy='16' r='7' fill='%23FFD54A'/%3E"
    "%3Cpath d='M16 2.5v4.2M16 25.3v4.2M2.5 16h4.2M25.3 16h4.2M6.5 6.5l3 3M22.5 22.5l3 3M6.5 25.5l3-3M22.5 9.5l3-3' "
    "stroke='%23FFD54A' stroke-width='2.8' stroke-linecap='round'/%3E%3C/svg%3E";

// NET-10a: header logo, 40x40 (shown 52x40, centred)
static const char HEADER_SVG[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 40 40\"><circle cx=\"20\" cy=\"20\" r=\"18.2\" fill=\"#0B1220\" stroke=\"#3B4C70\" stroke-width=\"1.6\"/><circle cx=\"20\" cy=\"20\" r=\"11\" fill=\"none\" stroke=\"#22324F\" stroke-width=\"1\"/><path d=\"M4.5 27.5C11 16 24 9 35.5 11\" fill=\"none\" stroke=\"#18BCF2\" stroke-width=\"1.6\" stroke-linecap=\"round\" stroke-dasharray=\"0.1 3.2\"/><circle cx=\"11\" cy=\"11.5\" r=\"1\" fill=\"#FFFFFF\"/><circle cx=\"28\" cy=\"28\" r=\"0.9\" fill=\"#FFFFFF\"/><circle cx=\"16\" cy=\"30.5\" r=\"0.7\" fill=\"#C9D3E6\"/><circle cx=\"31.5\" cy=\"20\" r=\"0.7\" fill=\"#C9D3E6\"/><g transform=\"translate(21.5 15.5) rotate(-28)\"><rect x=\"-6.6\" y=\"-1.9\" width=\"4.6\" height=\"3.8\" rx=\"0.5\" fill=\"#18BCF2\"/><rect x=\"2\" y=\"-1.9\" width=\"4.6\" height=\"3.8\" rx=\"0.5\" fill=\"#18BCF2\"/><rect x=\"-2\" y=\"-0.4\" width=\"4\" height=\"0.8\" fill=\"#9FB3D1\"/><rect x=\"-1.7\" y=\"-2.2\" width=\"3.4\" height=\"4.4\" rx=\"0.8\" fill=\"#F2F4F9\"/></g><circle cx=\"29.5\" cy=\"30.5\" r=\"3.2\" fill=\"#FFD54A\"/></svg>";

}  // namespace skyweb
