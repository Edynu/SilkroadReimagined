#include "sr/server/SqlServerBackup.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>

#include "sr/core/Text.h"

namespace sr::server {

namespace {

using U128 = unsigned __int128;

constexpr std::size_t kPage = 8192;
constexpr std::size_t kSector = 512;
constexpr std::size_t kHeader = 96;

// Page types (m_type).
constexpr std::uint8_t kData = 1, kIam = 10, kPfs = 11;
constexpr std::uint8_t kKnownTypes[] = {1, 2, 3, 4, 7, 8, 9, 10, 11, 13, 15, 16, 17};

template <typename T>
T Get(std::span<const std::uint8_t> bytes, std::size_t at) {
    T v{};
    if (at + sizeof(T) <= bytes.size()) std::memcpy(&v, bytes.data() + at, sizeof(T));
    return v;
}

std::string Lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

std::uint64_t PageKey(std::uint16_t file, std::uint32_t page) { return (std::uint64_t(file) << 32) | page; }

// A record in FixedVar form (the form of every record of the original's tables).
struct Record {
    int type = 0; // 0 primary, 1 forwarded, 2 forwarding stub, 3 index, 4 blob, 5..7 ghosts
    std::span<const std::uint8_t> fixed;
    int columns = 0;
    std::span<const std::uint8_t> nulls;
    struct Var {
        std::span<const std::uint8_t> bytes;
        bool complex = false; // a pointer to a large value (out of the row)
    };
    std::vector<Var> vars;

    bool Null(int bit) const { // bit from 1
        const int i = bit - 1;
        if (i < 0 || nulls.empty() || i >= columns || std::size_t(i / 8) >= nulls.size()) return false;
        return (nulls[std::size_t(i / 8)] >> (i % 8)) & 1;
    }
};

// The record at `at` of a page; false when its sizes do not fit the page.
bool ReadRecord(std::span<const std::uint8_t> page, std::size_t at, Record& r) {
    if (at < kHeader || at + 4 > page.size()) return false;
    const std::uint8_t a = page[at];
    r.type = (a >> 1) & 7;
    const bool hasNulls = (a & 0x10) != 0, hasVars = (a & 0x20) != 0;
    const std::uint16_t fixedEnd = Get<std::uint16_t>(page, at + 2);
    if (fixedEnd < 4 || at + fixedEnd > page.size()) return false;
    r.fixed = page.subspan(at + 4, fixedEnd - 4u);
    std::size_t pos = at + fixedEnd;
    r.columns = 0;
    r.nulls = {};
    r.vars.clear();
    if (hasNulls) {
        if (pos + 2 > page.size()) return false;
        r.columns = Get<std::uint16_t>(page, pos);
        pos += 2;
        const std::size_t bytes = (std::size_t(r.columns) + 7) / 8;
        if (pos + bytes > page.size()) return false;
        r.nulls = page.subspan(pos, bytes);
        pos += bytes;
    }
    if (hasVars) {
        if (pos + 2 > page.size()) return false;
        const std::uint16_t count = Get<std::uint16_t>(page, pos);
        pos += 2;
        if (pos + 2u * count > page.size()) return false;
        std::size_t start = pos + 2u * count - at; // offsets are from the record's start
        for (std::uint16_t i = 0; i < count; ++i) {
            const std::uint16_t raw = Get<std::uint16_t>(page, pos + 2u * i);
            const std::size_t end = raw & 0x7FFF;
            if (end < start || at + end > page.size()) return false;
            r.vars.push_back({page.subspan(at + start, end - start), (raw & 0x8000) != 0});
            start = end;
        }
    }
    return true;
}

// The slot array (at the page's end, slot 0 last): each record's place.
std::vector<std::uint16_t> Slots(std::span<const std::uint8_t> page) {
    const std::uint16_t count = Get<std::uint16_t>(page, 22);
    std::vector<std::uint16_t> out;
    if (2u * count > kPage - kHeader) return out;
    for (std::uint16_t i = 0; i < count; ++i) out.push_back(Get<std::uint16_t>(page, kPage - 2u * (i + 1u)));
    return out;
}

std::string TypeName(int xtype) {
    switch (xtype) {
    case 34: return "image";
    case 35: return "text";
    case 36: return "uniqueidentifier";
    case 40: return "date";
    case 41: return "time";
    case 42: return "datetime2";
    case 48: return "tinyint";
    case 52: return "smallint";
    case 56: return "int";
    case 58: return "smalldatetime";
    case 59: return "real";
    case 60: return "money";
    case 61: return "datetime";
    case 62: return "float";
    case 98: return "sql_variant";
    case 99: return "ntext";
    case 104: return "bit";
    case 106: return "decimal";
    case 108: return "numeric";
    case 122: return "smallmoney";
    case 127: return "bigint";
    case 165: return "varbinary";
    case 167: return "varchar";
    case 173: return "binary";
    case 175: return "char";
    case 189: return "timestamp";
    case 231: return "nvarchar";
    case 239: return "nchar";
    case 241: return "xml";
    default: return std::format("type{}", xtype);
    }
}

// UTF-16LE to UTF-8 (pairs joined; a lone half becomes U+FFFD).
std::string Utf16ToUtf8(std::span<const std::uint8_t> bytes) {
    std::string out;
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        char32_t c = char32_t(bytes[i] | (bytes[i + 1] << 8));
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < bytes.size()) {
            const char32_t low = char32_t(bytes[i + 2] | (bytes[i + 3] << 8));
            if (low >= 0xDC00 && low < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            } else {
                c = 0xFFFD;
            }
        } else if (c >= 0xD800 && c < 0xE000) {
            c = 0xFFFD;
        }
        core::AppendUtf8(out, c);
    }
    return out;
}

// Windows-1252 to UTF-8 (0x80..0x9F its own characters, the rest Latin-1's; its five holes U+FFFD).
std::string Cp1252ToUtf8(std::span<const std::uint8_t> bytes) {
    static constexpr char16_t kHigh[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                           0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                           0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};
    std::string out;
    for (std::uint8_t b : bytes) core::AppendUtf8(out, b >= 0x80 && b < 0xA0 ? char32_t(kHigh[b - 0x80]) : char32_t(b));
    return out;
}

// Text in COPY's form: backslash, tab, newline and carriage return escaped.
void AppendEscaped(std::string& out, std::string_view text) {
    for (char c : text) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '\t': out += "\\t"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\0': break; // PostgreSQL's text holds none
        default: out += c;
        }
    }
}

void AppendHex(std::string& out, std::span<const std::uint8_t> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    out += "\\\\x"; // COPY's text form of a bytea's \x
    for (std::uint8_t b : bytes) {
        out += kDigits[b >> 4];
        out += kDigits[b & 15];
    }
}

template <typename F>
void AppendFloat(std::string& out, F value) {
    char buffer[64];
    const auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value); // shortest that reads back the same
    out.append(buffer, ec == std::errc() ? end : buffer);
}

// days from 1900-01-01 and milliseconds into the day.
void AppendTimestamp(std::string& out, std::int64_t days, std::int64_t ms) {
    using namespace std::chrono;
    days += ms / 86'400'000;
    ms %= 86'400'000;
    const year_month_day d{sys_days{year{1900} / January / 1} + std::chrono::days{days}};
    out += std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}", int(d.year()), unsigned(d.month()), unsigned(d.day()), ms / 3'600'000,
                       ms / 60'000 % 60, ms / 1000 % 60, ms % 1000);
}

// A decimal of `scale` digits after the point from its magnitude.
void AppendDecimal(std::string& out, bool negative, U128 magnitude, int scale) {
    std::string digits;
    do {
        digits.insert(digits.begin(), char('0' + int(magnitude % 10)));
        magnitude /= 10;
    } while (magnitude != 0);
    if (scale > 0) {
        if (int(digits.size()) <= scale) digits.insert(0, std::size_t(scale - int(digits.size()) + 1), '0');
        digits.insert(digits.size() - std::size_t(scale), 1, '.');
    }
    if (negative) out += '-';
    out += digits;
}

} // namespace

struct SqlServerBackup::State {
    std::vector<std::uint8_t> data;
    std::unordered_map<std::uint64_t, std::size_t> pages;                  // (file, page) -> offset in `data`
    std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> byUnit; // a header's allocation unit -> its data pages
    std::unordered_map<std::uint64_t, std::uint64_t> firstIam;             // allocation unit -> its first IAM page
    std::vector<BackupTable> tables;

    std::span<const std::uint8_t> Page(std::uint64_t key) const {
        const auto it = pages.find(key);
        if (it == pages.end()) return {};
        return std::span<const std::uint8_t>(data).subspan(it->second, kPage);
    }

    bool Allocated(std::uint16_t file, std::uint32_t page) const {
        // One PFS page per 8,088 pages (the first is page 1); a byte each, 0x40 = allocated.
        const std::uint32_t pfsPage = page < 8088 ? 1u : (page / 8088u) * 8088u;
        const std::span<const std::uint8_t> pfs = Page(PageKey(file, pfsPage));
        if (pfs.empty() || pfs[1] != kPfs) return true;
        const std::size_t at = 100 + page % 8088u;
        return at < pfs.size() && (pfs[at] & 0x40) != 0;
    }

    bool ReadPages(std::string& error);
    // The records of a system table (by its allocation unit in the header) that are primary or forwarded.
    template <typename F>
    void SystemRows(std::uint32_t object, std::uint16_t index, F&& each) const;
    std::vector<std::uint64_t> IamPages(std::uint64_t unit) const;
    bool ReadCatalog(std::string& error);
};

bool SqlServerBackup::State::ReadPages(std::string& error) {
    const auto plausible = [&](std::size_t off) {
        const std::uint8_t* h = data.data() + off;
        if (h[0] != 1 || std::find(std::begin(kKnownTypes), std::end(kKnownTypes), h[1]) == std::end(kKnownTypes)) return false;
        const std::span<const std::uint8_t> head(h, kHeader);
        const std::uint16_t file = Get<std::uint16_t>(head, 36), slots = Get<std::uint16_t>(head, 22), freeData = Get<std::uint16_t>(head, 30);
        return file >= 1 && file <= 64 && slots <= 4096 && freeData <= kPage;
    };
    // The places modulo 8 KB that many pages share (a stray match inside a page is alone at its place).
    std::map<std::size_t, std::size_t> places;
    for (std::size_t off = 0; off + kPage <= data.size(); off += kSector)
        if (plausible(off)) ++places[off % kPage];
    std::set<std::size_t> kept;
    for (const auto& [place, count] : places)
        if (count >= 8) kept.insert(place);
    if (kept.empty()) {
        error = "no database pages found (not a SQL Server backup, or a compressed one)";
        return false;
    }
    for (std::size_t off = 0; off + kPage <= data.size(); off += kSector) {
        if (!kept.contains(off % kPage) || !plausible(off)) continue;
        std::uint8_t* p = data.data() + off;
        const std::span<const std::uint8_t> head(p, kHeader);
        // Torn-page detection: sectors 1..15 got a marker in their last byte's low two bits; the header has the originals.
        if (Get<std::uint16_t>(head, 4) & 0x100) {
            const std::uint32_t torn = Get<std::uint32_t>(head, 60);
            for (std::size_t k = 1; k < kPage / kSector; ++k) {
                std::uint8_t& last = p[kSector * k + kSector - 1];
                last = std::uint8_t((last & 0xFC) | ((torn >> (2 * k)) & 3));
            }
        }
        pages[PageKey(Get<std::uint16_t>(head, 36), Get<std::uint32_t>(head, 32))] = off; // a later copy wins
    }
    for (const auto& [key, off] : pages) {
        const std::span<const std::uint8_t> page(data.data() + off, kPage);
        if (page[1] != kData) continue;
        if (!Allocated(std::uint16_t(key >> 32), std::uint32_t(key))) continue;
        const std::uint64_t unit = (std::uint64_t(Get<std::uint16_t>(page, 6)) << 48) | (std::uint64_t(Get<std::uint32_t>(page, 24)) << 16);
        byUnit[unit].push_back(key);
    }
    for (auto& [unit, keys] : byUnit) std::sort(keys.begin(), keys.end());
    return true;
}

template <typename F>
void SqlServerBackup::State::SystemRows(std::uint32_t object, std::uint16_t index, F&& each) const {
    const auto it = byUnit.find((std::uint64_t(index) << 48) | (std::uint64_t(object) << 16));
    if (it == byUnit.end()) return;
    Record r;
    for (std::uint64_t key : it->second) {
        const std::span<const std::uint8_t> page = Page(key);
        for (std::uint16_t slot : Slots(page)) {
            if (!ReadRecord(page, slot, r) || (r.type != 0 && r.type != 1)) continue;
            each(r);
        }
    }
}

std::vector<std::uint64_t> SqlServerBackup::State::IamPages(std::uint64_t unit) const {
    std::vector<std::uint64_t> out;
    const auto first = firstIam.find(unit);
    if (first == firstIam.end()) return out;
    std::set<std::uint64_t> seen;
    for (std::uint64_t at = first->second; at != 0 && seen.insert(at).second;) {
        const std::span<const std::uint8_t> page = Page(at);
        if (page.empty() || page[1] != kIam) break;
        const std::vector<std::uint16_t> slots = Slots(page);
        if (slots.size() < 2 || slots[0] + 94u > kPage || slots[1] + 7992u > kPage) break;
        // Slot 0: the IAM header (its range's first page at 40, then 8 single pages of 6 bytes from 46); slot 1: one bit
        // per 8-page extent of the range.
        const std::span<const std::uint8_t> head = page.subspan(slots[0], 94);
        const std::uint32_t startPage = Get<std::uint32_t>(head, 40);
        const std::uint16_t startFile = Get<std::uint16_t>(head, 44);
        for (std::size_t k = 0; k < 8; ++k) {
            const std::uint32_t p = Get<std::uint32_t>(head, 46 + 6 * k);
            const std::uint16_t f = Get<std::uint16_t>(head, 50 + 6 * k);
            if (p != 0 || f != 0) out.push_back(PageKey(f, p));
        }
        const std::span<const std::uint8_t> bitmap = page.subspan(slots[1] + 4u, 7988);
        for (std::size_t byte = 0; byte < bitmap.size(); ++byte) {
            if (bitmap[byte] == 0) continue;
            for (int bit = 0; bit < 8; ++bit) {
                if (!((bitmap[byte] >> bit) & 1)) continue;
                const std::uint32_t extent = startPage + std::uint32_t(8 * (byte * 8 + std::size_t(bit)));
                for (std::uint32_t j = 0; j < 8; ++j) out.push_back(PageKey(startFile, extent + j));
            }
        }
        const std::uint32_t nextPage = Get<std::uint32_t>(page, 16);
        const std::uint16_t nextFile = Get<std::uint16_t>(page, 20);
        at = nextPage == 0 && nextFile == 0 ? 0 : PageKey(nextFile, nextPage);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool SqlServerBackup::State::ReadCatalog(std::string& error) {
    // sysschobjs (34): id, nsid, nsclass, status, type char(2) at 13, ...; its name the first variable column.
    struct Object {
        std::string name;
        int schema = 0;
        std::string type;
    };
    std::unordered_map<std::int32_t, Object> objects;
    SystemRows(34, 1, [&](const Record& r) {
        if (r.fixed.size() != 44 || r.vars.empty()) return;
        Object o;
        o.name = Utf16ToUtf8(r.vars[0].bytes);
        o.schema = Get<std::int32_t>(r.fixed, 4);
        o.type.assign(reinterpret_cast<const char*>(r.fixed.data() + 13), 2);
        objects[Get<std::int32_t>(r.fixed, 0)] = std::move(o);
    });
    // sysrowsets (5): rowsetid, ownertype, idmajor (object) at 9, idminor (index) at 13.
    std::unordered_map<std::int64_t, std::pair<std::int32_t, std::int32_t>> rowsets;
    SystemRows(5, 0, [&](const Record& r) {
        if (r.fixed.size() != 53) return;
        rowsets[Get<std::int64_t>(r.fixed, 0)] = {Get<std::int32_t>(r.fixed, 9), Get<std::int32_t>(r.fixed, 13)};
    });
    // sysallocunits (7): auid, type (1 in-row data) at 8, ownerid (rowset) at 9, ..., pgfirstiam at 35.
    std::unordered_map<std::int64_t, std::uint64_t> inRow; // rowset -> its in-row allocation unit
    SystemRows(7, 0, [&](const Record& r) {
        if (r.fixed.size() != 69) return;
        const std::uint64_t unit = Get<std::uint64_t>(r.fixed, 0);
        if (r.fixed[8] == 1) inRow[Get<std::int64_t>(r.fixed, 9)] = unit;
        firstIam[unit] = PageKey(Get<std::uint16_t>(r.fixed, 39), Get<std::uint32_t>(r.fixed, 35));
    });
    // syscolpars (41): id, number at 4, colid at 6, xtype at 10, length at 15, prec 17, scale 18, status at 23 (0x1 not
    // null, 0x4 identity), dflt (its default's object) at 33; the name the first variable column.
    struct ColPar {
        std::string name;
        int xtype = 0, length = 0, precision = 0, scale = 0, status = 0, defaultObject = 0, collation = 0;
    };
    std::map<std::pair<std::int32_t, std::int32_t>, ColPar> colpars;
    SystemRows(41, 1, [&](const Record& r) {
        if (r.fixed.size() != 41 || r.vars.empty() || Get<std::int16_t>(r.fixed, 4) != 0) return;
        ColPar c;
        c.name = Utf16ToUtf8(r.vars[0].bytes);
        c.xtype = r.fixed[10];
        c.length = Get<std::int16_t>(r.fixed, 15);
        c.precision = r.fixed[17];
        c.scale = r.fixed[18];
        c.collation = Get<std::int32_t>(r.fixed, 19);
        c.status = Get<std::int32_t>(r.fixed, 23);
        c.defaultObject = Get<std::int32_t>(r.fixed, 33);
        colpars[{Get<std::int32_t>(r.fixed, 0), Get<std::int32_t>(r.fixed, 6)}] = std::move(c);
    });
    // sysrscols (3): rsid, rscolid at 8, ..., offset at 40 (its low 16 bits: the leaf place), nullbit at 44, bitpos 48.
    struct RsCol {
        int leaf = 0, nullBit = 0, bitPos = 0;
    };
    std::map<std::pair<std::int64_t, std::int32_t>, RsCol> rscols;
    SystemRows(3, 0, [&](const Record& r) {
        if (r.fixed.size() != 54) return;
        RsCol c;
        c.leaf = std::int16_t(Get<std::uint16_t>(r.fixed, 40));
        c.nullBit = Get<std::int32_t>(r.fixed, 44);
        c.bitPos = Get<std::int16_t>(r.fixed, 48);
        rscols[{Get<std::int64_t>(r.fixed, 0), Get<std::int32_t>(r.fixed, 8)}] = c;
    });
    // sysobjvalues (60): valclass, objid at 1, subobjid at 5; a default's text (valclass 1) in its second variable column.
    std::unordered_map<std::int32_t, std::string> defaults;
    SystemRows(60, 1, [&](const Record& r) {
        if (r.fixed.size() != 13 || r.fixed[0] != 1 || r.vars.size() < 2 || Get<std::int32_t>(r.fixed, 5) != 0) return;
        const std::span<const std::uint8_t> text = r.vars[1].bytes;
        defaults[Get<std::int32_t>(r.fixed, 1)] = std::string(reinterpret_cast<const char*>(text.data()), text.size());
    });
    // sysidxstats (54): id, indid at 4, status at 8 (0x20: the primary key); sysiscols (55): idmajor, idminor, subid (the
    // key's place) at 8, ..., intprop (the column) at 16.
    std::set<std::pair<std::int32_t, std::int32_t>> primaryKeys;
    SystemRows(54, 1, [&](const Record& r) {
        if (r.fixed.size() == 35 && (Get<std::int32_t>(r.fixed, 8) & 0x20)) primaryKeys.insert({Get<std::int32_t>(r.fixed, 0), Get<std::int32_t>(r.fixed, 4)});
    });
    std::map<std::pair<std::int32_t, std::int32_t>, int> keyColumns; // (object, column) -> its place in the key
    SystemRows(55, 1, [&](const Record& r) {
        if (r.fixed.size() != 24) return;
        const std::int32_t object = Get<std::int32_t>(r.fixed, 0), index = Get<std::int32_t>(r.fixed, 4);
        if (primaryKeys.contains({object, index})) keyColumns[{object, Get<std::int32_t>(r.fixed, 16)}] = Get<std::int32_t>(r.fixed, 8);
    });

    for (const auto& [rowset, owner] : rowsets) {
        const auto [object, index] = owner;
        const auto o = objects.find(object);
        if (index > 1 || o == objects.end() || o->second.type != "U ") continue; // a table's heap (0) or clustered index (1)
        const auto unit = inRow.find(rowset);
        if (unit == inRow.end()) continue;
        BackupTable t;
        t.owner = o->second.schema == 1 ? "dbo" : std::format("schema{}", o->second.schema);
        t.name = o->second.name;
        t.allocationUnit = unit->second;
        for (auto it = rscols.lower_bound({rowset, 0}); it != rscols.end() && it->first.first == rowset; ++it) {
            const auto par = colpars.find({object, it->first.second});
            if (par == colpars.end()) continue; // a hidden column (a clustered index's uniquifier)
            BackupColumn c;
            c.name = par->second.name;
            c.typeId = par->second.xtype;
            c.type = TypeName(c.typeId);
            c.length = par->second.length;
            c.precision = par->second.precision;
            c.scale = par->second.scale;
            c.nullable = (par->second.status & 0x1) == 0;
            c.identity = (par->second.status & 0x4) != 0;
            if (const auto key = keyColumns.find({object, it->first.second}); key != keyColumns.end()) c.keyOrdinal = key->second;
            if (const auto d = defaults.find(par->second.defaultObject); par->second.defaultObject != 0 && d != defaults.end()) c.defaultValue = d->second;
            c.leafOffset = it->second.leaf;
            c.nullBit = it->second.nullBit;
            c.bitPosition = it->second.bitPos;
            c.collation = par->second.collation;
            t.columns.push_back(std::move(c));
        }
        if (!t.columns.empty()) tables.push_back(std::move(t));
    }
    std::sort(tables.begin(), tables.end(), [](const BackupTable& a, const BackupTable& b) { return Lower(a.name) < Lower(b.name); });
    if (tables.empty()) {
        error = "no tables found in the backup's catalog";
        return false;
    }
    return true;
}

SqlServerBackup::SqlServerBackup() = default;
SqlServerBackup::~SqlServerBackup() = default;
SqlServerBackup::SqlServerBackup(SqlServerBackup&&) noexcept = default;
SqlServerBackup& SqlServerBackup::operator=(SqlServerBackup&&) noexcept = default;

bool SqlServerBackup::Open(const std::filesystem::path& file, std::string& error) {
    auto s = std::make_unique<State>();
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = file.string() + ": cannot be read";
        return false;
    }
    in.seekg(0, std::ios::end);
    s->data.resize(std::size_t(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(s->data.data()), std::streamsize(s->data.size()));
    if (s->data.size() < 4 || std::memcmp(s->data.data(), "TAPE", 4) != 0) {
        error = file.string() + ": not a SQL Server backup (no TAPE block)";
        return false;
    }
    if (!s->ReadPages(error) || !s->ReadCatalog(error)) {
        error = file.string() + ": " + error;
        return false;
    }
    s_ = std::move(s);
    return true;
}

const std::vector<BackupTable>& SqlServerBackup::Tables() const {
    static const std::vector<BackupTable> kNone;
    return s_ ? s_->tables : kNone;
}

const BackupTable* SqlServerBackup::Find(std::string_view name) const {
    const std::string want = Lower(name);
    for (const BackupTable& t : Tables())
        if (Lower(t.name) == want) return &t;
    return nullptr;
}

std::size_t SqlServerBackup::Pages() const { return s_ ? s_->pages.size() : 0; }

bool SqlServerBackup::ReadRows(const BackupTable& table, const std::function<void(std::string&& line)>& row, std::string& error) const {
    if (!s_) {
        error = "no backup open";
        return false;
    }
    const State& s = *s_;
    // A fixed column's size in the record.
    const auto fixedSize = [](const BackupColumn& c) -> std::size_t {
        switch (c.typeId) {
        case 48: return 1;
        case 52: return 2;
        case 56: case 59: case 58: case 122: return 4;
        case 127: case 62: case 61: case 60: case 189: return 8;
        case 36: return 16;
        default: return std::size_t(std::max(c.length, 0)); // char, nchar, binary, numeric: their length
        }
    };
    Record r;
    std::string line;
    for (std::uint64_t key : s.IamPages(table.allocationUnit)) {
        const std::span<const std::uint8_t> page = s.Page(key);
        if (page.empty() || page[1] != kData || !s.Allocated(std::uint16_t(key >> 32), std::uint32_t(key))) continue;
        for (std::uint16_t slot : Slots(page)) {
            if (!ReadRecord(page, slot, r) || (r.type != 0 && r.type != 1)) continue;
            line.clear();
            for (std::size_t i = 0; i < table.columns.size(); ++i) {
                const BackupColumn& c = table.columns[i];
                if (i) line += '\t';
                // A column added after the row was written is not in it: NULL.
                if (r.Null(c.nullBit) || (c.nullBit > r.columns && r.columns > 0)) {
                    line += "\\N";
                    continue;
                }
                std::span<const std::uint8_t> v;
                if (c.typeId == 104) { // bit
                    const std::size_t at = std::size_t(c.leafOffset - 4);
                    if (c.leafOffset < 4 || at >= r.fixed.size()) return (error = std::format("{}.{}: a bit outside its row", table.name, c.name)), false;
                    line += (r.fixed[at] >> c.bitPosition) & 1 ? '1' : '0';
                    continue;
                }
                if (c.leafOffset > 0) {
                    const std::size_t at = std::size_t(c.leafOffset - 4), size = fixedSize(c);
                    if (at + size > r.fixed.size()) return (error = std::format("{}.{}: a value outside its row", table.name, c.name)), false;
                    v = r.fixed.subspan(at, size);
                } else {
                    const std::size_t at = std::size_t(-c.leafOffset - 1);
                    // Trailing empty variable columns are left out of the row (a NULL one says so in the null bitmap).
                    if (at < r.vars.size() && r.vars[at].complex) {
                        error = std::format("{}.{} ({}): a large value out of the row, which this does not read", table.name, c.name, c.type);
                        return false;
                    }
                    if (at < r.vars.size()) v = r.vars[at].bytes;
                }
                switch (c.typeId) {
                case 48: line += std::to_string(v[0]); break;
                case 52: line += std::to_string(Get<std::int16_t>(v, 0)); break;
                case 56: line += std::to_string(Get<std::int32_t>(v, 0)); break;
                case 127: line += std::to_string(Get<std::int64_t>(v, 0)); break;
                case 59: AppendFloat(line, Get<float>(v, 0)); break;
                case 62: AppendFloat(line, Get<double>(v, 0)); break;
                case 60: AppendDecimal(line, Get<std::int64_t>(v, 0) < 0, U128(std::abs(Get<std::int64_t>(v, 0))), 4); break;
                case 122: AppendDecimal(line, Get<std::int32_t>(v, 0) < 0, U128(std::abs(std::int64_t(Get<std::int32_t>(v, 0)))), 4); break;
                case 61: // datetime: 1/300 s since midnight, then days since 1900-01-01
                    AppendTimestamp(line, Get<std::int32_t>(v, 4), (std::int64_t(Get<std::uint32_t>(v, 0)) * 20 + 3) / 6);
                    break;
                case 58: // smalldatetime: minutes since midnight, then days since 1900-01-01
                    AppendTimestamp(line, Get<std::uint16_t>(v, 2), std::int64_t(Get<std::uint16_t>(v, 0)) * 60'000);
                    break;
                case 106:
                case 108: { // a sign byte (1 positive), then the magnitude little-endian
                    U128 m = 0;
                    for (std::size_t k = v.size(); k > 1; --k) m = (m << 8) | v[k - 1];
                    AppendDecimal(line, !v.empty() && v[0] != 1, m, c.scale);
                    break;
                }
                case 167:
                case 175:
                    if ((std::uint32_t(c.collation) >> 24) != 0) {
                        AppendEscaped(line, Cp1252ToUtf8(v));
                    } else {
                        AppendEscaped(line, core::Cp949ToUtf8(std::string_view(reinterpret_cast<const char*>(v.data()), v.size())));
                    }
                    break;
                case 231:
                case 239: AppendEscaped(line, Utf16ToUtf8(v)); break;
                case 165:
                case 173:
                case 189: AppendHex(line, v); break;
                case 36:
                    line += std::format("{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}", Get<std::uint32_t>(v, 0), Get<std::uint16_t>(v, 4),
                                        Get<std::uint16_t>(v, 6), v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15]);
                    break;
                default:
                    error = std::format("{}.{}: its type {} is not read", table.name, c.name, c.type);
                    return false;
                }
            }
            row(std::move(line));
        }
    }
    return true;
}

} // namespace sr::server
