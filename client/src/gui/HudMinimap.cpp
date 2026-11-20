// The minimap and the area banner.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

#include "gui/Hud.h"
#include "gui/HudWindows.h"

namespace sr::client {

namespace proto = game::proto;

namespace {

constexpr float kRegion = 1920.0f;
constexpr float kPi = 3.14159265f;

// The player's own entity, if the world has it.
const game::WorldView::Entity* Self(const HudContext& c) {
    return c.world != nullptr ? c.world->Find(c.world->Self()) : nullptr;
}

// A square marker of `size` at `centre`, turned clockwise on the screen by `angle`.
void Turned(ui::Painter& paint, const std::string& path, ui::Vec2 centre, float size, float angle) {
    const float c = std::cos(angle), s = std::sin(angle), h = size / 2.0f;
    const ui::Vec2 corners[4] = {{-h, -h}, {h, -h}, {h, h}, {-h, h}};
    ui::Vec2 p[4];
    for (int i = 0; i < 4; ++i) p[i] = {centre.x + corners[i].x * c - corners[i].y * s, centre.y + corners[i].x * s + corners[i].y * c};
    const ui::Vec2 uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    paint.ImageQuad(path, p, uv);
}

} // namespace

// --- The minimap -------------------------------------------------------------------------------------

void Minimap::Created(ui::Interface& ui) {
    Static::Created(ui);
    // The mask's control only lends its rect (the map's disc); the floor badge is for dungeons.
    if (Widget* area = Child(1)) area->visible = false;
    if (Widget* badge = Child(10)) badge->visible = false;
}

void Minimap::Update(ui::Frame& frame) {
    // The zoom eases at 50 pixels of region a second.
    const float step = 50.0f * frame.dt;
    scale_ = scale_ < target_ ? std::min(target_, scale_ + step) : std::max(target_, scale_ - step);
    const game::WorldView::Entity* self = Self(context_);
    if (self != nullptr) {
        const proto::WorldPosition& at = self->position;
        const auto set = [](Widget* w, const std::string& text) {
            if (w != nullptr && w->text() != text) w->SetText(text);
        };
        Widget* badge = Child(10);
        if ((at.region & 0x8000) != 0) {
            // A dungeon: no coordinates; the area is the room of the block the player's
            // floor is in, the floor badge its floor (the .dof's names, text keys: "1F-Chamber(A)", "2F").
            const HudContext::DungeonPlace& d = context_.dungeon;
            const auto text = [&](const std::string& key) {
                return !key.empty() && context_.hasText && context_.hasText(key) ? context_.Text(key) : std::string();
            };
            set(Child(3), {});
            set(Child(4), {});
            set(Child(2), d.region == at.region ? text(d.room) : std::string());
            if (badge != nullptr) {
                const std::string floor = d.region == at.region ? text(d.floor) : std::string();
                set(badge, floor);
                badge->visible = !floor.empty();
            }
        } else {
            if (badge != nullptr) badge->visible = false;
            set(Child(3), std::format("X:{:3}", CoordinateX(at.region, at.x)));
            set(Child(4), std::format("Y:{:3}", CoordinateY(at.region, at.z)));
            // The area: the region id as a decimal key of textzonename (blank without a row).
            const std::string key = std::to_string(at.region);
            set(Child(2), context_.hasText && context_.hasText(key) ? context_.Text(key) : std::string());
        }
    }
    if (Widget* b = Child(8); b != nullptr && frame.pointer.hovered == b) {
        frame.interface.ShowTooltip(context_.Text("UIIT_STT_TOGGLE_WORLD_MAP"), b->ScreenRect());
    }
}

void Minimap::Draw(ui::Frame& frame) {
    const Widget* areaWidget = Child(1);
    const game::WorldView::Entity* self = Self(context_);
    if (areaWidget == nullptr || self == nullptr) {
        Static::Draw(frame);
        return;
    }
    const proto::WorldPosition& at = self->position;
    // The tiles: the region's outdoors; in a dungeon its floor's: minimap_d\<folder>\<floor>_<X>x<Y>.ddj,
    // the dungeon's space cut into 1920-unit tiles counted from 128 (its x and z of 0 on tile 128's corner).
    std::string prefix = "minimap\\";
    int rx = at.region & 0xFF, ry = at.region >> 8;
    float lx = at.x, lz = at.z;
    if ((at.region & 0x8000) != 0) {
        const HudContext::DungeonPlace& d = context_.dungeon;
        if (d.region != at.region || d.folder.empty() || d.floor.empty()) {
            Static::Draw(frame);
            return;
        }
        std::string floor = d.floor;
        std::transform(floor.begin(), floor.end(), floor.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        prefix = std::format("minimap_d\\{}\\{}_", d.folder, floor);
        const float tx = std::floor(at.x / kRegion), tz = std::floor(at.z / kRegion);
        rx = int(tx) + 128;
        ry = int(tz) + 128;
        lx = at.x - tx * kRegion;
        lz = at.z - tz * kRegion;
    }
    const ui::Rect area = areaWidget->ScreenRect();
    const ui::Vec2 centre{area.x + area.w / 2.0f, area.y + area.h / 2.0f};
    const float S = scale_;
    // The 3 x 3 tiles around the player, north up, cut to the disc by mm_alpha's alpha (a tile not in the files stays
    // black).
    if (frame.paint.PushMask("interface\\minimap\\mm_alpha.ddj", area)) {
        frame.paint.PushClip(area);
        frame.paint.Fill(area, ui::Rgba(0, 0, 0));
        const float px = S * lx / kRegion, pz = S * (kRegion - lz) / kRegion;
        for (int j = 0; j < 3; ++j) {
            for (int i = 0; i < 3; ++i) {
                const int x = rx - 1 + i, y = ry + 1 - j;
                if (x < 0 || x > 255 || y < 0 || y > 255) continue;
                const ui::Rect cell{centre.x + float(i - 1) * S - px, centre.y + float(j - 1) * S - pz, S, S};
                frame.paint.Image(std::format("{}{}x{}.ddj", prefix, x, y), cell);
            }
        }
        // The ring around those asked for too, not drawn (ours): walking onto the next tile finds it in already
        // instead of black until it loads.
        for (int y = ry - 2; y <= ry + 2; ++y) {
            for (int x = rx - 2; x <= rx + 2; ++x) {
                if (std::abs(x - rx) < 2 && std::abs(y - ry) < 2) continue; // the 3 x 3 drawn above
                if (x < 0 || x > 255 || y < 0 || y > 255) continue;
                frame.paint.Prefetch(std::format("{}{}x{}.ddj", prefix, x, y));
            }
        }
        // Who is around, as dots under the frame, within 44.5 pixels, by kind: monsters red (a unique's ring 12 x 12),
        // NPCs blue, other players
        // yellow-green; items on the ground and the dead none.
        const proto::FlatPosition me = proto::ToFlat(at);
        for (const auto& [id, e] : context_.world->Entities()) {
            if (id == self->id) continue;
            const proto::FlatPosition p = proto::ToFlat(e.position);
            const float dx = float(p.x - me.x) * S / kRegion, dy = -float(p.z - me.z) * S / kRegion;
            if (std::hypot(dx, dy) >= 44.5f || e.kind == proto::EntityKind::GroundItem || e.kind == proto::EntityKind::SkillObject) continue;
            const char* sign = "interface\\minimap\\mm_sign_otherplayer.ddj";
            float size = 8.0f;
            if (e.kind == proto::EntityKind::Monster) {
                if (e.look.hp == 0) continue;
                sign = e.rarity == 3 ? "interface\\minimap\\mm_sign_unique.ddj" : "interface\\minimap\\mm_sign_monster.ddj";
                if (e.rarity == 3) size = 12.0f;
            } else if (e.look.name.empty()) {
                // An NPC; one with something about the player's quests its own sign (loads
                // mm_sign_questnpc.ddj beside mm_sign_npc.ddj).
                sign = context_.questNpc && context_.questNpc(id) ? "interface\\minimap\\mm_sign_questnpc.ddj" : "interface\\minimap\\mm_sign_npc.ddj";
            }
            frame.paint.Image(sign, {centre.x + dx - size / 2.0f, centre.y + dy - size / 2.0f, size, size});
        }
        frame.paint.PopClip();
        frame.paint.PopMask();
    }
    // The frame, then the player's arrow (east in the image) turned with the character's facing,
    // not the camera's.
    Static::Draw(frame);
    // The academy's and the party's members (the academy's first): each where it is (seen: its live
    // place; else where the last news put it), within 47 pixels a dot (8 x 8), farther an arrow on the 47-pixel circle
    // turned toward it (the pictures point up; the party's 16 x 16, the academy's 32 x 32).
    if (context_.state != nullptr) {
        const proto::FlatPosition me = proto::ToFlat(at);
        const auto member = [&](const proto::WorldPosition& where, const char* dot, const char* arrow, float arrowSize) {
            if (where.region == 0 || (where.region & 0x8000) != (at.region & 0x8000) || where.layer != at.layer) return;
            const proto::FlatPosition p = proto::ToFlat(where);
            const float dx = float(p.x - me.x) * S / kRegion, dy = -float(p.z - me.z) * S / kRegion;
            const float d = std::hypot(dx, dy);
            if (d <= 47.0f) {
                frame.paint.Image(dot, {centre.x + dx - 4.0f, centre.y + dy - 4.0f, 8.0f, 8.0f});
                return;
            }
            Turned(frame.paint, arrow, {centre.x + dx * 47.0f / d, centre.y + dy * 47.0f / d}, arrowSize, std::atan2(dx, -dy));
        };
        const game::PartyView& party = context_.state->party;
        const std::uint32_t selfCharacter = party.SelfId();
        for (const auto& [character, where] : context_.state->academy.Places()) {
            if (character == selfCharacter) continue;
            member(where, "interface\\minimap\\mm_sign_apprenticeship.ddj", "interface\\minimap\\mm_sign_apprenticeshiparrow.ddj", 32.0f);
        }
        if (party.party != 0) {
            for (const proto::PartyMemberRecord& m : party.members) {
                if (m.id == selfCharacter) continue;
                const game::WorldView::Entity* seen = m.entity != 0 ? context_.world->Find(m.entity) : nullptr;
                member(seen != nullptr ? seen->position : m.position, "interface\\minimap\\mm_sign_party.ddj", "interface\\minimap\\mm_sign_partyarrow.ddj",
                       16.0f);
            }
        }
    }
    // The player's marks (Tag Point): the hunting point's red arrow, 16 x 16 and turned
    // as the player's, where its target is (seen: where it stands; else where the server last said),
    // held on the edge 47 pixels out when farther.
    if (context_.state != nullptr) {
        const proto::FlatPosition me = proto::ToFlat(at);
        for (const game::Mark& m : context_.state->marks) {
            const game::WorldView::Entity* seen = context_.world->Find(m.entity);
            const proto::WorldPosition& where = seen != nullptr ? seen->position : m.position;
            if (where.region == 0) continue; // not told yet
            const proto::FlatPosition p = proto::ToFlat(where);
            float dx = float(p.x - me.x) * S / kRegion, dy = -float(p.z - me.z) * S / kRegion;
            if (const float d = std::hypot(dx, dy); d > 47.0f) {
                dx *= 47.0f / d;
                dy *= 47.0f / d;
            }
            Turned(frame.paint, "interface\\worldmap\\wmap_sign_huntingpoint.ddj", {centre.x + dx, centre.y + dy}, 16.0f,
                   (seen != nullptr ? seen->yaw : m.yaw) - kPi / 2.0f);
        }
    }
    Turned(frame.paint, "interface\\minimap\\mm_sign_character.ddj", centre, 16.0f, self->yaw - kPi / 2.0f);
}

bool Minimap::OnNotice(const ui::Notice& notice) {
    if (notice.kind != ui::Notice::Kind::Clicked || notice.source == nullptr || notice.source->parent() != this) return false;
    // Zoom by 19.2 between 64 and 256 pixels a region; the world map.
    switch (notice.control) {
    case 5: target_ = std::min(256.0f, target_ + 19.2f); return true;
    case 6: target_ = std::max(64.0f, target_ - 19.2f); return true;
    case 8:
        if (context_.hud != nullptr) context_.hud->Toggle(hud::kWorldMap);
        return true;
    default: return false;
    }
}

// --- The area banner ---------------------------------------------------------------------------------

void AreaBanner::Created(ui::Interface&) {
    // Its texts are drawn by the banner itself (the name half again as large); it takes no clicks.
    for (int id : {1, 11, 12}) {
        if (Widget* w = Child(id)) w->visible = false;
    }
    clickThrough = true;
    rect.w = rect.h = 0.0f;
}

void AreaBanner::Update(ui::Frame& frame) {
    const game::WorldView::Entity* self = Self(context_);
    if (self != nullptr && self->position.region != region_) {
        // Into another area: its name, description and monster levels.
        region_ = self->position.region;
        const std::string key = context_.regionKey ? context_.regionKey(region_) : std::string();
        if (key.empty()) {
            key_.clear();
        } else if (key != key_) {
            key_ = key;
            shownAt_ = context_.time;
        }
    }
    // 0.5 s in, until 3 s, 0.5 s out.
    const double t = context_.time - shownAt_;
    if (key_.empty() || t >= 3.5) {
        rect.w = rect.h = 0.0f;
        return;
    }
    alpha = float(t < 0.5 ? t / 0.5 : t > 3.0 ? (3.5 - t) / 0.5 : 1.0);
    const ui::Vec2 screen = frame.interface.screen();
    rect = {std::floor(screen.x / 2.0f) - 372.0f, 95.0f, 744.0f, 104.0f};
}

void AreaBanner::Draw(ui::Frame& frame) {
    if (rect.w <= 0.0f || key_.empty()) return;
    const float a = Opacity();
    frame.paint.Image("interface\\game\\area_deco.ddj", ScreenRect(), {0, 0}, {1, 1}, ui::WithAlpha(ui::kWhite, a));
    const float W = frame.interface.screen().x;
    const auto line = [&](const std::string& suffix) {
        const std::string text = context_.Text(key_ + suffix);
        return text == key_ + suffix || text == "xxx" ? std::string() : text;
    };
    // The name: font 4 at 1.5 times, bold, shadowed, centred around y 119.
    const std::string name = line("_01");
    if (!name.empty()) {
        constexpr float kScale = 1.5f;
        const float w = frame.paint.TextWidth(name, 4, kScale) / kScale, h = ui::TextLineHeight(4);
        const float x = std::floor(W / 2.0f) - std::trunc(w * kScale / 2.0f), y = 119.0f - std::trunc(h * kScale / 4.0f);
        const ui::Color shadow = ui::Rgba(0, 0, 0, std::uint8_t(255.0f * a)), colour = ui::WithAlpha(ui::Rgba(61, 253, 255), a);
        for (int pass = 0; pass < 2; ++pass) frame.paint.Text(name, {x + float(pass) + 1, y + 1, w * kScale + 4, h * kScale}, shadow, 4, 0, 0, kScale);
        for (int pass = 0; pass < 2; ++pass) frame.paint.Text(name, {x + float(pass), y, w * kScale + 4, h * kScale}, colour, 4, 0, 0, kScale);
    }
    ui::DrawLabel(frame.paint, line("_02"), {std::floor(W / 2.0f) - 343.0f, 156.0f, 691.0f, 13.0f}, ui::WithAlpha(ui::kWhite, a), 3, 1, 0, ui::kTextShadow);
    ui::DrawLabel(frame.paint, line("_03"), {std::floor(W / 2.0f) - 214.0f, 177.0f, 427.0f, 11.0f}, ui::WithAlpha(ui::Rgba(255, 255, 92), a), 1, 1, 0,
                  ui::kTextShadow);
    // A fortress's town (siegefortressreward.txt's DependentCityRegionCodeName): "Occupying Guild [guild-master]" under
    // the lines while a guild holds it (its place under the others: ours).
    if (context_.worldMap != nullptr && context_.fortress != nullptr) {
        for (const assets::FortressMapPlace& p : context_.worldMap->fortresses) {
            if (p.town != key_) continue;
            for (const game::proto::FortressRecord& f : context_.fortress->Fortresses()) {
                const std::string guild = f.holder != 0 ? f.holderName : f.ownerName;
                if (f.nameKey != "SN_" + p.fortress || guild.empty()) continue;
                ui::DrawLabel(frame.paint, FormatText(context_.Text("UIIT_MSG_FORTRESS_TOWN_GUILD_NAME"), {guild, f.masterName}),
                              {std::floor(W / 2.0f) - 214.0f, 193.0f, 427.0f, 11.0f}, ui::WithAlpha(ui::kWhite, a), 1, 1, 0, ui::kTextShadow);
            }
        }
    }
}

} // namespace sr::client
