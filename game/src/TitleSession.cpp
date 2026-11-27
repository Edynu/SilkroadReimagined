#include "sr/game/TitleSession.h"

#include <format>

#include "sr/core/Log.h"
#include "sr/game/WorldRecording.h"

namespace sr::game {

TitleSession::TitleSession(net::NetService& net, net::TlsClientConfig tls, std::string clientName)
    : net_(net), tls_(std::move(tls)), clientName_(std::move(clientName)) {}

TitleSession::~TitleSession() { Disconnect(); }

void TitleSession::Open(Link& link, const std::string& host, std::uint16_t port, TitleError::Where where) {
    if (link.connection) link.connection->Close();
    net::ConnectConfig c;
    c.host = host;
    c.port = port;
    c.tls = tls_;
    if (c.tls.serverName.empty()) c.tls.serverName = host;
    c.limits.maxFrame = 1u << 20; // a server may send more than a client may (lists, notices)
    link.connection = net_.Connect(c);
    link.welcomed = false;
    link.answered = false;
    link.where = where;
    proto::Hello hello;
    hello.client = clientName_;
    link.connection->Send(proto::Hello::kId, proto::Encode(hello)); // goes out once connected
}

template <typename T>
void TitleSession::Send(Link& link, const T& message) {
    // Between two game servers: the world does not listen until it has taken the character in.
    if (moving_ && &link == &game_ && T::kId != proto::LobbyEnter::kId) return;
    if (!link.connection || link.connection->State() == net::ConnectionState::Closed) {
        Fail(link, "not connected");
        return;
    }
    link.connection->Send(T::kId, proto::Encode(message));
}

void TitleSession::ConnectGateway(const std::string& host, std::uint16_t port) {
    Open(gateway_, host, port, TitleError::Where::Gateway);
    RequestServerList();
    RequestNotices();
}

void TitleSession::RequestServerList() { Send(gateway_, proto::ServerListRequest{}); }
void TitleSession::RequestNotices() { Send(gateway_, proto::NoticesRequest{}); }

void TitleSession::LogIn(const std::string& account, const std::string& password, std::uint16_t server, std::uint16_t lobby) {
    if (!haveList_ || list_.loginPort == 0) {
        Fail(login_, "no server list yet");
        return;
    }
    Open(login_, list_.loginHost, list_.loginPort, TitleError::Where::Login);
    proto::LoginRequest request;
    request.account = account;
    request.password = password;
    request.server = server;
    request.lobby = lobby;
    Send(login_, request);
}

void TitleSession::ListCharacters() { Send(game_, proto::CharacterListRequest{}); }
void TitleSession::Create(const proto::CharacterCreate& character) { Send(game_, character); }
void TitleSession::CheckName(const std::string& name) { Send(game_, proto::CharacterNameCheck{name}); }
void TitleSession::Delete(const std::string& name) { Send(game_, proto::CharacterDelete{name}); }
void TitleSession::Restore(const std::string& name) { Send(game_, proto::CharacterRestore{name}); }
void TitleSession::Select(const std::string& name) { Send(game_, proto::CharacterSelect{name}); }

void TitleSession::EnterWorld() { Send(game_, proto::WorldEnter{}); }

void TitleSession::MoveTo(const proto::WorldPosition& target, bool toward) {
    if (toward) {
        Send(game_, proto::MoveToward{target});
    } else {
        Send(game_, proto::MoveTo{target});
    }
    if (!moveWaiting_) {
        moveSent_ = Clock::now();
        moveWaiting_ = true;
    }
}

void TitleSession::Turn(float yaw) { Send(game_, proto::Turn{proto::HeadingOf(yaw)}); }

void TitleSession::LeaveWorld() { Send(game_, proto::WorldLeave{}); }

void TitleSession::RequestLogout(proto::LogoutKind kind) { Send(game_, proto::LogoutRequest{kind}); }
void TitleSession::CancelLogout() { Send(game_, proto::LogoutCancel{}); }
void TitleSession::CloseShop(std::uint32_t npc) { Send(game_, proto::ShopClose{npc}); }

void TitleSession::SetQuickSlot(const proto::QuickSlotRecord& record) { Send(game_, proto::QuickSlotSet{record}); }

void TitleSession::SendChat(const proto::ChatSend& line) { Send(game_, line); }
void TitleSession::SendGame(proto::MessageId id, const std::vector<std::byte>& payload) {
    if (game_.connection && !moving_) game_.connection->Send(id, payload);
}

void TitleSession::MoveItem(const proto::ItemMove& move) { Send(game_, move); }

void TitleSession::MoveAvatar(const proto::AvatarMove& move) { Send(game_, move); }

void TitleSession::UseGate(std::uint32_t gate, std::uint32_t destination) { Send(game_, proto::UseGate{gate, destination}); }

void TitleSession::UseItem(std::uint8_t slot, std::uint8_t option, std::uint32_t point, std::uint32_t target) {
    Send(game_, proto::UseItem{slot, option, point, target});
}
void TitleSession::SetAutoPotion(const proto::AutoPotionSettings& settings) { Send(game_, settings); }

void TitleSession::CancelReturn() { Send(game_, proto::CancelReturn{}); }
void TitleSession::CancelCasting(proto::CastingKind kind, std::uint32_t id) { Send(game_, proto::CancelCasting{kind, id}); }

void TitleSession::TalkToGate(std::uint32_t gate) { Send(game_, proto::GateTalk{gate}); }

void TitleSession::DropItem(std::uint8_t place) { Send(game_, proto::DropItem{place}); }

void TitleSession::DropGold(std::uint64_t amount) { Send(game_, proto::DropGold{amount}); }

void TitleSession::PickUp(std::uint32_t entity) { Send(game_, proto::PickUp{entity}); }
void TitleSession::Attack(std::uint32_t target) { Send(game_, proto::AttackTarget{target}); }
void TitleSession::Resurrect(std::uint8_t kind) { Send(game_, proto::Resurrect{kind}); }
void TitleSession::RaiseStat(std::uint8_t stat) { Send(game_, proto::RaiseStat{stat}); }
void TitleSession::RaiseMastery(std::uint32_t mastery) { Send(game_, proto::RaiseMastery{mastery}); }
void TitleSession::LearnSkill(std::uint32_t skill) { Send(game_, proto::LearnSkill{skill}); }
void TitleSession::UseSkill(std::uint32_t skill, std::uint32_t target, const proto::WorldPosition* point) {
    proto::UseSkill use{skill, target};
    if (point != nullptr) {
        use.hasPoint = true;
        use.point = *point;
    }
    Send(game_, use);
}
void TitleSession::CancelBuff(std::uint32_t token) { Send(game_, proto::CancelBuff{token}); }
void TitleSession::StopAction() { Send(game_, proto::ActionStop{}); }

void TitleSession::Shop(const proto::ShopOpen& open) { Send(game_, open); }

void TitleSession::Shop(const proto::ShopBuy& buy) { Send(game_, buy); }

void TitleSession::Shop(const proto::ShopSell& sell) { Send(game_, sell); }

void TitleSession::Shop(const proto::ShopBuyback& back) { Send(game_, back); }

void TitleSession::OpenStorage(std::uint32_t entity) { Send(game_, proto::StorageOpen{entity}); }

void TitleSession::MoveStorageGold(bool deposit, std::uint64_t amount, bool guild) {
    Send(game_, proto::StorageGold{deposit, amount, std::uint8_t(guild ? 1 : 0)});
}
void TitleSession::OpenGuildStorage(std::uint32_t entity) { Send(game_, proto::StorageOpen{entity, 1}); }
void TitleSession::CloseGuildStorage() { Send(game_, proto::StorageClose{1}); }

void TitleSession::MoveStorageItem(const proto::StorageMove& move) { Send(game_, move); }

void TitleSession::TalkToNpc(std::uint32_t entity) { Send(game_, proto::NpcTalk{entity}); }

void TitleSession::DesignateReturn(std::uint32_t gate) { Send(game_, proto::DesignateReturn{gate}); }

void TitleSession::DevGoto(std::uint32_t point) { Send(game_, proto::DevGoto{point}); }
void TitleSession::DevDie() { Send(game_, proto::DevDie{}); }
void TitleSession::Berserk() { Send(game_, proto::BerserkRequest{}); }
void TitleSession::AnswerRevival(bool accept) { Send(game_, proto::RevivalAnswer{std::uint8_t(accept ? 1 : 0)}); }
void TitleSession::PartyInvite(std::uint32_t target, std::uint8_t settings) { Send(game_, proto::PartyInvite{target, settings}); }
void TitleSession::AnswerPartyInvite(bool accept) { Send(game_, proto::PartyInviteAnswer{std::uint8_t(accept ? 1 : 0)}); }
void TitleSession::LeaveParty() { Send(game_, proto::PartyLeave{}); }
void TitleSession::KickPartyMember(std::uint32_t member) { Send(game_, proto::PartyKick{member}); }
void TitleSession::SetPartySettings(std::uint8_t settings) { Send(game_, proto::PartySetSettings{settings}); }
void TitleSession::RegisterPartyMatch(const proto::PartyMatchForm& form) { Send(game_, proto::PartyMatchRegister{form}); }
void TitleSession::DeletePartyMatch() { Send(game_, proto::PartyMatchDelete{}); }
void TitleSession::ListPartyMatches(const proto::PartyMatchList& query) { Send(game_, query); }
void TitleSession::JoinPartyMatch(std::uint32_t entry) { Send(game_, proto::PartyMatchJoin{entry}); }
void TitleSession::AnswerPartyMatchJoin(bool accept) { Send(game_, proto::PartyMatchJoinAnswer{std::uint8_t(accept ? 1 : 0)}); }

void TitleSession::SetViewLimits(std::uint16_t players, std::uint16_t monsters, std::uint16_t radius) { Send(game_, proto::ViewLimits{players, monsters, radius}); }

const SessionNetworkStats& TitleSession::Network() {
    if (game_.connection) network_.game = game_.connection->Stats();
    return network_;
}

void TitleSession::Update() {
    Drain(gateway_);
    Drain(login_);
    Drain(game_);
    // The game server closes a connection silent for long (PeerLimits::idleTimeout): one word every
    // 10 s while in its lobby or world, so an idle player stays.
    if (inLobby_ && game_.connection && game_.welcomed && Clock::now() >= nextKeepAlive_) {
        nextKeepAlive_ = Clock::now() + std::chrono::seconds(10);
        game_.connection->Send(proto::KeepAlive::kId, proto::Encode(proto::KeepAlive{}));
    }
}

std::vector<TitleEvent> TitleSession::TakeEvents() {
    std::vector<TitleEvent> out;
    out.swap(events_);
    return out;
}

void TitleSession::Disconnect() {
    for (Link* link : {&gateway_, &login_, &game_}) {
        if (link->connection) link->connection->Close();
        link->connection.reset();
    }
    inLobby_ = false;
}

void TitleSession::Fail(Link& link, std::string reason, bool mismatch) {
    TitleError e;
    e.where = link.where;
    e.reason = std::move(reason);
    e.protocolMismatch = mismatch;
    events_.push_back(std::move(e));
    if (link.connection) link.connection->Close();
    link.connection.reset();
    if (&link == &game_) inLobby_ = false;
}

void TitleSession::Drain(Link& link) {
    if (!link.connection) return;
    scratch_.clear();
    link.connection->Receive(scratch_);
    for (const net::Message& m : scratch_) {
        if (&link == &game_) {
            SessionNetworkStats::Kind& kind = network_.received[m.id];
            ++kind.messages;
            kind.bytes += m.payload.size() + net::kFrameHeaderSize;
        }
        const net::Connection* from = link.connection.get();
        Handle(link, m);
        if (!link.connection) return;
        // A server move opened the link's new connection (ServerMove): what the old one still had is not the new one's
        // (fed to it, its first message would fail the new link's Hello).
        if (link.connection.get() != from) return;
    }
    if (link.connection->State() == net::ConnectionState::Closed) {
        // The login server closes after its answer, as the game server does after a refusal; any
        // other close is a failure (the gateway's, a close before an answer, losing the lobby).
        const bool expected = link.answered && (&link == &login_ || (&link == &game_ && !inLobby_));
        if (!expected) {
            Fail(link, link.connection->CloseReason());
        } else {
            link.connection.reset();
        }
    }
}

void TitleSession::Handle(Link& link, const net::Message& m) {
    const auto decode = [&](auto& message) {
        if (proto::Decode(m.payload, message)) return true;
        Fail(link, std::format("a malformed message 0x{:04x}", m.id));
        return false;
    };
    if (!link.welcomed) {
        proto::Welcome welcome;
        if (m.id != proto::Welcome::kId || !decode(welcome)) {
            if (link.connection) Fail(link, "the server did not answer Hello");
            return;
        }
        if (welcome.result != proto::WelcomeResult::Accepted) {
            Fail(link, welcome.reason.empty() ? "refused" : welcome.reason, welcome.result == proto::WelcomeResult::ProtocolMismatch);
            return;
        }
        link.welcomed = true;
        return;
    }
    link.answered = true;
    // The skill lab's answer (a developer's spawn, its exam frame's): nothing for the client.
    if (m.id == proto::DevSpawned::kId) return;
    if (IsWorldMessage(m.id)) {
        TitleEvent event;
        if (!DecodeWorldEvent(m.id, m.payload, event)) {
            Fail(link, std::format("a malformed message 0x{:04x}", m.id));
            return;
        }
        if (recording_) recording_->messages.push_back({std::chrono::duration<double>(Clock::now() - recordStart_).count(), m.id, m.payload});
        // A walk's round trip (the F1 Network panel): the player's own new leg after a MoveTo.
        if (const auto* entered = std::get_if<proto::WorldEntered>(&event)) {
            self_ = entered->entity;
            moving_ = false;
        } else if (const auto* moved = std::get_if<proto::EntityMoved>(&event); moved && moveWaiting_ && moved->entity == self_) {
            network_.moveTrips.push_back(std::chrono::duration<double, std::milli>(Clock::now() - moveSent_).count());
            if (network_.moveTrips.size() > 64) network_.moveTrips.pop_front();
            moveWaiting_ = false;
        }
        events_.push_back(std::move(event));
        return;
    }
    switch (m.id) {
    case proto::ServerList::kId:
        if (proto::ServerList list; decode(list)) {
            list_ = list;
            haveList_ = true;
            events_.push_back(std::move(list));
        }
        return;
    case proto::Notices::kId:
        if (proto::Notices notices; decode(notices)) events_.push_back(std::move(notices));
        return;
    case proto::ServerMove::kId:
        // The world goes on on another game server of the shard.
        if (proto::ServerMove move; decode(move)) {
            moving_ = true;
            Open(game_, move.host, move.port, TitleError::Where::Game);
            proto::LobbyEnter enter;
            enter.ticket = move.ticket;
            Send(game_, enter);
            move.ticket.clear(); // the screens never need it
            events_.push_back(std::move(move));
        }
        return;
    case proto::LoginResult::kId:
        if (proto::LoginResult result; decode(result)) {
            if (result.status == proto::LoginStatus::Accepted) {
                // On to the chosen game server with the ticket.
                ticket_ = result.ticket;
                Open(game_, result.host, result.port, TitleError::Where::Game);
                proto::LobbyEnter enter;
                enter.ticket = ticket_;
                Send(game_, enter);
            }
            result.ticket.clear(); // the screens never need it
            events_.push_back(std::move(result));
        }
        return;
    case proto::LobbyEntered::kId:
        if (proto::LobbyEntered entered; decode(entered)) {
            if (moving_ && entered.status != proto::LobbyEnterStatus::Accepted) {
                Fail(game_, "the next game server would not take the character");
                return;
            }
            if (moving_) return; // the world follows on its own
            ticket_.clear();
            inLobby_ = entered.status == proto::LobbyEnterStatus::Accepted;
            // In: the gateway is done with (as the original hangs up on it after the login). Kept
            // open it would be closed for its silence (PeerLimits::idleTimeout) and that close
            // would fail the session; the title screen connects again when it opens.
            if (inLobby_ && gateway_.connection) {
                gateway_.connection->Close();
                gateway_.connection.reset();
            }
            events_.push_back(entered);
        }
        return;
    case proto::CharacterList::kId:
        if (proto::CharacterList list; decode(list)) events_.push_back(std::move(list));
        return;
    case proto::LobbyResult::kId:
        if (proto::LobbyResult result; decode(result)) events_.push_back(result);
        return;
    default: Fail(link, std::format("an unexpected message 0x{:04x}", m.id));
    }
}

} // namespace sr::game
