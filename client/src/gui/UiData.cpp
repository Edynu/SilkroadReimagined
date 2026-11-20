#include "gui/UiData.h"

#include "gui/UiWarmup.h"

#include "sr/assets/WorldMap.h"
#include "sr/audio/SoundTables.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "sr/core/Log.h"
#include "sr/core/Path.h"
#include "sr/core/Text.h"
#include "sr/core/Trace.h"

namespace sr::client {

namespace {

// A face from the system's font folder by its name ("Arial" -> arial.ttf); empty when missing.
std::vector<std::byte> ReadSystemFont(std::string_view face) {
    const char* windows = std::getenv("WINDIR");
    if (windows == nullptr || face.empty()) return {};
    std::string file;
    for (const char c : face) {
        if (c != ' ') file += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    std::ifstream in(std::filesystem::path(windows) / "Fonts" / (file + ".ttf"), std::ios::binary);
    if (!in) return {};
    std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(chars.size());
    std::memcpy(bytes.data(), chars.data(), chars.size());
    return bytes;
}

} // namespace

void UiData::Load(core::JobSystem& jobs, const assets::FileSystem& files, std::vector<std::string> layouts) {
    Wait();
    state_ = std::make_shared<State>();
    jobs_ = &jobs;
    jobs.Run(
        [state = state_, files = &files, layouts = std::move(layouts)] {
            SR_ZONE("interface: data");
            std::string error;
            assets::UiDefines defines;
            if (const auto file = files->Read("config\\define.txt")) defines = assets::DecodeUiDefines(core::DecodeTextFile(file->Bytes()));
            for (const std::string& path : layouts) {
                assets::UiLayout layout;
                const auto file = files->Read(path);
                if (file && assets::DecodeUiLayout(core::DecodeTextFile(file->Bytes()), defines, layout, error)) {
                    state->layouts.emplace(path, std::move(layout));
                } else {
                    core::LogWarning("interface: cannot read {}: {}", path, error.empty() ? "missing" : error);
                }
            }
            // The 1.188 windows built from binary layouts.
            for (const char* path : {"res_ui\\guild.2dt", "res_ui\\guild_r.2dt", "res_ui\\guild_w.2dt", "res_ui\\guild_gp.2dt", "res_ui\\guild_intro.2dt", "res_ui\\cumunity_friend.2dt", "res_ui\\cumunity_cut.2dt", "res_ui\\cumunity_note.2dt", "res_ui\\worldmapguide.2dt", "res_ui\\stallnetwork.2dt", "res_ui\\open_market_main.2dt", "res_ui\\open_market_1_1.2dt", "res_ui\\reputation.2dt", "res_ui\\frpvp.2dt", "res_ui\\questlist.2dt", "res_ui\\quest_re.2dt", "res_ui\\questminilist.2dt",
                                     "res_ui\\nifsummonpartyuserwnd.2dt", "res_ui\\nifcollectionwnd.2dt", "res_ui\\nifcollectionitemstory.2dt",
                                     "res_ui\\nifcollectionmsgbox.2dt", "res_ui\\nifundermenubar.2dt",
                                     "res_ui\\nifenchantwnd.2dt", "res_ui\\nifenchantalchemymsgbox.2dt", "res_ui\\silksearch.2dt",
                                     "res_ui\\arena_rule_select.2dt", "res_ui\\arena_guilduser_select.2dt", "res_ui\\arena_game_score.2dt",
                                     "res_ui\\arena_game_rank.2dt", "res_ui\\arena_game_result.2dt"}) {
                std::vector<assets::Ui2dtRecord> records;
                const auto file = files->Read(path);
                if (file && assets::DecodeUi2dt(file->Bytes(), records, error)) {
                    state->layouts2dt.emplace(path, std::move(records));
                } else {
                    core::LogWarning("interface: cannot read {}: {}", path, error.empty() ? "missing" : error);
                }
            }
            if (const auto file = files->Read("server_dep\\silkroad\\textdata\\textuisystem.txt")) {
                assets::DecodeTextCatalog(core::DecodeTextFile(file->Bytes()), state->text, error);
            }
            // The zone names (the minimap's area name by the region id as a decimal key, the area
            // banner's RN_..._01/_02/_03), and which area each region is in (regioncode.txt:
            // "1 <region> <key> <korean>"; the key xxx is none).
            if (const auto file = files->Read("server_dep\\silkroad\\textdata\\textzonename.txt")) {
                assets::TextCatalog zones;
                if (assets::DecodeTextCatalog(core::DecodeTextFile(file->Bytes()), zones, error)) state->text.merge(zones);
            }
            // Item, monster and NPC names (SN_...: the item tooltip) and the magic options (its blues);
            // in 1.188 textdataname.txt lists the files that hold them (TextData_Object.txt, ...).
            if (std::string why; !assets::LoadTextCatalog(*files, "server_dep\\silkroad\\textdata\\textdataname.txt", state->text, why) || !why.empty()) {
                core::LogWarning("ui: names: {}", why);
            }
            // What NPCs and gates say (textquest.txt, in 1.188 a list of TextQuest_*.txt: their
            // greetings), and which text each one's is (npcchat.txt: "1 <codename> <greeting key> <second key>").
            if (std::string why; !assets::LoadTextCatalog(*files, "server_dep\\silkroad\\textdata\\textquest.txt", state->text, why) || !why.empty()) {
                core::LogWarning("ui: quest texts: {}", why);
            }
            // The events' texts (textevent.txt): Event So-Ok's coded quests' titles and talks.
            if (std::string why; !assets::LoadTextCatalog(*files, "server_dep\\silkroad\\textdata\\textevent.txt", state->text, why) || !why.empty()) {
                core::LogWarning("ui: event texts: {}", why);
            }
            if (const auto file = files->Read("server_dep\\silkroad\\textdata\\npcchat.txt")) {
                const std::string text = core::DecodeTextFile(file->Bytes());
                std::size_t start = 0;
                while (start < text.size()) {
                    std::size_t end = text.find('\n', start);
                    if (end == std::string::npos) end = text.size();
                    std::string_view line = std::string_view(text).substr(start, end - start);
                    start = end + 1;
                    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
                    const std::size_t a = line.find('\t'), b = a == std::string_view::npos ? a : line.find('\t', a + 1);
                    if (b == std::string_view::npos || line.substr(0, a) != "1") continue;
                    const std::size_t c = line.find('\t', b + 1);
                    const std::string codename(line.substr(a + 1, b - a - 1));
                    state->npcGreetings[codename] = std::string(line.substr(b + 1, c == std::string_view::npos ? c : c - b - 1));
                    if (c != std::string_view::npos) {
                        const std::size_t d = line.find('\t', c + 1);
                        state->npcTalks[codename] = std::string(line.substr(c + 1, d == std::string_view::npos ? d : d - c - 1));
                    }
                }
            }
            if (const auto file = files->Read("server_dep\\silkroad\\textdata\\actionwnddata.txt")) {
                state->actions = assets::DecodeActionData(core::DecodeTextFile(file->Bytes()));
                // The stall network's icon (1013, its row commented out in 1.188's file):
                // shown again, at the commands' place 16 (the layout's 116, free).
                if (std::none_of(state->actions.begin(), state->actions.end(), [](const assets::ActionRow& r) { return r.id == 1013; })) {
                    state->actions.push_back({1013, "UIIT_STT_WARENETWORK_TITLE_SHOTCUT", "icon\\action\\icon_cha_shopnetwork.ddj", 1, 16});
                }
            }
            // The world map's maps, floors and labels.
            if (std::string why; !assets::LoadWorldMapData(*files, state->worldMap, why)) core::LogWarning("ui: world map: {}", why);
            // regioninfo.txt's dungeon fields: a dungeon region's minimap folder (its lines name the
            // regions as "x 128 ALL": the region id 0x8000 | x).
            if (const auto file = files->Read("server_dep\\silkroad\\textdata\\regioninfo.txt")) {
                audio::RegionAreaTable areas;
                std::string why;
                if (areas.Parse(core::DecodeTextFile(file->Bytes()), why)) {
                    for (int id = 1; id < 64; ++id) {
                        const std::uint16_t region = std::uint16_t(0x8000 | id);
                        if (const audio::RegionAreaTable::Area* a = areas.Find(region, 0.0f, 0.0f); a != nullptr && !a->dungeon.empty()) {
                            state->dungeonFolders[region] = a->dungeon;
                        }
                    }
                } else {
                    core::LogWarning("ui: regioninfo.txt: {}", why);
                }
            }
            if (const auto file = files->Read("server_dep\\silkroad\\textdata\\magicoption.txt")) {
                assets::DecodeMagicOptions(core::DecodeTextFile(file->Bytes()), state->magicOptions, error);
            }
            if (const auto file = files->Read("server_dep\\silkroad\\textdata\\refmagicoptgroup.txt")) {
                assets::DecodeMagicOptGroups(core::DecodeTextFile(file->Bytes()), state->magicOptGroups, error);
            }
            if (const auto file = files->Read("server_dep\\silkroad\\textdata\\regioncode.txt")) {
                const std::string text = core::DecodeTextFile(file->Bytes());
                std::size_t start = 0;
                while (start < text.size()) {
                    std::size_t end = text.find('\n', start);
                    if (end == std::string::npos) end = text.size();
                    const std::string_view line = std::string_view(text).substr(start, end - start);
                    start = end + 1;
                    std::vector<std::string_view> cells;
                    std::size_t at = 0;
                    while (at <= line.size() && cells.size() < 3) {
                        std::size_t tab = line.find('\t', at);
                        if (tab == std::string_view::npos) tab = line.size();
                        cells.push_back(line.substr(at, tab - at));
                        at = tab + 1;
                    }
                    if (cells.size() < 3 || cells[0] != "1" || cells[2] == "xxx") continue;
                    const int region = std::atoi(std::string(cells[1]).c_str());
                    std::string key(cells[2]);
                    while (!key.empty() && (key.back() == '\r' || key.back() == ' ')) key.pop_back();
                    if (region > 0 && region <= 0xFFFF && !key.empty()) state->regionKeys[std::uint16_t(region)] = std::move(key);
                }
            }
            // The interface font: the original asks Windows for the face UIC_STT_FONTNAME names (the
            // English column: Arial; CreateFontIndirectA), so the system's file;
            // else the game's own (Media.pk2 fonts\<"English font" in CP949>.ttf), as the HUD's.
            if (const auto face = state->text.find("UIC_STT_FONTNAME"); face != state->text.end()) {
                if (std::vector<std::byte> bytes = ReadSystemFont(face->second); !bytes.empty()) {
                    state->fontLoaded = state->font.Load(std::move(bytes), error);
                }
            }
            if (!state->fontLoaded) {
                if (const auto file = files->Read("fonts\\\xBF\xB5\xB9\xAE\xBC\xAD\xC3\xBC.ttf")) {
                    const auto bytes = file->Bytes();
                    state->fontLoaded = state->font.Load(std::vector<std::byte>(bytes.begin(), bytes.end()), error);
                }
            }
            if (!state->fontLoaded) core::LogWarning("interface: no font: {}", error);
            // What the face lacks (the grade's stars, ...) from a symbol face, as Windows' font linking gives the original.
            for (const char* fallback : {"Segoe UI Symbol", "Malgun"}) {
                std::string why;
                if (std::vector<std::byte> bytes = ReadSystemFont(std::string_view(fallback) == "Segoe UI Symbol" ? "seguisym" : "malgun");
                    state->fontLoaded && !bytes.empty() && state->font.LoadFallback(std::move(bytes), why)) {
                    break;
                }
            }
            // The English texts' slips (ours; the original shows them as they are): "Biginner's
            // movement scroll" (SN_ITEM_ETC_SPEED_UP_BASIC, its English in the wrong column).
            for (auto& [key, value] : state->text) {
                for (std::size_t at = value.find("Biginner"); at != std::string::npos; at = value.find("Biginner", at)) value.replace(at, 8, "Beginner");
            }
            state->ready.store(true, std::memory_order_release);
        },
        core::JobPriority::Streaming, &loading_);
}

const std::string& UiData::RegionKey(std::uint16_t region) const {
    static const std::string none;
    if (!Ready()) return none;
    const auto it = state_->regionKeys.find(region);
    return it == state_->regionKeys.end() ? none : it->second;
}

const std::vector<assets::Ui2dtRecord>* UiData::Layout2dt(const std::string& path) const {
    if (!Ready()) return nullptr;
    const auto it = state_->layouts2dt.find(path);
    return it == state_->layouts2dt.end() ? nullptr : &it->second;
}

const assets::UiLayout* UiData::Layout(const std::string& path) const {
    if (!Ready()) return nullptr;
    const auto it = state_->layouts.find(path);
    return it == state_->layouts.end() ? nullptr : &it->second;
}

const std::string& UiData::Text(const std::string& key) const {
    if (Ready()) {
        if (const auto it = state_->text.find(key); it != state_->text.end()) return it->second;
    }
    return key;
}

UiTextures::Ref UiTextures::Get(const std::string& path) {
    if (path.starts_with('@')) {
        const auto external = externals_.find(path);
        return external != externals_.end() ? external->second : Ref{};
    }
    auto it = ids_.find(path);
    // Ahead of the warm-up and the world's streaming (UiWarmup.h: kUiTexturePriority): the interface's pictures are few
    // and small and shown at once, and behind a world's flood a window or a map tile stayed blank for a while.
    if (it == ids_.end()) {
        it = ids_.emplace(path, Held{assets_.RequestTexture(path, kUiTexturePriority), true}).first;
        if (warmup_ != nullptr) warmup_->Note(path); // drawn this run: warmed up from the next on
    } else if (!it->second.shown) {
        // Prefetched, now shown: noted, and to the front of the queue if it is still waiting (asked again at the
        // interface's priority; that extra hold let go at once).
        it->second.shown = true;
        if (warmup_ != nullptr) warmup_->Note(path);
        if (assets_.State(it->second.id) == gfx::AssetState::Queued) assets_.Release(assets_.RequestTexture(path, kUiTexturePriority));
    }
    Ref ref;
    if (const auto info = assets_.Texture(it->second.id)) {
        ref.texture = ui::TextureRef{info->bindless, info->premultipliedAlpha, false};
        ref.width = float(info->width);
        ref.height = float(info->height);
        ref.resident = true;
    }
    // Counted the first time it is asked for and still not in (the warm-up's measure of what showed late).
    if (!ref.resident && warmup_ != nullptr && cold_.insert(path).second) warmup_->NoteCold(path);
    return ref;
}

void UiTextures::Prefetch(const std::string& path) {
    if (path.starts_with('@') || ids_.contains(path)) return;
    ids_.emplace(path, Held{assets_.RequestTexture(path, kUiWarmupPriority), false});
}

bool UiTextures::Exists(const std::string& path) {
    // The program's own: missing until made (a window does not wait for it to settle).
    if (path.starts_with('@')) return externals_.contains(path);
    Get(path);
    return assets_.State(ids_[path].id) != gfx::AssetState::Failed;
}

bool UiTextures::Settled() const {
    // What is shown (a prefetch is waited for by nothing).
    for (const auto& [path, held] : ids_) {
        if (!held.shown) continue;
        const gfx::AssetState s = assets_.State(held.id);
        if (s != gfx::AssetState::Resident && s != gfx::AssetState::Failed) return false;
    }
    return true;
}

void UiTextures::ReleaseAll() {
    for (const auto& [path, held] : ids_) assets_.Release(held.id);
    ids_.clear();
}

std::vector<std::string> AllLayouts(const assets::FileSystem& files) {
    // The tables other code reads from resinfo\.
    constexpr std::string_view kTables[] = {"atstructeffect", "avataritemdata", "effectenvsnd", "effectsound",
                                            "ifpartymemberviewer", "itemoption", "itemrare", "itemtypenumber",
                                            "regioninfo", "regionname", "skilleffect"};
    std::vector<std::string> out;
    for (const std::string& key : files.ListFolder("resinfo")) {
        if (!key.ends_with(".txt") || key.find('\\', 8) != std::string::npos) continue;
        const std::string_view stem = std::string_view(key).substr(8, key.size() - 12);
        if (std::find(std::begin(kTables), std::end(kTables), stem) == std::end(kTables)) out.push_back(key);
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace sr::client
