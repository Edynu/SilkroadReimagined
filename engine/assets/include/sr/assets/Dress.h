#pragma once

// How a player character looks, from the game's tables, as the original dresses one: the body from characterdata, the
// worn items from itemdata
// (an axe or dagger in the right hand by its "_r" model), the default garments on an empty chest or
// legs, the stance the weapon puts the body in, and the body shape byte (height and volume).

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sr::assets {

class FileSystem;
class CharacterTable;
class ItemTable;
struct ItemRow;
struct ObjectRow;
struct EquipGlow;
class EquipGlowTable;

// The body shape byte: the height in the low nibble, the volume in the high (each 0..4; 2 is the
// middle the create screens start at).
inline constexpr std::uint8_t kNeutralShape = 0x22;
inline constexpr std::uint8_t PackShape(int height, int volume) {
    return static_cast<std::uint8_t>((height & 0xF) | ((volume & 0xF) << 4));
}

struct DressedCharacter {
    std::string body;               // the figure's .bsr
    std::vector<std::string> items; // worn item .bsr resources, in wear order (garments first)
    std::string stance = "default"; // the animation set the body stands in
    float height = 1.0f;            // a uniform scale
    std::vector<std::pair<std::string, float>> volume; // bone name -> thickness across its length
    bool europe = false, female = false;
    // The body's regions a shown piece covers (charactervisualchange's Slot bits: 0x02 the mail's, 0x04 the shoulders',
    // 0x08 the hands', 0x10 the pants', 0x20 the boots'): which pieces are drawn and whether the default garments are.
    // The body's own meshes are hidden only by the pieces' cover tables (AssembleOutfit).
    std::uint32_t covered = 0;
    // Body and items as one asset key ("body|item|item"), the asset store's outfit form.
    std::string Key() const;
};

// Equipment slot of a worn item as the original numbers them (0 head, 1 chest, 2 shoulders,
// 3 hands, 4 legs, 5 feet, 6 weapon, 7 shield, 8 the job suit, TID 3/1/7); -1 for an item not drawn on the body.
int EquipSlot(const ItemRow& row);
// An avatar's place on the dress side: 0 a hat, 1 a dress, 2 an attachment (TID 3/1/13/1..3), 3 the
// Devil's Spirit (3/1/14/1); -1 for anything else. Drawn by Dress over the armour its visual covers.
int AvatarSlot(const ItemRow& row);

// server_dep\silkroad\textdata\charactervisualchange.txt, its equipment's rows (ConditionID 1): the body slots a
// worn item's visual covers (Slot: 0x01 head, 0x02 chest, 0x04 shoulders, 0x08 hands, 0x10 legs, 0x20 feet, 0x40
// weapon, 0x80 shield, 0x0700 the suits' own) and its priority. Columns: Service, ID, Name, ConditionID, AttachType,
// Pram1 (the item's type: TID1 << 2 | TID2 << 5 | TID3 << 7 | TID4 << 11), Pram2..4, StrPram1..4, Slot, OverlapSlot,
// OverlapSlotGroup, Priority, .... The avatars' rows (ConditionID 2)
// are by the type and Pram2, which an avatar's Param7 names (measured: a hat's 513 = 0x0201, a dress's 574 = 0x023E).
class VisualChangeTable {
public:
    struct Row {
        std::uint16_t slots = 0, overlap = 0;
        int group = 0, priority = 0;
    };
    bool Load(const FileSystem& fs, std::string& error);
    bool Parse(std::string_view utf8Text, std::string& error);
    const Row* ForItem(const ItemRow& row) const;
    std::size_t Size() const { return byType_.size(); }

private:
    std::unordered_map<std::uint32_t, Row> byType_;
    std::unordered_map<std::uint64_t, Row> byAvatar_;
};
// The animation set a weapon puts the body in ("default" without one).
std::string StanceOf(const ItemRow* weapon);
// The same by the weapon's kind (TID4; skilldata cols 50, 51): "default" for none or an unknown one.
std::string StanceOfWeaponKind(int kind);
// Whether a weapon leaves the other hand free for a shield (itemdata column 93 is 0).
bool OneHanded(const ItemRow& weapon);
float HeightScale(std::uint8_t shape);
// The bones the volume thickens or thins, with their factors.
std::vector<std::pair<std::string, float>> VolumeScales(std::uint8_t shape, bool female);

// The .bsr resources a worn item puts on the body (DressedCharacter::items): its own model, or an
// axe's or dagger's hands (_r, _l); empty when it has none.
std::vector<std::string> WornModels(const FileSystem& fs, const ItemTable& items, const ItemRow& row);

// A worn weapon's or shield's plus glow (ItemGlow.h) rides on its path in the outfit's key, so the asset store
// marks that item's parts: "item.bsr*glow:<texture>;<u>;<v>;<colour op>;<r,g,b>;<r,g,b>;<period ms>;<alpha test>".
std::string WithGlow(const std::string& itemPath, const EquipGlow& glow);
// Takes the glow off `path` into `glow`; false (both untouched) when it carries none or it does not read.
bool SplitGlow(std::string& path, EquipGlow& glow);

// Fails when the figure or its model is not in the tables. Unknown items are skipped. `plus`: the worn items'
// pluses beside `worn` (none: 0); with `glows`, a weapon or shield with a plus glows (WithGlow).
// With `visuals`, the worn visuals by their priority (the lowest number first) cover their slots, and one whose slots
// meet those covered is not drawn: a thief's suit hides the armour, a trader's or hunter's pack is drawn over it; the
// default garments only where nothing shown covers the chest or the legs (without: every item drawn).
// `garments` false: no default garments on an empty chest or legs (a bandit's figure).
bool Dress(const FileSystem& fs, const CharacterTable& characters, const ItemTable& items, std::uint32_t model,
           std::span<const std::uint32_t> worn, std::uint8_t shape, DressedCharacter& out, std::string& error,
           std::span<const std::uint8_t> plus = {}, const EquipGlowTable* glows = nullptr, const VisualChangeTable* visuals = nullptr,
           bool garments = true);

// --- The jobs' bandits ------------------------------------------------------------------
// A thief or hunter monster (characterdata 1/2/1/2, 1/2/1/3) has no model of its own: the 1.188 client draws it as a
// player figure (its dresser), picked and dressed by the byte its spawn carries.

// The figures it may take: `usableresobjiddata.txt`'s character rows by their Country, in file order (the table
// loader's kind 7): [0] the Chinese (1907..1932), [1] the European (14875..14900).
struct BanditBodies {
    std::array<std::vector<std::uint32_t>, 2> lists;
    bool Load(const FileSystem& fs, const CharacterTable& characters, std::string& error);
    bool Parse(std::string_view text, const CharacterTable& characters, std::string& error);
    bool Empty() const { return lists[0].empty() && lists[1].empty(); }
};
bool IsBandit(const ObjectRow& row);
// The figure and what it wears for `bandit` with the spawn's `appearance` byte, `weaponKind` its first skill's weapon
// (skilldata col 50; 2 for none): the race Europe for a row of Country 1 or a MOB_EU_ codename (every bandit row's
// Country is 3, and the original 1.188 client dresses them all Chinese; ours as the European ones look), else China; the body list[appearance %
// size], the weapon `ITEM_<race>_<kind>_<dd>_A` and a shield for kinds 2, 3, 7, 15, a hunter's armour by the weapon,
// the job suit; `dd` the degree by its level, each piece at that degree or the next lower one there is. The rolls the
// original leaves to each client (the suit's tier, the helm) drawn from the byte, so every viewer agrees (ours). A
// body of 0: none (no list).
struct BanditOutfit {
    std::uint32_t body = 0;
    std::vector<std::uint32_t> worn;
};
BanditOutfit OutfitOfBandit(const ObjectRow& bandit, std::uint8_t appearance, int weaponKind, const BanditBodies& bodies, const CharacterTable& characters,
                            const ItemTable& items);
// The degree its level gives: below 8 1, 16 2, 24 3, 32 4, 42 5, 52 6, 64 7, 76 8, 90 9, 104 10,
// 120 11, 141 12, 164 13, else 14.
int BanditDegree(int level);

} // namespace sr::assets
