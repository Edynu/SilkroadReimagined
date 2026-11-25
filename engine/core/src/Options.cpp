#include "sr/core/Options.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace sr::core {

namespace {

const char* const kQuality[] = {"UIIT_STT_LOW", "UIIT_STT_MIDDLE", "UIIT_STT_HIGH", "UIIT_STT_GRAPHIC_QUALITY_CONTROL_WAR",
                                "UIIT_STT_GRAPHIC_QUALITY_CONTROL_USER"};
const char* const kShadow[] = {"UIIT_STT_NONE", "UIIT_STT_CIRCLE", "UIIT_STT_DETAIL"};
const char* const kRange[] = {"1", "2", "3", "4", "5"};
const char* const kOffOn[] = {"UIIT_STT_OFF", "UIIT_STT_ON"};
const char* const kLowMidHigh[] = {"UIIT_STT_LOW", "UIIT_STT_MIDDLE", "UIIT_STT_HIGH"};
const char* const kEffects[] = {"UIIT_STT_EFFECT_ALL_OFF", "UIIT_STT_EFFECT_DAMEGE_ON", "UIIT_STT_EFFECT_ALL_ON"};
const char* const kClothes[] = {"UIIT_STT_OPTIMIZE_A_E_CLOTHES", "UIIT_CTL_WARENETWORK_DETAIL_NORMAL"};

const VideoOptions::Row kVideoRows[] = {
    {0, "UILM_TEXT_GRAPHIC_QUALITY", 5, kQuality},
    {1, "UIIT_STT_SHADOW_DETAIL", 3, kShadow},
    {2, "UIIT_STT_SCENERY_SIGHT_RANGE", 5, kRange},
    {3, "UIIT_STT_CHAR_SIGHT_RANGE", 5, kRange},
    {4, "UIIT_STT_WATER_REFLECTION", 2, kOffOn},
    {5, "UIIT_STT_WATER_DETAIL", 3, kLowMidHigh},
    {6, "UIIT_STT_METAL_DETAIL", 2, kOffOn},
    {7, "UIIT_STT_LIGHT_EFFECT", 2, kOffOn},
    {8, "UIIT_STT_FILTERING", 2, kOffOn},
    {9, "UIIT_STT_TEXTER_DETAIL", 3, kLowMidHigh},
    {10, "UIIT_STT_LENS_FLAIR", 2, kOffOn},
    {11, "UIIT_STT_BLOOM_EFFECT", 2, kOffOn},
    {12, "UIIT_STT_DYNAMIC_ANIMATION", 2, kOffOn},
    {13, "UIIT_STT_EFFECT_QUALITY", 3, kEffects},
    {15, "UIIT_STT_OPTIMIZE_A_E_EXPRESSION", 2, kClothes},
};

// The four presets (low, middle, high, "control war"); the custom bank is the fifth choice.
constexpr VideoOptions::Record kPresets[4] = {
    {0, 1, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0},
    {1, 1, 2, 2, 0, 1, 1, 1, 1, 2, 1, 1, 1, 2, 0, 0},
    {2, 2, 4, 4, 1, 2, 1, 1, 1, 2, 1, 1, 1, 2, 0, 0},
    {3, 0, 2, 4, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0},
};
// The original's default record: Middle, the shadow a circle.
constexpr VideoOptions::Record kDefaultRecord = {1, 1, 2, 2, 0, 1, 1, 1, 1, 2, 1, 0, 1, 2, 0, 0};

const char* const kInputLabels[InputOptions::kBindings] = {
    "UIIT_STT_CHARACTER", "UIIT_STT_INVENTORY", "UIIT_STT_SKILL", "UIIT_STT_ACTION", "UIIT_STT_PARTY", "UIIT_STT_QUEST",
    "UIIT_STT_COMMUNITY", "UIIT_STT_WORLDMAP", "UIIT_STT_HWANMODE", "UIIT_STT_HELP", "UIIT_STT_VIEW_DROPPED_ITEM",
    "UIIT_STT_MOUSE_RIGHT_BUTTON", "UIIT_CTL_TOG_SIT_STAND_TT", "UIIT_CTL_AUTOGET_TT", "UIIT_STT_OPTION_COS_INFO",
    "UIIT_STT_OPTION_COS_RIDE", "UIIT_STT_OPTION_COS_SUMMONCANCEL", "UIIT_STT_OPTION_COS_FOLLOW", "UIIT_STT_OPTION_COS_ATTACK",
    "UIIT_STT_OPTION_COS_AI", "UIIT_CTL_REPLY_TT", "UIIT_PAG_MACROPOTION_TITLE", "UIIT_STT_OPTION_COS",
    "UIIT_STT_TOGGLE_PARTYMATCH", "UIIT_CTL_ALCHEMYBOX", "UIIT_STT_WARENETWORK_TITLE", "UIIT_STT_ENEMYUSER_SELECT_KEY",
    "UIIT_STT_LATEST_TARGET", "UIIT_STT_ASSIST_TARGET", "UIIT_STT_FACE_TARGET", "UIIT_STT_USER_MOSTER_BLIND",
    "UIIT_CTL_TC_TRAININGCAMP", "UIIT_STT_BLIND_ALLY", "UIIT_STT_BLIND_ENEMY"};

constexpr std::array<int, InputOptions::kBindings> kDefaultKeys = {
    67, 73, 83, 65, 80, 81, 85, 77, 9, 72, 90, 88, 78, 71, 45, 36, 33, 46, 35, 34, 82, 84, 87, 69, 89, 70, 0, 0, 0, 0, 86, 76, 0, 0};

const GameOptions::Row kGameRows[] = {
    {"ownName", "UIIT_STT_ONESELF_SIGN", true, true, 0},
    {"playerNames", "UIIT_STT_OTHER_CHAR_SIGN", true, true, 0},
    {"monsterNames", "UIIT_STT_MONSTER_SIGN", true, true, 0},
    {"npcNames", "UIIT_STT_NPC_SIGN", true, true, 0},
    {"guildNames", "UIIT_STT_GUILDVIEW_SIGN", true, true, 0},
    {"fortressNames", "UIIT_STT_FORT_USERMARK_NAME_SIGN", true, true, 0},
    {"ownStatus", "UIIT_STT_QUICKSTATE_OWNER", false, true, 0},
    {"cosStatus", "UIIT_STT_QUICKSTATE_COS", false, true, 0},
    {"partyStatus", "UIIT_STT_QUICKSTATE_PARTY", false, true, 0},
    {"monsterStatus", "UIIT_STT_QUICKSTATE_MONSTER", false, true, 0},
    {"guide", "UIIT_STT_GUIDEVIEW_SIGN", true, true, 1},
    {"partyInvites", "UIIT_STT_INVITE_PARTY_SIGN", true, true, 1},
    {"eventGuide", "UIIT_STT_EVENTGUIDE_SIGN", true, true, 1},
    {"exchangeRequests", "UIIT_STT_REQUEST_EXCHANGE_SIGN", true, true, 1},
    {"whispers", "UIIT_STT_GET_WHISPER_SIGN", true, true, 1},
    {"hideSystemMessages", "UIIT_MSG_SYSTEM_WND_HIDEMSG", true, true, 1},
    {"partyBuffs", "UIIT_MSG_QUICK_PARTY_CHARACTER_BUFF", true, true, 1},
    {"windowMode", "UIIT_STT_WINDOWMODE_CHANGE", false, false, 1},
    {"highQualityIntro", "UIIT_STT_INTRO_USER_MOSTER_BLIND", true, true, 1},
    {"hideSilkCos", "UIIT_STT_BLIND_SILK_COS", false, false, 1},
    {"hpWarning", "UIIT_STT_CAUTION_HP", false, true, 1},
    {"mpWarning", "UIIT_STT_CAUTION_MP", false, true, 1},
    {"warningSound", "UIIT_STT_CAUTION_SOUND", false, true, 1},
    // The right button's (the original's Default: the short cut off, the camera's method 1): a right click on a player opens
    // its menu; method 1 a turn past 5 pixels opens none, method 2 the cursor goes back where the turn began (one or the other).
    {"actionShortcut", "UIIT_CTL_ACTION_LIST_ACTIVE", false, false, 1},
    {"cameraMethod1", "UIIT_CTL_CAMERA_ROTATE_METHOD1", true, true, 1},
    {"cameraMethod2", "UIIT_CTL_CAMERA_ROTATE_METHOD2", false, false, 1},
    // Ours (M-32): a click on an item of the player's own offer in the exchange takes it back; off, the original's way, it
    // is dragged back to the bag. Its label is ours (no text key).
    {"exchangeClickBack", "Exchange click-back", false, false, 1},
};
constexpr int kGameRowCount = static_cast<int>(sizeof(kGameRows) / sizeof(kGameRows[0]));
static_assert(kGameRowCount <= GameOptions::kMax);

std::vector<std::string_view> SplitOn(std::string_view s, char c) {
    std::vector<std::string_view> out;
    for (std::size_t at = 0; at <= s.size();) {
        std::size_t next = s.find(c, at);
        if (next == std::string_view::npos) next = s.size();
        out.push_back(s.substr(at, next - at));
        at = next + 1;
    }
    return out;
}

bool ParseInt(std::string_view s, int& out) {
    if (s.empty() || s.size() > 11) return false;
    char* end = nullptr;
    const std::string copy(s);
    const long v = std::strtol(copy.c_str(), &end, 10);
    if (end == copy.c_str() || *end != '\0' || v < -1000000 || v > 1000000) return false;
    out = static_cast<int>(v);
    return true;
}

bool ParseRecord(std::string_view s, VideoOptions::Record& out) {
    const auto parts = SplitOn(s, ',');
    if (parts.size() != VideoOptions::kSlots) return false;
    VideoOptions::Record r{};
    for (int i = 0; i < VideoOptions::kSlots; ++i) {
        if (!ParseInt(parts[static_cast<std::size_t>(i)], r[static_cast<std::size_t>(i)])) return false;
    }
    if (r[14] != 0) return false;
    for (int i = 0; i < VideoOptions::kSlots; ++i) {
        const int entries = VideoOptions::Entries(i);
        if (r[static_cast<std::size_t>(i)] < 0 || (entries == 0 ? r[static_cast<std::size_t>(i)] != 0 : r[static_cast<std::size_t>(i)] >= entries)) return false;
    }
    out = r;
    return true;
}

std::string RecordText(const VideoOptions::Record& r) {
    std::string out;
    for (int i = 0; i < VideoOptions::kSlots; ++i) out += (i ? "," : "") + std::to_string(r[static_cast<std::size_t>(i)]);
    return out;
}

} // namespace

// ---- video

const VideoOptions::Row* VideoOptions::Rows(int& count) {
    count = static_cast<int>(sizeof(kVideoRows) / sizeof(kVideoRows[0]));
    return kVideoRows;
}

int VideoOptions::Entries(int slot) {
    for (const Row& r : kVideoRows) {
        if (r.slot == slot) return r.entries;
    }
    return 0;
}

VideoOptions VideoOptions::Defaults() {
    VideoOptions v;
    v.records = {kDefaultRecord, kDefaultRecord};
    v.custom = v.records;
    return v;
}

void VideoOptions::ApplyLook(int look) {
    const int q = std::clamp(look, 0, 3);
    sunShadows = q == 0 ? 0 : q == 3 ? 4 : q; // cascaded shadows low / medium / ultra
    lightEffects = q == 0 ? 0 : q == 1 ? 1 : 2;
    ambientOcclusion = q == 0 ? 0 : q == 1 ? 1 : 2;
    water = q == 0 ? 0 : 1;
    lensFlare = q == 0 ? 1 : 2;
    bloom = q == 0 ? 1 : 2;
    toneMapping = q == 0 ? 0 : 1;
    colourGrading = q == 0 ? 0 : 1;
    weather = q == 0 ? 1 : 2;
    volumetricFog = q < 3 ? 0 : 1;
    clouds = q < 3 ? 0 : 1;
    softParticles = q == 0 ? 0 : 1;
    foliageSway = q == 0 ? 0 : 1;
    characterLighting = q == 0 ? 0 : 1;
    antiAliasing = q == 0 ? 0 : 1;
    viewDistance = q; // the original's, x2, x3, x4
    static constexpr int kShadowDistance[4] = {1, 1, 2, 3}; // Low medium, High far, Ultra very far
    shadowDistance = kShadowDistance[q];
    shadowCulling = q == 1 ? 2 : 1;
    static constexpr int kDetail[4] = {1, 0, 2, 3};
    detailDistance = kDetail[q];
    static constexpr int kLightReach[4] = {1, 0, 1, 2};
    lightReach = kLightReach[q];
}

int VideoOptions::LookByName(std::string_view name) {
    constexpr std::string_view kNames[] = {"classic", "modern-low", "modern-high", "modern-ultra"};
    for (int i = 0; i < 4; ++i) {
        if (name == kNames[i]) return i;
    }
    return -1;
}

bool VideoOptions::Change(int slot, int value) {
    const int entries = Entries(slot);
    if (entries == 0 || value < 0 || value >= entries) return false;
    const auto a = static_cast<std::size_t>(active);
    if (slot == 0) {
        if (records[a][0] == 4) custom[a] = records[a];
        records[a] = value == 4 ? custom[a] : kPresets[value];
        records[a][0] = value;
    } else {
        if (records[a][static_cast<std::size_t>(slot)] == value) return true;
        if (records[a][0] == 4) custom[a] = records[a];
        custom[a][static_cast<std::size_t>(slot)] = value;
        custom[a][0] = 4;
        records[a] = custom[a];
    }
    return true;
}

const std::array<VideoOptions::ExtraRow, 21>& VideoOptions::ExtraRows() {
    static const std::array<ExtraRow, 21> kRows = {{
        {"sunShadows", &VideoOptions::sunShadows, 4, 0},
        {"lightEffects", &VideoOptions::lightEffects, 2, 0},
        {"lensFlare", &VideoOptions::lensFlare, 2, 1},
        {"bloom", &VideoOptions::bloom, 2, 1},
        {"viewDistance", &VideoOptions::viewDistance, 3, 0},
        {"shadowDistance", &VideoOptions::shadowDistance, 3, 1},
        {"shadowCulling", &VideoOptions::shadowCulling, 2, 1},
        {"lightReach", &VideoOptions::lightReach, 2, 1},
        {"detailDistance", &VideoOptions::detailDistance, 3, 1},
        {"toneMapping", &VideoOptions::toneMapping, 2, 0},
        {"colourGrading", &VideoOptions::colourGrading, 1, 0},
        {"ambientOcclusion", &VideoOptions::ambientOcclusion, 2, 0},
        {"softParticles", &VideoOptions::softParticles, 1, 0},
        {"foliageSway", &VideoOptions::foliageSway, 1, 0},
        {"characterLighting", &VideoOptions::characterLighting, 1, 0},
        {"anisotropy", &VideoOptions::anisotropy, 4, 3},
        {"water", &VideoOptions::water, 1, 0},
        {"weather", &VideoOptions::weather, 2, 1},
        {"volumetricFog", &VideoOptions::volumetricFog, 2, 0},
        {"clouds", &VideoOptions::clouds, 1, 0},
        {"effectGlow", &VideoOptions::effectGlow, 10, 10},
    }};
    return kRows;
}

void VideoOptions::ResetActive() {
    const auto a = static_cast<std::size_t>(active);
    records[a] = kDefaultRecord;
    custom[a] = kDefaultRecord;
    brightness[a] = 2;
    frameLimit = 0;
    antiAliasing = 0;
    for (const ExtraRow& row : ExtraRows()) this->*row.field = row.initial;
}

float VideoOptions::SceneryDistance() const {
    static constexpr float kDistance[5] = {1500.0f, 2500.0f, 3500.0f, 4500.0f, 5500.0f};
    return kDistance[std::clamp(current()[2], 0, 4)];
}

const std::array<std::array<int, 2>, 11>& VideoOptions::DisplaySizes() {
    static const std::array<std::array<int, 2>, 11> sizes = {{{0, 0}, {1920, 1080}, {1680, 1050}, {1600, 900}, {1440, 900}, {1366, 768},
                                                              {1280, 1024}, {1280, 800}, {1280, 720}, {1024, 768}, {800, 600}}};
    return sizes;
}

const std::array<int, 11>& VideoOptions::FrameLimits() {
    static const std::array<int, 11> limits = {30, 45, 60, 75, 90, 120, 144, 165, 240, 0, -1};
    return limits;
}

// ---- audio

float AudioOptions::Gain(int level, bool muted) {
    // Level L is 20 L - 2000 hundredths of a dB: 0 is -20 dB, not silence; only the mute is silent.
    if (muted) return 0.0f;
    const int l = std::clamp(level, 0, 100);
    return std::pow(10.0f, (20.0f * static_cast<float>(l) - 2000.0f) / 2000.0f);
}

// ---- input

InputOptions InputOptions::Defaults() {
    InputOptions o;
    o.keys = kDefaultKeys;
    return o;
}

const char* InputOptions::Label(int slot) { return slot >= 0 && slot < kBindings ? kInputLabels[slot] : ""; }

bool InputOptions::CaptureAllowed(int vk) {
    if (vk <= 0 || vk > 255) return false;
    if (vk >= 112 && vk <= 123) return false; // F1..F12: the original refuses them
    static constexpr int kRefused[] = {12,  19,  21,  23,  24,  25,  27,  37,  38,  39,  40,  44,  48,  49,  50,  51,  52,  53,
                                       54,  55,  56,  57,  95,  96,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108,
                                       109, 110, 111, 144, 145, 229};
    return std::find(std::begin(kRefused), std::end(kRefused), vk) == std::end(kRefused);
}

bool InputOptions::Capture(int slot, int vk) {
    if (slot < 0 || slot >= kBindings || !CaptureAllowed(vk)) return false;
    for (int i = 0; i < kBindings; ++i) {
        if (i == slot) {
            keys[static_cast<std::size_t>(i)] = vk;
        } else if (keys[static_cast<std::size_t>(i)] == vk) {
            keys[static_cast<std::size_t>(i)] = 0;
        }
    }
    return true;
}

std::string InputOptions::KeyName(int vk) {
    switch (vk) {
    case 0: return " ";
    case 8: return "Back";
    case 9: return "Tab";
    case 13: return "Enter";
    case 16: return "Shift";
    case 17: return "Ctrl";
    case 18: return "Alt";
    case 20: return "C/L";
    case 32: return "Space";
    case 33: return "PgUp";
    case 34: return "PgDn";
    case 35: return "End";
    case 36: return "Home";
    case 45: return "Ins";
    case 46: return "Del";
    case 186: return ";";
    case 187: return "=";
    case 188: return ",";
    case 189: return "-";
    case 190: return ".";
    case 191: return "/";
    case 192: return "`";
    case 219: return "[";
    case 220: return "\\";
    case 221: return "]";
    case 222: return "'";
    default: break;
    }
    if (vk >= 112 && vk <= 123) return "F" + std::to_string(vk - 111);
    return std::string(1, static_cast<char>(vk & 255));
}

// ---- game

const GameOptions::Row* GameOptions::Rows(int& count) {
    count = kGameRowCount;
    return kGameRows;
}

GameOptions GameOptions::Initial() {
    GameOptions g;
    for (int i = 0; i < kGameRowCount; ++i) g.on[static_cast<std::size_t>(i)] = kGameRows[i].initial;
    return g;
}

GameOptions GameOptions::Reset() {
    GameOptions g;
    for (int i = 0; i < kGameRowCount; ++i) g.on[static_cast<std::size_t>(i)] = kGameRows[i].reset;
    return g;
}

bool GameOptions::Get(std::string_view key) const {
    for (int i = 0; i < kGameRowCount; ++i) {
        if (key == kGameRows[i].key) return on[static_cast<std::size_t>(i)];
    }
    return false;
}

void GameOptions::Set(std::string_view key, bool value) {
    for (int i = 0; i < kGameRowCount; ++i) {
        if (key == kGameRows[i].key) on[static_cast<std::size_t>(i)] = value;
    }
    // The camera's two methods are one choice (the original's pair: one on, the other off).
    const std::string_view other = key == "cameraMethod1" ? "cameraMethod2" : key == "cameraMethod2" ? "cameraMethod1" : "";
    for (int i = 0; i < kGameRowCount && !other.empty(); ++i) {
        if (other == kGameRows[i].key) on[static_cast<std::size_t>(i)] = !value;
    }
}

// ---- file

std::string Options::Serialize() const {
    std::ostringstream out;
    out << "# SroReimagined options (the Option window writes this file)\n";
    out << "video.active=" << video.active << "\n";
    out << "video.record0=" << RecordText(video.records[0]) << "\n";
    out << "video.record1=" << RecordText(video.records[1]) << "\n";
    out << "video.custom0=" << RecordText(video.custom[0]) << "\n";
    out << "video.custom1=" << RecordText(video.custom[1]) << "\n";
    out << "video.display=" << video.displayWidth << "x" << video.displayHeight << "\n";
    out << "video.frameLimit=" << video.frameLimit << "\n";
    out << "video.brightness0=" << video.brightness[0] << "\nvideo.brightness1=" << video.brightness[1] << "\n";
    out << "video.windowMode=" << video.windowMode << "\nvideo.antiAliasing=" << video.antiAliasing << "\n";
    for (const VideoOptions::ExtraRow& row : VideoOptions::ExtraRows()) out << "video." << row.key << "=" << video.*row.field << "\n";
    out << "audio.bgm=" << audio.bgm << "\naudio.effects=" << audio.effects << "\naudio.environment=" << audio.environment << "\n";
    out << "audio.muteBgm=" << audio.muteBgm << "\naudio.muteEffects=" << audio.muteEffects
        << "\naudio.muteEnvironment=" << audio.muteEnvironment << "\n";
    out << "camera.sight=" << static_cast<int>(sight) << "\n";
    out << "input.mouse=" << input.mouseMode << "\n";
    out << "input.keys=";
    for (int i = 0; i < InputOptions::kBindings; ++i) out << (i ? "," : "") << input.keys[static_cast<std::size_t>(i)];
    out << "\n";
    for (int i = 0; i < kGameRowCount; ++i) out << "game." << kGameRows[i].key << "=" << game.on[static_cast<std::size_t>(i)] << "\n";
    out << "game.beginnerMark=" << game.beginnerMark << "\n";
    out << "online.players=" << online.players << "\nonline.monsters=" << online.monsters << "\nonline.radius=" << online.radius << "\n";
    out << "beta.movement=" << (beta.movement == BetaOptions::Movement::Pathfinding ? "pathfinding" : "classic") << "\n";
    out << "beta.names=" << (beta.namesFar ? "far" : "original") << "\n";
    return out.str();
}

Options Options::Parse(std::string_view text) {
    Options o;
    for (const std::string_view raw : SplitOn(text, '\n')) {
        std::string_view line = raw;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.empty() || line.front() == '#') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view key = line.substr(0, eq), value = line.substr(eq + 1);
        int n = 0;
        const auto flag = [&](bool& out) {
            if (ParseInt(value, n) && (n == 0 || n == 1)) out = n == 1;
        };
        const auto level = [&](int& out) {
            if (ParseInt(value, n) && n >= 0 && n <= 100) out = n;
        };
        if (key == "video.active") {
            if (ParseInt(value, n) && (n == 0 || n == 1)) o.video.active = n;
        } else if (key == "video.record0" || key == "video.record1") {
            VideoOptions::Record r;
            if (ParseRecord(value, r)) o.video.records[key.back() == '1' ? 1 : 0] = r;
        } else if (key == "video.custom0" || key == "video.custom1") {
            VideoOptions::Record r;
            if (ParseRecord(value, r)) o.video.custom[key.back() == '1' ? 1 : 0] = r;
        } else if (key == "video.display") {
            const auto parts = SplitOn(value, 'x');
            int w = 0, h = 0;
            if (parts.size() == 2 && ParseInt(parts[0], w) && ParseInt(parts[1], h)) {
                for (const auto& size : VideoOptions::DisplaySizes()) {
                    if (size[0] == w && size[1] == h) {
                        o.video.displayWidth = w;
                        o.video.displayHeight = h;
                    }
                }
            }
        } else if (key == "video.brightness0" || key == "video.brightness1") {
            if (ParseInt(value, n) && n >= 0 && n <= 4) o.video.brightness[key.back() == '1' ? 1 : 0] = n;
        } else if (key == "video.frameLimit") {
            if (ParseInt(value, n)) {
                for (const int limit : VideoOptions::FrameLimits()) {
                    if (limit == n) o.video.frameLimit = n;
                }
            }
        } else if (key == "video.windowMode") {
            if (ParseInt(value, n) && n >= 0 && n <= 2) o.video.windowMode = n;
        } else if (key == "video.antiAliasing") {
            if (ParseInt(value, n) && n >= 0 && n < VideoOptions::kAntiAliasingChoices) o.video.antiAliasing = n;
        } else if (key.starts_with("video.") && std::any_of(VideoOptions::ExtraRows().begin(), VideoOptions::ExtraRows().end(),
                                                             [&](const VideoOptions::ExtraRow& row) { return key.substr(6) == row.key; })) {
            for (const VideoOptions::ExtraRow& row : VideoOptions::ExtraRows()) {
                if (key.substr(6) == row.key && ParseInt(value, n) && n >= 0 && n <= row.max) o.video.*row.field = n;
            }
        } else if (key == "audio.bgm") {
            level(o.audio.bgm);
        } else if (key == "audio.effects") {
            level(o.audio.effects);
        } else if (key == "audio.environment") {
            level(o.audio.environment);
        } else if (key == "audio.muteBgm") {
            flag(o.audio.muteBgm);
        } else if (key == "audio.muteEffects") {
            flag(o.audio.muteEffects);
        } else if (key == "audio.muteEnvironment") {
            flag(o.audio.muteEnvironment);
        } else if (key == "online.players") {
            if (ParseInt(value, n) && n >= 0 && n <= OnlineOptions::kMax) o.online.players = n;
        } else if (key == "online.monsters") {
            if (ParseInt(value, n) && n >= 0 && n <= OnlineOptions::kMax) o.online.monsters = n;
        } else if (key == "beta.movement") {
            if (value == "pathfinding") o.beta.movement = BetaOptions::Movement::Pathfinding;
            else if (value == "classic") o.beta.movement = BetaOptions::Movement::Classic;
        } else if (key == "beta.names") {
            if (value == "far") o.beta.namesFar = true;
            else if (value == "original") o.beta.namesFar = false;
        } else if (key == "online.radius") {
            if (ParseInt(value, n) && n >= OnlineOptions::kMinRadius && n <= OnlineOptions::kMaxRadius) o.online.radius = n;
        } else if (key == "camera.sight") {
            if (ParseInt(value, n) && n >= 0 && n <= 2) o.sight = static_cast<Sight>(n);
        } else if (key == "input.mouse") {
            if (ParseInt(value, n) && (n == 0 || n == 1)) o.input.mouseMode = n;
        } else if (key == "input.keys") {
            const auto parts = SplitOn(value, ',');
            if (parts.size() == InputOptions::kBindings) {
                std::array<int, InputOptions::kBindings> keys{};
                bool ok = true;
                for (int i = 0; i < InputOptions::kBindings && ok; ++i) {
                    ok = ParseInt(parts[static_cast<std::size_t>(i)], keys[static_cast<std::size_t>(i)]) &&
                         keys[static_cast<std::size_t>(i)] >= 0 && keys[static_cast<std::size_t>(i)] <= 255;
                }
                // A key is on one command only.
                for (int i = 0; i < InputOptions::kBindings && ok; ++i) {
                    for (int j = i + 1; j < InputOptions::kBindings && ok; ++j) {
                        ok = keys[static_cast<std::size_t>(i)] == 0 || keys[static_cast<std::size_t>(i)] != keys[static_cast<std::size_t>(j)];
                    }
                }
                if (ok) o.input.keys = keys;
            }
        } else if (key.starts_with("game.")) {
            const std::string_view name = key.substr(5);
            if (name == "beginnerMark") {
                flag(o.game.beginnerMark);
            } else if (ParseInt(value, n) && (n == 0 || n == 1)) {
                for (const GameOptions::Row& row : kGameRows) {
                    if (name == row.key) o.game.Set(name, n == 1);
                }
            }
        }
    }
    return o;
}

bool Options::Load(const std::string& path, Options& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream text;
    text << in.rdbuf();
    out = Parse(text.str());
    return true;
}

bool Options::Save(const std::string& path) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << Serialize();
    return static_cast<bool>(out);
}

} // namespace sr::core
