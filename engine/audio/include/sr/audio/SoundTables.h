#pragma once

// The game's sound tables (text already decoded to UTF-8, core::DecodeTextFile), all three from
// `server_dep\silkroad\textdata\`. 1.188 moved regioninfo.txt there too, from resinfo\; Data.pk2's
// copies (regioninfo.txt, shader\regioninfo.txt) are taken to be other tools' (not confirmed).
//
//  - effectsound.txt: which sound an object makes on an event. Tab-separated, `//` comments; columns
//    (after an empty first one): object (PLAYER, ITEM, UI, MOB_..., PCM_ / PCF_ the player characters'
//    voices, GUARD_..., COS_..., STRUCTURE_...), handle (SND_BUTTON_CLICK, SND_SWING1, SND_DMG,
//    VOC_DEATH ...), skill (skilldata's base name, col 5, for a player's skill; the codename for a
//    monster's), three events (a weapon or item kind, a ground, NORMAL / CRITYCAL ...), a number (not
//    used here), folder and file under prim\snd\, volume (percent), a description; `-` is empty.
//    Several rows of one key are alternatives (one is picked at random).
//  - effectenvsnd.txt: an area's music and ambience: `<1> name`, then the music's file name in
//    Music.pk2 in quotes, then `<2> day` and `<2> night`, each with `<3> "file.wav" min~max` (seconds
//    between plays; 0~0 loops all the time; files in prim\snd\env\).
//  - regioninfo.txt: the named areas, `#TOWN name` / `#FIELD name [dungeon]`, each followed by its
//    regions: `x y ALL` (the whole region) or `x y RECT x0 z0 x1 z1` (a part, in the region's own
//    units 0..1920). The area names are effectenvsnd.txt's.

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "sr/assets/RegionInfo.h"

namespace sr::audio {

struct EffectSoundRow {
    std::string object;
    std::string handle;
    std::string skill;                 // empty: any (the row is not tied to a skill)
    std::array<std::string, 3> events; // empty: any
    std::string path;                  // prim\snd\<folder><file>; empty for a row without a file
    float volume = 1.0f;               // the column's percent / 100
};

class EffectSoundTable {
public:
    bool Parse(std::string_view text, std::string& error);
    std::size_t Size() const { return rows_.size(); }
    const std::vector<EffectSoundRow>& Rows() const { return rows_; }

    // What a lookup asks: `object` (empty: any object), `handle`, `skill` (empty: the rows tied to no
    // skill; otherwise only that skill's), and events that must match when given (a row's empty event
    // matches anything).
    struct Key {
        std::string_view object, handle, skill;
        std::array<std::string_view, 3> events{};
    };
    // The rows with a file that match, in file order.
    std::vector<const EffectSoundRow*> Find(const Key& key) const;
    // One of them at random; null when none.
    const EffectSoundRow* Pick(const Key& key, std::minstd_rand& random) const;
    // Whether any row names this skill (as a skill's base name or codename).
    bool HasSkill(std::string_view skill) const { return bySkill_.contains(std::string(skill)); }
    bool HasObject(std::string_view object) const { return byObject_.contains(std::string(object)); }

private:
    std::vector<EffectSoundRow> rows_;
    std::unordered_map<std::string, std::vector<std::uint32_t>> bySkill_;  // rows tied to a skill
    std::unordered_map<std::string, std::vector<std::uint32_t>> byObject_; // rows tied to none
};

struct AreaSounds {
    struct Ambient {
        std::string path; // prim\snd\env\<file>
        float minSeconds = 0, maxSeconds = 0; // between plays; both 0: a loop
        bool Loop() const { return minSeconds <= 0.0f && maxSeconds <= 0.0f; }
    };
    std::string name;
    std::string music; // the file in Music.pk2 (at its root)
    std::vector<Ambient> day, night;
};

class AreaSoundTable {
public:
    bool Parse(std::string_view text, std::string& error);
    const AreaSounds* Find(std::string_view name) const;
    const std::vector<AreaSounds>& Areas() const { return areas_; }

private:
    std::vector<AreaSounds> areas_;
};

// regioninfo.txt's areas: assets::RegionAreaTable (the server reads the towns from it too).
using RegionAreaTable = assets::RegionAreaTable;

} // namespace sr::audio
