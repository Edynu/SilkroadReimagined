#pragma once

// The game's sound (O3, the start of C11): the sound engine
// the game thread owns, the original's tables (effectsound.txt, effectenvsnd.txt, regioninfo.txt;
// read on a job at the start, sr/audio/SoundTables.h), and what the game asks of it in its own words:
// an interface sound, a swing and its hit, a death, a level up, the music and ambience of the place
// the player is in. Everything here runs on the game thread; until the tables are in, the table
// sounds are skipped.

#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <glm/vec3.hpp>

#include "sr/audio/SoundSystem.h"
#include "sr/core/Jobs.h"
#include "sr/core/Options.h"

namespace sr::assets {
class FileSystem;
struct ItemRow;
} // namespace sr::assets
namespace sr::client {

class GameAudio {
public:
    GameAudio(core::JobSystem& jobs, const assets::FileSystem& files);
    ~GameAudio();
    GameAudio(const GameAudio&) = delete;
    GameAudio& operator=(const GameAudio&) = delete;

    // The output device (false: the game goes on without sound, every call ignored) with the
    // player's levels, and the tables read on a job.
    // `loopbackRate` > 0: on a loopback device at that rate (the videos), its mix taken with Render.
    bool Open(const core::AudioOptions& options, std::string& error, int loopbackRate = 0);
    // The next frames of the mix (stereo 16-bit, 2 samples a frame) when on a loopback device.
    void Render(std::span<std::int16_t> out) { sound_.RenderLoopback(out); }
    bool Loopback() const { return sound_.device().Loopback(); }
    void Shutdown();
    bool IsOpen() const { return sound_.IsOpen(); }
    const audio::SoundSystem& engine() const { return sound_; }

    // The Audio page's three levels and mutes, applied at once (the Options window calls it on every
    // change of a slider or a mute, and again with the kept levels on Cancel).
    void SetLevels(const core::AudioOptions& options);

    // Once a frame on the game thread (after the screen): decodes in, sounds started, ambience.
    void Update(double dt);

    // The ear: the camera's eye, where it looks, its up.
    void SetListener(glm::vec3 eye, glm::vec3 forward, glm::vec3 up);

    // effectsound.txt's interface and item sounds by handle (SND_BUTTON_CLICK, SND_WINDOW_OPEN,
    // SND_WINDOW_CLOSE, SND_LEVUP, SND_PICKUP, SND_POTION, ...; object UI or ITEM), at the listener;
    // `event` narrows a handle that has several (SND_EQUIP by the item's kind).
    void PlayUi(std::string_view handle, std::string_view event = {});
    // An item moved in the inventory or onto the body (the original's snd_equip by the item's kind).
    void Equip(const assets::ItemRow& item);
    // Gold fell on the ground there (SND_DROPITEM GOLD; ours: played when the coins appear).
    void GoldDropped(glm::vec3 at);

    // A fight (sr/audio/SoundTables.h, the skill column): `attacker` used the skill (the key its
    // sounds are under: a player's skill by its base name, a monster's by its codename) at a place;
    // its first swing sound (SND_SWING1 or SND_SWING_S1) and a monster's cry (VOC_SHOUT1) play.
    // `clipSounds`: its clip names its own swing and shout (ClipSound plays them at their moments).
    void Swing(std::uint32_t attacker, std::string_view skill, glm::vec3 at, bool clipSounds = false);
    // Whether effectsound.txt has rows under that skill key.
    bool HasSkillSounds(std::string_view skill) const;
    // It was struck there: its voice (VOC_MOAN of its characterdata codename's object, NORMAL or CRITYCAL).
    void Moan(std::string_view codename, bool critical, glm::vec3 at);
    // One sound of a clip's sound track, by its name in effectsound.txt: the skill's row, a player's by
    // its weapon (`weapon`: PUNCH, SWORD, ...), else the track's own wave, else the character's row.
    void ClipSound(std::string_view handle, std::string_view skill, std::string_view weapon, std::string_view codename, std::string_view wave, glm::vec3 at);
    // Its second swing sound (SND_SWING_S2, "swing #2"), at the shot's release (K8; ours: the
    // original's moment is not traced).
    void SecondSwing(std::string_view skill, glm::vec3 at);
    // A skill stage's sound (skilleffect.txt's SndBegin / SndEnd: a prim\snd\ path) at a place (K8).
    void PlayEffect(std::string_view path, glm::vec3 at);
    // One of its hits landed there (SND_DMG of the attacker's last skill; a critical one prefers the
    // CRITYCAL rows). A critical sounds SND_CRIDMG first, as the original's: the skill's own rows, else a player's
    // (`player`) PLAYER rows tied to no skill (batCriHit; in berserk, `berserk`, the HWAN row's batHwanCriHit).
    void Hit(std::uint32_t attacker, bool critical, glm::vec3 at, bool player = false, bool berserk = false);
    // A blocked hit (effectsound's PLAYER SND_BLOCKING): by the attacker's weapon
    // ("BOW", "CROSSBOW"; empty: any other) and whether it was critical.
    void Blocked(std::string_view weapon, bool critical, glm::vec3 at);
    // Someone died there: the voice (VOC_DEATH) and the fall (SND_DEATH) of its characterdata
    // codename's object (ours, measured over the tables: CHAR_*_MAN_X is PCM_X, CHAR_*_WOMAN_X PCF_X,
    // a monster MOB_CC_X... its codename less the country and the trailing parts until one is in).
    void Death(std::string_view codename, glm::vec3 at);

    // Where the player is: the region and the place in it, and whether it is night. The area there
    // (regioninfo.txt) picks the music (effectenvsnd.txt, Music.pk2) and the ambience by day or by
    // night; a place in no area keeps what plays.
    void SetPlace(std::uint16_t region, float x, float z, bool night);
    // A Music.pk2 file by name (cross-faded), or silence with an empty name. Ambience off too.
    void PlayMusic(std::string_view file);
    static constexpr float kMusicFadeSeconds = 5.0f; // the old track's fade-out before the new one starts
    // A quest scene's music (S_SetBGM): it plays over the area's, which no change of place replaces until it ends
    // (an empty name: the area's music again).
    void SceneMusic(std::string_view file);
    void Silence();

private:
    struct Tables;
    struct Ambient {
        std::string path;
        float minSeconds = 0, maxSeconds = 0;
        double next = 0.0;      // a one-shot's next play
        std::uint32_t loop = 0; // a loop's id
    };
    bool Ready() const; // the tables are in
    void StartAmbience(const std::vector<Ambient>& list);
    void StopAmbience();
    const std::string& VoiceObject(std::string_view codename);
    float Random(float low, float high);

    core::JobSystem& jobs_;
    const assets::FileSystem& files_;
    audio::SoundSystem sound_;
    std::shared_ptr<Tables> tables_;
    core::JobCounter tablesJob_;
    std::minstd_rand random_{0x5EED};
    double now_ = 0.0;
    std::unordered_map<std::uint32_t, std::string> lastSkill_; // attacker -> its last skill's key
    std::unordered_map<std::string, std::string> voiceObjects_; // codename -> effectsound object
    // The place's sound.
    std::string area_;
    std::string sceneMusic_; // SceneMusic's track while it plays
    bool night_ = false;
    std::vector<Ambient> ambience_;
};

} // namespace sr::client
