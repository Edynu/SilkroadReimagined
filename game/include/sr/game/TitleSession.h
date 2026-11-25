#pragma once

// The client's side of the title flow, headless: it talks to the
// gateway (server list, notices), the login server (account, password, chosen server) and the
// game server's lobby (the ticket, then characters), each over its own connection with Hello
// first; after the select, the same game connection carries the world (C6c: enter, move, leave;
// the world's messages come back as events). The screens call its requests and read what came back as events, once per frame
// (Update on the game thread; the sockets live on the NetService's threads).

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "sr/game/AcademyProtocol.h"
#include "sr/game/OpenMarketProtocol.h"
#include "sr/game/CommunityProtocol.h"
#include "sr/game/AlchemyProtocol.h"
#include "sr/game/ExchangeProtocol.h"
#include "sr/game/StallProtocol.h"
#include "sr/game/QuestProtocol.h"
#include "sr/game/InstanceProtocol.h"
#include "sr/game/CosProtocol.h"
#include "sr/game/FortressProtocol.h"
#include "sr/game/JobProtocol.h"
#include "sr/game/PkProtocol.h"
#include "sr/game/EventProtocol.h"
#include "sr/game/MallProtocol.h"
#include "sr/game/LobbyProtocol.h"
#include "sr/game/TitleProtocol.h"
#include "sr/game/WorldProtocol.h"
#include "sr/net/Connection.h"

namespace sr::game {

// Why a connection of the title flow failed.
struct TitleError {
    enum class Where : std::uint8_t { Gateway, Login, Game };
    Where where = Where::Gateway;
    std::string reason;
    bool protocolMismatch = false; // the server speaks another protocol version: update the client
};

using TitleEvent = std::variant<proto::ServerList, proto::Notices, proto::LoginResult, proto::LobbyEntered, proto::CharacterList,
                                proto::LobbyResult, TitleError, proto::WorldEntered, proto::WorldLeft, proto::EntityAppeared,
                                proto::EntityGone, proto::EntityMoved, proto::EntityTurned, proto::GroundItemFree, proto::GateRefused, proto::LogoutAnswer,
                                proto::QuickSlotList, proto::ChatAnswer, proto::ChatLine, proto::InventoryList, proto::ItemMoved,
                                proto::EquipmentChanged, proto::GateMenu, proto::ReturnPointSet, proto::GoldChanged,
                                proto::ReturnStarted, proto::ReturnCancelled, proto::BuffAdded, proto::BuffRemoved,
                                proto::NpcMenu, proto::StorageRefused, proto::StorageList, proto::StorageMoved, proto::StorageGoldChanged,
                                proto::ShopAnswer, proto::NpcRepaired, proto::EntityAction, proto::EntitySwing, proto::EntityCastCancelled, proto::EntityBlink, proto::EntityDamaged, proto::EntityStates, proto::EntityBuffs, proto::SkillObjectStruck, proto::EntityKnocked,
                                proto::Vitals, proto::Progress, proto::ExperienceGained, proto::EntityLevelUp, proto::SkillBookList,
                                proto::SkillBookAnswer, proto::SkillUseAnswer, proto::ActionHeld, proto::WeatherNow, proto::WorldClock, proto::AccountRestricted, proto::Cooldown, proto::ViewLimitsAnswer, proto::RevivalOffered, proto::SkillTook, proto::EntityLook,
                                proto::LinkAdded, proto::LinkPosition, proto::BerserkAnswer, proto::BerserkPoints, proto::EntityBerserk,
                                // The Action page's motions and marks
                                proto::EntityMotion, proto::EntityMarks, proto::EntityEmote, proto::FreePvpAnswer, proto::EntityFreePvp,
                                // Parties and party matching
                                proto::PartyInvited, proto::PartyResult, proto::PartyInfo, proto::PartyMemberJoined, proto::PartyMemberLeft,
                                proto::PartyDismissed, proto::PartyMemberUpdate, proto::PartyLeaderChanged, proto::PartySettingsChanged,
                                proto::PartyLoot, proto::ItemGained, proto::PartyMatchAnswer, proto::PartyMatchListing, proto::PartyMatchJoinRequest,
                                proto::GuildEvent,
                                // The fortress war
                                proto::FortressEvent,
                                // Friends, the block list and letters
                                proto::CommunityEvent,
                                // The Academy
                                proto::AcademyEvent,
                                // The Open Market
                                proto::OpenMarketEvent,
                                // The exchange between two players
                                proto::ExchangeEvent,
                                // Stalls and the stall network
                                proto::StallEvent,
                                // The quests; the bag grown by one's reward
                                proto::QuestEvent, proto::InventoryGrown,
                                // The Forgotten World
                                proto::InstanceEvent,
                                // Rides, transports and pets
                                proto::CosEvent, proto::JobEvent,
                                // Murderers
                                proto::PkEvent,
                                // Capture the Flag and Event So-Ok (55)
                                proto::EventEvent,
                                // The item mall, silk, Magic POP, the mall's items (57, 58)
                                proto::MallEvent,
                                // The world goes on on another game server of the shard
                                proto::ServerMove,
                                // The auto potion
                                proto::AutoPotionSettings, proto::ItemUsed, proto::ItemRefused, proto::Disconnected,
                                // A unique's news (0x300C 0x0C05 / 0x0C06)
                                proto::UniqueNotice,
                                // Alchemy
                                proto::AlchemyResult,
                                // The avatars worn
                                proto::AvatarList, proto::AvatarMoved>;

// What the game connection carried (the F1 Network panel): its totals, and what arrived by message.
struct SessionNetworkStats {
    net::ConnectionStats game;
    struct Kind {
        std::uint64_t messages = 0, bytes = 0; // bytes: frames (id + payload + length)
    };
    std::map<proto::MessageId, Kind> received;
    // A walk's round trip: from MoveTo sent to the player's own new leg arriving (ms, the last 64).
    std::deque<double> moveTrips;
};

struct WorldRecording;

class TitleSession {
public:
    // `tls`: the certificate the servers' chains end in (pinned in development) and, when its
    // serverName is set, the name every server's certificate carries; otherwise each server's host.
    TitleSession(net::NetService& net, net::TlsClientConfig tls, std::string clientName = "SroReimagined");
    ~TitleSession();

    // The gateway: connects (if needed) and asks for the server list and the notices.
    void ConnectGateway(const std::string& host, std::uint16_t port);
    void RequestServerList();
    void RequestNotices();
    // Logs in to the login server named by the last server list, for game server `server`. On
    // success the session goes on to that game server with the ticket by itself (LobbyEntered).
    void LogIn(const std::string& account, const std::string& password, std::uint16_t server, std::uint16_t lobby = 0);

    // The lobby (after LobbyEntered Accepted).
    void ListCharacters();
    void Create(const proto::CharacterCreate& character);
    void CheckName(const std::string& name);
    void Delete(const std::string& name);
    void Restore(const std::string& name);
    void Select(const std::string& name);
    // The world (after the select): in, a click's point, out (back to the lobby).
    void EnterWorld();
    // `toward`: a click on the sky, that way until something stops the walker (proto::MoveToward).
    void MoveTo(const proto::WorldPosition& target, bool toward = false);
    // The player turned where it stands (proto::Turn).
    void Turn(float yaw);
    void LeaveWorld();
    // The system menu's Restart / Exit: the server counts down, then the character leaves.
    void RequestLogout(proto::LogoutKind kind);
    // The count stopped (a click in the world while it runs: the original's 0x7006).
    void CancelLogout();
    // A shop put away (ShopClose: a specialty NPC's decides the safe trade).
    void CloseShop(std::uint32_t npc);
    // A quick slot the player changed (the server keeps them).
    void SetQuickSlot(const proto::QuickSlotRecord& record);
    // A chat line (PlayerState::QueueChat numbers it).
    void SendChat(const proto::ChatSend& line);
    // An item moved in the inventory (the server answers ItemMoved).
    void MoveItem(const proto::ItemMove& move);
    // An avatar put on or taken off (the server answers AvatarMoved).
    void MoveAvatar(const proto::AvatarMove& move);
    // At a gate (its reference id), to a point it links to (C6e).
    void UseGate(std::uint32_t gate, std::uint32_t destination);
    // The item in an inventory place used (the return scroll; ItemMoved answers).
    void UseItem(std::uint8_t slot, std::uint8_t option = 0, std::uint32_t point = 0, std::uint32_t target = 0); // target: the selection
    // The return scroll's reading cancelled (ReturnCancelled answers).
    void CancelReturn();
    // At a gate: what it offers (GateMenu), and its point as the return point (C7i).
    void TalkToGate(std::uint32_t gate);
    void DesignateReturn(std::uint32_t gate);
    // Near an NPC (its entity): what it offers (NpcMenu).
    void TalkToNpc(std::uint32_t entity);
    // At a storage keeper: the storage (StorageList); an item moved with it (StorageMoved).
    void OpenStorage(std::uint32_t entity);
    void MoveStorageItem(const proto::StorageMove& move);
    void MoveStorageGold(bool deposit, std::uint64_t amount, bool guild = false);
    // The guild's storage at an NPC that keeps guilds, and put away (another member may open it).
    void OpenGuildStorage(std::uint32_t entity);
    void CloseGuildStorage();
    // At a merchant: its shop open, a good bought, an item sold, one bought back (ShopAnswer).
    void Shop(const proto::ShopOpen& open);
    void Shop(const proto::ShopBuy& buy);
    void Shop(const proto::ShopSell& sell);
    void Shop(const proto::ShopBuyback& back);
    // Items on the ground: one of the bag dropped, gold dropped, one picked up.
    void DropItem(std::uint8_t place);
    void DropGold(std::uint64_t amount);
    void PickUp(std::uint32_t entity);
    // Fighting (C10b): the normal attack on a monster (0 stops); back to life after dying.
    void Attack(std::uint32_t target);
    void Resurrect(std::uint8_t kind);
    // A stat point into strength (0) or intelligence (1).
    void RaiseStat(std::uint8_t stat);
    // Skills (C7f): a mastery's next level, a skill's next level (its skilldata id).
    void RaiseMastery(std::uint32_t mastery);
    void LearnSkill(std::uint32_t skill);
    void UseSkill(std::uint32_t skill, std::uint32_t target, const proto::WorldPosition* point = nullptr);
    // The player's stop (proto::ActionStop): a ground click with one command held sends it before its walk.
    void StopAction();
    // Ends one of the player's buffs (its token; K4).
    void CancelBuff(std::uint32_t token);
    // A resurrection offered to the dead player, answered (K9).
    void AnswerRevival(bool accept);
    // A developer's teleport to a point, a developer's death (servers started with --dev-commands only).
    void DevGoto(std::uint32_t point);
    void DevDie();
    // Go berserk (the gauge full).
    void Berserk();
    // Parties: ask a player to party (a new party with `settings` when in
    // none), answer a request, leave (the master: dismiss), send a member away, change the settings
    // (the master); party matching: list the party, take the listing off, a page of the listings, ask
    // to join one, answer a join request (the leader).
    void PartyInvite(std::uint32_t target, std::uint8_t settings);
    void AnswerPartyInvite(bool accept);
    void LeaveParty();
    void KickPartyMember(std::uint32_t member);
    void SetPartySettings(std::uint8_t settings);
    void RegisterPartyMatch(const proto::PartyMatchForm& form);
    void DeletePartyMatch();
    void ListPartyMatches(const proto::PartyMatchList& query);
    void JoinPartyMatch(std::uint32_t entry);
    void AnswerPartyMatchJoin(bool accept);
    // The auto potion's settings kept (the server answers with what it kept, AutoPotionSettings).
    void SetAutoPotion(const proto::AutoPotionSettings& settings);
    // The player's own view (Online options; 0: the server's share): ViewLimitsAnswer says what applies.
    void SetViewLimits(std::uint16_t players, std::uint16_t monsters, std::uint16_t radius = 0);
    // A guild request (GuildProtocol.h) to the game server.
    template <typename T>
    void SendGuild(const T& message) {
        SendGame(T::kId, proto::Encode(message));
    }
    void SendGame(proto::MessageId id, const std::vector<std::byte>& payload);
    // The game connection's traffic so far.
    const SessionNetworkStats& Network();
    // Keeps every world message that arrives from now on in `recording` (C6d), or stops (null).
    void Record(WorldRecording* recording) {
        recording_ = recording;
        recordStart_ = Clock::now();
    }

    // Drains the connections; call once per frame.
    void Update();
    // What arrived since the last call, in order.
    std::vector<TitleEvent> TakeEvents();

    bool InLobby() const { return inLobby_; }
    const proto::ServerList* LastServerList() const { return haveList_ ? &list_ : nullptr; }
    void Disconnect();

private:
    struct Link {
        std::shared_ptr<net::Connection> connection;
        bool welcomed = false;
        bool answered = false; // something besides Welcome arrived
        TitleError::Where where = TitleError::Where::Gateway;
    };
    void Open(Link& link, const std::string& host, std::uint16_t port, TitleError::Where where);
    template <typename T>
    void Send(Link& link, const T& message);
    void Drain(Link& link);
    void Handle(Link& link, const net::Message& m);
    void Fail(Link& link, std::string reason, bool mismatch = false);

    net::NetService& net_;
    net::TlsClientConfig tls_;
    std::string clientName_;
    Link gateway_, login_, game_;
    std::vector<TitleEvent> events_;
    std::vector<net::Message> scratch_;
    proto::ServerList list_;
    bool moving_ = false; // between two game servers of the shard
    bool haveList_ = false;
    bool inLobby_ = false;
    std::vector<std::uint8_t> ticket_;
    using Clock = std::chrono::steady_clock;
    Clock::time_point nextKeepAlive_{};
    WorldRecording* recording_ = nullptr;
    Clock::time_point recordStart_;
    SessionNetworkStats network_;
    std::uint32_t self_ = 0;                  // the player's entity (WorldEntered)
    Clock::time_point moveSent_{};            // the last MoveTo not yet answered by its own leg
    bool moveWaiting_ = false;
};

} // namespace sr::game
