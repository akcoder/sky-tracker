// UI-77 "On this day": a space-history event for today's date, shown now and then among the
// alerts, and its card. Built in (no download); dates are those of the event where it
// happened, as usually given. Compiled in sky_extra.cpp (SKY_IMPL), BUILD-7.
#ifndef SKY_HISTORY_DECL
#define SKY_HISTORY_DECL
#include <cstdint>
namespace sat {
namespace hist {
struct Ev {
  uint8_t mo, d;
  int16_t y;
  const char *text;
};
// events on month/day (1-based), up to `max`, oldest first; returns how many
int on(int mo, int d, const Ev **out, int max);
}  // namespace hist
}  // namespace sat
#endif  // SKY_HISTORY_DECL

#if defined(SKY_IMPL) && !defined(SKY_HISTORY_IMPL)  // (the table once, wherever SKY_IMPL is on)
#define SKY_HISTORY_IMPL
namespace sat {
namespace hist {
static const Ev EVENTS[] = {
    {1, 3, 2019, "Chang'e 4 makes the first landing on the Moon's far side"},
    {1, 4, 2004, "NASA's Spirit rover lands on Mars"},
    {1, 14, 2005, "Huygens lands on Saturn's moon Titan"},
    {1, 16, 1969, "Soyuz 4 and 5 dock; two cosmonauts spacewalk across"},
    {1, 19, 2006, "New Horizons launches for Pluto"},
    {1, 25, 2004, "NASA's Opportunity rover lands on Mars"},
    {1, 27, 1967, "Apollo 1 fire: Grissom, White and Chaffee are lost"},
    {1, 28, 1986, "Challenger is lost 73 seconds after launch"},
    {1, 31, 1958, "Explorer 1, the first US satellite, reaches orbit"},
    {2, 1, 2003, "Columbia is lost on re-entry"},
    {2, 3, 1966, "Luna 9 makes the first soft landing on the Moon"},
    {2, 5, 1971, "Apollo 14 lands on the Moon"},
    {2, 6, 2018, "Falcon Heavy flies for the first time"},
    {2, 7, 1984, "Bruce McCandless makes the first untethered spacewalk"},
    {2, 11, 2016, "LIGO announces the first detection of gravitational waves"},
    {2, 14, 1990, "Voyager 1 takes the Pale Blue Dot picture of Earth"},
    {2, 18, 1930, "Clyde Tombaugh discovers Pluto"},
    {2, 18, 2021, "Perseverance lands in Jezero Crater, Mars"},
    {2, 19, 1986, "The core module of space station Mir is launched"},
    {2, 20, 1962, "John Glenn becomes the first American to orbit Earth"},
    {3, 1, 1966, "Venera 3 reaches Venus, the first craft to hit another planet"},
    {3, 6, 1986, "Vega 1 flies past Halley's Comet"},
    {3, 7, 2009, "The Kepler planet-hunting telescope launches"},
    {3, 13, 1781, "William Herschel discovers Uranus"},
    {3, 14, 1986, "Giotto flies through the coma of Halley's Comet"},
    {3, 16, 1926, "Robert Goddard flies the first liquid-fuelled rocket"},
    {3, 16, 1966, "Gemini 8 makes the first docking in orbit"},
    {3, 18, 1965, "Alexei Leonov makes the first spacewalk"},
    {3, 23, 2001, "Mir is brought down over the Pacific"},
    {4, 10, 2019, "The first image of a black hole (M87*) is released"},
    {4, 12, 1961, "Yuri Gagarin becomes the first human in space"},
    {4, 12, 1981, "Columbia flies the first Space Shuttle mission"},
    {4, 13, 1970, "An oxygen tank explodes on Apollo 13"},
    {4, 19, 1971, "Salyut 1, the first space station, is launched"},
    {4, 19, 2021, "Ingenuity makes the first powered flight on another planet"},
    {4, 21, 1972, "Apollo 16 lands on the Moon"},
    {4, 24, 1990, "The Hubble Space Telescope is launched"},
    {4, 28, 2001, "Dennis Tito becomes the first space tourist"},
    {5, 5, 1961, "Alan Shepard becomes the first American in space"},
    {5, 14, 1973, "Skylab, America's first space station, is launched"},
    {5, 25, 1961, "Kennedy asks Congress to land a man on the Moon"},
    {5, 25, 2008, "Phoenix lands near Mars' north pole"},
    {5, 25, 2012, "Dragon is the first commercial craft to berth at the ISS"},
    {5, 28, 1959, "Monkeys Able and Baker return alive from space"},
    {5, 30, 2020, "Crew Dragon Demo-2: the first crew on a commercial craft"},
    {6, 2, 1966, "Surveyor 1 makes the first US soft landing on the Moon"},
    {6, 3, 1965, "Ed White makes the first American spacewalk"},
    {6, 13, 2010, "Hayabusa returns the first asteroid sample to Earth"},
    {6, 16, 1963, "Valentina Tereshkova becomes the first woman in space"},
    {6, 18, 1983, "Sally Ride becomes the first American woman in space"},
    {6, 21, 2004, "SpaceShipOne makes the first private crewed spaceflight"},
    {6, 30, 1908, "The Tunguska blast flattens 2,000 km2 of Siberian forest"},
    {6, 30, 1971, "The Soyuz 11 crew are lost returning from Salyut 1"},
    {7, 1, 2004, "Cassini enters orbit around Saturn"},
    {7, 4, 1997, "Mars Pathfinder lands; Sojourner rolls off soon after"},
    {7, 14, 2015, "New Horizons flies past Pluto"},
    {7, 15, 1965, "Mariner 4 sends the first close-up pictures of Mars"},
    {7, 16, 1969, "Apollo 11 launches for the Moon"},
    {7, 17, 1975, "Apollo and Soyuz dock in orbit"},
    {7, 20, 1969, "Apollo 11 lands; Armstrong and Aldrin walk on the Moon"},
    {7, 20, 1976, "Viking 1 makes the first successful landing on Mars"},
    {7, 21, 2011, "Atlantis lands, ending the Space Shuttle program"},
    {7, 29, 1958, "Eisenhower signs the act creating NASA"},
    {7, 30, 1971, "Apollo 15 lands on the Moon"},
    {7, 31, 1971, "Apollo 15 drives the first rover on the Moon"},
    {8, 6, 2012, "Curiosity lands in Gale Crater, Mars"},
    {8, 12, 2018, "Parker Solar Probe launches toward the Sun"},
    {8, 19, 1960, "Belka and Strelka launch; they come home alive next day"},
    {8, 20, 1977, "Voyager 2 launches"},
    {8, 23, 1966, "Lunar Orbiter 1 takes the first picture of Earth from the Moon"},
    {8, 25, 1989, "Voyager 2 flies past Neptune"},
    {8, 25, 2012, "Voyager 1 crosses into interstellar space"},
    {9, 5, 1977, "Voyager 1 launches"},
    {9, 12, 1962, "Kennedy: \"We choose to go to the Moon\""},
    {9, 15, 2017, "Cassini ends its mission in Saturn's atmosphere"},
    {9, 23, 1846, "Johann Galle finds Neptune where it was predicted"},
    {9, 24, 2023, "OSIRIS-REx drops its asteroid Bennu sample in Utah"},
    {9, 26, 2022, "DART strikes the asteroid Dimorphos"},
    {9, 28, 2008, "Falcon 1 is the first private liquid-fuelled rocket in orbit"},
    {10, 1, 1958, "NASA opens for business"},
    {10, 4, 1957, "Sputnik 1, the first artificial satellite, is launched"},
    {10, 7, 1959, "Luna 3 sends the first pictures of the Moon's far side"},
    {10, 12, 1964, "Voskhod 1 carries the first crew of three"},
    {10, 15, 1997, "Cassini-Huygens launches for Saturn"},
    {10, 15, 2003, "Yang Liwei becomes China's first astronaut"},
    {10, 18, 1967, "Venera 4 sends the first data from inside Venus' atmosphere"},
    {10, 29, 1998, "John Glenn returns to space aged 77"},
    {10, 31, 2000, "Expedition 1 launches for the ISS"},
    {11, 2, 2000, "The first crew moves into the ISS"},
    {11, 3, 1957, "Laika rides Sputnik 2 into orbit"},
    {11, 12, 2014, "Philae lands on comet 67P/Churyumov-Gerasimenko"},
    {11, 14, 1971, "Mariner 9 becomes the first craft to orbit Mars"},
    {11, 15, 1988, "The shuttle Buran makes its only flight, uncrewed"},
    {11, 19, 1969, "Apollo 12 lands on the Moon"},
    {11, 20, 1998, "Zarya, the first ISS module, is launched"},
    {11, 26, 2018, "InSight lands on Mars"},
    {12, 2, 1993, "Endeavour launches to repair Hubble's optics"},
    {12, 3, 2018, "OSIRIS-REx arrives at asteroid Bennu"},
    {12, 4, 1998, "Unity, the first US ISS module, is launched"},
    {12, 7, 1972, "Apollo 17 launches; its crew take the Blue Marble photo"},
    {12, 11, 1972, "Apollo 17 lands on the Moon"},
    {12, 14, 1962, "Mariner 2 flies past Venus, the first planetary flyby"},
    {12, 14, 1972, "The last humans to walk on the Moon leave it"},
    {12, 15, 1970, "Venera 7 makes the first landing on another planet"},
    {12, 21, 1968, "Apollo 8 launches, the first crew to leave Earth orbit"},
    {12, 21, 2015, "A Falcon 9 first stage lands back after an orbital launch"},
    {12, 24, 1968, "Apollo 8 orbits the Moon and photographs Earthrise"},
    {12, 25, 2021, "The James Webb Space Telescope is launched"},
};
int on(int mo, int d, const Ev **out, int max) {
  int n = 0;
  for (const Ev &e : EVENTS)
    if (e.mo == mo && e.d == d && n < max)
      out[n++] = &e;
  return n;
}
}  // namespace hist
}  // namespace sat
#endif  // SKY_IMPL
