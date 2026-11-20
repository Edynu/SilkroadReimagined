#pragma once

// What a new character may be (C5d), from the client's own data, so that the
// server refuses what the original's create screens could not send.
//
// Names (the create screen): 2 to 12 characters (observed: the screen
// checks the length before it sends), each in the allowed ranges of
// textdata\abusefilter.txt (its "#ALLOW_ID_TABLE" rows; one column group of six per locale, group 4
// the English one: 0-9, A-Z, '_', a-z), and none of the group's name words: a word row has four flag columns, one per
// check (names, chat, guild names, stall
// texts); 1 refuses the word, lower-cased, as a whole (the whole name; a space-separated word of a
// chat line), 2 anywhere in the text. Lower-casing is A-Z
// only. The chat's check is here too: the client refuses a line before sending it.
//
// Models: the 13 figures per race and sex the screens offer (their UIO_NEWCHAR_* texts).
// Items: the starter rows of itemdata (codenames ending in "_DEF"): chest, legs and shoes of one
// armour type (HEAVY, LIGHT or CLOTHES) for the model's race and sex, and one of the race's
// weapons (5 Chinese, 9 European, as the screens' weapon texts). The European screen also offers
// no armour, and only the armour its weapon's class allows: sword,
// two-hand sword, axe heavy or light; dark staff, two-hand staff, harp clothes; crossbow, dagger
// light; cleric staff light or clothes. A one-handed weapon comes with the race's shield.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sr::assets {
class FileSystem;
class CharacterTable;
class ItemTable;
} // namespace sr::assets

namespace sr::game {

enum class NameCheck : std::uint8_t { Ok, BadLength, BadCharacter, Forbidden };

class NameRules {
public:
    // abusefilter.txt (UTF-8 text); `group` picks the locale's columns (4: English).
    bool Parse(std::string_view utf8Text, int group, std::string& error);
    // Built directly (tests, or a server without the file): a word refused anywhere in a name, or
    // only as the whole name; in chat anywhere, or only as a whole word.
    void Allow(char32_t first, char32_t last) { allowed_.emplace_back(first, last); }
    void Forbid(std::string_view word, bool anywhere = true);
    void ForbidInChat(std::string_view word, bool anywhere = true);

    NameCheck Check(std::string_view utf8Name) const;
    // A chat line the client may send: none of the chat's words in it.
    bool ChatAllowed(std::string_view utf8Line) const;
    std::size_t ForbiddenWords() const { return anywhere_.size() + whole_.size(); }
    std::size_t ChatWords() const { return chatAnywhere_.size() + chatWhole_.size(); }

    static constexpr std::size_t kMinLength = 2, kMaxLength = 12;

private:
    std::vector<std::pair<char32_t, char32_t>> allowed_;
    // Lower case (A-Z only, as the original).
    std::vector<std::u32string> anywhere_, chatAnywhere_;
    std::unordered_set<std::u32string> whole_, chatWhole_;
};

enum class CreationCheck : std::uint8_t { Ok, BadModel, BadArmour, BadWeapon };
enum class ArmourKind : std::uint8_t { Heavy, Light, Clothes };

class CreationRules {
public:
    enum class Race : std::uint8_t { China, Europe };
    struct Figure {
        Race race;
        bool male;
    };

    // The figures and starter items from the client's tables. False if any is missing.
    bool Load(const assets::CharacterTable& characters, const assets::ItemTable& items, std::string& error);
    // Built directly (tests).
    void AddFigure(std::uint32_t model, Race race, bool male) { figures_[model] = {race, male}; }
    void AddArmourSet(Race race, bool male, std::uint32_t chest, std::uint32_t legs, std::uint32_t shoes,
                      ArmourKind kind = ArmourKind::Heavy) {
        armour_.push_back({race, male, chest, legs, shoes, kind});
    }
    // `kinds`: the armour a European weapon allows (bit per ArmourKind; 0: any); `shield`: the shield
    // it comes with (0: two-handed).
    void AddWeapon(Race race, std::uint32_t item, std::uint8_t kinds = 0, std::uint32_t shield = 0) {
        weapons_[item] = {race, kinds, shield};
    }
    // The shield a starter weapon comes with (0 for none).
    std::uint32_t ShieldFor(std::uint32_t weapon) const;
    // Whether a weapon allows an armour kind (always for Chinese weapons).
    bool Allows(std::uint32_t weapon, ArmourKind kind) const;

    CreationCheck Check(std::uint32_t model, std::uint32_t chest, std::uint32_t legs, std::uint32_t shoes, std::uint32_t weapon) const;

    // What a create screen offers, in a stable order (ids ascending).
    struct Armour {
        std::uint32_t chest, legs, shoes;
        ArmourKind kind;
    };
    std::vector<std::uint32_t> FiguresOf(Race race, bool male) const;
    // A creatable figure's race and sex (none for any other model).
    std::optional<Figure> FigureOf(std::uint32_t model) const {
        const auto it = figures_.find(model);
        return it == figures_.end() ? std::nullopt : std::optional<Figure>(it->second);
    }
    std::vector<Armour> ArmourOf(Race race, bool male) const;
    std::vector<std::uint32_t> WeaponsOf(Race race) const;

    std::size_t Figures() const { return figures_.size(); }
    std::size_t ArmourSets() const { return armour_.size(); }
    std::size_t Weapons() const { return weapons_.size(); }

private:
    struct ArmourSet {
        Race race;
        bool male;
        std::uint32_t chest, legs, shoes;
        ArmourKind kind;
    };
    struct Weapon {
        Race race;
        std::uint8_t kinds; // allowed armour (European weapons), bit per ArmourKind; 0 any
        std::uint32_t shield;
    };
    std::unordered_map<std::uint32_t, Figure> figures_;
    std::vector<ArmourSet> armour_;
    std::unordered_map<std::uint32_t, Weapon> weapons_;
};

// Both, loaded from the mounted game archives.
struct CharacterRules {
    NameRules names;
    CreationRules creation;
    // itemdata, when Load read it from the files (the server's item checks); null otherwise.
    std::shared_ptr<const assets::ItemTable> items;
    bool Load(const assets::FileSystem& fs, std::string& error);
    // With the tables already read (the client keeps them to dress characters).
    bool Load(const assets::FileSystem& fs, const assets::CharacterTable& characters, const assets::ItemTable& items, std::string& error);
};

// A new character's numbers (inferred from how the original's new characters start; the formulas
// are RE'd with the character's stats in C6): level 1, strength and intelligence 20, 200 HP and MP.
struct StartingStats {
    static constexpr std::uint8_t kLevel = 1;
    static constexpr std::uint16_t kStrength = 20, kIntelligence = 20;
    static constexpr std::uint32_t kHp = 200, kMp = 200;
};

} // namespace sr::game
