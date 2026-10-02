#pragma once
// UI-46: constellation cards. For every constellation whose name the map can show
// (d3-celestial rank 1-2): what the name means, its story, its brightest star and one
// thing worth finding in it. Magnitudes are approximate.
#include <cstring>

namespace sat {
namespace lore {

struct Lore {
  const char *name, *meaning, *story, *star, *sight;
};

inline const Lore LORE[] = {
    {"Andromeda", "The chained princess",
     "Daughter of Queen Cassiopeia, chained to a sea rock for the monster Cetus and rescued by Perseus.",
     "Alpheratz, magnitude 2.1", "The Andromeda Galaxy (M31): 2.5 million light-years, the farthest thing the naked eye can see."},
    {"Aquarius", "The water bearer",
     "Ganymede, the youth Zeus's eagle carried off to pour wine for the gods.",
     "Sadalsuud, magnitude 2.9", "Home of the Eta Aquariid meteors (early May), dust from Halley's Comet."},
    {"Aquila", "The eagle", "The eagle that carried Zeus's thunderbolts and snatched Ganymede up to Olympus.",
     "Altair, magnitude 0.8, 17 light-years; it spins once every 9 hours", "Altair is one corner of the Summer Triangle, with Vega and Deneb."},
    {"Aries", "The ram", "The ram with the golden fleece that Jason and the Argonauts sailed to find.",
     "Hamal, magnitude 2.0", "It once held the spring equinox, still called the First Point of Aries."},
    {"Auriga", "The charioteer", "Erichthonius, a king of Athens said to have invented the four-horse chariot.",
     "Capella, magnitude 0.1: two giant stars and two red dwarfs", "Three star clusters, M36, M37 and M38, in binoculars."},
    {"Bootes", "The herdsman", "The ploughman who drives the two bears around the pole.",
     "Arcturus, magnitude 0.0, the brightest star in the northern sky", "Follow the arc of the Big Dipper's handle: arc to Arcturus."},
    {"Camelopardalis", "The giraffe", "No old myth: added in 1612 by the Dutch map maker Petrus Plancius to fill a dark gap.",
     "Beta Camelopardalis, magnitude 4.0", "Kemble's Cascade, a straight line of stars, in binoculars."},
    {"Cancer", "The crab", "Sent by Hera to bite Hercules's foot while he fought the Hydra; he crushed it.",
     "Tarf, magnitude 3.5", "The Beehive Cluster (M44): a misty patch to the eye, dozens of stars in binoculars."},
    {"Canes Venatici", "The hunting dogs", "The two dogs Asterion and Chara, held on a leash by Bootes. Added in 1687.",
     "Cor Caroli (\"Charles's heart\"), magnitude 2.9", "The Whirlpool Galaxy (M51), face-on, in a small telescope."},
    {"Canis Major", "The great dog", "Orion's larger hunting dog, following him across the sky.",
     "Sirius, magnitude -1.5, the brightest star of the night, 8.6 light-years", "The star cluster M41 just below Sirius."},
    {"Canis Minor", "The little dog", "Orion's smaller hunting dog.",
     "Procyon, magnitude 0.4, 11.5 light-years", "Procyon is a corner of the Winter Triangle, with Sirius and Betelgeuse."},
    {"Capricornus", "The sea goat", "Pan, who jumped into the Nile to escape the monster Typhon and became half fish.",
     "Deneb Algedi, magnitude 2.9", "The globular cluster M30."},
    {"Carina", "The keel", "The keel of the Argo, the ship of Jason and the Argonauts.",
     "Canopus, magnitude -0.7, the second-brightest star", "The Carina Nebula, far brighter than Orion's (a southern sky sight)."},
    {"Cassiopeia", "The queen", "Andromeda's vain mother, who boasted of her beauty and was set in the sky upside down.",
     "Schedar, magnitude 2.2", "The W circles the pole: north of about 30° it never sets."},
    {"Centaurus", "The centaur", "Chiron, the wise centaur who taught Achilles, Jason and Asclepius.",
     "Alpha Centauri, magnitude -0.3: the nearest star system, 4.4 light-years", "Omega Centauri, the biggest globular cluster (a southern sky sight)."},
    {"Cepheus", "The king", "King of Ethiopia, husband of Cassiopeia and father of Andromeda.",
     "Alderamin, magnitude 2.5", "Delta Cephei, whose steady pulsing became the yardstick for distances across the universe."},
    {"Cetus", "The sea monster", "The monster sent to devour Andromeda, turned to stone by Perseus with Medusa's head.",
     "Diphda, magnitude 2.0", "Mira, \"the wonderful\": it brightens and fades out of sight over 11 months."},
    {"Corona Borealis", "The northern crown", "Ariadne's wedding crown, thrown into the sky by Dionysus.",
     "Alphecca, magnitude 2.2", "T Coronae Borealis, a nova that blazes up about every 80 years (1866, 1946)."},
    {"Crux", "The southern cross", "The smallest constellation; used by sailors to find south.",
     "Acrux, magnitude 0.8", "The Coalsack, a dark cloud blotting out the Milky Way (a southern sky sight)."},
    {"Cygnus", "The swan", "Zeus in disguise, flying down the Milky Way.",
     "Deneb, magnitude 1.3, about 2,600 light-years: one of the most luminous stars known",
     "The Northern Cross, lying along the Milky Way."},
    {"Draco", "The dragon", "Ladon, the dragon that guarded the golden apples, killed by Hercules.",
     "Eltanin, magnitude 2.2", "Thuban was the pole star when the pyramids were built."},
    {"Eridanus", "The river", "The river where Phaethon fell after losing control of the Sun's chariot.",
     "Achernar, magnitude 0.5 (far south)", "Epsilon Eridani, a young Sun-like star only 10.5 light-years away."},
    {"Gemini", "The twins", "Castor and Pollux, brothers so close that Zeus set them in the sky together.",
     "Pollux, magnitude 1.1", "The Geminid meteors, around December 14: the year's richest shower."},
    {"Hercules", "The hero", "The strongest of heroes and his twelve labours; he kneels upside down in the sky.",
     "Kornephoros, magnitude 2.8", "M13, the Great Globular Cluster: some 300,000 stars in a ball."},
    {"Hydra", "The water snake", "The many-headed serpent of Lerna that Hercules slew. The largest constellation.",
     "Alphard (\"the solitary one\"), magnitude 2.0", "It stretches over a quarter of the sky."},
    {"Leo", "The lion", "The Nemean lion, whose hide no weapon could pierce, strangled by Hercules.",
     "Regulus, magnitude 1.4", "The Sickle, a backwards question mark; the Leonid meteors in mid-November."},
    {"Libra", "The scales", "The scales of justice: the only zodiac sign that isn't a creature.",
     "Zubeneschamali, magnitude 2.6", "Its star names mean the scorpion's northern and southern claws."},
    {"Lyra", "The lyre", "The lyre of Orpheus, whose music could charm animals, trees and stones.",
     "Vega, magnitude 0.0, 25 light-years; the pole star around the year 13,700",
     "The Ring Nebula (M57), a dying star's smoke ring; the Lyrid meteors in April."},
    {"Monoceros", "The unicorn", "No old myth: added in 1612 by Petrus Plancius.",
     "Beta Monocerotis, magnitude 3.7: a fine triple star", "The Rosette Nebula, a vast rose of glowing gas."},
    {"Ophiuchus", "The serpent bearer", "Asclepius, the healer so skilled he could raise the dead.",
     "Rasalhague, magnitude 2.1", "The Sun crosses it in early December: the \"13th sign\" of the zodiac."},
    {"Orion", "The hunter", "The giant hunter who boasted he could kill any beast, and was killed by a scorpion.",
     "Rigel, magnitude 0.1; Betelgeuse, a red supergiant", "The Orion Nebula (M42) below the belt: stars being born, 1,340 light-years."},
    {"Pavo", "The peacock", "Hera's bird, whose tail carries the hundred eyes of Argus.",
     "Peacock, magnitude 1.9", "The globular cluster NGC 6752 (a southern sky sight)."},
    {"Pegasus", "The winged horse", "Born from the blood of Medusa; the hero Bellerophon rode him.",
     "Enif, magnitude 2.4", "The Great Square of Pegasus; 51 Pegasi, the first Sun-like star found with a planet (1995)."},
    {"Perseus", "The hero", "The hero who beheaded Medusa and rescued Andromeda from Cetus.",
     "Mirfak, magnitude 1.8", "Algol, the Demon Star, dims every 2.9 days; the Perseid meteors on August 12."},
    {"Phoenix", "The phoenix", "The bird that is reborn from its own ashes. Added in the 1590s.",
     "Ankaa, magnitude 2.4", "A southern sky constellation."},
    {"Pisces", "The fishes", "Aphrodite and Eros, who turned into fish tied together to escape Typhon.",
     "Alpherg, magnitude 3.6", "The spring equinox point now lies here."},
    {"Piscis Austrinus", "The southern fish", "The fish that drinks the water poured by Aquarius.",
     "Fomalhaut, magnitude 1.2, \"the lonely star of autumn\"", "Fomalhaut has a ring of dust where planets may be forming."},
    {"Puppis", "The stern", "The stern of the Argo, the ship of the Argonauts.",
     "Naos, magnitude 2.2, one of the hottest stars the eye can see", "The star clusters M46 and M47, side by side."},
    {"Sagittarius", "The archer", "A centaur drawing his bow at the heart of the Scorpion.",
     "Kaus Australis, magnitude 1.8", "The centre of our galaxy and its black hole, 26,000 light-years away; the Teapot."},
    {"Scorpius", "The scorpion", "The scorpion that killed Orion: the two are set on opposite sides of the sky.",
     "Antares, magnitude 1.0, \"rival of Mars\" for its red colour", "A curved tail of bright stars, low in the south."},
    {"Taurus", "The bull", "Zeus as a white bull, carrying the princess Europa across the sea.",
     "Aldebaran, magnitude 0.9", "The Pleiades (Seven Sisters) and the Hyades; the Crab Nebula, from a star seen exploding in 1054."},
    {"Triangulum Australe", "The southern triangle", "Added by Dutch navigators in the 1590s.",
     "Atria, magnitude 1.9", "A southern sky constellation."},
    {"Ursa Major", "The great bear", "Callisto, turned into a bear by jealous Hera and set in the sky by Zeus.",
     "Alioth, magnitude 1.8", "The Big Dipper: the two end stars of its bowl point to Polaris."},
    {"Ursa Minor", "The little bear", "Arcas, Callisto's son, placed beside his mother.",
     "Polaris, the North Star, magnitude 2.0, within a degree of the pole", "The Little Dipper hangs from Polaris."},
    {"Vela", "The sails", "The sails of the Argo, the ship of the Argonauts.",
     "Gamma Velorum, magnitude 1.8", "The Vela supernova remnant (a southern sky sight)."},
    {"Virgo", "The maiden", "Demeter or her daughter Persephone, goddess of the harvest.",
     "Spica, magnitude 1.0", "The Virgo Cluster: over a thousand galaxies."},
};

inline const Lore *find(const char *name) {
  for (const auto &l : LORE)
    if (strcmp(l.name, name) == 0)
      return &l;
  return nullptr;
}

}  // namespace lore
}  // namespace sat
