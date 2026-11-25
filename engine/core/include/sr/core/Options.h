#pragma once

// The player's options (the original's Option window: video, audio, camera, input, game) as
// the client keeps them, with the defaults, limits and edit rules of the original, and a plain
// text file format (`key=value` lines; unknown or invalid lines are ignored, so an old or hand
// edited file never stops the game).
//
//  - video: two detail records (the active one is used) of 16 bytes (slot 0 is the graphic
//    quality preset: low / middle / high / "control war" / custom; the other slots are single
//    settings, each with its entries), the screen size (0 x 0: the window as it is) and a frame
//    limit; the scenery sight range (slot 2) is the far plane of the world;
//  - audio: three levels 0..100 (music, effects, environment) with a mute each;
//  - camera: the sight mode (free, third person, quarter view);
//  - input: the mouse mode and 34 key bindings (Windows virtual-key codes, 0 = none; a key is
//    on one command only);
//  - game: on / off switches (names over heads, status windows, invitations, warnings ...).

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace sr::core {

struct VideoOptions {
    static constexpr int kSlots = 16;
    using Record = std::array<int, kSlots>;
    int active = 0;           // which record is in use (0 or 1)
    std::array<Record, 2> records{};
    std::array<Record, 2> custom{}; // the "custom" bank of each record, kept while a preset is shown
    int displayWidth = 0, displayHeight = 0; // 0 x 0: the window as it is
    // The brightness of each record: 0 very dark, 1 dark, 2 normal, 3 bright, 4 very bright (the
    // original's five levels; a gamma in the frame's last step).
    std::array<int, 2> brightness{2, 2};
    float Gamma() const { static constexpr float kGamma[5] = {0.80f, 0.90f, 1.0f, 1.12f, 1.25f}; return kGamma[std::clamp(brightness[std::size_t(active)], 0, 4)]; }
    int frameLimit = 0;       // frames a second; 0: the display's rate (vertical sync); -1: unlimited
    // Our own rows (the modern renderer), after the original's.
    int windowMode = 0;       // 0 windowed, 1 borderless, 2 fullscreen
    int antiAliasing = 0;     // 0 off, 1 FXAA, 2..4 MSAA 2x / 4x / 8x
    int anisotropy = 3;       // texture filtering: 0 trilinear, 1..4 anisotropic 2x / 4x / 8x / 16x
    int sunShadows = 0;       // 0 the original's terrain lightmaps, 1..4 cascaded shadow maps low / medium / high / ultra
    int lightEffects = 0;     // local lights from effects: 0 the original's (none), 1 medium, 2 high (more, highlights)
    int lensFlare = 1;        // the original's lens flare row widened: 0 off, 1 the original's, 2 modern (+ light shafts)
    int bloom = 1;            // the original's bloom row widened: 0 off, 1 the original's glow, 2 modern
    int toneMapping = 0;      // 0 the original's (clamped), 1 filmic (ACES), 2 AgX
    int colourGrading = 0;    // 0 none (the original), 1 by the time of day
    int ambientOcclusion = 0; // 0 off, 1 low (half resolution), 2 high
    int softParticles = 0;    // effects fade where they meet the scene
    int foliageSway = 0;      // trees and grass move in the wind
    int effectGlow = 10;      // the effects' brightness in tenths: 10 the original's (the default); lower is ours
    int characterLighting = 0; // 0 the original's, 1 modern (wrap and rim light)
    int water = 0;            // 0 the original's, 1 modern (refraction, depth colour, caustics, Fresnel, foam)
    int weather = 1;          // 0 none, 1 the original's (rain, snow, lightning), 2 modern (+ wet ground, overcast light)
    int volumetricFog = 0;    // 0 off, 1 low, 2 high
    int clouds = 0;           // 0 the original's cloud layer, 1 volumetric
    int viewDistance = 0;     // the fog pushed out: 0 the original's, 1 x2, 2 x3, 3 x4 (more regions loaded)
    int shadowDistance = 1;   // how far the sun's shadows reach: 0 near (1800), 1 medium (2800), 2 far (4000), 3 very far (6000)
    int shadowCulling = 1;    // the shadows' casters: 0 all everywhere, 1 balanced, 2 fast (fewer, characters near only)
    int detailDistance = 1;   // levels of detail: how far terrain and small objects keep their detail (0 near .. 3 very far; scaled by the view distance)
    int lightReach = 1;       // how far lights from effects and windows show: 0 near (800), 1 medium (1500), 2 far (3000)
    // Our rows saved as `video.<key>`: the field and its largest value.
    struct ExtraRow {
        const char* key;
        int VideoOptions::*field;
        int max;
        int initial;
    };
    static const std::array<ExtraRow, 21>& ExtraRows();
    static constexpr int kAntiAliasingChoices = 5;
    // The scene's sample count (1 unless MSAA is chosen).
    std::uint32_t Samples() const { return antiAliasing >= 2 ? 1u << (antiAliasing - 1) : 1u; }
    float AnisotropyLevel() const { return anisotropy <= 0 ? 1.0f : static_cast<float>(1 << anisotropy); }

    // One row of the detail list.
    struct Row {
        int slot;
        const char* text; // interface text key
        int entries;
        const char* const* names; // entry texts (keys, or plain text when they do not start with UIIT)
    };
    static const Row* Rows(int& count);
    static int Entries(int slot); // 0 for an unused slot

    static VideoOptions Defaults();
    // Our looks: 0 Classic (every row of ours at the original's), 1 Modern Low, 2 High, 3 Ultra.
    void ApplyLook(int look);
    // "classic", "modern-low", "modern-high", "modern-ultra" -> 0..3; -1 when none of them.
    static int LookByName(std::string_view name);
    const Record& current() const { return records[static_cast<std::size_t>(active)]; }
    // Sets one slot of the active record by the original's rules: choosing a preset (slot 0)
    // loads it (custom loads the kept custom bank); any other change turns the record into
    // custom first. False when the slot or value is not allowed.
    bool Change(int slot, int value);
    // The original's Default button: the active record (and its custom bank) back to the
    // defaults, and the frame limit.
    void ResetActive();
    // The far plane of the world for the active record's scenery sight range.
    float SceneryDistance() const;
    // The screen sizes the list offers (the first is "the window as it is").
    static const std::array<std::array<int, 2>, 11>& DisplaySizes();
    // 30 .. 240, 0 (the display's rate, vertical sync), -1 (unlimited).
    static const std::array<int, 11>& FrameLimits();
};

struct AudioOptions {
    int bgm = 30, effects = 50, environment = 50;
    bool muteBgm = false, muteEffects = false, muteEnvironment = false;
    static AudioOptions Defaults() { return {50, 50, 50, false, false, false}; } // the Default button
    // The linear gain the original's volume curve gives a level (-20 dB at 0, 0 dB at 100; 0 when muted).
    static float Gain(int level, bool muted);
};

enum class Sight : int { Free = 0, ThirdPerson = 1, QuarterView = 2 };

struct InputOptions {
    static constexpr int kBindings = 34;
    int mouseMode = 0; // 0: the wheel changes the sight, 1: the wheel uses a skill
    std::array<int, kBindings> keys{};
    static InputOptions Defaults();
    // The text key naming a binding's command (UIIT_...).
    static const char* Label(int slot);
    // A key may be bound (the original refuses the number row, the numeric keypad, arrows,
    // Esc, Pause and a few more).
    static bool CaptureAllowed(int vk);
    // Binds `vk` to `slot`; the command that had it loses it. False when not allowed.
    bool Capture(int slot, int vk);
    // The label of a key on a binding button.
    static std::string KeyName(int vk);
};

struct GameOptions {
    struct Row {
        const char* key;  // the name in the options file
        const char* text; // interface text key
        bool initial;     // on a new install
        bool reset;       // after the Default button
        int group;        // 0: names and status windows, 1: everything else
    };
    static const Row* Rows(int& count);
    static constexpr int kMax = 32;
    std::array<bool, kMax> on{};
    bool beginnerMark = true; // the "newbie" sign over the head (level 19 and under)
    static GameOptions Initial();
    static GameOptions Reset(); // the Default button
    bool Get(std::string_view key) const;
    void Set(std::string_view key, bool value);
};

// Ours (the Option window's Online page): how many players and monsters the player wants to see,
// for a weaker computer or connection. 0: as many as the server allows; the server never sends more
// than its own shares.
struct OnlineOptions {
    static constexpr int kMax = 2000;
    static constexpr int kMinRadius = 200, kMaxRadius = 600; // how far the player sees, in the minimap's units
    int players = 0, monsters = 0;
    int radius = kMaxRadius;
};

// Ours, the Option window's Beta page: how a click on the ground moves the player. Classic walks straight where it was
// clicked and stops at the first wall, as the original's does (a click on the sky: that way); Pathfinding searches a way
// round walls and buildings on the client first, then walks it as straight legs the server checks as it checks clicks.
// Names over heads: the original's (every kind within 300 of the player, the one under the mouse at any distance) or ours
// (players' names at any distance, the others' within the name distance).
struct BetaOptions {
    enum class Movement : int { Classic = 0, Pathfinding = 1 };
    Movement movement = Movement::Classic;
    bool namesFar = false;
};

struct Options {
    OnlineOptions online;
    BetaOptions beta;
    VideoOptions video = VideoOptions::Defaults();
    AudioOptions audio;
    Sight sight = Sight::Free;
    InputOptions input = InputOptions::Defaults();
    GameOptions game = GameOptions::Initial();

    // The file's text, and back; Parse keeps every default a line does not set or sets wrongly.
    std::string Serialize() const;
    static Options Parse(std::string_view text);
    static bool Load(const std::string& path, Options& out);
    bool Save(const std::string& path) const;
};

} // namespace sr::core
