#include <doctest/doctest.h>

#include <cstdio>

#include "sr/core/Options.h"

using namespace sr::core;

TEST_CASE("options start as the original's defaults") {
    const Options o;
    CHECK(o.video.active == 0);
    CHECK(o.video.records[0][0] == 1); // middle quality
    CHECK(o.video.records[0][2] == 2); // scenery sight range 3
    CHECK(o.video.records[1] == o.video.records[0]);
    CHECK(o.video.SceneryDistance() == 3500.0f);
    CHECK(o.audio.bgm == 30);
    CHECK(AudioOptions::Defaults().bgm == 50);
    CHECK(o.sight == Sight::Free);
    CHECK(o.input.keys[0] == 'C');
    CHECK(o.input.keys[7] == 'M');
    CHECK(o.input.keys[8] == 9); // Tab
    CHECK(o.game.Get("ownName"));
    CHECK_FALSE(o.game.Get("hpWarning")); // warnings and the status windows start off ...
    CHECK(GameOptions::Reset().Get("hpWarning")); // ... the Default button turns them on
    CHECK_FALSE(GameOptions::Reset().Get("windowMode"));
}

TEST_CASE("choosing a quality preset loads it; any other change makes the record custom") {
    VideoOptions v = VideoOptions::Defaults();
    REQUIRE(v.Change(0, 2)); // high
    CHECK(v.current()[0] == 2);
    CHECK(v.current()[2] == 4);
    CHECK(v.current()[4] == 1); // water reflection on
    // Editing one slot while a preset shows changes only that byte of the custom bank (the
    // original does not copy the preset into it): bloom off here, the rest as custom was.
    REQUIRE(v.Change(11, 0));
    CHECK(v.current()[0] == 4);
    CHECK(v.current()[11] == 0);
    CHECK(v.current()[2] == 2);
    REQUIRE(v.Change(0, 0)); // low preset: the custom bank is kept
    CHECK(v.current()[2] == 0);
    REQUIRE(v.Change(0, 4)); // back to custom: the kept bank
    CHECK(v.current()[11] == 0);
    CHECK(v.current()[2] == 2);
    CHECK_FALSE(v.Change(2, 5));  // out of range
    CHECK_FALSE(v.Change(14, 0)); // an unused slot
    CHECK_FALSE(v.Change(0, -1));
    v.ResetActive();
    CHECK(v.current() == VideoOptions::Defaults().current());
    // the other record was never touched
    CHECK(v.records[1] == VideoOptions::Defaults().records[1]);
}

TEST_CASE("a key is bound to one command; some keys are refused") {
    InputOptions in = InputOptions::Defaults();
    REQUIRE(in.Capture(0, 'I')); // the character window takes I from the inventory
    CHECK(in.keys[0] == 'I');
    CHECK(in.keys[1] == 0);
    CHECK_FALSE(in.Capture(0, 27));  // Esc
    CHECK_FALSE(in.Capture(0, 37));  // an arrow
    CHECK_FALSE(in.Capture(0, '5')); // the number row is the quickslots
    CHECK_FALSE(in.Capture(40, 'B'));
    CHECK(in.keys[0] == 'I');
    CHECK(InputOptions::KeyName(112) == "F1");
    CHECK(InputOptions::KeyName('C') == "C");
    CHECK(InputOptions::KeyName(9) == "Tab");
    CHECK(std::string(InputOptions::Label(0)) == "UIIT_STT_CHARACTER");
}

TEST_CASE("the audio curve") {
    CHECK(AudioOptions::Gain(0, false) == doctest::Approx(0.1f)); // -20 dB: the original never silences at 0
    CHECK(AudioOptions::Gain(100, true) == 0.0f);
    CHECK(AudioOptions::Gain(100, false) == doctest::Approx(1.0f));
    CHECK(AudioOptions::Gain(50, false) == doctest::Approx(0.3162f).epsilon(0.01)); // -10 dB
}

TEST_CASE("options survive a round trip through their file text; bad lines are ignored") {
    Options o;
    o.video.Change(0, 2);
    o.video.active = 1;
    o.video.displayWidth = 1280;
    o.video.displayHeight = 720;
    o.video.frameLimit = 120;
    o.video.windowMode = 1;
    o.video.antiAliasing = 1;
    o.video.anisotropy = 4;
    o.video.sunShadows = 2;
    o.video.lightEffects = 1;
    o.video.lensFlare = 2;
    o.video.bloom = 1;
    o.video.toneMapping = 2;
    o.video.ambientOcclusion = 2;
    o.video.foliageSway = 1;
    o.audio.bgm = 7;
    o.audio.muteEffects = true;
    o.sight = Sight::ThirdPerson;
    o.input.mouseMode = 1;
    o.input.Capture(3, 'B');
    o.game.Set("monsterNames", false);
    o.game.Set("hpWarning", true);
    o.game.beginnerMark = false;
    o.online.players = 120;
    o.online.monsters = 60;
    const Options back = Options::Parse(o.Serialize());
    CHECK(back.online.players == 120);
    CHECK(back.online.monsters == 60);
    CHECK(Options::Parse("online.players=5000\n").online.players == 0); // past the most: kept at the default
    // The Beta page's movement: Classic by default, Pathfinding kept, a word it does not know ignored.
    CHECK(Options{}.beta.movement == BetaOptions::Movement::Classic);
    Options moving;
    moving.beta.movement = BetaOptions::Movement::Pathfinding;
    CHECK(Options::Parse(moving.Serialize()).beta.movement == BetaOptions::Movement::Pathfinding);
    CHECK(Options::Parse("beta.movement=flying\n").beta.movement == BetaOptions::Movement::Classic);
    CHECK(back.video.active == 1);
    CHECK(back.video.records == o.video.records);
    CHECK(back.video.custom == o.video.custom);
    CHECK(back.video.displayWidth == 1280);
    CHECK(back.video.displayHeight == 720);
    CHECK(back.video.frameLimit == 120);
    CHECK(back.video.windowMode == 1);
    CHECK(back.video.antiAliasing == 1);
    CHECK(back.video.anisotropy == 4);
    CHECK(back.video.sunShadows == 2);
    CHECK(back.video.lightEffects == 1);
    CHECK(back.video.lensFlare == 2);
    CHECK(back.video.bloom == 1);
    CHECK(back.video.toneMapping == 2);
    CHECK(back.video.ambientOcclusion == 2);
    CHECK(back.video.foliageSway == 1);
    CHECK(Options::Parse("video.toneMapping=3\n").video.toneMapping == 0); // out of range: kept at the default
    CHECK(Options::Parse("video.lensFlare=0\n").video.lensFlare == 0);
    CHECK(back.video.AnisotropyLevel() == 16.0f);
    CHECK(Options::Parse("video.frameLimit=-1\n").video.frameLimit == -1); // unlimited
    CHECK(Options::Parse("video.frameLimit=144\n").video.frameLimit == 144);
    CHECK(Options::Parse("video.antiAliasing=4\n").video.antiAliasing == 4); // MSAA 8x
    CHECK(Options::Parse("video.antiAliasing=4\n").video.Samples() == 8u);
    CHECK(Options::Parse("video.antiAliasing=5\n").video.antiAliasing == 0);
    CHECK(back.video.Samples() == 1u); // FXAA renders one sample
    CHECK(back.audio.bgm == 7);
    CHECK(back.audio.muteEffects);
    CHECK(back.sight == Sight::ThirdPerson);
    CHECK(back.input.mouseMode == 1);
    CHECK(back.input.keys == o.input.keys);
    CHECK_FALSE(back.game.Get("monsterNames"));
    CHECK(back.game.Get("hpWarning"));
    CHECK_FALSE(back.game.beginnerMark);

    const Options bad = Options::Parse(
        "video.active=7\naudio.bgm=300\naudio.effects=abc\ncamera.sight=9\nvideo.display=1x1\nvideo.frameLimit=77\n"
        "input.keys=1,2,3\nvideo.record0=1,2,3\ngame.nothing=1\ngarbage\n# comment\n\nvideo.frameLimit=60\n");
    CHECK(bad.video.active == 0);
    CHECK(bad.audio.bgm == 30);
    CHECK(bad.audio.effects == 50);
    CHECK(bad.sight == Sight::Free);
    CHECK(bad.video.displayWidth == 0);
    CHECK(bad.video.frameLimit == 60); // the one good line still counts
    CHECK(bad.input.keys == InputOptions::Defaults().keys);
    CHECK(bad.video.records == VideoOptions::Defaults().records);

    // two commands on one key: the line is refused as a whole
    const Options clash = Options::Parse("input.keys=67,67,83,65,80,81,85,77,9,72,90,88,78,71,45,36,33,46,35,34,82,84,87,69,89,70,0,0,0,0,86,76,0,0\n");
    CHECK(clash.input.keys == InputOptions::Defaults().keys);
}

TEST_CASE("options load from and save to a file") {
    const std::string path = "sr_options_test.cfg";
    Options o;
    o.audio.environment = 12;
    REQUIRE(o.Save(path));
    Options loaded;
    REQUIRE(Options::Load(path, loaded));
    CHECK(loaded.audio.environment == 12);
    std::remove(path.c_str());
    CHECK_FALSE(Options::Load("no_such_options_file.cfg", loaded));
}
