#pragma once
// UI-84: the fact cards of the Planets tab (touch the picture): when each planet was found and by whom, what it is named
// for, and a few facts. Written for the 432 x 346 px card: about 14 lines of 48 characters, so they stay short.
// Order: Mercury, Venus, Earth, Mars, Jupiter, Saturn, Uranus, Neptune, Pluto (the strip's order), then the Sun and the Moon
// (the picture viewer's Sun and Moon tabs).
namespace sat {
namespace facts {
struct Body {
  const char *name;
  const char *found;  // "Discovered": who and when
  const char *named;  // the name, the legend
  const char *facts;
};
constexpr int N = 11;
inline const Body &get(int i) {
  static const Body B[N] = {
      {"Mercury",
       "Known since antiquity: Sumerians wrote of it about 3000 BC.",
       "The Romans' messenger god (Greek Hermes): it darts around the Sun faster than any planet. The Greeks gave its "
       "morning and evening sightings two names.",
       "Smallest planet. Year 88 days, sunrise to sunrise 176. 430 C by day, -180 C at night. No moons. Mariner 10 "
       "flew by in 1974; MESSENGER orbited it 2011-2015."},
      {"Venus",
       "Known since antiquity as the Morning and Evening Star. Galileo saw its phases in 1610: proof that the planets "
       "circle the Sun.",
       "The Roman goddess of love and beauty (Greek Aphrodite), for the brightest planet in the sky.",
       "Hottest planet: 465 C under crushing air, 90 times Earth's pressure. Spins backwards, and its day (243 Earth "
       "days) is longer than its year (225). Venera 7 landed in 1970."},
      {"Earth",
       "Aristarchus first put the Sun at the centre in the 3rd century BC; Copernicus revived it in 1543. Eratosthenes "
       "measured its size about 240 BC from shadows.",
       "From an old Germanic word for ground. The only planet not named for a god (the Romans' Tellus, the Greeks' "
       "Gaia, was the goddess).",
       "4.54 billion years old, 71% ocean, one Moon. Apollo 8 took Earthrise in 1968; Apollo 17 took this Blue Marble "
       "on 7 December 1972."},
      {"Mars",
       "Known since antiquity: the Egyptians called it the Red One. Kepler found the ellipse from its orbit (1609). "
       "Asaph Hall found its moons in 1877.",
       "The Roman god of war (Greek Ares), for its blood-red colour. Its moons are Phobos and Deimos: Fear and Panic.",
       "Red from iron rust. Day 24 h 37 m, year 687 days. Olympus Mons, {km:22} high, is the tallest volcano known. "
       "Rovers: Sojourner 1997, Curiosity 2012, Perseverance 2021."},
      {"Jupiter",
       "Known since antiquity. In January 1610 Galileo saw four moons circling it: Io, Europa, Ganymede and Callisto, "
       "the first worlds seen orbiting another.",
       "King of the Roman gods (Greek Zeus). Its moons are his loves: Io, Europa and Callisto, and Ganymede his "
       "cup-bearer.",
       "More mass than all the other planets together. Day 9 h 56 m, year 11.9 years. The Great Red Spot is a storm "
       "wider than Earth, seen for 190 years or more. Juno arrived in 2016."},
      {"Saturn",
       "Known since antiquity, the farthest planet seen by eye. Galileo saw odd 'ears' in 1610; Huygens knew them as a "
       "ring in 1655 and found Titan. Cassini saw the gap in 1675.",
       "The Roman god of farming (Greek Cronus, father of Zeus). Saturday is his day.",
       "Lighter than water. The rings are ice, in places only about 10 m thick. Year 29.5 years. Over 140 moons. "
       "Cassini orbited 2004-2017; its Huygens probe landed on Titan in 2005."},
      {"Uranus",
       "Found by William Herschel on 13 March 1781, from Bath, England: the first planet found with a telescope. "
       "Earlier astronomers had logged it as a star.",
       "Herschel wished it 'Georgium Sidus', for King George III. The name that stuck is the Greek sky god Ouranos, "
       "father of Cronus: the only planet named for a Greek god.",
       "Tipped on its side (98 degrees). Year 84 years, -224 C. Moons named for Shakespeare's characters. Voyager 2 "
       "alone has visited, in 1986. Just visible by eye under dark skies."},
      {"Neptune",
       "Found by mathematics first: Le Verrier in France and Adams in England worked out where Uranus's wobble pointed. "
       "Johann Galle saw it on 23 September 1846 within 1 degree of the spot.",
       "The Roman sea god (Greek Poseidon), for its deep blue. Galileo had drawn it as a star in 1612 without "
       "knowing.",
       "Winds up to {kmh:2,000}. Year 165 years: its first lap since discovery ended in 2011. Triton, found 17 days "
       "after the planet, orbits backwards. Voyager 2 flew by in 1989."},
      {"Pluto",
       "Found by Clyde Tombaugh at Lowell Observatory, Arizona, on 18 February 1930, comparing photographs plate by "
       "plate. Charon was found in 1978.",
       "The Roman god of the underworld (Greek Hades), proposed by Venetia Burney, 11, of Oxford. Its first two "
       "letters are Percival Lowell's initials.",
       "Year 248 years, -230 C. Its orbit crosses Neptune's but they never meet. Five moons. New Horizons flew by on "
       "14 July 2015 and found a heart-shaped plain. (The IAU called it a dwarf planet in 2006.)"},
      {"Sun",
       "Always known. Galileo drew its spots in 1610 (Fabricius and Scheiner saw them too). Helium was found in its light in "
       "1868, 27 years before it was found on Earth.",
       "Old English sunne. The Romans' Sol, the Greeks' Helios, Egypt's Ra: the god who crossed the sky each day in a "
       "chariot or a boat.",
       "A star {km:1.39 million} across, 99.86% of all the mass in the solar system. Light takes 8 min 20 s to reach us. "
       "5500 C at the surface, 15 million C at the core. This picture is NOAA's SUVI, in ultraviolet."},
      {"Moon",
       "Always known. Galileo's telescope showed its mountains and craters in 1609. Luna 3 photographed the far side in "
       "1959; Armstrong and Aldrin landed on 20 July 1969.",
       "Old English mona. The Roman goddess Luna, the Greek Selene: she drove the Moon's chariot across the night sky. "
       "Monday is Moon day.",
       "{km:384,400} away, a quarter of Earth's width. Phases repeat every 29.5 days. The same face always points at us. "
       "It drifts away 3.8 cm a year. The picture is NASA's Dial-A-Moon, from LRO."},
  };
  return B[i < 0 ? 0 : i >= N ? N - 1 : i];
}
}  // namespace facts
}  // namespace sat
