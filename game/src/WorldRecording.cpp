#include "sr/game/WorldRecording.h"

#include <cstring>
#include <fstream>

#include "sr/game/WorldView.h"

namespace sr::game {

namespace {

constexpr char kMagic[8] = {'S', 'R', 'W', 'R', 'E', 'C', '0', '1'};

template <typename T>
void Put(std::vector<char>& out, T value) {
    const char* p = reinterpret_cast<const char*>(&value);
    out.insert(out.end(), p, p + sizeof(T));
}

template <typename T>
bool Get(const std::vector<char>& in, std::size_t& at, T& value) {
    if (in.size() - at < sizeof(T)) return false;
    std::memcpy(&value, in.data() + at, sizeof(T));
    at += sizeof(T);
    return true;
}

template <typename T>
bool As(std::span<const std::byte> payload, TitleEvent& out) {
    T message;
    if (!proto::Decode(payload, message)) return false;
    out = std::move(message);
    return true;
}

} // namespace

bool WorldRecording::Save(const std::filesystem::path& path, std::string& error) const {
    std::vector<char> out(kMagic, kMagic + sizeof(kMagic));
    Put(out, std::uint32_t(messages.size()));
    for (const RecordedMessage& m : messages) {
        Put(out, m.time);
        Put(out, std::uint16_t(m.id));
        Put(out, std::uint32_t(m.payload.size()));
        const char* p = reinterpret_cast<const char*>(m.payload.data());
        out.insert(out.end(), p, p + m.payload.size());
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file || !file.write(out.data(), std::streamsize(out.size()))) {
        error = "cannot write " + path.string();
        return false;
    }
    return true;
}

bool WorldRecording::Load(const std::filesystem::path& path, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open " + path.string();
        return false;
    }
    const std::vector<char> in((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::size_t at = sizeof(kMagic);
    std::uint32_t count = 0;
    if (in.size() < sizeof(kMagic) || std::memcmp(in.data(), kMagic, sizeof(kMagic)) != 0 || !Get(in, at, count)) {
        error = path.string() + ": not a world recording";
        return false;
    }
    messages.clear();
    for (std::uint32_t i = 0; i < count; ++i) {
        RecordedMessage m;
        std::uint16_t id = 0;
        std::uint32_t size = 0;
        if (!Get(in, at, m.time) || !Get(in, at, id) || !Get(in, at, size) || in.size() - at < size) {
            error = path.string() + ": cut short at message " + std::to_string(i);
            return false;
        }
        m.id = id;
        m.payload.resize(size);
        std::memcpy(m.payload.data(), in.data() + at, size);
        at += size;
        messages.push_back(std::move(m));
    }
    return true;
}

bool IsWorldMessage(proto::MessageId id) {
    switch (id) {
    case proto::WorldEntered::kId:
    case proto::WorldLeft::kId:
    case proto::EntityAppeared::kId:
    case proto::EntityGone::kId:
    case proto::EntityMoved::kId:
    case proto::EntityTurned::kId:
    case proto::GateRefused::kId:
    case proto::LogoutAnswer::kId:
    case proto::QuickSlotList::kId:
    case proto::ChatAnswer::kId:
    case proto::ChatLine::kId:
    case proto::InventoryList::kId:
    case proto::InventoryGrown::kId:
    case proto::ItemMoved::kId:
    case proto::EquipmentChanged::kId:
    case proto::GateMenu::kId:
    case proto::ReturnPointSet::kId:
    case proto::GoldChanged::kId:
    case proto::ReturnStarted::kId:
    case proto::ReturnCancelled::kId:
    case proto::BuffAdded::kId:
    case proto::BuffRemoved::kId:
    case proto::NpcMenu::kId:
    case proto::StorageRefused::kId:
    case proto::StorageList::kId:
    case proto::StorageMoved::kId:
    case proto::StorageGoldChanged::kId:
    case proto::ShopAnswer::kId:
    case proto::NpcRepaired::kId:
    case proto::EntityAction::kId:
    case proto::EntitySwing::kId:
    case proto::EntityCastCancelled::kId:
    case proto::EntityBlink::kId:
    case proto::EntityDamaged::kId:
    case proto::EntityStates::kId:
    case proto::EntityBuffs::kId:
    case proto::SkillObjectStruck::kId:
    case proto::EntityKnocked::kId:
    case proto::RevivalOffered::kId:
    case proto::SkillTook::kId:
    case proto::EntityLook::kId:
    case proto::LinkAdded::kId:
    case proto::LinkPosition::kId:
    case proto::BerserkAnswer::kId:
    case proto::BerserkPoints::kId:
    case proto::EntityBerserk::kId:
    case proto::EntityMotion::kId:
    case proto::EntityMarks::kId:
    case proto::EntityEmote::kId:
    case proto::FreePvpAnswer::kId:
    case proto::EntityFreePvp::kId:
    case proto::Vitals::kId:
    case proto::Progress::kId:
    case proto::ExperienceGained::kId:
    case proto::EntityLevelUp::kId:
    case proto::SkillBookList::kId:
    case proto::SkillBookAnswer::kId:
    case proto::SkillUseAnswer::kId:
    case proto::ActionHeld::kId:
    case proto::WeatherNow::kId:
    case proto::WorldClock::kId:
    case proto::AccountRestricted::kId:
    case proto::Cooldown::kId:
    case proto::PartyInvited::kId:
    case proto::PartyResult::kId:
    case proto::PartyInfo::kId:
    case proto::PartyMemberJoined::kId:
    case proto::PartyMemberLeft::kId:
    case proto::PartyDismissed::kId:
    case proto::PartyMemberUpdate::kId:
    case proto::PartyLeaderChanged::kId:
    case proto::PartySettingsChanged::kId:
    case proto::PartyLoot::kId:
    case proto::ItemGained::kId:
    case proto::PartyMatchAnswer::kId:
    case proto::PartyMatchListing::kId:
    case proto::PartyMatchJoinRequest::kId:
    case proto::AutoPotionSettings::kId:
    case proto::ItemUsed::kId:
    case proto::UniqueNotice::kId:
    case proto::ItemRefused::kId:
    case proto::Disconnected::kId:
    case proto::AlchemyResult::kId:
    case proto::AvatarList::kId:
    case proto::AvatarMoved::kId:
    case proto::ViewLimitsAnswer::kId: return true;
    default: return (proto::IsGuildMessage(id) && id >= proto::GuildAnswer::kId) || (proto::IsFortressMessage(id) && id >= proto::FortressAnswer::kId) ||
                    (proto::IsCommunityMessage(id) && id >= proto::CommunityLists::kId) || (proto::IsExchangeMessage(id) && id >= proto::ExchangeAsked::kId) ||
                    (proto::IsStallMessage(id) && id >= proto::StallAnswer::kId) || (proto::IsQuestMessage(id) && id < proto::QuestTalk::kId) ||
                    (proto::IsInstanceMessage(id) && id < proto::SelectGap::kId) || (proto::IsCosMessage(id) && id < proto::CosRide::kId) ||
                    (proto::IsJobMessage(id) && id >= proto::JobInfoMessage::kId) || (proto::IsMallMessage(id) && proto::IsMallAnswer(id)) ||
                    proto::IsPkMessage(id) || (proto::IsAcademyMessage(id) && !proto::IsAcademyRequest(id)) ||
                    (proto::IsOpenMarketMessage(id) && !proto::IsOpenMarketRequest(id)) ||
                    (proto::IsEventMessage(id) && id >= proto::FlagWorldNotice::kId && (id < proto::ArenaRegister::kId || id >= proto::ArenaNotice::kId));
    }
}

const char* WorldMessageName(proto::MessageId id) {
    switch (id) {
    case proto::WorldEntered::kId: return "WorldEntered";
    case proto::WorldLeft::kId: return "WorldLeft";
    case proto::EntityAppeared::kId: return "EntityAppeared";
    case proto::EntityGone::kId: return "EntityGone";
    case proto::EntityMoved::kId: return "EntityMoved";
    case proto::EntityTurned::kId: return "EntityTurned";
    case proto::EntityAction::kId: return "EntityAction";
    case proto::EntitySwing::kId: return "EntitySwing";
    case proto::EntityCastCancelled::kId: return "EntityCastCancelled";
    case proto::EntityBlink::kId: return "EntityBlink";
    case proto::EntityDamaged::kId: return "EntityDamaged";
    case proto::EntityStates::kId: return "EntityStates";
    case proto::EntityBuffs::kId: return "EntityBuffs";
    case proto::SkillObjectStruck::kId: return "SkillObjectStruck";
    case proto::EntityKnocked::kId: return "EntityKnocked";
    case proto::RevivalOffered::kId: return "RevivalOffered";
    case proto::SkillTook::kId: return "SkillTook";
    case proto::EntityLook::kId: return "EntityLook";
    case proto::LinkAdded::kId: return "LinkAdded";
    case proto::LinkPosition::kId: return "LinkPosition";
    case proto::BerserkAnswer::kId: return "BerserkAnswer";
    case proto::BerserkPoints::kId: return "BerserkPoints";
    case proto::EntityBerserk::kId: return "EntityBerserk";
    case proto::EntityMotion::kId: return "EntityMotion";
    case proto::EntityMarks::kId: return "EntityMarks";
    case proto::EntityEmote::kId: return "EntityEmote";
    case proto::FreePvpAnswer::kId: return "FreePvpAnswer";
    case proto::EntityFreePvp::kId: return "EntityFreePvp";
    case proto::EntityLevelUp::kId: return "EntityLevelUp";
    case proto::EquipmentChanged::kId: return "EquipmentChanged";
    case proto::ChatLine::kId: return "ChatLine";
    case proto::ChatAnswer::kId: return "ChatAnswer";
    case proto::Vitals::kId: return "Vitals";
    case proto::Progress::kId: return "Progress";
    case proto::ExperienceGained::kId: return "ExperienceGained";
    case proto::BuffAdded::kId: return "BuffAdded";
    case proto::BuffRemoved::kId: return "BuffRemoved";
    case proto::ItemMoved::kId: return "ItemMoved";
    case proto::InventoryList::kId: return "InventoryList";
    case proto::InventoryGrown::kId: return "InventoryGrown";
    case proto::GoldChanged::kId: return "GoldChanged";
    case proto::SkillUseAnswer::kId: return "SkillUseAnswer";
    case proto::ActionHeld::kId: return "ActionHeld";
    case proto::WeatherNow::kId: return "WeatherNow";
    case proto::WorldClock::kId: return "WorldClock";
    case proto::AccountRestricted::kId: return "AccountRestricted";
    case proto::Cooldown::kId: return "Cooldown";
    case proto::ViewLimitsAnswer::kId: return "ViewLimitsAnswer";
    case proto::PartyInvited::kId: return "PartyInvited";
    case proto::PartyResult::kId: return "PartyResult";
    case proto::PartyInfo::kId: return "PartyInfo";
    case proto::PartyMemberJoined::kId: return "PartyMemberJoined";
    case proto::PartyMemberLeft::kId: return "PartyMemberLeft";
    case proto::PartyDismissed::kId: return "PartyDismissed";
    case proto::PartyMemberUpdate::kId: return "PartyMemberUpdate";
    case proto::PartyLeaderChanged::kId: return "PartyLeaderChanged";
    case proto::PartySettingsChanged::kId: return "PartySettingsChanged";
    case proto::PartyLoot::kId: return "PartyLoot";
    case proto::ItemGained::kId: return "ItemGained";
    case proto::PartyMatchAnswer::kId: return "PartyMatchAnswer";
    case proto::PartyMatchListing::kId: return "PartyMatchListing";
    case proto::PartyMatchJoinRequest::kId: return "PartyMatchJoinRequest";
    case proto::AutoPotionSettings::kId: return "AutoPotionSettings";
    case proto::ItemUsed::kId: return "ItemUsed";
    case proto::UniqueNotice::kId: return "UniqueNotice";
    case proto::ItemRefused::kId: return "ItemRefused";
    case proto::Disconnected::kId: return "Disconnected";
    case proto::AlchemyResult::kId: return "AlchemyResult";
    case proto::AvatarList::kId: return "AvatarList";
    case proto::AvatarMoved::kId: return "AvatarMoved";
    default:
        return proto::IsPkMessage(id)          ? proto::PkMessageName(id)
               : proto::IsEventMessage(id)     ? proto::EventMessageName(id)
               : proto::IsMallMessage(id)      ? proto::MallMessageName(id)
               : proto::IsJobMessage(id)       ? proto::JobMessageName(id)
               : proto::IsCosMessage(id)       ? proto::CosMessageName(id)
               : proto::IsInstanceMessage(id)  ? proto::InstanceMessageName(id)
               : proto::IsQuestMessage(id)     ? proto::QuestMessageName(id)
               : proto::IsStallMessage(id)     ? proto::StallMessageName(id)
               : proto::IsExchangeMessage(id)  ? proto::ExchangeMessageName(id)
               : proto::IsCommunityMessage(id) ? proto::CommunityMessageName(id)
               : proto::IsAcademyMessage(id)   ? proto::AcademyMessageName(id)
               : proto::IsOpenMarketMessage(id) ? proto::OpenMarketMessageName(id)
               : proto::IsFortressMessage(id)  ? proto::FortressMessageName(id)
                                               : proto::GuildMessageName(id);
    }
}

bool DecodeWorldEvent(proto::MessageId id, std::span<const std::byte> payload, TitleEvent& out) {
    switch (id) {
    case proto::WorldEntered::kId: return As<proto::WorldEntered>(payload, out);
    case proto::WorldLeft::kId: return As<proto::WorldLeft>(payload, out);
    case proto::EntityAppeared::kId: return As<proto::EntityAppeared>(payload, out);
    case proto::EntityGone::kId: return As<proto::EntityGone>(payload, out);
    case proto::EntityMoved::kId: return As<proto::EntityMoved>(payload, out);
    case proto::EntityTurned::kId: return As<proto::EntityTurned>(payload, out);
    case proto::GateRefused::kId: return As<proto::GateRefused>(payload, out);
    case proto::LogoutAnswer::kId: return As<proto::LogoutAnswer>(payload, out);
    case proto::QuickSlotList::kId: return As<proto::QuickSlotList>(payload, out);
    case proto::ChatAnswer::kId: return As<proto::ChatAnswer>(payload, out);
    case proto::ChatLine::kId: return As<proto::ChatLine>(payload, out);
    case proto::InventoryList::kId: return As<proto::InventoryList>(payload, out);
    case proto::InventoryGrown::kId: return As<proto::InventoryGrown>(payload, out);
    case proto::ItemMoved::kId: return As<proto::ItemMoved>(payload, out);
    case proto::EquipmentChanged::kId: return As<proto::EquipmentChanged>(payload, out);
    case proto::GateMenu::kId: return As<proto::GateMenu>(payload, out);
    case proto::ReturnPointSet::kId: return As<proto::ReturnPointSet>(payload, out);
    case proto::GoldChanged::kId: return As<proto::GoldChanged>(payload, out);
    case proto::ReturnStarted::kId: return As<proto::ReturnStarted>(payload, out);
    case proto::ReturnCancelled::kId: return As<proto::ReturnCancelled>(payload, out);
    case proto::BuffAdded::kId: return As<proto::BuffAdded>(payload, out);
    case proto::BuffRemoved::kId: return As<proto::BuffRemoved>(payload, out);
    case proto::NpcMenu::kId: return As<proto::NpcMenu>(payload, out);
    case proto::StorageRefused::kId: return As<proto::StorageRefused>(payload, out);
    case proto::StorageList::kId: return As<proto::StorageList>(payload, out);
    case proto::StorageMoved::kId: return As<proto::StorageMoved>(payload, out);
    case proto::StorageGoldChanged::kId: return As<proto::StorageGoldChanged>(payload, out);
    case proto::ShopAnswer::kId: return As<proto::ShopAnswer>(payload, out);
    case proto::NpcRepaired::kId: return As<proto::NpcRepaired>(payload, out);
    case proto::EntityAction::kId: return As<proto::EntityAction>(payload, out);
    case proto::EntitySwing::kId: return As<proto::EntitySwing>(payload, out);
    case proto::EntityCastCancelled::kId: return As<proto::EntityCastCancelled>(payload, out);
    case proto::EntityBlink::kId: return As<proto::EntityBlink>(payload, out);
    case proto::EntityDamaged::kId: return As<proto::EntityDamaged>(payload, out);
    case proto::EntityStates::kId: return As<proto::EntityStates>(payload, out);
    case proto::EntityBuffs::kId: return As<proto::EntityBuffs>(payload, out);
    case proto::SkillObjectStruck::kId: return As<proto::SkillObjectStruck>(payload, out);
    case proto::EntityKnocked::kId: return As<proto::EntityKnocked>(payload, out);
    case proto::RevivalOffered::kId: return As<proto::RevivalOffered>(payload, out);
    case proto::SkillTook::kId: return As<proto::SkillTook>(payload, out);
    case proto::EntityLook::kId: return As<proto::EntityLook>(payload, out);
    case proto::LinkAdded::kId: return As<proto::LinkAdded>(payload, out);
    case proto::LinkPosition::kId: return As<proto::LinkPosition>(payload, out);
    case proto::BerserkAnswer::kId: return As<proto::BerserkAnswer>(payload, out);
    case proto::BerserkPoints::kId: return As<proto::BerserkPoints>(payload, out);
    case proto::EntityBerserk::kId: return As<proto::EntityBerserk>(payload, out);
    case proto::EntityMotion::kId: return As<proto::EntityMotion>(payload, out);
    case proto::EntityMarks::kId: return As<proto::EntityMarks>(payload, out);
    case proto::EntityEmote::kId: return As<proto::EntityEmote>(payload, out);
    case proto::FreePvpAnswer::kId: return As<proto::FreePvpAnswer>(payload, out);
    case proto::EntityFreePvp::kId: return As<proto::EntityFreePvp>(payload, out);
    case proto::Vitals::kId: return As<proto::Vitals>(payload, out);
    case proto::Progress::kId: return As<proto::Progress>(payload, out);
    case proto::ExperienceGained::kId: return As<proto::ExperienceGained>(payload, out);
    case proto::EntityLevelUp::kId: return As<proto::EntityLevelUp>(payload, out);
    case proto::SkillBookList::kId: return As<proto::SkillBookList>(payload, out);
    case proto::SkillBookAnswer::kId: return As<proto::SkillBookAnswer>(payload, out);
    case proto::SkillUseAnswer::kId: return As<proto::SkillUseAnswer>(payload, out);
    case proto::ActionHeld::kId: return As<proto::ActionHeld>(payload, out);
    case proto::WeatherNow::kId: return As<proto::WeatherNow>(payload, out);
    case proto::WorldClock::kId: return As<proto::WorldClock>(payload, out);
    case proto::AccountRestricted::kId: return As<proto::AccountRestricted>(payload, out);
    case proto::Cooldown::kId: return As<proto::Cooldown>(payload, out);
    case proto::ViewLimitsAnswer::kId: return As<proto::ViewLimitsAnswer>(payload, out);
    case proto::PartyInvited::kId: return As<proto::PartyInvited>(payload, out);
    case proto::PartyResult::kId: return As<proto::PartyResult>(payload, out);
    case proto::PartyInfo::kId: return As<proto::PartyInfo>(payload, out);
    case proto::PartyMemberJoined::kId: return As<proto::PartyMemberJoined>(payload, out);
    case proto::PartyMemberLeft::kId: return As<proto::PartyMemberLeft>(payload, out);
    case proto::PartyDismissed::kId: return As<proto::PartyDismissed>(payload, out);
    case proto::PartyMemberUpdate::kId: return As<proto::PartyMemberUpdate>(payload, out);
    case proto::PartyLeaderChanged::kId: return As<proto::PartyLeaderChanged>(payload, out);
    case proto::PartySettingsChanged::kId: return As<proto::PartySettingsChanged>(payload, out);
    case proto::PartyLoot::kId: return As<proto::PartyLoot>(payload, out);
    case proto::ItemGained::kId: return As<proto::ItemGained>(payload, out);
    case proto::PartyMatchAnswer::kId: return As<proto::PartyMatchAnswer>(payload, out);
    case proto::PartyMatchListing::kId: return As<proto::PartyMatchListing>(payload, out);
    case proto::PartyMatchJoinRequest::kId: return As<proto::PartyMatchJoinRequest>(payload, out);
    case proto::AutoPotionSettings::kId: return As<proto::AutoPotionSettings>(payload, out);
    case proto::ItemUsed::kId: return As<proto::ItemUsed>(payload, out);
    case proto::UniqueNotice::kId: return As<proto::UniqueNotice>(payload, out);
    case proto::ItemRefused::kId: return As<proto::ItemRefused>(payload, out);
    case proto::Disconnected::kId: return As<proto::Disconnected>(payload, out);
    case proto::AlchemyResult::kId: return As<proto::AlchemyResult>(payload, out);
    case proto::AvatarList::kId: return As<proto::AvatarList>(payload, out);
    case proto::AvatarMoved::kId: return As<proto::AvatarMoved>(payload, out);
    default: {
        proto::GuildEvent guild;
        if (proto::IsInstanceMessage(id)) {
            proto::InstanceEvent instance;
            if (!proto::DecodeInstanceEvent(id, payload, instance)) return false;
            out = std::move(instance);
            return true;
        }
        if (proto::IsQuestMessage(id)) {
            proto::QuestEvent quest;
            if (!proto::DecodeQuestEvent(id, payload, quest)) return false;
            out = std::move(quest);
            return true;
        }
        if (proto::IsStallMessage(id)) {
            proto::StallEvent stall;
            if (!proto::DecodeStallEvent(id, payload, stall)) return false;
            out = std::move(stall);
            return true;
        }
        if (proto::IsPkMessage(id)) {
            proto::PkEvent pk;
            if (!proto::DecodePkEvent(id, payload, pk)) return false;
            out = std::move(pk);
            return true;
        }
        if (proto::IsEventMessage(id)) {
            proto::EventEvent flag;
            if (!proto::DecodeEventEvent(id, payload, flag)) return false;
            out = std::move(flag);
            return true;
        }
        if (proto::IsMallMessage(id)) {
            proto::MallEvent mall;
            if (!proto::DecodeMallEvent(id, payload, mall)) return false;
            out = std::move(mall);
            return true;
        }
        if (proto::IsJobMessage(id)) {
            proto::JobEvent job;
            if (!proto::DecodeJobEvent(id, payload, job)) return false;
            out = std::move(job);
            return true;
        }
        if (proto::IsCosMessage(id)) {
            proto::CosEvent cos;
            if (!proto::DecodeCosEvent(id, payload, cos)) return false;
            out = std::move(cos);
            return true;
        }
        if (proto::IsExchangeMessage(id)) {
            proto::ExchangeEvent exchange;
            if (!proto::DecodeExchangeEvent(id, payload, exchange)) return false;
            out = std::move(exchange);
            return true;
        }
        if (proto::IsOpenMarketMessage(id)) {
            proto::OpenMarketEvent market;
            if (!proto::DecodeOpenMarketEvent(id, payload, market)) return false;
            out = std::move(market);
            return true;
        }
        if (proto::IsAcademyMessage(id)) {
            proto::AcademyEvent academy;
            if (!proto::DecodeAcademyEvent(id, payload, academy)) return false;
            out = std::move(academy);
            return true;
        }
        if (proto::IsCommunityMessage(id)) {
            proto::CommunityEvent community;
            if (!proto::DecodeCommunityEvent(id, payload, community)) return false;
            out = std::move(community);
            return true;
        }
        if (proto::IsFortressMessage(id)) {
            proto::FortressEvent fortress;
            if (!proto::DecodeFortressEvent(id, payload, fortress)) return false;
            out = std::move(fortress);
            return true;
        }
        if (!proto::DecodeGuildEvent(id, payload, guild)) return false;
        out = std::move(guild);
        return true;
    }
    }
}

std::vector<TitleEvent> WorldReplay::TakeUntil(double time) {
    std::vector<TitleEvent> out;
    const auto& messages = recording_->messages;
    for (; next_ < messages.size() && messages[next_].time < time; ++next_) {
        TitleEvent event;
        if (DecodeWorldEvent(messages[next_].id, messages[next_].payload, event)) {
            out.push_back(std::move(event));
        } else {
            ++malformed_;
        }
    }
    return out;
}

std::uint64_t ReplayHash(const WorldRecording& recording, double rate, double settle, WorldView& view) {
    view.Clear();
    WorldReplay replay(recording);
    const double step = 1.0 / rate, end = recording.Duration() + settle;
    // Before each step, what arrived before its start: the same split however it is driven.
    for (std::int64_t i = 0; double(i) * step <= end; ++i) {
        for (const TitleEvent& event : replay.TakeUntil(double(i) * step)) view.Apply(event);
        view.Step(step);
    }
    return view.Hash();
}

} // namespace sr::game
