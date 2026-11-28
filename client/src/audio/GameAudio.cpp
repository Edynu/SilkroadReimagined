#include "audio/GameAudio.h"

#include <algorithm>
#include <atomic>
#include <cctype>

#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/audio/SoundTables.h"
#include "sr/core/Jobs.h"
#include "sr/core/Log.h"
#include "sr/core/Text.h"
#include "sr/core/Trace.h"

namespace sr::client {

namespace {

audio::Vec3 ToAudio(glm::vec3 v) { return {v.x, v.y, v.z}; }

// The file name of a row's path (after the last backslash), lower case.
std::string FileName(const std::string& path) {
    std::string name = path.substr(path.rfind('\\') + 1);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return name;
}

// SND_EQUIP's event for an item (effectsound.txt's ITEM rows; the type ids TID1..TID4 as
// game::KindOf reads them). Ours: the original's choice by the item's type is not traced; this is
// the table's names matched to the types. Empty: no sound known for it.
std::string_view EquipEvent(const assets::ItemRow& row) {
    const auto& t = row.typeId;
    if (t[0] != 3) return {};
    if (t[1] == 3) {
        if (t[2] == 1) return "POTION";
        if (t[2] == 4) return "QUIVER"; // arrows and bolts
        if (row.codename.find("_SCROLL_") != std::string::npos) return "SCROLL";
        return {};
    }
    if (t[1] != 1) return {};
    const int part = t[3];
    switch (t[2]) {
    case 6: { // weapons, by TID4 (as assets::StanceOf)
        constexpr std::string_view kWeapons[] = {{}, {}, "SWORD", "BLADE", "SPEAR", "TBLADE", "BOW", "SWORD",
                                                 "TSWORD", "DUELAXE", "WAND", "STAFF", "CROSSBOW", "DAGGER", "HARP", "WAND"};
        return part >= 0 && part < int(std::size(kWeapons)) ? kWeapons[part] : std::string_view();
    }
    case 4: return "SHIELD";
    case 2: case 3: case 10: case 11: { // light and heavy armour, by part (Items.cpp's order)
        constexpr std::string_view kArmour[] = {{}, "HELM", "PAULDRONS", "BREASTPLATE", "CUISSE", "GAUNTLET", "GREAVE"};
        return part >= 1 && part <= 6 ? kArmour[part] : std::string_view();
    }
    case 1: case 9: { // garments: the table has a cap, a robe, gloves and shoes
        constexpr std::string_view kGarment[] = {{}, "CAP", "ROBE", "ROBE", "ROBE", "GLOVES", "SHOES"};
        return part >= 1 && part <= 6 ? kGarment[part] : std::string_view();
    }
    case 5: case 12: { // accessories
        constexpr std::string_view kAccessory[] = {{}, "EARRING", "NECKLACE", "RING"};
        return part >= 1 && part <= 3 ? kAccessory[part] : std::string_view();
    }
    default: return {};
    }
}

} // namespace

struct GameAudio::Tables {
    std::atomic<bool> ready{false};
    bool ok = false;
    audio::EffectSoundTable effects;
    audio::AreaSoundTable areas;
    audio::RegionAreaTable regions;
};

GameAudio::GameAudio(core::JobSystem& jobs, const assets::FileSystem& files) : jobs_(jobs), files_(files), sound_(jobs, files) {}

GameAudio::~GameAudio() { Shutdown(); }

bool GameAudio::Ready() const { return tables_ && tables_->ready.load(std::memory_order_acquire) && tables_->ok; }

bool GameAudio::Open(const core::AudioOptions& options, std::string& error, int loopbackRate) {
    if (!sound_.OpenLoopback(loopbackRate, error)) return false;
    SetLevels(options);
    tables_ = std::make_shared<Tables>();
    // The tables on a job (waited for at Shutdown: it reads the file system).
    jobs_.Run(
        [tables = tables_, files = &files_] {
            SR_ZONE("audio: tables");
            std::string error;
            const auto text = [&](const char* path) {
                const std::optional<assets::FileData> data = files->Read(path);
                return data ? core::DecodeTextFile(data->Bytes()) : std::string();
            };
            tables->ok = tables->effects.Parse(text("server_dep\\silkroad\\textdata\\effectsound.txt"), error) &&
                         tables->areas.Parse(text("server_dep\\silkroad\\textdata\\effectenvsnd.txt"), error) &&
                         tables->regions.Parse(text("server_dep\\silkroad\\textdata\\regioninfo.txt"), error);
            if (tables->ok) {
                core::LogInfo("audio: {} sound rows, {} areas with music, {} named areas", tables->effects.Size(), tables->areas.Areas().size(),
                              tables->regions.Areas().size());
            } else {
                core::LogWarning("audio: {}", error);
            }
            tables->ready.store(true, std::memory_order_release);
        },
        core::JobPriority::Streaming, &tablesJob_);
    return true;
}

void GameAudio::Shutdown() {
    jobs_.Wait(tablesJob_);
    StopAmbience();
    sound_.Shutdown();
}

void GameAudio::SetLevels(const core::AudioOptions& options) {
    sound_.SetGain(audio::Channel::Music, core::AudioOptions::Gain(options.bgm, options.muteBgm));
    sound_.SetGain(audio::Channel::Effects, core::AudioOptions::Gain(options.effects, options.muteEffects));
    sound_.SetGain(audio::Channel::Environment, core::AudioOptions::Gain(options.environment, options.muteEnvironment));
}

void GameAudio::SetListener(glm::vec3 eye, glm::vec3 forward, glm::vec3 up) { sound_.SetListener(ToAudio(eye), ToAudio(forward), ToAudio(up)); }

void GameAudio::SetOcclusion(std::function<float(glm::vec3 ear, glm::vec3 sound)> occluded) {
    if (!occluded) {
        sound_.occluded = nullptr;
        return;
    }
    sound_.occluded = [test = std::move(occluded)](audio::Vec3 ear, audio::Vec3 at) { return test({ear.x, ear.y, ear.z}, {at.x, at.y, at.z}); };
}

float GameAudio::Random(float low, float high) { return high > low ? std::uniform_real_distribution<float>(low, high)(random_) : low; }

void GameAudio::Update(double dt) {
    if (!sound_.IsOpen()) return;
    now_ += dt;
    // The ambience's one-shots, each again after a while in its range.
    for (Ambient& a : ambience_) {
        if (a.loop != 0 || now_ < a.next) continue;
        sound_.Play(a.path, audio::Channel::Environment);
        a.next = now_ + Random(a.minSeconds, a.maxSeconds);
    }
    sound_.Update(dt);
}

void GameAudio::PlayUi(std::string_view handle, std::string_view event) {
    if (!Ready()) return;
    audio::EffectSoundTable::Key key;
    key.object = "UI";
    key.handle = handle;
    key.events[0] = event;
    const audio::EffectSoundRow* row = tables_->effects.Pick(key, random_);
    if (row == nullptr) {
        key.object = "ITEM";
        row = tables_->effects.Pick(key, random_);
    }
    if (row != nullptr) sound_.Play(row->path, audio::Channel::Effects, row->volume);
}

void GameAudio::Equip(const assets::ItemRow& item) {
    const std::string_view event = EquipEvent(item);
    if (!event.empty()) PlayUi("SND_EQUIP", event);
}

void GameAudio::GoldDropped(glm::vec3 at) {
    if (!Ready()) return;
    audio::EffectSoundTable::Key key{"ITEM", "SND_DROPITEM", {}, {"GOLD"}};
    if (const audio::EffectSoundRow* row = tables_->effects.Pick(key, random_)) sound_.PlayAt(row->path, audio::Channel::Effects, ToAudio(at), row->volume);
}

void GameAudio::ClipSound(std::string_view handle, std::string_view skill, std::string_view weapon, std::string_view codename, std::string_view wave, glm::vec3 at) {
    if (!Ready() || handle.empty()) return;
    std::string name(handle);
    for (char& c : name) c = char(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    const audio::EffectSoundTable& t = tables_->effects;
    const audio::EffectSoundRow* row = skill.empty() ? nullptr : t.Pick({{}, name, skill}, random_);
    if (row == nullptr && !weapon.empty() && !name.starts_with("VOC_")) {
        audio::EffectSoundTable::Key key{"PLAYER", name, {}};
        key.events[0] = weapon;
        row = t.Pick(key, random_);
    }
    if (row == nullptr && !wave.empty() && files_.Exists(wave)) {
        sound_.PlayAt(wave, audio::Channel::Effects, ToAudio(at), 1.0f);
        return;
    }
    if (row == nullptr && !codename.empty()) {
        if (const std::string& object = VoiceObject(codename); !object.empty()) row = t.Pick({object, name, {}}, random_);
    }
    if (row != nullptr) sound_.PlayAt(row->path, audio::Channel::Effects, ToAudio(at), row->volume);
}

void GameAudio::Swing(std::uint32_t attacker, std::string_view skill, glm::vec3 at, bool clipSounds) {
    if (skill.empty()) return;
    lastSkill_[attacker] = std::string(skill);
    if (!Ready()) return;
    const audio::EffectSoundTable& t = tables_->effects;
    if (!clipSounds) {
        const audio::EffectSoundRow* swing = t.Pick({{}, "SND_SWING1", skill}, random_);
        if (swing == nullptr) swing = t.Pick({{}, "SND_SWING_S1", skill}, random_);
        if (swing != nullptr) sound_.PlayAt(swing->path, audio::Channel::Effects, ToAudio(at), swing->volume);
        if (const audio::EffectSoundRow* shout = t.Pick({{}, "VOC_SHOUT1", skill}, random_)) {
            sound_.PlayAt(shout->path, audio::Channel::Effects, ToAudio(at), shout->volume);
        }
    }
    // Its activation (the bow's normal skills, stealth: SND_ACTIVATE).
    if (const audio::EffectSoundRow* on = t.Pick({{}, "SND_ACTIVATE", skill}, random_)) sound_.PlayAt(on->path, audio::Channel::Effects, ToAudio(at), on->volume);
}

void GameAudio::SecondSwing(std::string_view skill, glm::vec3 at) {
    if (skill.empty() || !Ready()) return;
    if (const audio::EffectSoundRow* swing = tables_->effects.Pick({{}, "SND_SWING_S2", skill}, random_)) {
        sound_.PlayAt(swing->path, audio::Channel::Effects, ToAudio(at), swing->volume);
    }
}

void GameAudio::PlayEffect(std::string_view path, glm::vec3 at, float volume) {
    if (path.empty()) return;
    sound_.PlayAt(path, audio::Channel::Effects, ToAudio(at), volume);
}

void GameAudio::Blocked(std::string_view weapon, bool critical, glm::vec3 at) {
    if (!Ready()) return;
    // The rows of the weapon (none named: the plain ones), the critical's or the normal's, else either.
    std::vector<const audio::EffectSoundRow*> fit;
    for (const bool exact : {true, false}) {
        audio::EffectSoundTable::Key key{"PLAYER", "SND_BLOCKING", {}};
        if (exact) key.events[1] = critical ? "CRITYCAL" : "NORMAL";
        for (const audio::EffectSoundRow* r : tables_->effects.Find(key)) {
            if (r->events[0] == weapon) fit.push_back(r);
        }
        if (!fit.empty()) break;
    }
    if (fit.empty()) return;
    const audio::EffectSoundRow* row = fit[std::uniform_int_distribution<std::size_t>(0, fit.size() - 1)(random_)];
    sound_.PlayAt(row->path, audio::Channel::Effects, ToAudio(at), row->volume);
}

void GameAudio::Hit(std::uint32_t attacker, bool critical, glm::vec3 at, bool player, bool berserk) {
    if (!Ready()) return;
    const auto it = lastSkill_.find(attacker);
    if (it == lastSkill_.end()) return; // its swing was not seen
    const audio::EffectSoundTable& t = tables_->effects;
    if (critical) {
        // SND_CRIDMG: the skill's own rows (the Flame Master's), else a player's rows tied to no skill, the HWAN one in
        // berserk and the other out of it.
        const audio::EffectSoundRow* row = t.Pick({{}, "SND_CRIDMG", it->second}, random_);
        if (row == nullptr && player) {
            std::vector<const audio::EffectSoundRow*> fit;
            for (const audio::EffectSoundRow* r : t.Find({"PLAYER", "SND_CRIDMG", {}})) {
                if ((r->events[0] == "HWAN") == berserk) fit.push_back(r);
            }
            if (!fit.empty()) row = fit[std::uniform_int_distribution<std::size_t>(0, fit.size() - 1)(random_)];
        }
        if (row != nullptr) {
            sound_.PlayAt(row->path, audio::Channel::Effects, ToAudio(at), row->volume);
            return;
        }
    }
    audio::EffectSoundTable::Key key{{}, "SND_DMG", it->second};
    key.events[2] = "CRITYCAL";
    const audio::EffectSoundRow* row = critical ? t.Pick(key, random_) : nullptr;
    key.events[2] = "NORMAL";
    if (row == nullptr) row = t.Pick(key, random_);
    if (row != nullptr) sound_.PlayAt(row->path, audio::Channel::Effects, ToAudio(at), row->volume);
}

void GameAudio::Guarded(std::string_view skill, glm::vec3 at) {
    if (!Ready() || skill.empty()) return;
    const audio::EffectSoundRow* row = tables_->effects.Pick({"PLAYER", "SND_DDMG", skill}, random_);
    if (row == nullptr) row = tables_->effects.Pick({{}, "SND_DDMG", skill}, random_);
    if (row == nullptr) return;
    core::LogInfo("audio: {}'s guard hit: {}", skill, row->path);
    sound_.PlayAt(row->path, audio::Channel::Effects, ToAudio(at), row->volume);
}

const std::string& GameAudio::VoiceObject(std::string_view codename) {
    const auto found = voiceObjects_.find(std::string(codename));
    if (found != voiceObjects_.end()) return found->second;
    const audio::EffectSoundTable& t = tables_->effects;
    std::string object;
    // The words of the codename (CHAR_EU_MAN_WARRIOR, MOB_CH_MANGNYANG_CLON ...).
    std::vector<std::string> words;
    for (std::size_t a = 0; a <= codename.size();) {
        const std::size_t b = std::min(codename.find('_', a), codename.size());
        words.emplace_back(codename.substr(a, b - a));
        a = b + 1;
    }
    if (words.size() >= 4 && words[0] == "CHAR" && (words[2] == "MAN" || words[2] == "WOMAN")) {
        object = (words[2] == "MAN" ? "PCM" : "PCF");
        for (std::size_t i = 3; i < words.size(); ++i) object += "_" + words[i];
        if (!t.HasObject(object)) object.clear();
    } else if (words.size() >= 2) {
        // As it is, then without the two-letter country, each less its last word, until one is known.
        std::vector<std::vector<std::string>> tries{words};
        if (words.size() >= 3 && words[1].size() == 2) {
            std::vector<std::string> w = words;
            w.erase(w.begin() + 1);
            tries.push_back(std::move(w));
        }
        for (std::vector<std::string> w : tries) {
            for (; w.size() >= 2 && object.empty(); w.pop_back()) {
                std::string candidate = w[0];
                for (std::size_t i = 1; i < w.size(); ++i) candidate += "_" + w[i];
                if (t.HasObject(candidate)) object = std::move(candidate);
            }
            if (!object.empty()) break;
        }
        // Then a kin's: without one word after the country (MOB_SD_BLOOD_SANDMAN is MOB_SD_SANDMAN's: ours).
        for (std::size_t drop = 2; object.empty() && words.size() >= 4 && drop + 1 < words.size(); ++drop) {
            std::string candidate;
            for (std::size_t i = 0; i < words.size(); ++i) {
                if (i == drop) continue;
                candidate += (candidate.empty() ? "" : "_") + words[i];
            }
            if (t.HasObject(candidate)) object = std::move(candidate);
        }
    }
    return voiceObjects_.emplace(std::string(codename), std::move(object)).first->second;
}

bool GameAudio::HasSkillSounds(std::string_view skill) const { return Ready() && tables_->effects.HasSkill(skill); }

void GameAudio::Moan(std::string_view codename, bool critical, glm::vec3 at) {
    if (!Ready() || codename.empty()) return;
    const std::string& object = VoiceObject(codename);
    if (object.empty()) return;
    const audio::EffectSoundTable& t = tables_->effects;
    audio::EffectSoundTable::Key key{object, "VOC_MOAN"};
    key.events[0] = critical ? "CRITYCAL" : "NORMAL";
    std::vector<const audio::EffectSoundRow*> voices = t.Find(key);
    if (voices.empty()) voices = t.Find({object, "VOC_MOAN"});
    // A player character's object holds both countries' voices (vc* Chinese, vu* European): its own country's.
    if (codename.starts_with("CHAR_")) {
        const std::string_view prefix = codename.starts_with("CHAR_EU") ? "vu" : "vc";
        std::vector<const audio::EffectSoundRow*> own;
        for (const audio::EffectSoundRow* r : voices) {
            if (FileName(r->path).starts_with(prefix)) own.push_back(r);
        }
        if (!own.empty()) voices = std::move(own);
    }
    if (voices.empty()) return;
    const audio::EffectSoundRow* r = voices[std::uniform_int_distribution<std::size_t>(0, voices.size() - 1)(random_)];
    sound_.PlayAt(r->path, audio::Channel::Effects, ToAudio(at), r->volume);
}

void GameAudio::Death(std::string_view codename, glm::vec3 at) {
    if (!Ready() || codename.empty()) return;
    const std::string& object = VoiceObject(codename);
    if (object.empty()) return;
    const audio::EffectSoundTable& t = tables_->effects;
    std::vector<const audio::EffectSoundRow*> voices = t.Find({object, "VOC_DEATH"});
    // A player character's object holds the Chinese voices (files vc*) and the European ones (vu*):
    // ours, the country's by the codename.
    if (codename.starts_with("CHAR_")) {
        const std::string_view prefix = codename.starts_with("CHAR_EU") ? "vu" : "vc";
        std::vector<const audio::EffectSoundRow*> own;
        for (const audio::EffectSoundRow* r : voices) {
            if (FileName(r->path).starts_with(prefix)) own.push_back(r);
        }
        if (!own.empty()) voices = std::move(own);
    }
    if (!voices.empty()) {
        const audio::EffectSoundRow* r = voices[std::uniform_int_distribution<std::size_t>(0, voices.size() - 1)(random_)];
        sound_.PlayAt(r->path, audio::Channel::Effects, ToAudio(at), r->volume);
    }
    if (const audio::EffectSoundRow* fall = t.Pick({object, "SND_DEATH"}, random_)) sound_.PlayAt(fall->path, audio::Channel::Effects, ToAudio(at), fall->volume);
}

void GameAudio::SetPlace(std::uint16_t region, float x, float z, bool night) {
    if (!sound_.IsOpen() || !Ready()) return;
    const audio::RegionAreaTable::Area* area = tables_->regions.Find(region, x, z);
    const std::string name = area != nullptr ? area->name : std::string();
    if (name == area_ && night == night_) return;
    // Any new area (none too, or one without sounds) releases the old one's ambience at once, as the original's sound
    // body does; the music goes on until an area names another.
    const audio::AreaSounds* sounds = area != nullptr ? tables_->areas.Find(area->name) : nullptr;
    const bool areaChanged = name != area_;
    area_ = name;
    night_ = night;
    if (sounds == nullptr) {
        StopAmbience();
        return;
    }
    if (areaChanged) {
        core::LogInfo("audio: area {} ({}): {}", area_, area->town ? "town" : "field", sounds->music);
        if (sceneMusic_.empty()) PlayMusic(sounds->music);
    }
    std::vector<Ambient> list;
    for (const audio::AreaSounds::Ambient& a : night ? sounds->night : sounds->day) list.push_back({a.path, a.minSeconds, a.maxSeconds});
    StartAmbience(list);
}

// The original's change of music: the track playing fades out (ten steps of a tenth of its volume, one each stream
// step: ours 0.5 s), then the new one starts at full volume.
void GameAudio::PlayMusic(std::string_view file) { sound_.PlayMusic(file, kMusicFadeSeconds, true); }

void GameAudio::SceneMusic(std::string_view file) {
    if (!file.empty()) {
        sceneMusic_ = std::string(file);
        PlayMusic(file);
        return;
    }
    if (sceneMusic_.empty()) return;
    sceneMusic_.clear();
    const audio::AreaSounds* sounds = Ready() && !area_.empty() ? tables_->areas.Find(area_) : nullptr;
    PlayMusic(sounds != nullptr ? sounds->music : std::string());
}

void GameAudio::Silence() {
    sound_.PlayMusic({});
    StopAmbience();
    area_.clear();
    lastSkill_.clear(); // the world's entities go with it
}

void GameAudio::StartAmbience(const std::vector<Ambient>& list) {
    StopAmbience();
    ambience_ = list;
    for (Ambient& a : ambience_) {
        if (a.minSeconds <= 0.0f && a.maxSeconds <= 0.0f) {
            a.loop = sound_.StartLoop(a.path, audio::Channel::Environment);
        } else {
            a.next = now_ + Random(a.minSeconds, a.maxSeconds); // not all at once on arrival
        }
    }
}

void GameAudio::StopAmbience() {
    for (const Ambient& a : ambience_) sound_.StopLoop(a.loop);
    ambience_.clear();
}

} // namespace sr::client
