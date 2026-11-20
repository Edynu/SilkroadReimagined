#include "sr/game/Stall.h"

#include <algorithm>
#include <variant>

namespace sr::game {

using Code = proto::StallCode;

void StallView::Clear() {
    owner_ = 0;
    mine_ = open_ = false;
    ownerName_.clear();
    name_.clear();
    greeting_.clear();
    for (auto& i : items_) i.reset();
    people_.clear();
    chat_.clear();
    ++revision_;
}

void StallView::Reset() {
    Clear();
    rows_.clear();
    total_ = 0;
    pages_ = page_ = 0;
    ++networkRevision_;
}

bool StallView::Up(int place) const {
    if (!mine_) return false;
    return std::any_of(items_.begin(), items_.end(), [&](const std::optional<Item>& i) { return i && i->item.slot == place; });
}

std::vector<StallView::Line> StallView::Apply(const proto::StallEvent& event, std::uint32_t self) {
    std::vector<Line> out;
    std::visit(
        [&](const auto& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, proto::StallAnswer>) {
                out.push_back({Line::Kind::Refused, m.code});
            } else if constexpr (std::is_same_v<T, proto::StallView>) {
                Clear();
                owner_ = m.owner;
                mine_ = m.owner == self;
                ownerName_ = m.ownerName;
                name_ = m.name;
                greeting_ = m.greeting;
                open_ = m.open != 0;
                for (const proto::StallItem& i : m.items) {
                    if (i.slot < proto::kStallSlots) items_[i.slot] = Item{i.item, i.price};
                }
                people_ = m.people;
            } else if constexpr (std::is_same_v<T, proto::StallSlotChanged>) {
                if (m.item.slot < proto::kStallSlots) {
                    if (m.present != 0) {
                        items_[m.item.slot] = Item{m.item.item, m.item.price};
                    } else {
                        items_[m.item.slot].reset();
                    }
                    ++revision_;
                }
            } else if constexpr (std::is_same_v<T, proto::StallStateChanged>) {
                const bool opened = m.open != 0 && !open_, modifying = m.open == 0 && open_;
                open_ = m.open != 0;
                name_ = m.name;
                greeting_ = m.greeting;
                ++revision_;
                if (opened || modifying) out.push_back({Line::Kind::Opened, Code::Ok, open_});
                if (m.network != 0) out.push_back({Line::Kind::Registered, m.network == 1 ? Code::Ok : Code(m.network)});
            } else if constexpr (std::is_same_v<T, proto::StallPeopleChanged>) {
                std::erase_if(people_, [&](const proto::StallPerson& p) { return p.entity == m.person.entity; });
                if (m.joined != 0) people_.push_back(m.person);
                ++revision_;
            } else if constexpr (std::is_same_v<T, proto::StallChatLine>) {
                chat_.push_back({m.name, m.text});
                if (chat_.size() > 100) chat_.erase(chat_.begin());
                ++revision_;
            } else if constexpr (std::is_same_v<T, proto::StallSold>) {
                Line l{Line::Kind::Sold, Code::Ok, false, m.network != 0, m.buyer, m.commission};
                if (m.slot < proto::kStallSlots) {
                    if (items_[m.slot]) l.item = items_[m.slot]->item; // what was sold, for its line
                    items_[m.slot].reset();
                }
                ++revision_;
                if (mine_) out.push_back(l);
            } else if constexpr (std::is_same_v<T, proto::StallBought>) {
                Line l{Line::Kind::Bought, m.code, false, m.network != 0};
                l.item = m.item;
                l.gold = m.gold;
                out.push_back(l);
                if (m.network != 0 && buying_ != 0) {
                    if (m.code == Code::Ok || m.code == Code::Changed) DropRow(buying_);
                    buying_ = 0;
                }
            } else if constexpr (std::is_same_v<T, proto::StallLeft>) {
                if (owner_ != 0) out.push_back({Line::Kind::Left, m.code, false, false, ownerName_});
                Clear();
            } else if constexpr (std::is_same_v<T, proto::NetworkPage>) {
                if (m.code != Code::Ok) {
                    out.push_back({Line::Kind::Refused, m.code});
                    return;
                }
                page_ = m.page;
                pages_ = m.pages;
                total_ = m.total;
                rows_[m.page] = m.rows;
                ++networkRevision_;
            }
        },
        event);
    return out;
}

void StallView::NewSearch(std::uint32_t group, std::uint8_t degree) {
    group_ = group;
    degree_ = degree;
    rows_.clear();
    page_ = pages_ = 0;
    total_ = 0;
    ++networkRevision_;
}

const std::vector<proto::NetworkRow>* StallView::Rows(std::uint16_t page) const {
    const auto it = rows_.find(page);
    return it != rows_.end() ? &it->second : nullptr;
}

void StallView::DropRow(std::uint64_t listing) {
    for (auto& [page, rows] : rows_) std::erase_if(rows, [&](const proto::NetworkRow& r) { return r.listing == listing; });
    ++networkRevision_;
}

} // namespace sr::game
