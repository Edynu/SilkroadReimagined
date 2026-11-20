#include "sr/game/Sets.h"

#include <algorithm>
#include <charconv>

#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/core/Text.h"
#include "sr/game/Items.h"
#include "sr/game/States.h"

namespace sr::game {

namespace {

constexpr std::string_view kSetFile = "server_dep\\silkroad\\textdata\\refsetitemgroup.txt";
constexpr std::string_view kGroupFile = "server_dep\\silkroad\\textdata\\refmagicoptgroup.txt";

std::string_view Trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

long long Number(std::string_view s) {
    s = Trimmed(s);
    long long v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

std::vector<std::string_view> Cells(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = line.find('\t', start);
        out.push_back(line.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) return out;
        start = end + 1;
    }
}

const std::vector<SetOption> kNoOptions;

} // namespace

bool DecodeSetGroups(std::string_view text, std::vector<SetGroup>& out, std::string& error) {
    out.clear();
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = Trimmed(text.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line.starts_with("//")) continue;
        const std::vector<std::string_view> c = Cells(line);
        if (c.size() < 18 || Number(c[0]) != 1) continue;
        SetGroup set;
        set.id = int(Number(c[1]));
        set.codename = std::string(Trimmed(c[2]));
        set.nameKey = std::string(Trimmed(c[4]));
        set.magicMask = std::uint32_t(Number(c[7]));
        for (std::size_t i = 0; i < set.groups.size(); ++i) set.groups[i] = std::uint32_t(std::max(0LL, Number(c[8 + i])));
        out.push_back(std::move(set));
    }
    if (out.empty()) {
        error = "no item set rows";
        return false;
    }
    return true;
}

const SetGroup* SetTable::Find(int id) const {
    const auto it = sets_.find(id);
    return it != sets_.end() ? &it->second : nullptr;
}

std::uint32_t SetTable::PickGroup(const SetGroup& set, int pieces) {
    // 1 << (pieces - 2), then down a bit at a time until a group is there; the groups are kept by their bit,
    // a column of 0 has none.
    if (pieces < 2 || pieces - 2 >= 32) return 0;
    for (int bit = std::min(pieces - 2, int(set.groups.size()) - 1); bit >= 0; --bit) {
        if (set.groups[std::size_t(bit)] != 0) return set.groups[std::size_t(bit)];
    }
    return 0;
}

std::uint32_t SetTable::GroupFor(int setId, int pieces) const {
    const SetGroup* set = Find(setId);
    return set != nullptr ? PickGroup(*set, pieces) : 0;
}

const std::vector<SetOption>& SetTable::OptionsOf(std::uint32_t group) const {
    const auto it = options_.find(group);
    return it != options_.end() ? it->second : kNoOptions;
}

int SetTable::PlusOf(std::uint32_t group) const {
    int plus = 0;
    for (const SetOption& o : OptionsOf(group)) {
        if (o.code == kSetPlusCode) plus = int(o.value); // each row recalculates the pieces: the last one stands
    }
    return plus;
}

int SetTable::ResistOf(std::uint32_t group) const {
    int resist = 0;
    for (const SetOption& o : OptionsOf(group)) {
        if (o.code == kSetResistCode) resist = int(o.value); // one source: a row replaces the one before
    }
    return resist;
}

SetTable MakeSetTable(const std::vector<SetGroup>& sets, const std::vector<assets::MagicOptGroupRow>& groups,
                      const std::vector<assets::MagicOptionRow>& options) {
    SetTable table;
    std::vector<std::uint32_t> named;
    for (const SetGroup& s : sets) {
        for (std::uint32_t g : s.groups) {
            if (g != 0) named.push_back(g);
        }
        table.Add(s);
    }
    for (const assets::MagicOptGroupRow& row : groups) {
        if (row.magicType != 2 || row.link <= 0 || std::find(named.begin(), named.end(), std::uint32_t(row.link)) == named.end()) continue;
        const assets::MagicOptionRow* option = FindOption(options, row.optId);
        if (option == nullptr) continue;
        table.AddOption(std::uint32_t(row.link), {row.optId, option->attribute, std::uint32_t(std::max(0, row.value))});
    }
    return table;
}

bool LoadSetTable(const assets::FileSystem& fs, const std::vector<assets::MagicOptionRow>& options,
                  const std::vector<assets::MagicOptGroupRow>* groups, SetTable& out, std::string& error) {
    const auto setFile = fs.Read(kSetFile);
    if (!setFile) {
        error = "refsetitemgroup.txt is missing";
        return false;
    }
    std::vector<SetGroup> sets;
    if (!DecodeSetGroups(core::DecodeTextFile(setFile->Bytes()), sets, error)) return false;
    std::vector<assets::MagicOptGroupRow> fromFile;
    if (groups == nullptr || groups->empty()) {
        const auto groupFile = fs.Read(kGroupFile);
        if (!groupFile) {
            error = "refmagicoptgroup.txt is missing";
            return false;
        }
        if (!assets::DecodeMagicOptGroups(core::DecodeTextFile(groupFile->Bytes()), fromFile, error)) return false;
        groups = &fromFile;
    }
    out = MakeSetTable(sets, *groups, options);
    return true;
}

std::uint64_t SetStamp(const ItemInstance& item) {
    std::uint64_t h = 1469598103934665603ull; // FNV-1a
    const auto mix = [&](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8 * i)) & 0xFF;
            h *= 1099511628211ull;
        }
    };
    mix(item.refId);
    mix(item.plus);
    mix(item.advanced);
    mix(item.variance);
    for (const ItemInstance::Option& o : item.options) mix((std::uint64_t(o.value) << 32) | o.id);
    for (const ItemSocket& s : item.sockets) mix((std::uint64_t(s.made) << 63) | (std::uint64_t(s.level) << 32) | s.skill);
    return h;
}

// --- The operator -----------------------------------------------------------------------------------------------

std::int64_t SetWear::KeyOf(const SetPiece& piece) {
    // An item not stored yet: its kind and place (ours; every worn item has a serial on our server).
    return piece.serial != 0 ? piece.serial : -(std::int64_t(piece.refId) * 16 + piece.place) - 1;
}

void SetWear::PutOn(const SetPiece& piece) {
    // a piece of a set counted, its set to be looked at.
    if (piece.setId == 0) return;
    Status& s = sets_[piece.setId];
    ++s.count;
    s.dirty = true;
}

void SetWear::TakeOff(const SetPiece& piece) {
    // one less; the set forgotten at none (the group it put on with it;
    // its changes on the keeper stay until something takes the set's source off).
    if (piece.setId == 0) return;
    const auto it = sets_.find(piece.setId);
    if (it == sets_.end()) return; // the original logs "Invalid data is used!"
    --it->second.count;
    it->second.dirty = true;
    if (it->second.count <= 0) sets_.erase(it);
}

void SetWear::Remove(const SetPiece& piece) {
    // the piece's own changes and, for a piece of a set, every change
    // whose source is the set (its resists).
    if (piece.setId != 0) standing_.erase(piece.setId);
    forced_.erase(KeyOf(piece));
}

void SetWear::Install(const SetTable& table, const SetPiece& piece) {
    // its requirements, its contributions (none for a broken
    // one: answers 0 and the rest is skipped), then the operators: the set's looked at for a piece of a set
    // (operator 2).
    if (!piece.fits || piece.broken || piece.setId == 0) return;
    Refresh(table);
}

void SetWear::Refresh(const SetTable& table) {
    // Over every set to be looked at, by SetID. The group found is kept in
    // one place for the whole pass and a set of fewer than 2 pieces does not write it
    // (such a set takes the group found for the set before it in the pass).
    std::uint32_t found = 0;
    for (auto& [setId, status] : sets_) {
        if (!status.dirty) continue;
        if (status.count >= 2) {
            const SetGroup* set = table.Find(setId);
            if (set == nullptr) continue; // "not registered in _RefSetItemGroup": left to be looked at
            found = SetTable::PickGroup(*set, status.count);
        }
        status.dirty = false;
        if (found == status.group) continue;
        status.group = found;
        // the pieces made again with no raise, the set's changes off.
        Raise(setId, 0);
        standing_.erase(setId);
        if (found == 0) continue;
        // The group's rows by their code (the set the source).
        for (const SetOption& o : table.OptionsOf(found)) {
            if (o.code == kSetPlusCode) {
                Raise(setId, int(o.value));
            } else {
                standing_[setId][o.code] = o.value;
            }
        }
    }
}

void SetWear::Raise(int setId, int value) {
    // The inventory's each worn piece of the set by place made again with the raise (RecalculateStats with
    // it) and installed again; a broken one installs nothing and ends the pass.
    for (const SetPiece& p : worn_) {
        if (p.setId != setId) continue;
        raised_[KeyOf(p)] = {value, p.stamp};
        if (p.broken) break;
        // Installed again without its requirements tested: an unfit piece's contributions stand from now on.
        if (!p.fits) forced_.insert(KeyOf(p));
    }
}

void SetWear::Update(const SetTable& table, std::vector<SetPiece> worn, int level) {
    std::sort(worn.begin(), worn.end(), [](const SetPiece& a, const SetPiece& b) { return a.place < b.place; });
    if (!entered_) {
        // The entry: every worn piece counted (Init), then each installed in place order (flag 0: nothing removed).
        entered_ = true;
        worn_ = std::move(worn);
        level_ = level;
        for (const SetPiece& p : worn_) PutOn(p);
        for (const SetPiece& p : worn_) Install(table, p);
        return;
    }
    const std::vector<SetPiece> before = std::move(worn_);
    worn_ = std::move(worn);
    const auto find = [](const std::vector<SetPiece>& in, std::int64_t key) -> const SetPiece* {
        for (const SetPiece& p : in) {
            if (KeyOf(p) == key) return &p;
        }
        return nullptr;
    };
    // A move (OnItemMoveResult): what came off and what went on, counted (added first, as does a swap),
    // then each piece that came off taken out (its set looked at when nothing went on), then each that went on installed.
    std::vector<SetPiece> added, removed;
    for (const SetPiece& p : worn_) {
        if (find(before, KeyOf(p)) == nullptr) added.push_back(p);
    }
    for (const SetPiece& p : before) {
        if (find(worn_, KeyOf(p)) == nullptr) removed.push_back(p);
    }
    for (const SetPiece& p : added) PutOn(p);
    for (const SetPiece& p : removed) TakeOff(p);
    for (const SetPiece& p : removed) {
        Remove(p);
        if (p.setId != 0 && added.empty()) Refresh(table);
    }
    for (const SetPiece& p : added) Install(table, p);
    // A piece broken (taken out) or mended (taken out and installed again): OffsetItemDurability.
    for (const SetPiece& p : worn_) {
        const SetPiece* was = find(before, KeyOf(p));
        if (was == nullptr || was->broken == p.broken) continue;
        Remove(p);
        if (!p.broken) Install(table, p);
    }
    // A level changed: every worn piece taken out and installed again, in place order (RefreshEquipmentContributions
    // with its flag 1).
    if (level != level_) {
        level_ = level;
        for (const SetPiece& p : worn_) {
            Remove(p);
            Install(table, p);
        }
    }
}

int SetWear::RaiseOf(std::int64_t serial, std::uint64_t stamp) const {
    const auto it = raised_.find(serial);
    return it != raised_.end() && it->second.stamp == stamp ? it->second.value : 0;
}

int SetWear::RaiseOf(const SetPiece& piece) const { return RaiseOf(KeyOf(piece), piece.stamp); }

int SetWear::ResistAll() const {
    int total = 0;
    for (const auto& [setId, codes] : standing_) {
        if (const auto it = codes.find(kSetResistCode); it != codes.end()) total += int(it->second);
    }
    return total;
}

std::vector<std::pair<int, SetOption>> SetWear::OtherStanding() const {
    std::vector<std::pair<int, SetOption>> out;
    for (const auto& [setId, codes] : standing_) {
        for (const auto& [code, value] : codes) {
            if (code != kSetResistCode) out.push_back({setId, SetOption{0, code, value}});
        }
    }
    return out;
}

void SetWear::AddResists(StateDefence& defence) const {
    const int v = ResistAll();
    if (v == 0) return;
    for (int& e : defence.elementResist) e += v;
    for (const int bucket : {15, 8, 0, 3}) defence.percent[std::size_t(bucket)] += v;
}

int SetWear::Pieces(int setId) const {
    const auto it = sets_.find(setId);
    return it != sets_.end() ? it->second.count : 0;
}

std::uint32_t SetWear::Group(int setId) const {
    const auto it = sets_.find(setId);
    return it != sets_.end() ? it->second.group : 0;
}

} // namespace sr::game
