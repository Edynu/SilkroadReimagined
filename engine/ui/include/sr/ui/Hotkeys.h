#pragma once

// The original's keys: 34 actions the player can bind, with
// their defaults, and the keys whose jobs are fixed. The interface asks which action a key does;
// what the action does belongs to its window or feature.

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

#include "sr/ui/Input.h"

namespace sr::ui {

// The 34 bindable actions, in the original's order (its key options list them so).
enum class KeyAction : std::uint8_t {
    Character,      // C: the main window's Character page
    Inventory,      // I
    Skills,         // S
    Actions,        // A
    Party,          // P
    Quests,         // Q
    Community,      // U: the community window
    WorldMap,       // M
    Berserk,        // Tab: only with the gauge full
    Help,           // H: the game guide
    ShowDroppedItems, // Z, while held
    RightButtonSkill, // X: the underbar's M slot
    SitStand,       // N
    PickUp,         // G
    PetInfo,        // Insert
    PetRide,        // Home
    PetUnsummon,    // Page Up
    PetFollow,      // Delete
    PetAttack,      // End
    PetBehaviour,   // Page Down
    Reply,          // R: answer the last whisper
    AutoPotion,     // T
    NextPet,        // W
    PartyMatching,  // E
    Alchemy,        // Y
    StallNetwork,   // F
    TargetEnemyPlayer, // no default
    LastTarget,     // no default
    AssistTarget,   // no default
    FaceTarget,     // no default
    HideOthers,     // V, while held
    Academy,        // L: the main window's Academy page
    HideAllies,     // no default; on or off
    HideEnemies,    // no default; on or off
    Count
};
constexpr std::size_t kKeyActionCount = static_cast<std::size_t>(KeyAction::Count);

// The text key the original's key options label an action with (UIIT_...).
std::string_view KeyActionLabel(KeyAction action);

// Keys whose jobs are fixed and may not be bound (the original's key options refuse them):
// Escape, the arrows, the digits, F1 to F12 (and Pause, Print Screen, the number pad and the lock
// keys, which our input does not report).
bool KeyIsFixed(Key key);

// Which key does which action: the original's defaults, changed by the player.
class KeyBindings {
public:
    KeyBindings() { Reset(); }
    void Reset(); // the defaults
    static Key Default(KeyAction action);
    Key Get(KeyAction action) const { return keys_[static_cast<std::size_t>(action)]; }
    // Binds a key (taking it from any other action); false for a fixed key.
    bool Set(KeyAction action, Key key);
    // The action a key does, if any.
    std::optional<KeyAction> Find(Key key) const;

private:
    std::array<Key, kKeyActionCount> keys_{};
};

} // namespace sr::ui
