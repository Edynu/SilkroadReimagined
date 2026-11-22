// Checks against a real Silkroad Online 1.188 install (E:\Silkroad\VSRO_Client). Set SR_GAME_DIR to the
// client folder (the one containing Data.pk2) to run them; otherwise skipped.

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include <map>
#include <set>

#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/assets/Outfit.h"
#include "sr/assets/IntroScript.h"
#include "sr/assets/Shop.h"
#include "sr/assets/Gacha.h"
#include "sr/assets/Effect.h"
#include "sr/assets/Environment.h"
#include "sr/assets/SkillAnimation.h"
#include "sr/game/MagicPop.h"
#include "sr/game/Mall.h"
#include "sr/game/Skills.h"
#include "sr/assets/ItemGlow.h"
#include "sr/assets/Dress.h"
#include "sr/assets/ExeCursors.h"
#include <algorithm>
#include "sr/assets/Animation.h"
#include "sr/assets/Model.h"
#include "sr/assets/Terrain.h"
#include "sr/assets/Texture.h"
#include "sr/assets/WorldMap.h"
#include "sr/assets/RegionInfo.h"
#include "sr/assets/StallNetworkData.h"
#include "sr/assets/UiLayout.h"
#include "sr/core/Text.h"
#include "sr/core/Path.h"

using namespace sr::assets;

namespace {

std::filesystem::path GameDir() {
    const char* dir = std::getenv("SR_GAME_DIR");
    return dir == nullptr ? std::filesystem::path{} : std::filesystem::path(dir);
}

} // namespace

TEST_CASE("the 1.188 archives have the expected contents") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipping game data checks");
        return;
    }

    struct Expected {
        const char* name;
        std::size_t files;
        std::size_t folders;
        std::uint64_t bytes;
    };
    // Counted independently of our reader.
    const Expected expected[] = {
        {"Data.pk2", 49467, 1993, 2126129628},
        {"Map.pk2", 14354, 89, 925876557},
        {"Media.pk2", 19499, 133, 701211804},
        {"Music.pk2", 45, 0, 60069905},
        {"Particles.pk2", 3292, 26, 80134573},
    };
    for (const auto& e : expected) {
        CAPTURE(e.name);
        Pk2Archive archive;
        std::string error;
        REQUIRE_MESSAGE(archive.Open(dir / e.name, error), error);
        std::uint64_t bytes = 0;
        for (const auto& f : archive.Files()) {
            bytes += f.size;
        }
        CHECK(archive.Files().size() == e.files);
        CHECK(archive.FolderCount() == e.folders);
        CHECK(bytes == e.bytes);
    }
}

TEST_CASE("files resolve through the merged file system") {
    const auto dir = GameDir();
    if (dir.empty()) {
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);

    auto ddj = fs.Read("water/water101.ddj"); // present in both Map.pk2 and Data.pk2
    REQUIRE(ddj.has_value());
    REQUIRE(ddj->Size() >= 12);
    CHECK(std::string(reinterpret_cast<const char*>(ddj->Bytes().data()), 12) == "JMXVDDJ 1000");
    CHECK(fs.Resolve("water/water101.ddj")->Name() == "Map.pk2"); // mounted before Data.pk2

    auto bms = fs.Read("res\\nature\\oasis\\tarim\\oas_tarim_hand.bms");
    REQUIRE(bms.has_value());
    CHECK(std::string(reinterpret_cast<const char*>(bms->Bytes().data()), 12) == "JMXVBMS 0109");
}

TEST_CASE("every .ddj texture decodes") {
    const auto dir = GameDir();
    if (dir.empty()) {
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);

    std::map<std::string, int> bySource;
    int total = 0;
    int failures = 0;
    int premultiplied = 0;
    for (const std::string& path : fs.ListFiles()) {
        const std::string key = sr::core::NormalizeAssetPath(path);
        if (!key.ends_with(".ddj")) {
            continue;
        }
        ++total;
        auto data = fs.Read(path);
        TextureData tex;
        if (!data || !DecodeDdj(data->Bytes(), tex, error)) {
            if (++failures <= 10) {
                MESSAGE(path << ": " << error);
            }
            continue;
        }
        ++bySource[tex.sourceFormat];
        premultiplied += tex.premultipliedAlpha ? 1 : 0;
    }
    // The merged file system's .ddj files (a path in two archives counts once); v1.150 had 18,134.
    CHECK(total == 32633);
    CHECK(failures == 0);
    CHECK(premultiplied == bySource["DXT2"]);
    for (const auto& [format, count] : bySource) {
        MESSAGE(format << ": " << count);
    }
}

TEST_CASE("every mesh, material set and resource decodes") {
    const auto dir = GameDir();
    if (dir.empty()) {
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);

    std::map<std::string, int> ok, failed;
    std::size_t skinned = 0, lightmapped = 0, parts = 0, partsWithMaterial = 0, brokenInGameData = 0;
    std::size_t withNavMesh = 0, navCells = 0;
    int navFailed = 0;
    for (const std::string& path : fs.ListFiles()) {
        const std::string key = sr::core::NormalizeAssetPath(path);
        const std::string ext = key.substr(key.find_last_of('.') + 1);
        if (ext != "bms" && ext != "bmt" && ext != "bsr") {
            continue;
        }
        auto data = fs.Read(path);
        REQUIRE(data.has_value());
        bool decoded = false;
        if (ext == "bms") {
            MeshData mesh;
            decoded = DecodeBms(data->Bytes(), mesh, error);
            skinned += mesh.IsSkinned() ? 1 : 0;
            lightmapped += mesh.lightmapPath.empty() ? 0 : 1;
            ObjectNavMesh nav;
            std::string navError;
            if (!DecodeBmsNavMesh(data->Bytes(), nav, navError)) {
                if (++navFailed <= 5) MESSAGE(path << ": navmesh: " << navError);
            } else if (!nav.empty()) {
                ++withNavMesh;
                navCells += nav.cells.size();
            }
        } else if (ext == "bmt") {
            std::vector<MaterialData> materials;
            decoded = DecodeBmt(data->Bytes(), path, materials, error);
        } else {
            // Four developer test files use older versions (0107/0108).
            if (std::string(reinterpret_cast<const char*>(data->Bytes().data()), 12) != "JMXVRES 0109") {
                continue;
            }
            ResourceData resource;
            decoded = DecodeBsr(data->Bytes(), resource, error);
            // Effect-only resources (sunbeams, lamps, empty.bsr) have no meshes.
            if (decoded && !resource.meshPaths.empty()) {
                ModelData model;
                if (LoadModel(fs, path, model, error)) {
                    parts += model.parts.size();
                    for (const auto& part : model.parts) partsWithMaterial += part.hasMaterial ? 1 : 0;
                } else if (error.starts_with("missing")) {
                    // the shipped data references files it does not contain
                    if (++brokenInGameData <= 24) MESSAGE("broken in the game data: " << path << ": " << error);
                } else {
                    decoded = false;
                }
            }
        }
        if (decoded) {
            ++ok[ext];
        } else if (++failed[ext] <= 5) {
            MESSAGE(path << ": " << error);
        }
    }
    CHECK(ok["bms"] == 17086);
    CHECK(ok["bmt"] == 3133);
    CHECK(ok["bsr"] == 5559); // every JMXVRES 0109 file
    CHECK(failed.empty());
    CHECK(skinned == 4118);
    CHECK(lightmapped == 4062);
    // Resources naming files the archives lack: 16 summer-event decorations, a test field, a Jupiter
    // boss (later content), waterline05 and waterline06 (v1.150 had the waterlines, room101 and
    // roc_event_cam, which 1.188 completes).
    CHECK(brokenInGameData == 20);
    MESSAGE("resource parts: " << parts << ", with a material: " << partsWithMaterial);
    // Object navmeshes (.bms section 7): every one must read exactly.
    CHECK(navFailed == 0);
    CHECK(withNavMesh > 0);
    MESSAGE("meshes with an object navmesh: " << withNavMesh << " (" << navCells << " cells)");
}

TEST_CASE("every skeleton, animation and compound decodes") {
    const auto dir = GameDir();
    if (dir.empty()) {
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);

    std::map<std::string, int> ok, failed, skipped;
    for (const std::string& path : fs.ListFiles()) {
        const std::string key = sr::core::NormalizeAssetPath(path);
        const std::string ext = key.substr(key.find_last_of('.') + 1);
        if (ext != "bsk" && ext != "ban" && ext != "cpd") {
            continue;
        }
        auto data = fs.Read(path);
        REQUIRE(data.has_value());
        // prim\...\mob_select.bsk/.ban lack the "JMXV" prefix: a developer leftover.
        if (data->Size() < 4 || std::string(reinterpret_cast<const char*>(data->Bytes().data()), 4) != "JMXV") {
            ++skipped[ext];
            continue;
        }
        bool decoded = false;
        if (ext == "bsk") {
            SkeletonData skeleton;
            decoded = DecodeBsk(data->Bytes(), skeleton, error);
            for (std::size_t i = 0; decoded && i < skeleton.bones.size(); ++i) {
                decoded = skeleton.bones[i].parent < static_cast<int>(i); // parents first
            }
        } else if (ext == "ban") {
            AnimationData animation;
            decoded = DecodeBan(data->Bytes(), animation, error);
        } else {
            CompoundData compound;
            decoded = DecodeCpd(data->Bytes(), compound, error);
        }
        if (decoded) {
            ++ok[ext];
        } else if (++failed[ext] <= 5) {
            MESSAGE(path << ": " << error);
        }
    }
    CHECK(ok["bsk"] == 807);
    CHECK(ok["ban"] == 3665);
    CHECK(ok["cpd"] == 70);
    CHECK(skipped["bsk"] == 4);
    CHECK(skipped["ban"] == 1);
    CHECK(failed.empty());
}

TEST_CASE("every region mesh, object placement file and map index decodes") {
    const auto dir = GameDir();
    if (dir.empty()) {
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);

    AssetIndex objects, tiles;
    auto objectIfo = fs.Read("object.ifo");
    auto tileIfo = fs.Read("tile2d.ifo");
    REQUIRE(objectIfo.has_value());
    REQUIRE(tileIfo.has_value());
    REQUIRE_MESSAGE(DecodeObjectIndex(objectIfo->Bytes(), objects, error), error);
    REQUIRE_MESSAGE(DecodeTileIndex(tileIfo->Bytes(), tiles, error), error);
    CHECK(objects.size() == 2767);
    CHECK(tiles.size() == 603);

    std::map<std::string, int> ok, failed;
    std::size_t placements = 0, unknownObjects = 0, unknownTiles = 0, skippedOffMap = 0;
    for (const std::string& path : fs.ListFiles()) {
        const std::string key = sr::core::NormalizeAssetPath(path);
        const bool mesh = key.ends_with(".m"), objectsFile = key.ends_with(".o2");
        if (!mesh && !objectsFile) continue;
        // Rows 0 and 1 (files 0/0.m to 1/1.m) are five off-map developer regions in an
        // older, larger block layout; the real world starts at row 35.
        if (mesh && (key.starts_with("0\\") || key.starts_with("1\\"))) {
            ++skippedOffMap;
            continue;
        }
        auto data = fs.Read(path);
        bool decoded = false;
        if (mesh) {
            TerrainRegion region;
            decoded = DecodeMapMesh(data->Bytes(), region, error);
            for (const auto& block : region.blocks) {
                for (auto word : block.textures) unknownTiles += tiles.contains(TerrainBlock::TextureId(word)) ? 0 : 1;
            }
        } else {
            std::vector<ObjectPlacement> list;
            decoded = DecodeMapObjects(data->Bytes(), list, error);
            placements += list.size();
            for (const auto& p : list) unknownObjects += objects.contains(p.objectId) ? 0 : 1;
        }
        const std::string kind = mesh ? "m" : "o2";
        if (decoded) {
            ++ok[kind];
        } else if (++failed[kind] <= 5) {
            MESSAGE(path << ": " << error);
        }
    }
    CHECK(ok["m"] == 3437);
    CHECK(skippedOffMap == 5);
    CHECK(ok["o2"] == 3300);
    CHECK(failed.empty());
    MESSAGE(placements << " placements, " << unknownObjects << " with unknown object ids, " << unknownTiles
                       << " height samples with unknown tile ids");
}

TEST_CASE("environment.ifo decodes and every region block's environment is in it") {
    const auto dir = GameDir();
    if (dir.empty()) return;
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);
    auto file = fs.Read("environment.ifo");
    REQUIRE(file.has_value());
    EnvironmentData environments;
    REQUIRE_MESSAGE(DecodeEnvironment(file->Bytes(), environments, error), error);
    MESSAGE("environments: " << environments.entries.size());
    CHECK(environments.entries.size() > 10);
    int withKeys = 0;
    for (const EnvironmentEntry& e : environments.entries) {
        if (!e.Curve(EnvironmentCurve::Diffuse).empty() && !e.Curve(EnvironmentCurve::Fog).empty()) ++withKeys;
        for (const auto& curve : e.curves) {
            for (const EnvironmentKey& k : curve) {
                CHECK(k.t >= -0.001f);
                CHECK(k.t <= 1.001f);
            }
        }
    }
    CHECK(withKeys == static_cast<int>(environments.entries.size()));
    std::map<int, int> missing;
    int blocks = 0;
    for (const std::string& path : fs.ListFiles()) {
        const std::string key = sr::core::NormalizeAssetPath(path);
        if (!key.ends_with(".m") || key.starts_with("0\\") || key.starts_with("1\\")) continue;
        auto data = fs.Read(path);
        TerrainRegion region;
        if (!DecodeMapMesh(data->Bytes(), region, error)) continue;
        for (const TerrainBlock& block : region.blocks) {
            ++blocks;
            if (environments.Find(block.environmentId) == nullptr) ++missing[block.environmentId];
        }
    }
    for (const auto& [id, count] : missing) MESSAGE("environment id " << id << " (" << count << " blocks) is not in environment.ifo");
    CHECK(blocks > 0);
    CHECK(missing.size() <= 2);
}

TEST_CASE("every region's lightmap (.t) decodes") {
    const auto dir = GameDir();
    if (dir.empty()) return;
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);
    int ok = 0, failed = 0;
    std::map<std::string, int> sizes;
    for (const std::string& path : fs.ListFiles()) {
        const std::string key = sr::core::NormalizeAssetPath(path);
        // Region lightmaps are <z>\<x>.t (Media.pk2's root SV.T is something else).
        if (!key.ends_with(".t") || key.find('\\') == std::string::npos || key.starts_with("0\\") || key.starts_with("1\\")) continue;
        auto data = fs.Read(path);
        TextureData texture;
        if (DecodeMapLightmap(data->Bytes(), texture, error)) {
            ++ok;
            ++sizes[std::to_string(texture.width) + "x" + std::to_string(texture.height)];
        } else if (++failed <= 5) {
            MESSAGE(path << ": " << error);
        }
    }
    for (const auto& [size, count] : sizes) MESSAGE("lightmaps " << size << ": " << count);
    CHECK(ok > 0);
    CHECK(failed == 0);
}

TEST_CASE("every interface layout and the interface text table decode") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);

    auto defineFile = fs.Read("config\\define.txt");
    REQUIRE(defineFile);
    const UiDefines defines = DecodeUiDefines(sr::core::DecodeTextFile(defineFile->Bytes()));
    MESSAGE(defines.size() << " layout defines");

    std::size_t layouts = 0, failed = 0, controls = 0;
    for (const std::string& path : fs.ListFiles()) {
        const std::string key = sr::core::NormalizeAssetPath(path);
        if (!key.starts_with("resinfo\\") || !key.ends_with(".txt")) continue;
        auto data = fs.Read(path);
        UiLayout layout;
        if (DecodeUiLayout(sr::core::DecodeTextFile(data->Bytes()), defines, layout, error)) {
            ++layouts;
            for (const auto& section : layout.sections) controls += section.nodes.size();
        } else if (++failed <= 5) {
            MESSAGE(path << ": " << error);
        }
    }
    CHECK(layouts > 100);
    CHECK(failed == 0);
    MESSAGE(layouts << " layouts, " << controls << " controls");

    // The login screen the client draws.
    auto title = fs.Read("resinfo\\pstitle.txt");
    REQUIRE(title);
    UiLayout layout;
    REQUIRE_MESSAGE(DecodeUiLayout(sr::core::DecodeTextFile(title->Bytes()), defines, layout, error), error);
    for (const char* name : {"GDR_STA_LOGINWINDOW", "GDR_EDIT_ID", "GDR_EDIT_PASS", "GDR_BTN_OK", "GDR_BTN_CANCEL",
                             "GDR_BTN_SERVER", "GDR_STA_SERVER", "GDR_STA_LOGO", "GDR_STA_SCREENUP",
                             "GDR_STA_SCREENDOWN", "GDR_STA_SERVERWINDOW", "GDR_LIST_SERVER"}) {
        CAPTURE(name);
        CHECK(layout.Find(name) != nullptr);
    }
    const UiLayoutNode* window = layout.Find("GDR_STA_LOGINWINDOW");
    if (window != nullptr) CHECK(fs.Exists(window->ddj));

    auto textFile = fs.Read("server_dep\\silkroad\\textdata\\textuisystem.txt");
    REQUIRE(textFile);
    TextCatalog text;
    REQUIRE_MESSAGE(DecodeTextCatalog(sr::core::DecodeTextFile(textFile->Bytes()), text, error), error);
    MESSAGE(text.size() << " interface strings");
    CHECK(text.size() > 1000);
    if (const UiLayoutNode* ok = layout.Find("GDR_BTN_OK")) {
        CAPTURE(ok->text);
        CHECK(!text[ok->text].empty());
    }
}

TEST_CASE("the name tables load from the files textdataname.txt and textquest.txt list (1.188)") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);
    TextCatalog names;
    REQUIRE_MESSAGE(LoadTextCatalog(fs, "server_dep\\silkroad\\textdata\\textdataname.txt", names, error), error);
    CHECK_MESSAGE(error.empty(), error);
    MESSAGE(names.size() << " names");
    CHECK(names["SN_ITEM_ETC_HP_POTION_02"] == "HP Recovery Potion (Small)"); // TextData_Object.txt
    CHECK(names["SN_ITEM_CH_SWORD_01_A"] == "Copper Sword");                 // TextData_Equip&Skill.txt
    CHECK(names["SN_MOB_CH_MANGNYANG"] == "Mangyang");
    // Every item the client has its name (the share without one measured, kept from growing).
    ItemTable items;
    REQUIRE_MESSAGE(items.Load(fs, error), error);
    std::size_t named = 0;
    for (const auto& [code, row] : items.rows()) named += names.contains(row.nameKey);
    MESSAGE(named << " of " << items.size() << " items named");
    CHECK(named * 100 >= items.size() * 99);

    TextCatalog quest;
    error.clear();
    REQUIRE_MESSAGE(LoadTextCatalog(fs, "server_dep\\silkroad\\textdata\\textquest.txt", quest, error), error);
    CHECK_MESSAGE(error.empty(), error);
    MESSAGE(quest.size() << " quest and NPC texts");
    CHECK(quest.size() > 1000);
}

TEST_CASE("the title intro scripts decode to camera tracks over existing regions") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);
    std::size_t scripts = 0;
    for (const std::string& path : fs.ListFiles()) {
        const std::string key = sr::core::NormalizeAssetPath(path);
        if (!key.starts_with("script\\intro\\") || !key.ends_with(".txt")) continue;
        auto data = fs.Read(path);
        std::vector<CameraKey> keys;
        CAPTURE(path);
        if (!DecodeIntroCamera(sr::core::DecodeTextFile(data->Bytes()), keys, error)) {
            MESSAGE(path << ": " << error); // not every intro script needs a camera
            continue;
        }
        ++scripts;
        MESSAGE(path << ": " << keys.size() << " camera keys, sector " << keys.front().sectorY << "\\"
                     << keys.front().sectorX);
        CHECK(fs.Exists(RegionFile(RegionId(keys.front().sectorX, keys.front().sectorY), ".m")));
    }
    CHECK(fs.Exists("script\\intro\\constantinople.txt"));
    CHECK(scripts >= 1);
}

TEST_CASE("every quest scene script decodes; the Roc's entrance has its camera paths, its Roc and its end") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);
    CharacterTable characters;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    std::size_t scripts = 0;
    for (const std::string& path : fs.ListFiles()) {
        const std::string key = sr::core::NormalizeAssetPath(path);
        if (!key.starts_with("script\\q_script\\") || !key.ends_with(".txt")) continue;
        CAPTURE(path);
        auto data = fs.Read(path);
        REQUIRE(data);
        std::vector<SceneCommand> commands;
        REQUIRE_MESSAGE(DecodeSceneScript(sr::core::DecodeTextFile(data->Bytes()), commands, error), error);
        ++scripts;
        // Every camera key reads, every object a character of characterdata, and it ends.
        bool ends = false;
        for (const SceneCommand& c : commands) {
            CameraKey k;
            if (c.verb == "S_CameraInsert") CHECK(SceneCameraKey(c, true, k));
            if (c.verb == "S_CameraTeleport") CHECK(SceneCameraKey(c, false, k));
            if (c.verb == "S_ObjectCreate") CHECK_MESSAGE(characters.Find(std::string_view(c.args.at(1))) != nullptr, c.args.at(1));
            ends |= c.verb == "S_End";
        }
        CHECK(ends);
    }
    CHECK(scripts == 10);
    auto roc = fs.Read("script\\q_script\\roc_entrance.txt");
    REQUIRE(roc);
    std::vector<SceneCommand> commands;
    REQUIRE(DecodeSceneScript(sr::core::DecodeTextFile(roc->Bytes()), commands, error));
    std::size_t inserts = 0, starts = 0;
    for (const SceneCommand& c : commands) {
        inserts += c.verb == "S_CameraInsert";
        starts += c.verb == "S_CameraStart";
    }
    CHECK(inserts == 18);
    CHECK(starts == 6);
    CHECK(commands.front().verb == "S_Start");
    const auto end = std::find_if(commands.begin(), commands.end(), [](const SceneCommand& c) { return c.verb == "S_End"; });
    REQUIRE(end != commands.end());
    CHECK(end->time == doctest::Approx(41.0f));
    const auto sound = std::find_if(commands.begin(), commands.end(), [](const SceneCommand& c) { return c.verb == "S_SoundPlay"; });
    REQUIRE(sound != commands.end());
    CHECK(sound->args.at(0) == "Prim\\Snd\\Mob_Roc\\roc_entrance.ogg");
}

TEST_CASE("character creation models resolve through characterdata") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);
    CharacterTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    MESSAGE(table.size() << " characterdata rows");
    // The 13 figures per race and gender the creation screen offers.
    const char* figures[4][13] = {
        {"NOBLEBOY", "SCHOLAR", "PERFORMER", "MERCHANT", "WARRIOR", "MONK", "ADVENTURER", "FIGHTER", "NECROMANCER",
         "PRIEST", "BOGY", "MONKEY", "TATTOO"},
        {"NOBLEGIRL", "SCHOLAR", "KISAENG", "MERCHANT", "ASSASSIN", "WARRIOR", "ADVENTURER", "FIGHTER", "NECROMENCERW",
         "NECROMENCERB", "FOX", "BOGY", "KANGSI"},
        {"NOBLE", "EXORCIST", "NECROMENCER", "MERCHANT", "PRIEST", "KNIGHT", "WARRIOR", "GLADIATOR", "BARBARIAN",
         "ADVENTURER", "ANGEL", "DEVIL", "WEREWOLF"},
        {"NOBLE", "WITCH", "SUMMONER", "MERCHANT", "ORACLE", "CRUSADER", "AMAZONESS", "KNIGHT", "ADVENTURER",
         "GLADIATOR", "ANGEL", "DEVIL", "SUCCUBUS"}};
    const char* prefixes[4] = {"CHAR_CH_MAN_", "CHAR_CH_WOMAN_", "CHAR_EU_MAN_", "CHAR_EU_WOMAN_"};
    for (int set = 0; set < 4; ++set) {
        for (const char* figure : figures[set]) {
            const std::string codename = std::string(prefixes[set]) + figure;
            CAPTURE(codename);
            const ObjectRow* row = table.Find(codename);
            REQUIRE(row != nullptr);
            CHECK(!row->modelPath.empty());
            CHECK(!CharacterTable::ResolveModel(fs, *row).empty());
            // Player characters: TypeID 1.1 (the spawn decoder classifies by it), found by id too.
            CHECK(row->typeId[0] == 1);
            CHECK(row->typeId[1] == 1);
            CHECK(table.Find(row->refObjId) == row);
        }
    }
}

TEST_CASE("cover tables and starter equipment of the creatable characters") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);
    CharacterTable characters;
    ItemTable items;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    REQUIRE_MESSAGE(items.Load(fs, error), error);
    MESSAGE(items.size() << " itemdata rows");

    // Cover table of every player body.
    for (const char* prefix : {"CHAR_CH_MAN_", "CHAR_CH_WOMAN_", "CHAR_EU_MAN_", "CHAR_EU_WOMAN_"}) {
        const std::string codename = std::string(prefix) + "ADVENTURER";
        CAPTURE(codename);
        const ObjectRow* row = characters.Find(codename);
        REQUIRE(row != nullptr);
        const std::string path = CharacterTable::ResolveModel(fs, *row);
        auto file = fs.Read(path);
        REQUIRE(file);
        ResourceData resource;
        REQUIRE(DecodeBsr(file->Bytes(), resource, error));
        PartLink link;
        CHECK(FindPartLink(file->Bytes(), true, resource.meshPaths.size(), link));
        std::string meshes;
        for (std::size_t i = 0; i < resource.meshPaths.size(); ++i) meshes += "\n  " + std::to_string(i) + " " + resource.meshPaths[i];
        std::string pairs;
        for (const auto& pair : link.pairs) pairs += " " + std::to_string(pair[0]) + "->" + std::to_string(pair[1]);
        MESSAGE(codename << " " << path << " cover:" << pairs << meshes);
    }

    // Starter items (the _DEF rows creation grants) and their cover tables.
    std::size_t resolved = 0, withLink = 0, total = 0;
    for (const char* race : {"CH", "EU"}) {
        for (const char* sex : {"M", "W"}) {
            for (const char* armour : {"HEAVY", "LIGHT", "CLOTHES"}) {
                for (const char* part : {"BA", "LA", "FA"}) {
                    const std::string codename = std::string("ITEM_") + race + "_" + sex + "_" + armour + "_01_" + part + "_A_DEF";
                    const ItemRow* row = items.Find(codename);
                    if (row == nullptr) continue; // not every race has every armour kind
                    ++total;
                    const std::string path = items.ResolveModel(fs, *row);
                    CAPTURE(codename);
                    CHECK(!path.empty());
                    if (path.empty()) continue;
                    ++resolved;
                    auto file = fs.Read(path);
                    ResourceData resource;
                    PartLink link;
                    if (file && DecodeBsr(file->Bytes(), resource, error) &&
                        FindPartLink(file->Bytes(), false, resource.meshPaths.size(), link)) {
                        ++withLink;
                    } else {
                        MESSAGE(codename << " " << path << ": no item cover table");
                    }
                }
            }
        }
    }
    MESSAGE(resolved << "/" << total << " starter armour pieces resolve, " << withLink << " with a cover table");
    CHECK(total > 0);
    for (const char* weapon : {"ITEM_CH_SWORD_01_A_DEF", "ITEM_CH_BOW_01_A_DEF", "ITEM_EU_SWORD_01_A_DEF",
                               "ITEM_EU_CROSSBOW_01_A_DEF", "ITEM_CH_SHIELD_01_A_DEF", "ITEM_EU_SHIELD_01_A_DEF"}) {
        CAPTURE(weapon);
        const ItemRow* row = items.Find(weapon);
        REQUIRE(row != nullptr);
        const std::string path = items.ResolveModel(fs, *row);
        CHECK(!path.empty());
        ModelData model;
        if (!path.empty() && LoadModel(fs, path, model, error)) {
            MESSAGE(weapon << " " << path << " attach bone '" << model.resource.skeletonAttachBone << "', "
                           << model.skeleton.bones.size() << " own bones");
        }
    }

    // Item cover tables over the whole corpus: how many item .bsr files have one.
    std::size_t itemBsr = 0, itemLinks = 0;
    for (const std::string& path : fs.ListFiles()) {
        const std::string key = sr::core::NormalizeAssetPath(path);
        if (!key.starts_with("res\\item\\") || !key.ends_with(".bsr")) continue;
        ++itemBsr;
        auto file = fs.Read(path);
        ResourceData resource;
        PartLink link;
        if (file && DecodeBsr(file->Bytes(), resource, error) &&
            FindPartLink(file->Bytes(), false, resource.meshPaths.size(), link)) {
            ++itemLinks;
        }
    }
    MESSAGE(itemLinks << "/" << itemBsr << " item .bsr files have a cover table");
}

// Diagnostics for dressing characters: run with
//   sr_tests -tc="outfit report*" -s
// and read the MESSAGE lines (body meshes and cover keys, each item's cover table,
// attach bone and how every item bone was bound, which body meshes got hidden).
TEST_CASE("the player figures' animation sets carry the states the game plays") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);
    CharacterTable characters;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    // The stage plays a figure's states by their clips: PICK (38) is in the default set (weapon
    // stances fall back to it), as STAND1 (0) and RUN (7) are.
    for (const char* body : {"CHAR_CH_MAN_ADVENTURER", "CHAR_CH_WOMAN_ADVENTURER", "CHAR_EU_MAN_NOBLE", "CHAR_EU_WOMAN_NOBLE"}) {
        const ObjectRow* row = characters.Find(body);
        REQUIRE(row != nullptr);
        const auto file = fs.Read(CharacterTable::ResolveModel(fs, *row));
        REQUIRE(file);
        ResourceData resource;
        REQUIRE_MESSAGE(DecodeBsr(file->Bytes(), resource, error), error);
        for (const std::uint32_t state : {0u, 7u, 38u}) {
            CHECK_MESSAGE(!resource.StateAnimation("default", state).empty(), std::string(body) << ": no clip for state " << state);
        }
    }
}

TEST_CASE("outfit report for a few creation outfits") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);
    CharacterTable characters;
    ItemTable items;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    REQUIRE_MESSAGE(items.Load(fs, error), error);

    struct Outfit {
        const char* body;
        std::vector<const char*> items;
    };
    const Outfit outfits[] = {
        {"CHAR_CH_WOMAN_ADVENTURER",
         {"ITEM_CH_W_HEAVY_01_BA_A_DEF", "ITEM_CH_W_HEAVY_01_LA_A_DEF", "ITEM_CH_W_HEAVY_01_FA_A_DEF",
          "ITEM_CH_SWORD_01_A_DEF", "ITEM_CH_SHIELD_01_A_DEF"}},
        {"CHAR_CH_WOMAN_ADVENTURER", {"ITEM_CH_W_CLOTHES_01_BA_A_DEF", "ITEM_CH_W_CLOTHES_01_LA_A_DEF",
                                      "ITEM_CH_W_CLOTHES_01_FA_A_DEF", "ITEM_CH_SPEAR_01_A_DEF"}},
        {"CHAR_EU_MAN_NOBLE", {"ITEM_EU_M_HEAVY_01_BA_A_DEF", "ITEM_EU_M_HEAVY_01_LA_A_DEF",
                               "ITEM_EU_M_HEAVY_01_FA_A_DEF", "ITEM_EU_SWORD_01_A_DEF", "ITEM_EU_SHIELD_01_A_DEF"}},
        // A garment's hands (cover mode 2) over the forearm (the user's report 2026-11-22).
        {"CHAR_CH_WOMAN_KANGSI", {"ITEM_CH_W_CLOTHES_01_BA_A", "ITEM_CH_W_CLOTHES_02_AA_A"}},
    };
    for (const Outfit& outfit : outfits) {
        const ObjectRow* row = characters.Find(outfit.body);
        REQUIRE(row != nullptr);
        const std::string bodyPath = CharacterTable::ResolveModel(fs, *row);
        std::string text = std::string("\n==== ") + outfit.body + " " + bodyPath + "\n";
        if (auto file = fs.Read(bodyPath)) {
            ResourceData resource;
            if (DecodeBsr(file->Bytes(), resource, error)) {
                PartLink link;
                const bool found = FindPartLink(file->Bytes(), true, resource.meshPaths.size(), link);
                text += "body cover table: " + std::string(found ? "" : "NOT FOUND");
                for (const auto& pair : link.pairs) text += " " + std::to_string(pair[0]) + "->" + std::to_string(pair[1]);
                text += "\n";
                for (std::size_t i = 0; i < resource.meshPaths.size(); ++i) {
                    text += "  body mesh " + std::to_string(i) + " " + resource.meshPaths[i] + "\n";
                }
                text += "  stances:";
                for (const auto& set : resource.animationSets) text += " " + set.name;
                text += "\n";
            }
        }
        std::vector<std::string> itemPaths;
        for (const char* codename : outfit.items) {
            const ItemRow* item = items.Find(codename);
            if (item == nullptr) {
                text += std::string("item ") + codename + ": NOT IN itemdata\n";
                continue;
            }
            const std::string path = items.ResolveModel(fs, *item);
            text += std::string("item ") + codename + " tid " + std::to_string(item->typeId[0]) + "." +
                    std::to_string(item->typeId[1]) + "." + std::to_string(item->typeId[2]) + "." +
                    std::to_string(item->typeId[3]) + " -> " + (path.empty() ? "NO MODEL" : path) + "\n";
            if (path.empty()) continue;
            itemPaths.push_back(path);
            if (auto file = fs.Read(path)) {
                ResourceData resource;
                if (DecodeBsr(file->Bytes(), resource, error)) {
                    PartLink link;
                    const bool found = FindPartLink(file->Bytes(), false, resource.meshPaths.size(), link);
                    text += "  cover table: " + std::string(found ? "" : "NOT FOUND") + " a=" + std::to_string(link.a) +
                            " b=" + std::to_string(link.b) + " c=" + std::to_string(link.c) + " keys";
                    for (const auto& pair : link.pairs) text += " " + std::to_string(pair[0]);
                    text += "\n  skeleton " + resource.skeletonPath + ", attach bone '" + resource.skeletonAttachBone + "'\n";
                }
            }
        }
        ModelData model;
        OutfitReport report;
        REQUIRE_MESSAGE(AssembleOutfit(fs, bodyPath, itemPaths, model, report, error), error);
        for (const auto& line : report.bindings) text += "  bind " + line + "\n";
        for (const auto& line : report.hiddenMeshes) text += "  hidden " + line + "\n";
        for (const auto& line : report.warnings) text += "  warning " + line + "\n";
        MESSAGE(text);
    }
}

// Writes every control of the in-game interface layouts (and the textdata
// strings they name) to $SR_UI_DUMP, for checking the HUD against the original.
TEST_CASE("in-game interface layouts dump (SR_UI_DUMP)") {
    const auto dir = GameDir();
    const char* dumpPath = std::getenv("SR_UI_DUMP");
    if (dir.empty() || dumpPath == nullptr) {
        MESSAGE("SR_GAME_DIR or SR_UI_DUMP not set - skipped");
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);
    UiDefines defines;
    if (auto file = fs.Read("config\\define.txt")) defines = DecodeUiDefines(sr::core::DecodeTextFile(file->Bytes()));
    TextCatalog text;
    if (auto file = fs.Read("server_dep\\silkroad\\textdata\\textuisystem.txt")) {
        DecodeTextCatalog(sr::core::DecodeTextFile(file->Bytes()), text, error);
    }
    std::FILE* out = std::fopen(dumpPath, "w");
    REQUIRE(out != nullptr);
    const char* files[] = {"ginterface",   "ifunderbar",    "ifplayerminiinfo", "ifsystemwnd", "ifmainpopup",
                           "ifplayerinfo_trijob2", "ifinventory", "ifequipment", "ifskill", "ifquest",
                           "ifchatviewer", "ifsystemmessage", "ifminimap", "iftargetwindow", "ifextquickslot",
                           "ifoption", "ifmessagebox"};
    for (const char* name : files) {
        const std::string path = std::string("resinfo\\") + name + ".txt";
        auto file = fs.Read(path);
        if (!file) {
            std::fprintf(out, "== %s: missing\n", path.c_str());
            continue;
        }
        UiLayout layout;
        if (!DecodeUiLayout(sr::core::DecodeTextFile(file->Bytes()), defines, layout, error)) {
            std::fprintf(out, "== %s: %s\n", path.c_str(), error.c_str());
            continue;
        }
        std::fprintf(out, "== %s\n", path.c_str());
        for (const UiLayoutSection& section : layout.sections) {
            std::fprintf(out, "Section %s (%g,%g,%g,%g) style %d\n", section.name.c_str(), section.rect.x, section.rect.y,
                         section.rect.w, section.rect.h, section.style);
            for (const UiLayoutNode& n : section.nodes) {
                const auto t = text.find(n.text);
                std::fprintf(out, "  %s:%s id %d rect %g,%g,%g,%g", n.name.c_str(), n.type.c_str(), n.id, n.rect.x,
                             n.rect.y, n.rect.w, n.rect.h);
                if (n.hasClientRect) {
                    std::fprintf(out, " client %g,%g,%g,%g", n.clientRect.x, n.clientRect.y, n.clientRect.w,
                                 n.clientRect.h);
                }
                if (!n.ddj.empty()) std::fprintf(out, " ddj %s", n.ddj.c_str());
                if (n.uv[0] != 0 || n.uv[1] != 0 || n.uv[2] != 1 || n.uv[3] != 1) {
                    std::fprintf(out, " uv %g,%g,%g,%g", n.uv[0], n.uv[1], n.uv[2], n.uv[3]);
                }
                if (!n.text.empty()) {
                    std::fprintf(out, " text %s", n.text.c_str());
                    if (t != text.end()) std::fprintf(out, " \"%s\"", t->second.c_str());
                }
                std::fprintf(out, " font %d align %d/%d", n.fontIndex, n.hAlign, n.vAlign);
                if (n.hasFontColor) {
                    std::fprintf(out, " color %d,%d,%d,%d", n.fontColor.a, n.fontColor.r, n.fontColor.g, n.fontColor.b);
                }
                if (n.style != 0) std::fprintf(out, " style %d", n.style);
                if (!n.subSection.empty()) std::fprintf(out, " sub %s", n.subSection.c_str());
                std::fprintf(out, "\n");
            }
        }
    }
    std::fclose(out);
    MESSAGE("wrote " << std::string(dumpPath));
}

TEST_CASE("skills, masteries and skill groups load") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);
    SkillTable skills;
    REQUIRE_MESSAGE(skills.Load(fs, error), error);
    CHECK(skills.size() > 1000);
    CHECK(skills.masteries().size() >= 13);
    std::size_t onBoards = 0;
    for (const MasteryRow& m : skills.masteries()) {
        const auto board = skills.Board(m.id);
        onBoards += board.size();
        MESSAGE("mastery " << m.id << " (" << m.nameKey << ", tab " << m.tab << "): " << board.size() << " skills on the board");
    }
    CHECK(onBoards > 100);
    MESSAGE(skills.size() << " skills");

    // The tooltip's numbers for expendables: a scroll's skill by its codename, the params and limits.
    const SkillRow* speed = skills.Find(std::string_view("SKILL_ETC_SPEED_UP_BASIC_01"));
    REQUIRE(speed != nullptr);
    CHECK(speed->speedPercent == 100);
    ItemTable items;
    REQUIRE_MESSAGE(items.Load(fs, error), error);
    const ItemRow* scroll = items.Find(std::string_view("ITEM_ETC_SPEED_UP_BASIC"));
    REQUIRE(scroll != nullptr);
    CHECK(scroll->paramTexts[0] == "SKILL_ETC_SPEED_UP_BASIC_01");
    CHECK(scroll->maxPossess == -1);
    const ItemRow* ret = items.Find(std::string_view("ITEM_ETC_SCROLL_RETURN_01"));
    REQUIRE(ret != nullptr);
    CHECK(ret->params[0] == 30000);
    CHECK(ret->paramTexts[2] == "RESURRECT");
    const ItemRow* cure = items.Find(std::string_view("ITEM_ETC_CURE_ALL_01"));
    REQUIRE(cure != nullptr);
    CHECK(cure->params[5] == 36);
}

TEST_CASE("every encrypted skill shard skilldataenc.txt lists decrypts to the client's rows (the socket stones' skills among them)") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);
    const std::vector<std::string> shards = ListedShards(fs, "skilldataenc.txt");
    REQUIRE(shards.size() == 7);
    std::size_t rows = 0;
    for (const std::string& path : shards) {
        CAPTURE(path);
        const auto file = fs.Read(path);
        REQUIRE(file);
        std::vector<std::byte> bytes(file->Bytes().begin(), file->Bytes().end());
        REQUIRE(DecryptTextTable(bytes));
        // UTF-16LE with its BOM, every row its 118 columns.
        REQUIRE(bytes.size() > 2);
        CHECK(std::to_integer<int>(bytes[0]) == 0xFF);
        CHECK(std::to_integer<int>(bytes[1]) == 0xFE);
        std::vector<SkillRow> decoded;
        REQUIRE(DecodeSkillRows(sr::core::DecodeTextFile(bytes), decoded, error));
        rows += decoded.size();
    }
    // A plain table (no marker at its end: the older plain shard, the server's rebuilt ones) is not touched.
    const auto plain = fs.Read(std::string("server_dep") + char(92) + "silkroad" + char(92) + "textdata" + char(92) + "skilldata_20000.txt");
    REQUIRE(plain);
    std::vector<std::byte> untouched(plain->Bytes().begin(), plain->Bytes().end());
    const std::vector<std::byte> before = untouched;
    CHECK_FALSE(DecryptTextTable(untouched));
    CHECK(untouched == before);
    MESSAGE(rows << " skill rows in the encrypted shards");
    CHECK(rows == 31400);
    // The skill table: the encrypted shards, as the 1.188 client reads them: every one of their rows, the socket stones'
    // 48 in service, the F grade's 20 and the board skills the plain copy lacks.
    SkillTable skills;
    REQUIRE_MESSAGE(skills.Load(fs, error), error);
    MESSAGE(skills.size() << " skills");
    CHECK(skills.size() == 31400);
    for (std::uint32_t id = 31203; id <= 31250; ++id) CHECK(skills.Find(id) != nullptr);
    for (std::uint32_t id = 33079; id <= 33098; ++id) CHECK(skills.Find(id) != nullptr);
    CHECK(skills.Find(std::uint32_t(31232))->codename == "SKILL_ITEM_ETC_SOCKET_STONE_HEAL_E_01");
    CHECK(skills.Find(std::string_view("SKILL_CH_SWORD_PASSIVE_A_11")) != nullptr);
}

TEST_CASE("every skill's parameter tail walks to its end with the 1.188 parser's tag lengths") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);
    SkillTable skills;
    REQUIRE_MESSAGE(skills.Load(fs, error), error);
    // Every row: the tags, stepped by their lengths, end exactly at the tail's end or at 'ssou'
    // (the 1.188 parser stops there); never at a code the parser does not know.
    std::size_t rows = 0, unknown = 0, tags = 0;
    std::map<std::uint32_t, std::size_t> perTag;
    for (const auto& [id, row] : skills.rows()) {
        ++rows;
        const std::vector<std::uint32_t>& p = row.params;
        for (std::size_t i = 0; i < p.size();) {
            if (p[i] == 0) {
                ++i;
                continue;
            }
            const int length = sr::assets::SkillParamLength(p[i]);
            if (length <= 0) {
                if (++unknown <= 5) MESSAGE(row.codename << ": unknown code " << p[i] << " at " << i);
                break;
            }
            ++tags;
            ++perTag[p[i]];
            if (p[i] == sr::assets::SkillTag("ssou")) break;
            i += static_cast<std::size_t>(length);
        }
    }
    MESSAGE(rows << " skills, " << tags << " tags of " << perTag.size() << " kinds");
    CHECK(rows > 20000);
    CHECK(unknown == 0);
    CHECK(perTag.size() > 100);
    // A row read back: Anti Devil Bow - Missile's first level shoots for 16 ~ 22 at 50% (its 'att').
    const SkillRow* bow = skills.Find(std::string_view("SKILL_CH_BOW_CHAIN_A_01"));
    REQUIRE(bow != nullptr);
    const auto att = sr::assets::FindSkillParam(*bow, sr::assets::SkillTag("att"));
    REQUIRE(att);
    CHECK(att[0] == 6);
    CHECK(att[1] == 50);
    CHECK(att[2] == 16);
    CHECK(att[3] == 22);
    CHECK_FALSE(sr::assets::FindSkillParam(*bow, sr::assets::SkillTag("dura")));
}

TEST_CASE("the skills catalogue: every board skill group does something known") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);
    SkillTable skills;
    REQUIRE_MESSAGE(skills.Load(fs, error), error);
    SkillAnimationTable animations;
    REQUIRE_MESSAGE(animations.Load(fs, error), error);
    CharacterTable characters;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    // The players' bodies by race (0 China, 1 Europe), their animation sets by stance.
    std::array<std::vector<ResourceData>, 2> bodies;
    for (const auto& [code, race] : {std::pair{"CHAR_CH_MAN_ADVENTURER", 0}, {"CHAR_CH_WOMAN_ADVENTURER", 0}, {"CHAR_EU_MAN_NOBLE", 1}, {"CHAR_EU_WOMAN_NOBLE", 1}}) {
        const ObjectRow* row = characters.Find(std::string_view(code));
        REQUIRE(row != nullptr);
        ModelData model;
        REQUIRE_MESSAGE(LoadModel(fs, CharacterTable::ResolveModel(fs, *row), model, error), error);
        ResourceData sets;
        sets.animationPaths = model.animationPaths;
        sets.animationSets = model.animationSets;
        bodies[std::size_t(race)].push_back(std::move(sets));
    }
    // Each group once, by its first level, as the boards show them.
    std::map<std::string, int> kinds;
    std::size_t groups = 0, unknown = 0, noAnimation = 0, clipsChecked = 0;
    std::map<std::string, int> missingClips; // "stance state" -> skills
    for (const MasteryRow& m : skills.masteries()) {
        for (const SkillRow* s : skills.Board(m.id)) {
            if (!s->inService) continue;
            ++groups;
            const sr::game::SkillTraits t = sr::game::TraitsOf(*s);
            if (!t.Any()) {
                if (++unknown <= 10) MESSAGE("no known kind: " << s->codename);
                continue;
            }
            const std::pair<bool, const char*> flags[] = {
                {t.passive, "passive"},   {t.attack && !t.area && !t.chain, "attack (one target)"}, {t.attack && t.area, "attack (area)"},
                {t.attack && t.chain, "attack (chain)"}, {t.states, "gives states"}, {t.imbue, "imbue"}, {t.buff, "buff"}, {t.debuff, "debuff"},
                {t.toggle, "toggle"},     {t.heal, "heal"},      {t.cure, "cure"},          {t.resurrect, "resurrect"},
                {t.harmony, "harmony"},   {t.taunt, "taunt"},    {t.stealth, "stealth"},    {t.detect, "detect"},
                {t.teleport, "teleport"}, {t.transform, "transform"}, {t.summon, "summon"}, {t.trap, "trap"},
                {t.link, "link"},         {t.reflect, "absorb / reflect"}};
            for (const auto& [on, name] : flags) kinds[name] += on ? 1 : 0;
            // An active skill has its animation row (its base name in skilleffect.txt).
            const SkillAnimation* anim = t.passive ? nullptr : animations.Find(s->baseName);
            if (!t.passive && anim == nullptr) {
                if (++noAnimation <= 10) MESSAGE("no animation row: " << s->codename << " (" << s->baseName << ")");
            }
            if (anim == nullptr) continue;
            // Its clips in the stance of every weapon it takes (none: the empty-handed set), on each
            // body of its race: what the client plays without falling back to another stance.
            std::vector<std::string> stances;
            for (const int w : s->weaponKinds) {
                if (w != 255) stances.push_back(sr::assets::StanceOfWeaponKind(w));
            }
            // One that takes any weapon (255; the 1.188 rows of the Warrior's one-handed shield skills A and B, 7 in the
            // older plain copy) plays its row's group's set whatever is in hand, as our client does (PlayScreen: skillaniset2
            // col 5); none: the empty-handed set.
            if (stances.empty()) stances.push_back(anim->group.empty() ? std::string("default") : anim->group);
            std::vector<int> states = anim->ready;
            states.insert(states.end(), anim->wait.begin(), anim->wait.end());
            states.insert(states.end(), anim->shot.begin(), anim->shot.end());
            for (const int race : {0, 1}) {
                if (s->race != race && s->race != 3) continue;
                for (const ResourceData& body : bodies[std::size_t(race)]) {
                    for (const std::string& stance : stances) {
                        for (const int state : states) {
                            ++clipsChecked;
                            if (body.StateAnimation(stance, std::uint32_t(state)).empty()) {
                                const bool inDefault = !body.StateAnimation("default", std::uint32_t(state)).empty();
                                const std::string key = stance + " " + std::to_string(state) + (inDefault ? " (in default)" : " (nowhere)");
                                if (missingClips[key]++ == 0) MESSAGE("no clip: " << s->codename << " in " << stance << ", state " << state);
                            }
                        }
                    }
                }
            }
        }
    }
    MESSAGE(groups << " skill groups on the boards; " << clipsChecked << " clips looked up");
    for (const auto& [name, n] : kinds) MESSAGE("  " << name << ": " << n);
    // A state its stance lacks comes from the empty-handed set (the client's fallback; six skills' gestures
    // made without the weapon: the Rogue's stealth change, detection and poison guard, the Bard's dance).
    // A clip found nowhere is a skill shown wrong.
    int nowhere = 0;
    for (const auto& [key, n] : missingClips) {
        MESSAGE("  not in its stance: " << key << ": " << n << " skill bodies");
        nowhere += key.ends_with("(nowhere)") ? 1 : 0;
    }
    CHECK(groups > 400);
    CHECK(unknown == 0);
    CHECK(noAnimation == 0);
    CHECK(nowhere == 0);
}

TEST_CASE("NPC shops: every good is an item, tabs count up per NPC") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);
    ShopTable shops;
    REQUIRE_MESSAGE(shops.Load(fs, error), error);
    ItemTable items;
    REQUIRE_MESSAGE(items.Load(fs, error), error);
    MESSAGE(shops.size() << " NPCs with a shop");
    CHECK(shops.size() > 20);
    std::size_t goods = 0, unknown = 0;
    for (const auto& [npc, shop] : shops.shops()) {
        int expected = 0;
        for (const ShopTabGroup& group : shop.groups) {
            for (const ShopTab& tab : group.tabs) {
                CHECK(tab.index == expected++);
                for (const ShopGood& good : tab.goods) {
                    ++goods;
                    if (items.Find(good.itemCodename) == nullptr) {
                        if (++unknown <= 5) MESSAGE(npc << ": unknown item " << good.itemCodename);
                    }
                }
            }
        }
    }
    MESSAGE(goods << " goods, " << unknown << " not in itemdata");
    CHECK(goods > 500);
    CHECK(unknown == 0);
    // The wolf's flute at every stable, in its Pet tab (ours on top of the 1.188 tables: Shop.cpp kExtraGoods).
    int stables = 0;
    for (const auto& [npc, shop] : shops.shops()) {
        for (const ShopTabGroup& group : shop.groups) {
            for (const ShopTab& tab : group.tabs) {
                if (!tab.code.ends_with("_STABLE_TAB3")) continue;
                ++stables;
                REQUIRE(tab.goods.size() == 1);
                CHECK(tab.goods[0].itemCodename == "ITEM_COS_P_FLUTE");
                CHECK(tab.goods[0].price == 2000000);
            }
        }
    }
    CHECK(stables >= 5);
}

TEST_CASE("the client's cursors come out of sro_client.exe") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    const std::filesystem::path exe = std::filesystem::path(dir) / "sro_client.exe";
    if (!std::filesystem::exists(exe)) {
        MESSAGE("no sro_client.exe in the game folder - skipped");
        return;
    }
    std::FILE* f = std::fopen(exe.string().c_str(), "rb");
    REQUIRE(f != nullptr);
    std::vector<std::byte> bytes(static_cast<std::size_t>(std::filesystem::file_size(exe)));
    const std::size_t read = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    REQUIRE(read == bytes.size());
    std::vector<sr::assets::CursorImage> cursors;
    std::string error;
    REQUIRE_MESSAGE(sr::assets::DecodeExeCursors(bytes, cursors, error), error);
    // 0xAE, 0xAF: the Socket page's pick modes (making, removing).
    for (const std::uint16_t id : {0x95, 0x97, 0x98, 0x99, 0xAE, 0xAF}) {
        CAPTURE(id);
        const auto it = std::find_if(cursors.begin(), cursors.end(), [&](const auto& c) { return c.id == id; });
        REQUIRE(it != cursors.end());
        CHECK(it->width == 32);
        CHECK(it->rgba.size() == static_cast<std::size_t>(it->width * it->height * 4));
    }
    MESSAGE(cursors.size() << " cursors");
}

TEST_CASE("every .efp effect decodes; skill effect stages load") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    sr::assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(sr::assets::MountGameArchives(fs, dir, error), error);
    int ok = 0, failed = 0, oldLayout = 0;
    for (const std::string& path : fs.ListFiles()) {
        if (!sr::core::NormalizeAssetPath(path).ends_with(".efp")) continue;
        auto data = fs.Read(path);
        REQUIRE(data.has_value());
        sr::assets::EffectData effect;
        if (sr::assets::DecodeEfp(data->Bytes(), effect, error)) {
            ++ok;
        } else if (data->Bytes().size() >= 12 && std::memcmp(data->Bytes().data(), "JMXVEFF 0000", 12) == 0) {
            // The older layout (Effect.cpp: its header measured, its bodies not decoded): 7 files, the
            // Chinese lightning and cold ganggi effects and a sword afterimage.
            ++oldLayout;
        } else if (++failed <= 10) {
            MESSAGE(path << ": " << error);
        }
    }
    MESSAGE(ok << " effects decode, " << oldLayout << " in the old JMXVEFF 0000 layout (not read), " << failed << " fail");
    CHECK(oldLayout <= 7);
    CHECK(ok > 0);
    CHECK(failed == 0);

    sr::assets::SkillAnimationTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    MESSAGE(table.size() << " skill animation rows");

    // .bsr modifier sets: every one is read; count the attached effects and glows.
    int unread = 0, withParticles = 0, withMaterials = 0, missingEffects = 0;
    for (const std::string& path : fs.ListFiles()) {
        if (!sr::core::NormalizeAssetPath(path).ends_with(".bsr")) continue;
        auto data = fs.Read(path);
        sr::assets::ResourceData resource;
        if (!data || !sr::assets::DecodeBsr(data->Bytes(), resource, error)) continue;
        if (resource.modifiersUnread && ++unread <= 10) MESSAGE(path << ": modifiers not read");
        bool particles = false, materials = false;
        for (const auto& set : resource.modifierSets) {
            particles = particles || !set.particles.empty();
            materials = materials || !set.materials.empty();
            for (const auto& p : set.particles) {
                if (!fs.Exists(p.effect) && ++missingEffects <= 5) MESSAGE(path << ": no effect " << p.effect);
            }
        }
        withParticles += particles;
        withMaterials += materials;
    }
    MESSAGE(withParticles << " resources attach effects, " << withMaterials << " animate materials, " << missingEffects
                          << " attached effects missing");
    CHECK(unread == 0);
}

TEST_CASE("clip sound tracks: which clips name their sounds, at which moments") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    sr::assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(sr::assets::MountGameArchives(fs, dir, error), error);
    std::map<std::string, int> groups, handles, keys;
    int sets = 0, events = 0, namedByPath = 0, unnamed = 0, missingWave = 0;
    const auto lower = [](std::string s) {
        for (char& c : s) c = char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        return s;
    };
    for (const std::string& path : fs.ListFiles()) {
        if (!sr::core::NormalizeAssetPath(path).ends_with(".bsr")) continue;
        auto data = fs.Read(path);
        sr::assets::ResourceData resource;
        if (!data || !sr::assets::DecodeBsr(data->Bytes(), resource, error)) continue;
        std::set<std::string> clips;
        for (const std::string& a : resource.animationPaths) {
            std::string name = lower(a);
            if (const auto slash = name.find_last_of("\\/"); slash != std::string::npos) name = name.substr(slash + 1);
            if (const auto dot = name.rfind('.'); dot != std::string::npos) name = name.substr(0, dot);
            clips.insert(name);
        }
        for (const auto& set : resource.modifierSets) {
            if (set.sounds.empty()) continue;
            ++sets;
            ++keys["kind " + std::to_string(set.kind) + (set.state < 0 ? " no state" : " a state")];
            if (clips.contains(lower(set.animationSet))) {
                ++namedByPath;
            } else if (++unnamed <= 8) {
                MESSAGE(path << ": no clip file called " << set.animationSet);
            }
            for (const auto& track : set.sounds) {
                ++groups[lower(track.group)];
                for (const auto& e : track.events) {
                    ++events;
                    ++handles[lower(e.handle)];
                    if (!e.wave.empty() && !fs.Exists(e.wave) && ++missingWave <= 5) MESSAGE(path << ": no wave " << e.wave);
                }
            }
        }
    }
    std::string text;
    for (const auto& [k, n] : keys) text += k + " = " + std::to_string(n) + "; ";
    MESSAGE(sets << " clips with sounds, " << events << " events; by clip file name " << namedByPath << ", not " << unnamed << "; " << text);
    text.clear();
    for (const auto& [k, n] : groups) text += k + " = " + std::to_string(n) + "; ";
    MESSAGE("groups: " << text);
    text.clear();
    for (const auto& [k, n] : handles) text += k + " = " + std::to_string(n) + "; ";
    MESSAGE("handles: " << text);
    CHECK(events > 0);
    // A Chinese man's first sword swing and his bow's: what a skill's clip says it sounds like, and when.
    auto data = fs.Read("res\\char\\china\\chinaman_adventurer.bsr");
    sr::assets::ResourceData man;
    REQUIRE(data);
    REQUIRE_MESSAGE(sr::assets::DecodeBsr(data->Bytes(), man, error), error);
    const auto sword = man.StateSounds("sword", 2);
    REQUIRE(sword.size() == 4);
    CHECK(sword[0].ms == 185);
    CHECK(sword[0].handle == "snd_swing1");
    CHECK(sword.back().ms == 561);
    const auto bow = man.StateSounds("bow", 2);
    REQUIRE(bow.size() == 1);
    CHECK(bow[0].ms == 441);
    CHECK(bow[0].handle == "snd_swing2");
    CHECK(man.StateSounds("sword", 400).empty());
}

TEST_CASE("ambient modifiers: every texture animation scrolls; the fountains' water scrolls, the waterfalls blend") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    sr::assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(sr::assets::MountGameArchives(fs, dir, error), error);
    // The corpus rule AmbientLook rests on: every texture animation is in an always-on
    // set and scrolls (no flip-books); material modifiers that blend are counted by kind.
    int scrolling = 0, otherTexture = 0, translucent = 0, blendedEdges = 0;
    for (const std::string& path : fs.ListFiles()) {
        if (!sr::core::NormalizeAssetPath(path).ends_with(".bsr")) continue;
        auto data = fs.Read(path);
        sr::assets::ResourceData resource;
        if (!data || !sr::assets::DecodeBsr(data->Bytes(), resource, error)) continue;
        for (const auto& set : resource.modifierSets) {
            for (const auto& t : set.textures) {
                if (set.kind == 2 && t.words[1] == 1) {
                    ++scrolling;
                } else if (++otherTexture <= 5) {
                    MESSAGE(path << ": texture animation of kind " << set.kind << ", mode " << t.words[1]);
                }
            }
            if (set.kind != 2) continue;
            for (const auto& m : set.materials) {
                if (m.enables[1] == 0) continue;
                if (m.enables[0] != 0 && m.enables[2] == 0) ++blendedEdges;
                else ++translucent;
            }
        }
    }
    MESSAGE(scrolling << " scrolling textures; blending material modifiers: " << translucent << " translucent, " << blendedEdges
                      << " alpha-tested with depth writes (drawn cut out)");
    CHECK(scrolling == 162);
    // One texture animation in an animation state's set (mob\god\flame_adjutant.bsr, kind 1, mode 0):
    // not drawn yet (AmbientLook takes the always-on sets only).
    CHECK(otherTexture == 1);
    CHECK(translucent > 50);

    const auto partNamed = [](const sr::assets::ModelData& model, std::string_view material) -> const sr::assets::ModelPart* {
        for (const auto& part : model.parts) {
            if (part.material.name == material) return &part;
        }
        return nullptr;
    };
    // The fountain in the middle of Hotan: its water (material 3) scrolls along v, 0.48 a second, solid.
    sr::assets::ModelData hotan;
    REQUIRE_MESSAGE(sr::assets::LoadModel(fs, "res\\bldg\\oasis\\hotang\\oas_hot_portal.bsr", hotan, error), error);
    const auto* water = partNamed(hotan, "Oas_hot_potal_water");
    REQUIRE(water != nullptr);
    CHECK(water->materialIndex == 3);
    CHECK(water->ambient.scrolls);
    CHECK(water->ambient.uvRate[5] == doctest::Approx(0.48f));
    CHECK(water->ambient.uvRate[4] == 0.0f);
    CHECK_FALSE(water->ambient.blended);
    const auto* wall = partNamed(hotan, "oas_hot_potal_wall");
    REQUIRE(wall != nullptr);
    CHECK_FALSE(wall->ambient.scrolls);
    // Constantinople's fountain: its centre water turns and stretches (the matrix's 2 x 2 grows).
    sr::assets::ModelData constantinople;
    REQUIRE_MESSAGE(sr::assets::LoadModel(fs, "res\\bldg\\europe\\constantinople\\euro_esteuro_fountain01.bsr", constantinople, error), error);
    const auto* centre = partNamed(constantinople, "euro_est_centerwater");
    REQUIRE(centre != nullptr);
    CHECK(centre->ambient.scrolls);
    CHECK(centre->ambient.uvRate[0] == doctest::Approx(0.02f));
    CHECK(centre->ambient.uvRate[2] == doctest::Approx(-0.02f));
    // A waterfall's line: scrolling, blended additively (source alpha, one), no alpha test.
    sr::assets::ModelData waterline;
    REQUIRE_MESSAGE(sr::assets::LoadModel(fs, "res\\nature\\particle\\waterline01.bsr", waterline, error), error);
    REQUIRE(!waterline.parts.empty());
    const auto& line = waterline.parts.front().ambient;
    CHECK(line.scrolls);
    CHECK(line.uvRate[5] == doctest::Approx(-1.0f));
    CHECK(line.blended);
    CHECK_FALSE(line.alphaTest);
    CHECK(line.sourceBlend == 5);
    CHECK(line.destinationBlend == 2);
    CHECK(line.tint == doctest::Approx(2.0f * 0.4745f).epsilon(0.01)); // its key colour, "modulate 2x"
}

TEST_CASE("every in-service monster and NPC row has a model (clones take their base row's)") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    sr::assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(sr::assets::MountGameArchives(fs, dir, error), error);
    sr::assets::CharacterTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    int withModel = 0, without = 0, missingFile = 0;
    for (const auto& [codename, row] : table.rows()) {
        if (!row.inService || row.typeId[0] != 1 || row.typeId[1] != 2) continue;
        if (row.modelPath.empty()) {
            if (++without <= 10) MESSAGE(codename << ": no model (link '" << row.linkedCodename << "')");
        } else if (sr::assets::CharacterTable::ResolveModel(fs, row).empty()) {
            if (++missingFile <= 10) MESSAGE(codename << ": model file " << row.modelPath << " not found");
        } else {
            ++withModel;
        }
    }
    MESSAGE(withModel << " monster / NPC rows with a model, " << without << " without, " << missingFile << " with a missing file");
    CHECK(withModel > 0);
}

TEST_CASE("every quest row decodes, with its title and objective keys; magic options decode") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    sr::assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(sr::assets::MountGameArchives(fs, dir, error), error);
    sr::assets::QuestTable quests;
    REQUIRE_MESSAGE(quests.Load(fs, error), error);
    const sr::assets::QuestRow* tutorial = quests.Find("QTUTORIAL_CH");
    REQUIRE(tutorial != nullptr);
    CHECK(tutorial->titleKey.starts_with("SN_"));
    auto file = fs.Read("server_dep\\silkroad\\textdata\\magicoption.txt");
    REQUIRE(file);
    std::vector<sr::assets::MagicOptionRow> options;
    REQUIRE_MESSAGE(sr::assets::DecodeMagicOptions(sr::core::DecodeTextFile(file->Bytes()), options, error), error);
    CHECK(options.size() > 100);
    // Every row's Prob and each group's ReqClass beside it (the drops' blues).
    for (const sr::assets::MagicOptionRow& o : options) {
        CHECK(o.reqClasses.size() == o.availGroups.size());
        CHECK(o.prob >= 0.0f);
        CHECK(o.prob <= 1.0f);
    }
    // magicoptionassign.txt: every row a kind and its options, each a magic option's name.
    const auto assignFile = fs.Read("server_dep\\silkroad\\textdata\\magicoptionassign.txt");
    REQUIRE(assignFile);
    std::vector<sr::assets::MagicOptionAssignRow> assign;
    REQUIRE_MESSAGE(sr::assets::DecodeMagicOptionAssign(sr::core::DecodeTextFile(assignFile->Bytes()), assign, error), error);
    CHECK(assign.size() == 74);
    for (const sr::assets::MagicOptionAssignRow& a : assign) {
        CHECK(!a.options.empty());
        for (const std::string& name : a.options) {
            CHECK(std::any_of(options.begin(), options.end(), [&](const sr::assets::MagicOptionRow& o) { return o.optionName == name; }));
        }
    }
}

TEST_CASE("the teleport tables: every gate stands somewhere, every link joins two points") {
    const auto dir = GameDir();
    if (dir.empty()) return;
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);
    sr::assets::TeleportTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    MESSAGE(table.Gates().size() << " gates, " << table.Points().size() << " points, " << table.Links().size() << " links");
    CHECK(table.Gates().size() >= 20);
    int placed = 0;
    for (const auto& g : table.Gates()) {
        if (g.region == 0) continue; // the instance quest's gate stands nowhere
        ++placed;
        if ((g.region & 0x8000) == 0) { // outdoors: inside its region
            CHECK(g.x >= 0.0f);
            CHECK(g.x <= 1920.0f);
            CHECK(g.z >= 0.0f);
            CHECK(g.z <= 1920.0f);
        }
    }
    CHECK(placed >= 20);
    int arrivals = 0;
    for (const auto& p : table.Points()) arrivals += p.HasArrival() ? 1 : 0;
    CHECK(arrivals > 100);
    for (const auto& l : table.Links()) {
        CAPTURE(l.from);
        CAPTURE(l.to);
        CHECK(table.Point(l.from) != nullptr);
        CHECK(table.Point(l.to) != nullptr);
    }
    // Jangan's gate (the pavilion with the stone, region 168,97) leads to Donwhang for 5,000.
    const auto* jangan = table.Gate(2094);
    REQUIRE(jangan != nullptr);
    CHECK(jangan->region == sr::assets::RegionId(168, 97));
    const auto* from = table.PointOf(2094);
    REQUIRE(from != nullptr);
    const auto links = table.LinksFrom(from->id);
    for (const auto* l : links) MESSAGE("Jangan's gate -> " << table.Point(l->to)->codename << " for " << l->fee);
    REQUIRE(links.size() == 3); // 1.188 (v1.150: Donwhang only)
    const auto donwhang = std::find_if(links.begin(), links.end(), [&](const auto* l) { return table.Point(l->to)->codename == "GATE_WC"; });
    REQUIRE(donwhang != links.end());
    CHECK((*donwhang)->fee == 5000);
}

TEST_CASE("worn items: which materials an environment map covers (their alpha a shine mask) and which are cut out") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    sr::assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(sr::assets::MountGameArchives(fs, dir, error), error);
    // The items' environment maps (type 0x40000, ModDataEnvMap): an always-on one covers
    // a material whose alpha is then the shine's mask (no alpha test), or tests only alpha 0 with its flag 2.
    int resources = 0, withEnv = 0, envAll = 0, envOne = 0, envAlphaZero = 0, cutOut = 0, cutOutUnderEnv = 0;
    for (const std::string& path : fs.ListFiles()) {
        const std::string norm = sr::core::NormalizeAssetPath(path);
        if (norm.find("item\\") == std::string::npos || norm.find("\\etc\\") != std::string::npos || !norm.ends_with(".bsr")) continue;
        sr::assets::ModelData model;
        if (!sr::assets::LoadModel(fs, path, model, error)) continue;
        ++resources;
        bool any = false;
        for (const auto& set : model.resource.modifierSets) {
            if (set.kind != 2) continue;
            for (const auto& e : set.envMaps) {
                any = true;
                (e.material == 0xFFFFFFFFu ? envAll : envOne) += 1;
                if ((e.alphaFlags & 2) != 0) ++envAlphaZero;
            }
        }
        if (any) ++withEnv;
        for (const auto& part : model.parts) {
            if (!part.hasMaterial || (part.material.flags & 0x200u) == 0 || (part.material.flags & 0x30000000u) != 0) continue;
            ++cutOut;
            if (part.ambient.envMap) ++cutOutUnderEnv;
        }
    }
    MESSAGE(resources << " item resources, " << withEnv << " with an always-on environment map (" << envAll << " on every material, "
                      << envOne << " on one; " << envAlphaZero << " testing alpha 0); " << cutOut << " parts flagged cut out (0x200), "
                      << cutOutUnderEnv << " of them under an environment map");
    CHECK(resources == 1747);
    CHECK(withEnv == 1348);
    CHECK(envAll == 1305);
    CHECK(envOne == 43);
    CHECK(envAlphaZero == 591);
    CHECK(cutOut == 1112);
    CHECK(cutOutUnderEnv == 851);

    // The Seal of Sun bow (degree 10): one material flagged cut out (0x341), under an environment map on every
    // material that tests alpha 0 (its alpha flags 3), which decides: the bow's see-through parts (alpha 0) are cut
    // out, its shine mask (the rest of the alpha) is not.
    sr::assets::ModelData bow;
    REQUIRE_MESSAGE(sr::assets::LoadModel(fs, "res\\item\\china\\weapon\\bow_10.bsr", bow, error), error);
    REQUIRE(bow.parts.size() == 1);
    CHECK(bow.parts[0].material.flags == 0x341u);
    CHECK(bow.parts[0].ambient.envMap);
    CHECK(bow.parts[0].ambient.envAlphaZero);
}

TEST_CASE("the flying arrow (cha_arrow_normal.bsr) lies along z, its tip at the origin and its shaft toward -z") {
    // The bow's and crossbow's shot (skilleffect.txt) flies this
    // model; the game screen turns its length to the flight.
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    sr::assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(sr::assets::MountGameArchives(fs, dir, error), error);
    sr::assets::ModelData model;
    REQUIRE_MESSAGE(sr::assets::LoadModel(fs, "res\\item\\china\\weapon\\cha_arrow_normal.bsr", model, error), error);
    const auto& b = model.bounds;
    MESSAGE("arrow bounds " << b.min[0] << "," << b.min[1] << "," << b.min[2] << " .. " << b.max[0] << "," << b.max[1] << "," << b.max[2]);
    const float x = b.max[0] - b.min[0], y = b.max[1] - b.min[1], z = b.max[2] - b.min[2];
    CHECK(z > 10.0f * x);
    CHECK(z > 10.0f * y);
    CHECK(b.max[2] < 1.0f); // the tip leads: +z is its flight's way
}

TEST_CASE("itemrare.txt: the glow of every rare weapon and shield, on a bone of the item's own model") {
    // the original hangs these on slots 6 and 7's models when the
    // item's rarity is 2; every listed item is such an item, every effect exists, and every bone is in
    // the item model's skeleton (the outfit's sockets: what the game screen anchors them on).
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    sr::assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(sr::assets::MountGameArchives(fs, dir, error), error);
    // itemrare.txt alone (ItemGlowTable::Load adds itemoptionefp.txt: the next case).
    sr::assets::ItemGlowTable glows;
    auto rareFile = fs.Read("resinfo\\itemrare.txt");
    REQUIRE(rareFile);
    REQUIRE_MESSAGE(glows.Decode(sr::core::DecodeTextFile(rareFile->Bytes()), error), error);
    sr::assets::ItemTable items;
    REQUIRE_MESSAGE(items.Load(fs, error), error);
    MESSAGE(glows.lines() << " lines, " << glows.size() << " items");
    CHECK(glows.lines() == 911);
    int unknown = 0, notRare = 0, notWorn = 0, missingEffect = 0, missingBone = 0, checked = 0;
    std::map<std::string, sr::assets::SkeletonData> skeletons;
    for (const auto& [codename, effects] : glows.Items()) {
        const sr::assets::ItemRow* row = items.Find(codename);
        if (row == nullptr) {
            if (++unknown <= 5) MESSAGE(codename << ": not in itemdata");
            continue;
        }
        if (!row->Rare() && ++notRare <= 5) MESSAGE(codename << ": rarity " << row->rarity);
        const int slot = sr::assets::EquipSlot(*row);
        if (slot != 6 && slot != 7 && ++notWorn <= 5) MESSAGE(codename << ": slot " << slot);
        // The item's model(s): an axe or dagger's hands, else its own.
        const std::vector<std::string> models = sr::assets::WornModels(fs, items, *row);
        if (models.empty() && ++missingBone <= 10) MESSAGE(codename << ": no model");
        for (const sr::assets::ItemGlowEffect& e : effects) {
            if (!fs.Exists(e.effect) && ++missingEffect <= 5) MESSAGE(codename << ": no effect " << e.effect);
            CHECK(e.scale > 0.1f);
            for (const std::string& model : models) {
                auto it = skeletons.find(model);
                if (it == skeletons.end()) {
                    sr::assets::ModelData data;
                    std::string ignored;
                    sr::assets::LoadModel(fs, model, data, ignored);
                    it = skeletons.emplace(model, std::move(data.skeleton)).first;
                }
                ++checked;
                if (it->second.Find(e.bone) < 0 && ++missingBone <= 10) MESSAGE(codename << ": " << model << " has no bone " << e.bone);
            }
        }
    }
    // The table also lists items the client's itemdata does not have (degree 12, _HONOR twins and
    // legend weapons: 144); the original skips those as it loads (v1.150's). The rare set
    // items (rarity 6) are listed beside the Seals (rarity 2).
    MESSAGE(checked << " effect x model pairs checked, " << unknown << " items not in itemdata");
    CHECK(unknown == 144);
    CHECK(notRare == 0);
    CHECK(notWorn == 0);
    CHECK(missingEffect == 0);
    CHECK(missingBone == 0);
    CHECK(checked == 821);
}

TEST_CASE("the plus: itemoptionefp.txt's sparks from +8, itemoption.txt's glow types, itemtypenumber.txt's rows") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    sr::assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(sr::assets::MountGameArchives(fs, dir, error), error);
    // The enchantment's sparks: every line of every weapon and shield from +8 to +100.
    auto enchantFile = fs.Read("resinfo\\itemoptionefp.txt");
    REQUIRE(enchantFile);
    sr::assets::ItemGlowTable enchant;
    REQUIRE_MESSAGE(enchant.Add(sr::core::DecodeTextFile(enchantFile->Bytes()), "itemoptionefp.txt", error), error);
    CHECK(enchant.lines() == 1316);
    int wrongRange = 0, missingEffect = 0;
    for (const auto& [codename, effects] : enchant.Items()) {
        for (const auto& e : effects) {
            if (e.minPlus != 8 || e.maxPlus != 100) ++wrongRange;
            if (!fs.Exists(e.effect) && ++missingEffect <= 5) MESSAGE(codename << ": no effect " << e.effect);
        }
    }
    CHECK(wrongRange == 0);
    CHECK(missingEffect == 0);
    // Both files in one table: the Seal of Sun bow keeps its rare glow at any plus and adds the sparks from +8.
    sr::assets::ItemGlowTable all;
    REQUIRE_MESSAGE(all.Load(fs, error), error);
    CHECK(all.lines() == 911 + 1316);
    const auto* bow = all.Find("ITEM_CH_BOW_10_C_RARE");
    REQUIRE(bow != nullptr);
    const auto shown = [&](int plus) { return std::count_if(bow->begin(), bow->end(), [&](const auto& e) { return e.ShowsAt(plus); }); };
    CHECK(shown(9) == shown(0) + 1);
    CHECK(shown(7) == shown(0));
    const auto spark = std::find_if(bow->begin(), bow->end(), [](const auto& e) { return e.minPlus == 8; });
    REQUIRE(spark != bow->end());
    CHECK(spark->effect == "system\\system_enchantbow_a_01.efp");
    CHECK(spark->bone == "Bone01");
    CHECK(spark->scale == doctest::Approx(1.9f));

    // The plus glow: 1,315 weapons and shields of four types, seven rows each (from +3, +5 .. +10).
    sr::assets::EquipGlowTable plus;
    REQUIRE_MESSAGE(plus.Load(fs, error), error);
    CHECK(plus.types() == 1315);
    CHECK(plus.rows() == 28);
    for (const auto& row : plus.Rows()) {
        CHECK(fs.Exists(row.texture));
        CHECK(row.addOp == 7);
        CHECK(row.periodMs == 1000);
    }
    // The +9 Seal of Sun bow (type 2): option_texture14 moving (0.005, -0.005) every 20 ms, yellow to orange over a
    // second, modulate 2x, cut out where its alpha is 0.
    const auto* nine = plus.Find("ITEM_CH_BOW_10_C_RARE", 9);
    REQUIRE(nine != nullptr);
    CHECK(nine->type == 2);
    CHECK(nine->minPlus == 9);
    CHECK(nine->texture == "prim\\mtrl\\itemoption\\option_texture14.ddj");
    CHECK(nine->scroll[0] == doctest::Approx(0.005f));
    CHECK(nine->scroll[1] == doctest::Approx(-0.005f));
    CHECK(nine->colorOp == 5);
    CHECK(nine->from[2] == doctest::Approx(0.51f));
    CHECK(nine->to[0] == doctest::Approx(0.831f));
    CHECK(nine->alphaTest);
    CHECK(plus.Find("ITEM_CH_BOW_10_C_RARE", 0) == nullptr);
    CHECK(plus.Find("ITEM_CH_BOW_10_C_RARE", 2) == nullptr);
    REQUIRE(plus.Find("ITEM_CH_BOW_10_C_RARE", 4) != nullptr);
    CHECK(plus.Find("ITEM_CH_BOW_10_C_RARE", 4)->minPlus == 3);
    REQUIRE(plus.Find("ITEM_CH_BOW_10_C_RARE", 12) != nullptr);
    CHECK(plus.Find("ITEM_CH_BOW_10_C_RARE", 12)->minPlus == 10);

    // Dressed with it: the bow's path carries the glow; at +2 it does not.
    sr::assets::CharacterTable characters;
    sr::assets::ItemTable items;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    REQUIRE_MESSAGE(items.Load(fs, error), error);
    const sr::assets::ItemRow* bowRow = items.Find(std::string_view("ITEM_CH_BOW_10_C_RARE"));
    const sr::assets::ObjectRow* man = characters.Find(std::string_view("CHAR_CH_MAN_ADVENTURER"));
    REQUIRE(bowRow != nullptr);
    REQUIRE(man != nullptr);
    const std::uint32_t worn[] = {bowRow->refObjId};
    const auto dressedAt = [&](std::uint8_t level) {
        sr::assets::DressedCharacter dressed;
        const std::uint8_t pluses[] = {level};
        REQUIRE_MESSAGE(sr::assets::Dress(fs, characters, items, man->refObjId, worn, sr::assets::kNeutralShape, dressed, error, pluses, &plus), error);
        return dressed;
    };
    const auto glowing = [](const sr::assets::DressedCharacter& d) {
        return std::count_if(d.items.begin(), d.items.end(), [](const std::string& p) { return p.find("*glow:") != std::string::npos; });
    };
    const sr::assets::DressedCharacter atNine = dressedAt(9);
    CHECK(glowing(atNine) == 1);
    CHECK(glowing(dressedAt(2)) == 0);
    for (std::string path : atNine.items) {
        sr::assets::EquipGlow back;
        if (!sr::assets::SplitGlow(path, back)) continue;
        CHECK(path == "res\\item\\china\\weapon\\bow_10.bsr");
        CHECK(back.texture == nine->texture);
        CHECK(back.colorOp == 5);
    }
}

TEST_CASE("the player figures' hit clip moves a few bones (played over the body); the falls move all") {
    // A hit (DAMAGE1, 3): an overlay of a few bones the game screen plays over what the body plays;
    // the fall (DIE1, 4) and the lying loop (DIE1_RM, 36): whole clips. From the default set, as the
    // game screen plays them in any stance.
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    sr::assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(sr::assets::MountGameArchives(fs, dir, error), error);
    sr::assets::CharacterTable characters;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    for (const char* code : {"CHAR_CH_MAN_ADVENTURER", "CHAR_EU_MAN_NOBLE"}) {
        CAPTURE(code);
        const sr::assets::ObjectRow* row = characters.Find(std::string_view(code));
        REQUIRE(row != nullptr);
        sr::assets::ModelData model;
        REQUIRE_MESSAGE(sr::assets::LoadModel(fs, sr::assets::CharacterTable::ResolveModel(fs, *row), model, error), error);
        sr::assets::ResourceData sets;
        sets.animationPaths = model.animationPaths;
        sets.animationSets = model.animationSets;
        for (std::uint32_t state : {3u, 4u, 36u}) {
            CAPTURE(state);
            const std::string path = sets.StateAnimation("default", state);
            REQUIRE_FALSE(path.empty());
            const auto file = fs.Read(path);
            REQUIRE_MESSAGE(file.has_value(), path);
            sr::assets::AnimationData anim;
            REQUIRE_MESSAGE(sr::assets::DecodeBan(file->Bytes(), anim, error), error);
            MESSAGE(code << " state " << state << ": " << path << " " << anim.durationMs << " ms, " << anim.keyTimesMs.size() << " keys, " << anim.tracks.size()
                          << " tracks, loop " << anim.loop);
            CHECK(anim.keyTimesMs.size() > 1);
            int matched = 0;
            for (const auto& t : anim.tracks) {
                for (const auto& b : model.skeleton.bones) matched += b.name == t.boneName ? 1 : 0;
            }
            if (state == 3) {
                CHECK(matched > 0);
                CHECK(matched * 2 < int(model.skeleton.bones.size()));
            } else {
                CHECK(matched * 2 > int(model.skeleton.bones.size()));
            }
        }
    }
}

TEST_CASE("the player skills' stages: what each shows (effects, held and flying models), on which bones") {
    // What world/StageEffects can start for every player skill a player can use:
    // a stage's .efp, or a model (an arrow, a thrown sword) that carries the skill's trail and force
    // effects (SCT_ARROW); and whether every bone a stage names is on the player's body or its weapon.
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);
    SkillTable skills;
    REQUIRE_MESSAGE(skills.Load(fs, error), error);
    SkillAnimationTable table;
    REQUIRE_MESSAGE(table.Load(fs, error), error);
    CharacterTable characters;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    // The bodies' bones (both races share theirs: Bip01 ...).
    std::vector<SkeletonData> bodies;
    for (const char* code : {"CHAR_CH_MAN_ADVENTURER", "CHAR_EU_MAN_NOBLE"}) {
        const ObjectRow* row = characters.Find(std::string_view(code));
        REQUIRE(row != nullptr);
        ModelData model;
        REQUIRE_MESSAGE(LoadModel(fs, CharacterTable::ResolveModel(fs, *row), model, error), error);
        bodies.push_back(std::move(model.skeleton));
    }
    const auto onBody = [&](const std::string& bone) {
        return std::all_of(bodies.begin(), bodies.end(), [&](const SkeletonData& s) { return s.Find(bone) >= 0; });
    };
    const auto isEffect = [](const std::string& p) { return sr::core::NormalizeAssetPath(p).ends_with(".efp"); };
    const auto isModel = [](const std::string& p) { return sr::core::NormalizeAssetPath(p).ends_with(".bsr"); };
    int players = 0, none = 0, effects = 0, modelsOnly = 0, modelsCarry = 0, nothing = 0;
    std::map<std::string, int> missingBones, modelPaths;
    for (const auto& [id, s] : skills.rows()) {
        if (!s.inService || s.activity == 0 || !(s.codename.starts_with("SKILL_CH_") || s.codename.starts_with("SKILL_EU_"))) continue;
        ++players;
        const bool named = !s.baseName.empty() && s.baseName != "xxx";
        const SkillAnimation* anim = table.Find(named ? s.baseName : s.codename);
        const std::vector<SkillEffectStage>* stages = table.Stages(s.codename);
        if ((stages == nullptr || stages->empty()) && anim != nullptr && !anim->stages.empty()) stages = &anim->stages;
        if (stages == nullptr || stages->empty()) {
            ++none;
            continue;
        }
        bool effect = false, model = false, carries = false;
        for (const SkillEffectStage& st : *stages) {
            if (st.phase > 2 && st.phase != 6) continue; // the swing's phases and a buff's start
            if (isEffect(st.object)) {
                effect = true;
                for (const auto* a : {&st.start, &st.target}) {
                    if (!a->bone.empty() && !onBody(a->bone)) ++missingBones[a->bone];
                }
            } else if (isModel(st.object)) {
                model = true;
                ++modelPaths[sr::core::NormalizeAssetPath(st.object)];
                const bool arrow = std::find(st.scripts.begin(), st.scripts.end(), "SCT_ARROW") != st.scripts.end();
                if (arrow && anim != nullptr && (!anim->arrowTrail.empty() || !anim->arrowForce.empty())) carries = true;
            }
        }
        if (effect) {
            ++effects;
        } else if (model) {
            ++modelsOnly;
            modelsCarry += carries;
        } else {
            ++nothing;
        }
    }
    MESSAGE(players << " player skills: " << effects << " start an effect on the swing, " << modelsOnly << " only models ("
                    << modelsCarry << " of them carry the skill's trail / force), " << nothing << " neither, " << none << " no stages");
    for (const auto& [bone, n] : missingBones) MESSAGE("effect stages on a bone not on the body: " << bone << " x" << n);
    // Measured on the 1.188 client's own (encrypted) skill shards: 699 more player skills than its older plain copy.
    CHECK(players == 5415);
    CHECK(effects == 4940);
    CHECK(modelsOnly == 356);
    CHECK(modelsCarry == 356);
    // The one bone not on the body is the weapon's tip (the crossbow skills' motion effects), which
    // the world screen finds among the worn items' sockets.
    CHECK(missingBones.size() == 1);
    CHECK(missingBones["ai_end"] == 177);
    ItemTable items;
    REQUIRE_MESSAGE(items.Load(fs, error), error);
    const ItemRow* crossbow = items.Find(std::string_view("ITEM_EU_CROSSBOW_01_A"));
    REQUIRE(crossbow != nullptr);
    const std::vector<std::string> crossbowModels = WornModels(fs, items, *crossbow);
    REQUIRE_FALSE(crossbowModels.empty());
    ModelData crossbowModel;
    REQUIRE_MESSAGE(LoadModel(fs, crossbowModels.front(), crossbowModel, error), error);
    CHECK(crossbowModel.skeleton.Find("ai_end") >= 0);
    // Every stage model has the Bone01 SCT_ARROW hangs the trail and force on.
    for (const auto& [path, n] : modelPaths) {
        ModelData model;
        REQUIRE_MESSAGE(LoadModel(fs, path, model, error), error);
        CAPTURE(path);
        CHECK(model.skeleton.Find("Bone01") >= 0);
        MESSAGE("stage model " << path << ": " << n << " stages");
    }
    CHECK(modelPaths.size() == 3);
}

TEST_CASE("the skill stages' sounds (skilleffect.txt SndBegin, SndEnd) are all in the game files (K8)") {
    const auto dir = GameDir();
    if (dir.empty()) {
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);
    const auto file = fs.Read("server_dep\\silkroad\\textdata\\skilleffect.txt");
    REQUIRE(file);
    std::unordered_map<std::string, SkillAnimation> rows;
    std::unordered_map<std::string, std::vector<SkillEffectStage>> stages;
    REQUIRE_MESSAGE(DecodeSkillAnimations(sr::core::DecodeTextFile(file->Bytes()), rows, error, nullptr, &stages), error);
    // Every stage's two sounds: under prim\snd\, each file there (all of them decode: the test below).
    std::set<std::string> files;
    int begins = 0, ends = 0;
    std::map<int, int> byPhase;
    for (const auto& [name, list] : stages) {
        for (const SkillEffectStage& s : list) {
            if (!s.soundBegin.empty()) {
                ++begins;
                ++byPhase[s.phase];
                files.insert(s.soundBegin);
            }
            if (!s.soundEnd.empty()) {
                ++ends;
                files.insert(s.soundEnd);
            }
        }
    }
    int missing = 0;
    std::set<std::string> distinct; // the data writes some names in other capitals (Skill2 and skill2)
    for (const std::string& f : files) {
        CHECK_MESSAGE(f.starts_with("prim\\snd\\"), f);
        distinct.insert(sr::core::NormalizeAssetPath(f));
        if (!fs.Exists(f)) {
            ++missing;
            MESSAGE("no file: " << f);
        }
    }
    std::string phases;
    for (const auto& [phase, n] : byPhase) phases += " " + std::to_string(phase) + ":" + std::to_string(n);
    MESSAGE(begins << " stages start with a sound, " << ends << " end with one; " << distinct.size() << " files; by phase" << phases);
    CHECK(begins > 900);
    CHECK(ends > 100);
    CHECK(distinct.size() == 188);
    CHECK(missing == 0);
}

TEST_CASE("the world map's tables: every map's, floor's and label's pictures (worldmap_mapinfo.txt, worldmap_localinfo.txt)") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);
    WorldMapData data;
    REQUIRE_MESSAGE(LoadWorldMapData(fs, data, error), error);
    CHECK(data.maps.size() == 21);
    CHECK(data.dungeons.size() == 23);
    // The world: 33 x 11 tiles of 4 x 4 regions, 32 pixels to a region.
    const WorldMapRow* world = data.Map(0);
    REQUIRE(world != nullptr);
    CHECK(world->tiled);
    CHECK(world->TilesAcross() == 33);
    CHECK(world->TilesDown() == 11);
    CHECK(world->drawW == (world->regionRight - world->regionLeft + 1) * 32);
    CHECK(world->Tile(0, 0) == "interface\\worldmap\\map\\map_world_46x113.ddj");
    // Every map's and floor's pictures are in the files, but for the later content's (Jupiter's: maps 16 and 17,
    // floors 2020..2023).
    std::set<int> missing;
    for (const WorldMapRow& m : data.maps) {
        bool all = true;
        if (m.tiled) {
            for (int i = 0; i < m.TilesAcross(); ++i) {
                for (int j = 0; j < m.TilesDown(); ++j) all = all && fs.Exists(m.Tile(i, j));
            }
        } else {
            all = fs.Exists(m.picture);
        }
        if (!all) missing.insert(m.id);
        CHECK(m.drawW > 0);
        CHECK(m.drawH > 0);
        CHECK(m.Right() > m.Left());
        CHECK(m.Top() > m.Bottom());
    }
    for (const DungeonMapRow& d : data.dungeons) {
        bool all = true;
        for (int i = 0; i < d.tilesAcross; ++i) {
            for (int j = 0; j < d.tilesDown; ++j) all = all && fs.Exists(d.Tile(i, j));
        }
        if (!all) missing.insert(d.id);
        CHECK((d.region & 0x8000) != 0);
        CHECK(d.tilesAcross == d.regionRight - d.regionLeft + 1);
        CHECK(d.tilesDown == d.regionTop - d.regionBottom + 1);
        // Each floor of a dungeon of several finds the others.
        for (int f = 1; f <= d.floors; ++f) CHECK_MESSAGE(data.Floor(d, f) != nullptr, d.id << " floor " << f);
    }
    CHECK(missing == std::set<int>{16, 17, 2020, 2021, 2022, 2023});
    CHECK(data.Dungeon(2001)->Label() == "1F");
    CHECK(data.Dungeon(2005)->Label() == "B1");
    CHECK(data.DungeonsOf(0x8001).size() == 4); // Donhwang's cave: four floors in one region
    CHECK(data.DungeonsOf(0x8007).size() == 1); // the tomb: a region a floor
    // Labels: on known maps, their pictures in the files (but the later content's).
    std::map<std::string, int> missingPictures;
    int texts = 0, pictures = 0;
    for (const WorldMapLabel& l : data.labels) {
        CHECK_MESSAGE((data.Map(l.map) != nullptr || data.Dungeon(l.map) != nullptr), l.id << " on map " << l.map);
        if (l.picture) {
            ++pictures;
            if (!fs.Exists(l.what)) ++missingPictures[l.what];
        } else {
            ++texts;
        }
    }
    for (const auto& [path, n] : missingPictures) MESSAGE("no picture: " << path << " (" << n << ")");
    MESSAGE(texts << " texts, " << pictures << " pictures");
    CHECK(texts > 300);
    CHECK(pictures > 300);
}

TEST_CASE("the fortress hammer: held in its race's sword set, its swing (ANI_HAMMER) found there") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);
    SkillTable skills;
    REQUIRE_MESSAGE(skills.Load(fs, error), error);
    SkillAnimationTable animations;
    REQUIRE_MESSAGE(animations.Load(fs, error), error);
    CharacterTable characters;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    ItemTable items;
    REQUIRE_MESSAGE(items.Load(fs, error), error);
    const ItemRow* hammer = items.Find(std::string_view("ITEM_FORT_FORTRESS_HAMMER_06"));
    REQUIRE(hammer != nullptr);
    CHECK(hammer->typeId[3] == 16);
    for (const auto& [code, skillCode] : {std::pair{"CHAR_CH_MAN_ADVENTURER", "SKILL_CH_FORTRESSWEAPON"}, {"CHAR_CH_WOMAN_ADVENTURER", "SKILL_CH_FORTRESSWEAPON"},
                                          {"CHAR_EU_MAN_NOBLE", "SKILL_EU_FORTRESSWEAPON"}, {"CHAR_EU_WOMAN_NOBLE", "SKILL_EU_FORTRESSWEAPON"}}) {
        const std::string body = code;
        CAPTURE(body);
        const ObjectRow* row = characters.Find(std::string_view(code));
        REQUIRE(row != nullptr);
        const std::uint32_t worn[] = {hammer->refObjId};
        DressedCharacter dressed;
        REQUIRE_MESSAGE(Dress(fs, characters, items, row->refObjId, worn, kNeutralShape, dressed, error), error);
        const SkillRow* skill = skills.Find(std::string_view(skillCode));
        REQUIRE(skill != nullptr);
        CHECK(skill->weaponKinds[0] == 16);
        const bool named = !skill->baseName.empty() && skill->baseName != "xxx";
        const SkillAnimation* anim = animations.Find(named ? skill->baseName : skill->codename);
        REQUIRE(anim != nullptr);
        CHECK(anim->group == dressed.stance);
        REQUIRE(anim->shot.size() == 1);
        CHECK(anim->shot[0] == AnimationState("ANI_HAMMER"));
        ModelData model;
        REQUIRE_MESSAGE(LoadModel(fs, CharacterTable::ResolveModel(fs, *row), model, error), error);
        ResourceData sets;
        sets.animationPaths = model.animationPaths;
        sets.animationSets = model.animationSets;
        CHECK(sets.StateAnimation(dressed.stance, std::uint32_t(anim->shot[0])).find("fortresshammer") != std::string::npos);
    }
}

TEST_CASE("the skills' clips from their row's group: their lengths against the skill's times (K10)") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);
    SkillTable skills;
    REQUIRE_MESSAGE(skills.Load(fs, error), error);
    SkillAnimationTable animations;
    REQUIRE_MESSAGE(animations.Load(fs, error), error);
    CharacterTable characters;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    std::array<std::vector<ResourceData>, 2> bodies;
    for (const auto& [code, race] : {std::pair{"CHAR_CH_MAN_ADVENTURER", 0}, {"CHAR_CH_WOMAN_ADVENTURER", 0}, {"CHAR_EU_MAN_NOBLE", 1}, {"CHAR_EU_WOMAN_NOBLE", 1}}) {
        const ObjectRow* row = characters.Find(std::string_view(code));
        REQUIRE(row != nullptr);
        ModelData model;
        REQUIRE_MESSAGE(LoadModel(fs, CharacterTable::ResolveModel(fs, *row), model, error), error);
        ResourceData sets;
        sets.animationPaths = model.animationPaths;
        sets.animationSets = model.animationSets;
        bodies[std::size_t(race)].push_back(std::move(sets));
    }
    std::map<std::string, int> lengths; // a .ban's length (ms), read once
    const auto lengthOf = [&](const std::string& path) {
        if (const auto it = lengths.find(path); it != lengths.end()) return it->second;
        int ms = -1;
        if (const auto file = fs.Read(path)) {
            AnimationData clip;
            if (DecodeBan(file->Bytes(), clip, error)) ms = int(clip.durationMs);
        }
        return lengths[path] = ms;
    };
    // Each group once (its first level), on each body of its race, its clips from its row's group.
    std::size_t casts = 0, inGroup = 0, fromDefault = 0, missing = 0;
    std::size_t readyClips = 0, readyNear = 0, shotClips = 0, shotNear = 0, shotPastLastHit = 0;
    std::map<int, int> readyGap, shotGap; // clip length minus the skill's time, in 100 ms steps
    int far = 0;
    for (const MasteryRow& m : skills.masteries()) {
        for (const SkillRow* s : skills.Board(m.id)) {
            if (!s->inService) continue;
            const SkillAnimation* anim = animations.Find(s->baseName);
            if (anim == nullptr || (anim->ready.empty() && anim->wait.empty() && anim->shot.empty())) continue;
            for (const int race : {0, 1}) {
                if (s->race != race && s->race != 3) continue;
                for (const ResourceData& body : bodies[std::size_t(race)]) {
                    ++casts;
                    const auto clip = [&](int state) {
                        std::string path = body.StateAnimation(anim->group, std::uint32_t(state));
                        if (!path.empty()) {
                            ++inGroup;
                        } else if (!(path = body.StateAnimation("default", std::uint32_t(state))).empty()) {
                            ++fromDefault;
                        } else {
                            ++missing;
                            MESSAGE("no clip: " << s->codename << " " << anim->group << " state " << state);
                        }
                        return path;
                    };
                    for (const int state : anim->ready) {
                        const std::string path = clip(state);
                        if (path.empty()) continue;
                        const int ms = lengthOf(path);
                        ++readyClips;
                        if (std::abs(ms - s->preparingMs) <= 50) ++readyNear;
                        ++readyGap[(ms - s->preparingMs) / 100];
                    }
                    for (const int state : anim->wait) clip(state);
                    for (const int state : anim->shot) {
                        const std::string path = clip(state);
                        if (path.empty()) continue;
                        const int ms = lengthOf(path);
                        ++shotClips;
                        const std::vector<int> hits = body.StateEvents(anim->group, std::uint32_t(state));
                        if (hits.empty() || ms >= hits.back()) ++shotPastLastHit;
                        // The swing's time: its casting when it has no wait clip, its duration and
                        // the durations of the rows its chain adds (col 9).
                        int time = (anim->wait.empty() ? s->castingMs : 0) + s->durationMs;
                        int rows = 1;
                        for (const SkillRow* next = skills.Find(s->chainNext); next != nullptr && rows < 16; next = skills.Find(next->chainNext), ++rows) {
                            time += next->durationMs;
                        }
                        ++shotGap[(ms - time) / 100];
                        if (std::abs(ms - time) <= 200) ++shotNear;
                        if (std::abs(ms - time) > 200 && ++far <= 12) MESSAGE("far: " << s->codename << " clip " << ms << " ms, time " << time << " (" << rows << " rows), hits " << hits.size());
                    }
                }
            }
        }
    }
    MESSAGE(casts << " skill casts: clips " << inGroup << " in the row's group, " << fromDefault << " from the default set, " << missing << " missing");
    MESSAGE("ready clips " << readyClips << ": within 50 ms of the preparing time " << readyNear);
    for (const auto& [gap, n] : readyGap) MESSAGE("  ready clip - preparing, " << gap * 100 << " ms: " << n);
    MESSAGE("shot clips " << shotClips << ": at least their last hit moment " << shotPastLastHit);
    for (const auto& [gap, n] : shotGap) MESSAGE("  shot clip - its swing's time, " << gap * 100 << " ms: " << n);
    // Every clip in its row's group's set (the 1.188 client plays them from it); a ready clip lasts
    // the preparing time; the shot clip about the swing's time, a chain's rows counted.
    CHECK(missing == 0);
    CHECK(fromDefault == 0);
    CHECK(readyNear * 100 >= readyClips * 98);
    CHECK(shotNear * 10 >= shotClips * 9);
    CHECK(shotPastLastHit * 100 >= shotClips * 99);
}

#ifdef SR_TEST_AUDIO
#include <atomic>
#include <mutex>

#include "sr/audio/Decode.h"
#include "sr/audio/SoundTables.h"
#include "sr/core/Jobs.h"

TEST_CASE("every sound and music file decodes, and the sound tables resolve") {
    const auto dir = GameDir();
    if (dir.empty()) {
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);

    // Every .wav (Data.pk2 prim\snd\...) and .ogg (Music.pk2), decoded whole on the job system.
    std::vector<std::string> files;
    for (const std::string& path : fs.ListFiles()) {
        const std::string key = sr::core::NormalizeAssetPath(path);
        if (key.ends_with(".wav") || key.ends_with(".ogg")) files.push_back(key);
    }
    std::atomic<int> wavs{0}, oggs{0}, failures{0}, stereo{0}, empty{0};
    std::atomic<std::uint64_t> milliseconds{0};
    std::mutex messages;
    std::vector<std::string> failed;
    sr::core::JobSystem jobs;
    jobs.ParallelFor(files.size(), 1, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            const std::string& key = files[i];
            ++(key.ends_with(".wav") ? wavs : oggs);
            auto data = fs.Read(key);
            sr::audio::Pcm pcm;
            std::string why;
            if (!data || !sr::audio::DecodeSound(data->Bytes(), pcm, why)) {
                ++failures;
                const std::lock_guard lock(messages);
                failed.push_back(key + ": " + (data ? why : std::string("unreadable")));
                continue;
            }
            stereo += pcm.channels == 2 ? 1 : 0;
            empty += pcm.Frames() == 0 ? 1 : 0;
            milliseconds += std::uint64_t(double(pcm.Frames()) * 1000.0 / double(pcm.rate));
        }
    });
    for (const std::string& f : failed) MESSAGE(f);
    CHECK(wavs.load() == 2159);
    CHECK(oggs.load() == 45);
    CHECK(failures.load() == 0);
    CHECK(empty.load() == 1); // prim\snd\player\black.wav: a data chunk of no samples
    MESSAGE(stereo.load() << " stereo files, " << milliseconds.load() / 60000 << " minutes in all");

    // The music streams a piece at a time as well (reads past the end go on from the start).
    {
        auto data = fs.Read("donwhang_town.ogg");
        REQUIRE(data);
        sr::audio::OggStream stream;
        REQUIRE_MESSAGE(stream.Open(std::vector<std::byte>(data->Bytes().begin(), data->Bytes().end()), error), error);
        CHECK(stream.Channels() == 2);
        std::vector<std::int16_t> piece(22050 * std::size_t(stream.Channels()));
        std::size_t total = 0;
        for (int i = 0; i < 200; ++i) total += stream.Read(piece.data(), 22050, true); // longer than the track
        CHECK(total == 200u * 22050u);
    }

    // The tables the client reads (server_dep's effectsound, effectenvsnd and, in 1.188, regioninfo).
    const auto text = [&](const char* path) {
        auto data = fs.Read(path);
        return data ? sr::core::DecodeTextFile(data->Bytes()) : std::string();
    };
    sr::audio::EffectSoundTable effects;
    REQUIRE_MESSAGE(effects.Parse(text("server_dep\\silkroad\\textdata\\effectsound.txt"), error), error);
    CHECK(effects.Size() == 5674);
    int withFile = 0, missing = 0;
    for (const sr::audio::EffectSoundRow& row : effects.Rows()) {
        if (row.path.empty()) continue;
        ++withFile;
        missing += fs.Exists(row.path) ? 0 : 1;
    }
    // Measured: 394 rows name a file the game does not have (pets, a few skills): gaps in the data.
    CHECK(withFile == 5647);
    CHECK(missing == 394);
    // What the client plays resolves to files.
    std::minstd_rand random;
    const std::pair<const char*, const char*> kUi[] = {{"SND_BUTTON_CLICK", ""}, {"SND_WINDOW_OPEN", ""}, {"SND_WINDOW_CLOSE", ""},
                                                       {"SND_LEVUP", ""},        {"SND_PICKUP", ""},      {"SND_POTION", ""},
                                                       {"SND_EQUIP", "SWORD"},   {"SND_DROPITEM", "GOLD"}};
    for (const auto& [handle, event] : kUi) {
        // As GameAudio::PlayUi asks: the UI object's row, else the ITEM object's.
        sr::audio::EffectSoundTable::Key key;
        key.object = "UI";
        key.handle = handle;
        key.events[0] = event;
        const sr::audio::EffectSoundRow* row = effects.Pick(key, random);
        if (row == nullptr) {
            key.object = "ITEM";
            row = effects.Pick(key, random);
        }
        CHECK_MESSAGE(row != nullptr, handle);
        if (row != nullptr) CHECK_MESSAGE(fs.Exists(row->path), row->path);
    }
    // A skill's swing and hit, by its base name (a player's) and by its codename (a monster's).
    CHECK(effects.Find({{}, "SND_SWING1", "SKILL_CH_SWORD_BASE"}).size() == 2);
    CHECK(effects.Find({{}, "SND_DMG", "SKILL_CH_SWORD_BASE"}).size() == 6);
    CHECK(!effects.Find({{}, "SND_DMG", "MSKILL_CH_MANGNYANG_ATTACK01"}).empty());
    CHECK(effects.HasObject("PCM_WARRIOR"));
    CHECK(effects.HasObject("MOB_MANGNYANG"));

    sr::audio::AreaSoundTable areas;
    REQUIRE_MESSAGE(areas.Parse(text("server_dep\\silkroad\\textdata\\effectenvsnd.txt"), error), error);
    CHECK(areas.Areas().size() == 51);
    int missingMusic = 0;
    for (const sr::audio::AreaSounds& a : areas.Areas()) {
        // The Jupiter temple's areas and the mirror dimension name music Music.pk2 does not have
        // (1.188 lists that later content but ships none of it).
        if (!fs.Exists(a.music)) {
            ++missingMusic;
            MESSAGE("no music: " << a.name << ": " << a.music);
            CHECK(a.music.starts_with("jupiter_"));
        }
        CHECK(!a.day.empty());
        CHECK(!a.night.empty());
    }
    CHECK(missingMusic == 6);
    sr::audio::RegionAreaTable regions;
    REQUIRE_MESSAGE(regions.Parse(text("server_dep\\silkroad\\textdata\\regioninfo.txt"), error), error);
    CHECK(regions.Areas().size() == 43);
    // Two named areas have no sounds of their own: 보상인던 (a reward dungeon) and 잊혀진세계_ (the
    // Forgotten World, the trailing '_' as written).
    int silent = 0;
    for (const auto& a : regions.Areas()) {
        if (areas.Find(a.name) == nullptr && ++silent <= 4) MESSAGE("no sounds for the area " << a.name);
    }
    CHECK(silent == 2);
    // Constantinople (78, 105) and Jangan (168, 97) are towns with their own music.
    const auto musicAt = [&](int x, int y) {
        const auto* area = regions.Find(std::uint16_t(x | y << 8), 960.0f, 960.0f);
        const auto* sounds = area != nullptr ? areas.Find(area->name) : nullptr;
        std::string music = sounds != nullptr ? sounds->music : std::string();
        std::transform(music.begin(), music.end(), music.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return music;
    };
    CHECK(musicAt(78, 105) == "easterneurope_town.ogg");
    CHECK(musicAt(168, 97) == "jangan_town.ogg");
}
#endif

TEST_CASE("the stall network's tables: 15 classes, 57 types, one group per item, the towns") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);
    StallNetworkData network;
    REQUIRE_MESSAGE(network.Load(fs, error), error);
    CHECK(network.Classes().size() == 15);
    std::size_t types = 0;
    std::set<std::uint32_t> groups;
    for (const StallNetworkClass& k : network.Classes()) {
        for (const StallNetworkType& t : k.types) {
            ++types;
            groups.insert(t.group);
            CHECK(network.TypeOf(t.group) == &t);
        }
    }
    CHECK(types == 57);
    CHECK(groups.size() == 57);
    // Every item (TID1 3) in at most one group, and that group a type of the tree; the few without one are the
    // trade goods and a handful of others (measured: 86 of 11,920 in the database's table; here the client's).
    ItemTable items;
    REQUIRE_MESSAGE(items.Load(fs, error), error);
    std::size_t grouped = 0, all = 0;
    for (const auto& [code, row] : items.rows()) {
        if (row.typeId[0] != 3) continue;
        ++all;
        const std::uint32_t g = network.GroupOf(std::uint8_t(row.typeId[0]), std::uint8_t(row.typeId[1]), std::uint8_t(row.typeId[2]), std::uint8_t(row.typeId[3]));
        if (g == 0) continue;
        ++grouped;
        CHECK(network.TypeOf(g) != nullptr);
    }
    MESSAGE(grouped << " of " << all << " items in a network group");
    CHECK(grouped * 100 >= all * 95);
    CHECK(network.GroupOf(3, 3, 1, 1) == 42); // the HP potions: Recovery
    CHECK(network.GroupOf(3, 3, 13, 1) == 45); // an exact row wins over 3/3/13/0
    // The towns: ten (regioninfo.txt's #TOWN), Jangan's and Constantinople's centres among them.
    RegionAreaTable towns;
    REQUIRE_MESSAGE(towns.Load(fs, error), error);
    std::size_t count = 0;
    for (const auto& a : towns.Areas()) count += a.town;
    CHECK(count == 10);
    CHECK(towns.TownAt(std::uint16_t(168 | 97 << 8), 960.0f, 960.0f) >= 0);
    CHECK(towns.TownAt(std::uint16_t(79 | 105 << 8), 960.0f, 960.0f) >= 0);
}

TEST_CASE("charactervisualchange.txt: the equipment's slots and priorities; a thief's suit hides the armour, a trader's pack does not") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    sr::assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(sr::assets::MountGameArchives(fs, dir, error), error);
    sr::assets::VisualChangeTable visuals;
    REQUIRE_MESSAGE(visuals.Load(fs, error), error);
    MESSAGE(visuals.Size() << " equipment visuals");
    sr::assets::CharacterTable characters;
    sr::assets::ItemTable items;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    REQUIRE_MESSAGE(items.Load(fs, error), error);
    const auto row = [&](const char* codename) {
        const sr::assets::ItemRow* r = items.Find(std::string_view(codename));
        REQUIRE_MESSAGE(r != nullptr, codename);
        return r;
    };
    const sr::assets::ItemRow* chest = row("ITEM_CH_M_CLOTHES_01_BA_A");
    const sr::assets::ItemRow* trader = row("ITEM_CH_M_TRADE_TRADER_02");
    const sr::assets::ItemRow* thief = row("ITEM_CH_M_TRADE_THIEF_02");
    const sr::assets::ItemRow* hunter = row("ITEM_CH_M_TRADE_HUNTER_02");
    // The rows 100 (trader), 102 (thief), 103 (hunter) and an armour's.
    REQUIRE(visuals.ForItem(*trader) != nullptr);
    CHECK(visuals.ForItem(*trader)->slots == 0x0700);
    CHECK(visuals.ForItem(*trader)->priority == 10);
    REQUIRE(visuals.ForItem(*thief) != nullptr);
    CHECK(visuals.ForItem(*thief)->slots == 0x073F);
    CHECK(visuals.ForItem(*hunter)->slots == 0x0700);
    REQUIRE(visuals.ForItem(*chest) != nullptr);
    CHECK(visuals.ForItem(*chest)->slots == 0x0002);
    CHECK(visuals.ForItem(*chest)->priority == 250);
    CHECK(sr::assets::EquipSlot(*thief) == 8);

    const sr::assets::ObjectRow* man = characters.Find(std::string_view("CHAR_CH_MAN_ADVENTURER"));
    REQUIRE(man != nullptr);
    const auto dressed = [&](const sr::assets::ItemRow* suit) {
        const std::uint32_t worn[] = {chest->refObjId, suit->refObjId};
        sr::assets::DressedCharacter d;
        REQUIRE_MESSAGE(sr::assets::Dress(fs, characters, items, man->refObjId, worn, sr::assets::kNeutralShape, d, error, {}, nullptr, &visuals), error);
        return d;
    };
    const auto has = [&](const sr::assets::DressedCharacter& d, const sr::assets::ItemRow* item) {
        const std::string path = items.ResolveModel(fs, *item);
        return !path.empty() && std::find(d.items.begin(), d.items.end(), path) != d.items.end();
    };
    const auto garment = [](const sr::assets::DressedCharacter& d, const char* part) {
        return std::any_of(d.items.begin(), d.items.end(), [&](const std::string& p) { return p.ends_with(part); });
    };
    // A thief's suit: the suit, no chest armour, no default garments.
    const auto asThief = dressed(thief);
    CHECK(has(asThief, thief));
    CHECK_FALSE(has(asThief, chest));
    CHECK_FALSE(garment(asThief, "ba.bsr"));
    CHECK_FALSE(garment(asThief, "la.bsr"));
    // A trader's: its pack over the armour, which stays; the legs' default garment too.
    const auto asTrader = dressed(trader);
    CHECK(has(asTrader, trader));
    CHECK(has(asTrader, chest));
    CHECK(garment(asTrader, "la.bsr"));
}

TEST_CASE("The jobs' bandits' look: usableresobjiddata.txt's figures, a thief's and a hunter's outfit by the spawn's byte") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    sr::assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(sr::assets::MountGameArchives(fs, dir, error), error);
    sr::assets::CharacterTable characters;
    sr::assets::ItemTable items;
    sr::assets::SkillTable skills;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    REQUIRE_MESSAGE(items.Load(fs, error), error);
    REQUIRE_MESSAGE(skills.Load(fs, error), error);
    sr::assets::BanditBodies bodies;
    REQUIRE_MESSAGE(bodies.Load(fs, characters, error), error);
    // 26 each: 13 men then 13 women, in file order (1907..1932, 14875..14900).
    REQUIRE(bodies.lists[0].size() == 26);
    REQUIRE(bodies.lists[1].size() == 26);
    CHECK(bodies.lists[0][0] == 1907);
    CHECK(characters.Find(bodies.lists[0][0])->codename == "CHAR_CH_MAN_ADVENTURER");
    CHECK(characters.Find(bodies.lists[0][13])->codename == "CHAR_CH_WOMAN_ADVENTURER");
    CHECK(bodies.lists[1][0] == 14875);
    // The degrees by level.
    CHECK(sr::assets::BanditDegree(1) == 1);
    CHECK(sr::assets::BanditDegree(60) == 7);
    CHECK(sr::assets::BanditDegree(104) == 11);
    CHECK(sr::assets::BanditDegree(200) == 14);
    const auto codes = [&](const sr::assets::BanditOutfit& o) {
        std::vector<std::string> out;
        for (const std::uint32_t id : o.worn) out.push_back(items.Find(id)->codename);
        return out;
    };
    const auto has = [](const std::vector<std::string>& list, std::string_view prefix) {
        return std::any_of(list.begin(), list.end(), [&](const std::string& c) { return c.starts_with(prefix); });
    };
    int thieves = 0, hunters = 0;
    for (const auto& [code, row] : characters.rows()) {
        if (!sr::assets::IsBandit(row) || (!code.starts_with("MOB_THIEF_NPC_") && !code.starts_with("MOB_HUNTER_NPC_")) || row.level != 60) continue;
        const sr::assets::SkillRow* skill = row.skills[0] != 0 ? skills.Find(row.skills[0]) : nullptr;
        const int kind = skill != nullptr && skill->weaponKinds[0] != 255 ? skill->weaponKinds[0] : 2;
        for (const int appearance : {0, 13, 0x20, 0x40 + 3}) {
            const sr::assets::BanditOutfit o = sr::assets::OutfitOfBandit(row, std::uint8_t(appearance), kind, bodies, characters, items);
            REQUIRE(o.body == bodies.lists[0][std::size_t(appearance % 26)]);
            const bool woman = appearance % 26 >= 13;
            const std::vector<std::string> worn = codes(o);
            // The suit of its job, the tier from the byte's bit 5.
            const std::string tier = (appearance & 0x20) != 0 ? "03" : "02";
            const bool hunter = row.typeId[3] == 3;
            CHECK(has(worn, std::string("ITEM_CH_") + (woman ? "F" : "M") + "_TRADE_" + (hunter ? "HUNTER_" : "THIEF_") + tier));
            // The Chinese weapon of its kind at degree 7 (level 60).
            const char* weapon = kind == 2 ? "ITEM_CH_SWORD_07_A" : kind == 4 ? "ITEM_CH_SPEAR_07_A" : kind == 6 ? "ITEM_CH_BOW_07_A" : "";
            if (*weapon != 0) CHECK_MESSAGE(has(worn, weapon), code << " " << weapon);
            if (kind == 2) CHECK(has(worn, "ITEM_CH_SHIELD_07_A"));
            // A hunter's six armour parts, the helm by bit 6; a thief none.
            if (hunter) {
                CHECK(has(worn, std::string("ITEM_CH_") + (woman ? "W" : "M") + "_"));
                CHECK(has(worn, std::string("ITEM_CH_") + (woman ? "W" : "M")) == true);
                const bool helm = std::any_of(worn.begin(), worn.end(), [](const std::string& c) { return c.find("_HA_A") != std::string::npos; });
                CHECK(helm == ((appearance & 0x40) != 0));
                ++hunters;
            } else {
                CHECK_FALSE(std::any_of(worn.begin(), worn.end(), [](const std::string& c) { return c.find("_BA_A") != std::string::npos; }));
                ++thieves;
            }
            // Dressed as a figure (no default garments).
            sr::assets::DressedCharacter dressed;
            REQUIRE_MESSAGE(sr::assets::Dress(fs, characters, items, o.body, o.worn, 0, dressed, error, {}, nullptr, nullptr, false), error);
            CHECK(!dressed.items.empty());
        }
    }
    CHECK(thieves > 0);
    CHECK(hunters > 0);
}

TEST_CASE("the item mall's catalogue: 5 shops, 21 tabs, 235 goods, each priced in silk with an item of itemdata") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);
    MallTable mall;
    REQUIRE_MESSAGE(mall.Load(fs, error), error);
    ItemTable items;
    REQUIRE_MESSAGE(items.Load(fs, error), error);
    CHECK(mall.shops().size() == 5);
    std::size_t tabs = 0;
    for (const MallShop& s : mall.shops()) tabs += s.tabs.size();
    CHECK(tabs == 21);
    CHECK(mall.Goods() == 235);
    int avatars = 0;
    for (const MallShop& s : mall.shops()) {
        for (const MallTab& t : s.tabs) {
            for (const MallGood& g : t.goods) {
                CHECK_MESSAGE((g.price > 0 || g.free), g.package); // ours: the Open Market ticket given away (kFreeMallPackages)
                CHECK_MESSAGE(g.price <= 398, g.package);
                const ItemRow* row = items.Find(std::string_view(g.itemCodename));
                REQUIRE_MESSAGE(row != nullptr, g.itemCodename);
                CHECK_MESSAGE(row->cashItem, g.itemCodename);
                avatars += (g.saleTag & 32) != 0;
            }
        }
    }
    CHECK(avatars == 103);
    // Special's Magic POP card at 10 silk; the quiver of 10,000 in one place.
    const MallShop* consume = mall.Shop("MALL_CONSUME");
    REQUIRE(consume != nullptr);
    bool card = false;
    for (const MallTab& t : consume->tabs) {
        for (const MallGood& g : t.goods) {
            if (g.itemCodename == "ITEM_MALL_GACHA_CARD") card = g.price == 10 && t.code == "MALL_CONSUME_SPECIAL";
            if (g.itemCodename == "ITEM_MALL_QUIVER") {
                const ItemRow* row = items.Find(std::string_view(g.itemCodename));
                CHECK(sr::game::MaxStack(*row) == 10000);
                CHECK(sr::game::MallPlacesNeeded(g, *row, 1) == 1);
                CHECK(sr::game::MallItems(g, *row, 1, 0).at(0).count == 10000);
            }
        }
    }
    CHECK(card);
    // The menu: Consume, Avatar, Pet, Premium, Alchemy, the hot store, Package, the basket in service (Event out).
    int inService = 0;
    for (const MallMenuRow& m : mall.menu()) inService += m.service;
    CHECK(inService == 8);
    CHECK(mall.hot().size() == 6);
}

TEST_CASE("Magic POP's data: 3,302 prizes and 42 consolations whose weights sum to 10,000; the machine; the tree; every prize an item") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(MountGameArchives(fs, dir, error), error);
    GachaTable gacha;
    REQUIRE_MESSAGE(gacha.Load(fs, error), error);
    ItemTable items;
    REQUIRE_MESSAGE(items.Load(fs, error), error);
    REQUIRE(gacha.machines().size() == 1);
    CHECK(gacha.machines()[0].npc == 9251);
    CHECK(gacha.machines()[0].selection == 1);
    CHECK(gacha.machines()[0].waste == 2);
    int prizes = 0, consolations = 0, weights = 0, always = 0, optioned = 0, visible = 0;
    std::set<std::uint32_t> ids;
    for (const GachaEntry& e : gacha.entries()) {
        CHECK_MESSAGE(items.Find(e.item) != nullptr, e.item);
        CHECK(ids.insert(e.id).second); // GachaID unique
        visible += e.visible;
        if (e.set == 1) {
            ++prizes;
            CHECK(e.count == 1);
            always += e.ratio == 10000;
            if (!e.options.empty()) {
                std::vector<sr::game::pop::Token> tokens;
                CHECK_MESSAGE(sr::game::pop::ParseOptions(e.options, tokens), e.options);
                ++optioned;
            }
        } else if (e.set == 2) {
            ++consolations;
            weights += e.ratio;
        }
    }
    CHECK(prizes == 3302);
    CHECK(consolations == 42);
    CHECK(weights == 10000);
    CHECK(always == 14);
    CHECK(optioned == 3276);
    CHECK(visible == 0); // the original announces no win with this data
    const auto waste = sr::game::pop::MakeWasteSet(gacha, 2);
    CHECK(waste.sums.back() == 10000);
    CHECK(gacha.tree().size() == 65);
    // Every middle category of the tree finds prizes (the selection window's filter) but the avatars' three and the
    // "etc" node's (1.188's set has none of them).
    int empty = 0;
    for (const GachaTreeRow& node : gacha.tree()) {
        bool found = false;
        for (const GachaEntry& e : gacha.entries()) {
            const ItemRow* row = items.Find(e.item);
            if (e.set == 1 && row != nullptr && sr::game::pop::InFilter(*row, node.typeId, 0)) found = true;
        }
        if (!found) MESSAGE("no prize under " << node.midText);
        empty += !found;
    }
    CHECK(empty == 4);
}

TEST_CASE("The body under worn pieces: only a mode 1 piece's cover table hides it, as the original's (2026-11-22)") {
    const auto dir = GameDir();
    if (dir.empty()) {
        MESSAGE("SR_GAME_DIR not set - skipped");
        return;
    }
    sr::assets::FileSystem fs;
    std::string error;
    REQUIRE_MESSAGE(sr::assets::MountGameArchives(fs, dir, error), error);
    sr::assets::VisualChangeTable visuals;
    REQUIRE_MESSAGE(visuals.Load(fs, error), error);
    sr::assets::CharacterTable characters;
    sr::assets::ItemTable items;
    REQUIRE_MESSAGE(characters.Load(fs, error), error);
    REQUIRE_MESSAGE(items.Load(fs, error), error);
    // The body's meshes an outfit hides (their file names).
    const auto hidden = [&](const char* body, std::vector<const char*> worn) {
        const sr::assets::ObjectRow* figure = characters.Find(std::string_view(body));
        REQUIRE(figure != nullptr);
        std::vector<std::uint32_t> ids;
        for (const char* w : worn) {
            const sr::assets::ItemRow* r = items.Find(std::string_view(w));
            REQUIRE_MESSAGE(r != nullptr, w);
            ids.push_back(r->refObjId);
        }
        sr::assets::DressedCharacter d;
        REQUIRE_MESSAGE(sr::assets::Dress(fs, characters, items, figure->refObjId, ids, sr::assets::kNeutralShape, d, error, {}, nullptr, &visuals), error);
        sr::assets::ModelData model;
        sr::assets::OutfitReport report;
        REQUIRE_MESSAGE(sr::assets::AssembleOutfit(fs, d.body, d.items, model, report, error), error);
        std::set<std::string> names;
        for (const std::string& path : report.hiddenMeshes) names.insert(path.substr(path.find_last_of('\\') + 1));
        std::string text = std::string(body) + " hides:";
        for (const std::string& n : names) text += " " + n;
        MESSAGE(text);
        return names;
    };
    // The thief's suit lists the whole body but the face (its keys' values 0 and 1): all of it is hidden under it.
    const auto thief = hidden("CHAR_CH_MAN_MONK", {"ITEM_CH_M_TRADE_THIEF_04"});
    for (const char* part : {"man_torso_upper.bms", "man_torso_lower.bms", "man_arm_upper.bms", "man_arm_lower.bms", "man_pelvis.bms", "man_thigh.bms", "man_calf.bms"}) {
        CHECK_MESSAGE(thief.contains(part), part);
    }
    // A garment's top (keys 2 and 3, the second's value 1) hides both torso meshes; its hands (mode 2) hide nothing:
    // the forearm is drawn under them.
    const auto garment = hidden("CHAR_CH_WOMAN_KANGSI", {"ITEM_CH_W_CLOTHES_01_BA_A", "ITEM_CH_W_CLOTHES_02_AA_A"});
    CHECK(garment.contains("woman_torso_upper.bms"));
    CHECK(garment.contains("woman_torso_lower.bms"));
    CHECK_FALSE(garment.contains("woman_arm_fore.bms"));
    // A European woman's first heavy top ends above the waist and does not list it: her lower torso stays (Mage, 2026-11-18).
    const auto mage = hidden("CHAR_EU_WOMAN_NOBLE", {"ITEM_EU_W_HEAVY_01_BA_A_DEF", "ITEM_EU_W_HEAVY_01_LA_A_DEF", "ITEM_EU_W_HEAVY_01_FA_A_DEF"});
    CHECK_FALSE(mage.contains("woman_torso_lower.bms"));
}
