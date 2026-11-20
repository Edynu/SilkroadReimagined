#include "sr/ui/Hotkeys.h"

namespace sr::ui {

namespace {

struct Row {
    Key key;
    std::string_view label;
};

// The defaults (the original's) and the key options' labels, by action.
constexpr std::array<Row, kKeyActionCount> kRows{{
    {Key::C, "UIIT_STT_CHARACTER"},
    {Key::I, "UIIT_STT_INVENTORY"},
    {Key::S, "UIIT_STT_SKILL"},
    {Key::A, "UIIT_STT_ACTION"},
    {Key::P, "UIIT_STT_PARTY"},
    {Key::Q, "UIIT_STT_QUEST"},
    {Key::U, "UIIT_STT_COMMUNITY"},
    {Key::M, "UIIT_STT_WORLDMAP"},
    {Key::Tab, "UIIT_STT_HWANMODE"},
    {Key::H, "UIIT_STT_HELP"},
    {Key::Z, "UIIT_STT_VIEW_DROPPED_ITEM"},
    {Key::X, "UIIT_STT_MOUSE_RIGHT_BUTTON"},
    {Key::N, "UIIT_CTL_TOG_SIT_STAND_TT"},
    {Key::G, "UIIT_CTL_AUTOGET_TT"},
    {Key::Insert, "UIIT_STT_OPTION_COS_INFO"},
    {Key::Home, "UIIT_STT_OPTION_COS_RIDE"},
    {Key::PageUp, "UIIT_STT_OPTION_COS_SUMMONCANCEL"},
    {Key::Delete, "UIIT_STT_OPTION_COS_FOLLOW"},
    {Key::End, "UIIT_STT_OPTION_COS_ATTACK"},
    {Key::PageDown, "UIIT_STT_OPTION_COS_AI"},
    {Key::R, "UIIT_CTL_REPLY_TT"},
    {Key::T, "UIIT_PAG_MACROPOTION_TITLE"},
    {Key::W, "UIIT_STT_OPTION_COS"},
    {Key::E, "UIIT_STT_TOGGLE_PARTYMATCH"},
    {Key::Y, "UIIT_CTL_ALCHEMYBOX"},
    {Key::F, "UIIT_STT_WARENETWORK_TITLE"},
    {Key::None, "UIIT_STT_ENEMYUSER_SELECT_KEY"},
    {Key::None, "UIIT_STT_LATEST_TARGET"},
    {Key::None, "UIIT_STT_ASSIST_TARGET"},
    {Key::None, "UIIT_STT_FACE_TARGET"},
    {Key::V, "UIIT_STT_USER_MOSTER_BLIND"},
    {Key::L, "UIIT_CTL_TC_TRAININGCAMP"},
    {Key::None, "UIIT_STT_BLIND_ALLY"},
    {Key::None, "UIIT_STT_BLIND_ENEMY"},
}};

} // namespace

std::string_view KeyActionLabel(KeyAction action) { return kRows[static_cast<std::size_t>(action)].label; }

bool KeyIsFixed(Key key) {
    return key == Key::Escape || (key >= Key::Num0 && key <= Key::Num9) ||
           (key >= Key::Left && key <= Key::Down) || (key >= Key::F1 && key <= Key::F12);
}

void KeyBindings::Reset() {
    for (std::size_t i = 0; i < kKeyActionCount; ++i) keys_[i] = kRows[i].key;
}

Key KeyBindings::Default(KeyAction action) { return kRows[static_cast<std::size_t>(action)].key; }

bool KeyBindings::Set(KeyAction action, Key key) {
    if (KeyIsFixed(key)) return false;
    for (Key& k : keys_) {
        if (k == key) k = Key::None;
    }
    keys_[static_cast<std::size_t>(action)] = key;
    return true;
}

std::optional<KeyAction> KeyBindings::Find(Key key) const {
    if (key == Key::None) return std::nullopt;
    for (std::size_t i = 0; i < kKeyActionCount; ++i) {
        if (keys_[i] == key) return static_cast<KeyAction>(i);
    }
    return std::nullopt;
}

} // namespace sr::ui
