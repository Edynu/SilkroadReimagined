#include "title/TitleScreen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "sr/assets/Terrain.h"
#include "sr/core/Log.h"
#include "sr/core/Text.h"
#include "sr/core/Trace.h"
#include "sr/platform/Window.h"
#ifdef SR_PLAYER_BUILD
#include "PlayerServer.h"
#endif

namespace sr::client {

namespace proto = game::proto;

namespace {

// Observed in the title's setup and login window: the window art the code
// puts in place of pstitle.txt's, and the server list's row faces.
constexpr const char* kLoginWindow = "interface\\outer\\login_window_europe.ddj";
constexpr const char* kServerSelect = "interface\\outer\\server_select.ddj";
constexpr const char* kServerRollover = "interface\\outer\\server_rollover.ddj";

// Placement (inferred, from the prototype; to check against in RE-8): the
// letterbox bars at 172 and 1030 of a 1200-high virtual screen, the login window 288 x 140 under
// the centre of the space between them, the server list 240 x 340 with 13 rows of 20.
constexpr float kVirtualHeight = 1200.0f, kBarTop = 172.0f, kBarBottom = 1030.0f;
constexpr int kServerRows = 13;
constexpr float kServerRowHeight = 20.0f;

#ifndef SR_PLAYER_BUILD
std::string ReadFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}
#endif

// The original's load grading (inferred from its texts and the English thresholds in
// textuisystem: UIO_STT_SERVER_35 = 35, UIO_STT_SERVER_75 = 97).
const char* LoadKey(const proto::ServerEntry& s, ui::Color& color) {
    if (s.state == proto::ServerState::Offline) {
        color = ui::Rgba(0xc0, 0xbd, 0xff);
        return "UIO_STT_SERVER_TEST"; // "Check"
    }
    if (s.state == proto::ServerState::Full || (s.capacity > 0 && s.online >= s.capacity)) {
        color = ui::Rgba(0xff, 0x58, 0x58);
        return "UIO_STT_SERVER_MAX_FULL"; // "FULL"
    }
    const float load = s.capacity > 0 ? 100.0f * float(s.online) / float(s.capacity) : 0.0f;
    if (load < 35.0f) {
        color = ui::Rgba(0x6d, 0xff, 0xef);
        return "UIO_STT_EXCELLENT"; // "Easy"
    }
    if (load < 97.0f) {
        color = ui::Rgba(0xff, 0xda, 0x49);
        return "UIO_STT_CROWDEDNESS"; // "Populated"
    }
    color = ui::Rgba(0xff, 0xb5, 0x41);
    return "UIO_STT_SERVER_FULL"; // "Crowded"
}

core::DesignRect Rect(float x, float y, float w, float h) { return {x, y, w, h}; }

} // namespace

void TitleScreen::Enter(ScreenContext& context) {
    shared_ = std::make_shared<TitleShared>();
    shared_->net = std::make_unique<net::NetService>(1);
    net::TlsClientConfig tls;
#ifdef SR_PLAYER_BUILD
    // A players' build plays on the shard built into it (PlayerServer.h), whatever its settings say.
    tls.trustedCertificatePem = std::string(player::kCertificate);
    tls.serverName = std::string(player::kServerName);
#else
    const std::string certificate = context.config.GetString("server-certificate", "dev\\server.pem");
    tls.trustedCertificatePem = ReadFile(certificate);
    tls.serverName = context.config.GetString("server-name", "localhost");
    if (tls.trustedCertificatePem.empty()) core::LogWarning("title: cannot read the server certificate {}", certificate);
#endif
    // Its name and version in Hello: the shard turns an older one away (the servers' ClientGate.h).
    shared_->session = std::make_unique<game::TitleSession>(*shared_->net, tls, "SroReimagined " SR_CLIENT_VERSION);
    shared_->data.Load(context.jobs, context.files, {kTitleLayout, kSelectLayout, kCreateChinaLayout, kCreateEuropeLayout});
    // The character tables from now on, not from the lobby: the login hides their time.
    shared_->LoadCharacters(context.jobs, context.files);
    // The intro flight's camera keys (the original reads the intro's name from its options;
    // v1.150 ships Constantinople's).
    intro_ = std::make_shared<Intro>();
    context.jobs.Run(
        [intro = intro_, files = &context.files] {
            SR_ZONE("title: intro script");
            std::string error;
            const auto file = files->Read("script\\intro\\constantinople.txt");
            if (!file || !assets::DecodeIntroCamera(core::DecodeTextFile(file->Bytes()), intro->keys, error)) {
                core::LogWarning("title: intro script: {}", file ? error : std::string("not found"));
                intro->keys.clear();
            }
            intro->ready.store(true, std::memory_order_release);
        },
        core::JobPriority::Streaming, &introJob_);
    textures_ = std::make_unique<UiTextures>(context.assets, context.uiWarmup);
    kit_ = std::make_unique<UiKit>(ui_, *textures_, shared_->data);
    loading_.Begin(context.assets, "interface\\loading\\loading_constantinople.ddj");

#ifdef SR_PLAYER_BUILD
    const std::string gateway(player::kGateway);
#else
    const std::string gateway = context.config.GetString("gateway", "127.0.0.1:15779");
#endif
    const std::size_t colon = gateway.rfind(':');
    const std::string host = colon == std::string::npos ? gateway : gateway.substr(0, colon);
    const int port = colon == std::string::npos ? 15779 : std::atoi(gateway.c_str() + colon + 1);
    shared_->session->ConnectGateway(host, std::uint16_t(port));
    message_ = "Connecting...";
    // Back from a game the server closed, saying why (TitleShared::kFarewell): said here instead.
    if (const auto it = context.handoff.find(TitleShared::kFarewell); it != context.handoff.end()) {
        if (it->second) message_ = *std::static_pointer_cast<std::string>(it->second);
        context.handoff.erase(it);
    }
    busy_ = true;
    account_ = context.config.GetString("account", "");
    password_ = context.config.GetString("password", "");
    autoLogin_ = !account_.empty() && !password_.empty();
    shared_->server = std::uint16_t(context.config.GetInt("server", 0));
    lobbyServer_ = shared_->server; // --server N: that game server shows the characters (developers)
}

void TitleScreen::Exit(ScreenContext& context, FrameSnapshot& snapshot) {
    context.jobs.Wait(introJob_);
    // The tables' jobs read the file system: waited for unless the lobby took the session (it waits).
    if (shared_ && shared_.use_count() == 1) context.jobs.Wait(shared_->charactersJob);
    loading_.End(context.assets);
    if (world_) world_->Exit(context, snapshot);
    world_.reset();
    if (const rhi::TextureHandle t = atlas_.Release()) snapshot.releaseTextures.push_back(t);
    textures_.reset();
    kit_.reset();
    shared_.reset();
}

void TitleScreen::HandleEvents() {
    for (game::TitleEvent& event : shared_->session->TakeEvents()) {
        if (auto* list = std::get_if<proto::ServerList>(&event)) {
            connected_ = true;
            if (!busy_ || message_ == "Connecting...") {
                busy_ = false;
                message_.clear();
            }
            const auto chosen = std::find_if(list->servers.begin(), list->servers.end(),
                                             [&](const proto::ServerEntry& s) { return s.id == shared_->server; });
            if (chosen == list->servers.end()) {
                // The first open server, as the original preselects one.
                for (const proto::ServerEntry& s : list->servers) {
                    if (s.state == proto::ServerState::Open) {
                        shared_->server = s.id;
                        break;
                    }
                }
            }
            for (const proto::ServerEntry& s : list->servers) {
                if (s.id == shared_->server) shared_->serverName = s.name;
            }
            if (autoLogin_ && shared_->server != 0) {
                autoLogin_ = false;
                Submit();
            }
        } else if (auto* result = std::get_if<proto::LoginResult>(&event)) {
            const UiData& d = shared_->data;
            switch (result->status) {
            case proto::LoginStatus::Accepted: message_ = "Connecting to " + shared_->serverName + "..."; break;
            case proto::LoginStatus::WrongPassword: {
                char text[256];
                std::snprintf(text, sizeof(text), d.Text("UIIT_STT_GLOBAL_PASSWORD_INPUT_ERROR").c_str(), int(result->attempts),
                              int(result->attemptLimit));
                message_ = text;
                break;
            }
            case proto::LoginStatus::UnknownAccount: message_ = "No such account."; break; // ours: the original's text is about missing details
            case proto::LoginStatus::Blocked:
                message_ = TitleShared::BlockedWords(result->reason, result->blockedUntil);
                break;
            case proto::LoginStatus::AlreadyLoggedIn: message_ = d.Text("UIO_MSG_ERROR_OVERLAP"); break;
            case proto::LoginStatus::ServerFull: message_ = d.Text("UIO_MSG_ERROR_SERVER_BUSY_CONNECT_IMPOSSIBILE"); break;
            case proto::LoginStatus::ServerUnavailable: message_ = d.Text("UIO_MSG_ERROR_ACCOUNT_CONNECT_IMPOSSIBILE"); break;
            case proto::LoginStatus::TooManyAttempts: message_ = d.Text("UIO_MSG_ERROR_CONTENT_FAIL_INSUFFICIENT_IP"); break;
            case proto::LoginStatus::Invalid: message_ = "Enter an ID of 3-32 letters or digits and a password of 6-64."; break;
            case proto::LoginStatus::Busy: message_ = d.Text("UIO_MSG_ERROR_SEVER_CONNECT"); break;
            }
            if (result->status != proto::LoginStatus::Accepted) {
                busy_ = false;
                password_.clear();
            }
        } else if (auto* entered = std::get_if<proto::LobbyEntered>(&event)) {
            if (entered->status == proto::LobbyEnterStatus::Accepted) {
                shared_->account = account_;
                // To black first, then the lobby (it fades in from black).
                leaving_ = "characters";
                fade_.Out(0.5f);
            } else {
                busy_ = false;
                message_ = entered->status == proto::LobbyEnterStatus::AlreadyLoggedIn
                               ? shared_->data.Text("UIO_MSG_ERROR_OVERLAP")
                               : shared_->data.Text("UIO_MSG_ERROR_SEVER_CONNECT");
            }
        } else if (auto* error = std::get_if<game::TitleError>(&event)) {
            core::LogWarning("title: {} connection: {}", error->where == game::TitleError::Where::Gateway ? "gateway"
                                                         : error->where == game::TitleError::Where::Login ? "login" : "game server",
                             error->reason);
            busy_ = false;
            message_ = error->protocolMismatch ? "This client is out of date for the server." : "UIO_MSG_ERROR_SEVER_CONNECT";
            if (error->protocolMismatch) {
                // The server's own words say what is out and where (ClientGate.h); an older server's are its protocol's.
                outdated_ = error->reason.starts_with("Your client") ? error->reason
                                                                    : "This client is out of date for the server. Start the launcher to update, or "
                                                                      "download the new client from the website.";
                message_.clear();
            }
            if (error->where == game::TitleError::Where::Gateway) connected_ = true; // nothing more is coming
        }
    }
}

void TitleScreen::Submit() {
    if (account_.empty()) {
        message_ = "Enter your ID.";
        focusAccount_ = true;
    } else if (password_.empty()) {
        message_ = "Enter your password.";
    } else if (shared_->server == 0) {
        message_ = "Choose a server first.";
    } else {
        busy_ = true;
        message_ = "Logging in...";
        shared_->session->LogIn(account_, password_, shared_->server, lobbyServer_);
    }
}

void TitleScreen::Update(ScreenContext& context, FrameSnapshot& snapshot) {
    SR_ZONE("title: update");
    shared_->session->Update();
    HandleEvents();
    if (!next.empty()) {
        context.holdCapture = true; // leaving: the next screen draws the frame worth capturing
        return;
    }

    UpdateIntro(context, snapshot);
    const float width = float(context.device.SwapchainWidth()), height = float(context.device.SwapchainHeight());
    fade_.Update(float(context.dt));
    if (!leaving_.empty() && fade_.Done()) {
        context.handoff[TitleShared::kHandoff] = shared_; // the session goes on in the lobby
        next = leaving_;
        context.holdCapture = true;
        return;
    }
    if (!shared_->data.Ready()) {
        context.holdCapture = true;
        HideUntilShown(context, snapshot);
        return;
    }
    ui::Font* font = shared_->data.Font();
    if (font) atlas_.Sync(context.device, *font);
    ui_.font = font;
    ui_.getClipboard = context.getClipboard;
    ui_.setClipboard = context.setClipboard;
    ui_.NewFrame(*context.input, {width, height}, context.mouseBlocked, context.keyboardBlocked);
    kit_->grid().Fit(width, height);
    ui_.BeginLayer("title", ui::LayerKind::Window);
    ui_.draw().SetWhite(font ? font->texture() : ui::TextureRef{}, font ? font->whiteUv() : ui::Vec2{0.5f, 0.5f});
    Draw(context, width, height);
    ui_.EndLayer();
    ui_.BeginLayer("title: fade", ui::LayerKind::Overlay);
    ui_.draw().SetWhite(font ? font->texture() : ui::TextureRef{}, font ? font->whiteUv() : ui::Vec2{0.5f, 0.5f});
    fade_.Draw(*kit_, width, height);
    ui_.EndLayer();
    ui_.EndFrame();
    if (font) atlas_.Sync(context.device, *font); // glyphs drawn for the first time this frame
    // The screen shows all at once, the interface and the flight together, once the world around
    // the intro, the interface data and its art are in; black until then, and never black again.
    if (!shown_ && world_ && world_->Settled() && textures_->Settled()) {
        shown_ = true;
        loading_.End(context.assets);
        fade_.In(0.5f); // from black to the title, the flight already going
    }
    if (shown_) {
        for (const ui::DrawList* list : ui_.DrawOrder()) AppendUi(snapshot.ui, *list);
    }
    HideUntilShown(context, snapshot);
    context.textInput = ui_.typing();
    // A capture waits for the art, the fade and the first answer from the gateway.
    if (!shown_ || !fade_.Done() || !connected_ || busy_ || autoLogin_) context.holdCapture = true;
}

void TitleScreen::HideUntilShown(ScreenContext& context, FrameSnapshot& snapshot) {
    if (shown_) return;
    // The world streams and the art loads behind the loading screen (the scene is not drawn).
    snapshot.scene.active = false;
    snapshot.clearColor = {0.0f, 0.0f, 0.0f, 1.0f};
    float progress = world_ ? world_->StreamProgress(context.assets) * 0.9f : 0.0f;
    if (shared_->data.Ready()) progress += 0.1f;
    loading_.SetProgress(progress);
    loadingList_.Clear();
    loading_.Draw(context.assets, loadingList_, float(context.device.SwapchainWidth()), float(context.device.SwapchainHeight()));
    AppendUi(snapshot.ui, loadingList_);
}

void TitleScreen::Draw(ScreenContext& context, float width, float height) {
    const assets::UiLayout* layout = shared_->data.Layout(kTitleLayout);
    const float top = std::round(kBarTop / kVirtualHeight * height), bottom = std::round(kBarBottom / kVirtualHeight * height);
    kit_->FillScreen({0, 0, width, top}, ui::Rgba(0, 0, 0)); // the letterbox (inferred, as the prototype)
    kit_->FillScreen({0, bottom, width, height - bottom}, ui::Rgba(0, 0, 0));
    if (!layout) {
        kit_->Text("Cannot read resinfo\\pstitle.txt", Rect(16, 16, 900, 20), ui::Rgba(255, 110, 110));
        return;
    }
    // The letterbox bars span the whole window, their art stretched across them.
    if (const auto* up = layout->Find("GDR_STA_SCREENUP")) kit_->ImageScreen(up->ddj, {0, 0, width, top});
    if (const auto* down = layout->Find("GDR_STA_SCREENDOWN")) kit_->ImageScreen(down->ddj, {0, bottom, width, height - bottom});
    if (const auto* title = layout->Find("GDR_STA_TITLE")) {
        const float s = std::min(height / kVirtualHeight, width / 800.0f);
        kit_->ImageScreen(title->ddj, {title->rect.x * s, title->rect.y * s, title->rect.w * s, title->rect.h * s});
    }

    // Everything else is on the design grid, centred between the bars.
    const float designTop = kBarTop / kVirtualHeight * core::DesignGrid::kHeight;
    const float designBottom = kBarBottom / kVirtualHeight * core::DesignGrid::kHeight;
    const float cx = core::DesignGrid::kWidth * 0.5f;
    const float cy = std::floor(designTop + (designBottom - designTop) * 0.5f);
    if (const auto* logo = layout->Find("GDR_STA_LOGO")) {
        const float s = core::DesignGrid::kHeight / kVirtualHeight;
        kit_->Image(logo->ddj, Rect(cx - 254.0f * s, std::min(cy - 114.0f, cy + 6.0f - 172.0f * s - 8.0f), 508.0f * s, 172.0f * s));
    }
    if (!outdated_.empty()) {
        DrawOutdated(context); // in place of the login: nothing else to do with this client
    } else if (serverListOpen_) {
        DrawServerList(*layout, cx, cy);
    } else {
        DrawLoginWindow(*layout, cx, cy);
    }

    const core::DesignPoint corner = kit_->grid().ToDesign({5.0f, height - 25.0f});
    // The original prints its client's version here; ours: SroReimagined's, and the game data it reads (vSRO 1.188).
    kit_->Text("SroReimagined " SR_CLIENT_VERSION " (1.188)", Rect(corner.x, corner.y, 240, 20), ui::Rgba(255, 128, 128));
    if (!message_.empty()) {
        const core::DesignPoint below = kit_->grid().ToDesign({0.0f, bottom});
        const bool blink = std::fmod(context.time, 1.0) < 0.85;
        // A text key resolved now: it may have been set before the text table was read.
        kit_->Text(shared_->data.Text(message_), Rect(0, below.y + 24.0f, core::DesignGrid::kWidth, 30),
                   blink ? ui::Rgba(255, 220, 140) : ui::Rgba(255, 220, 140, 160), 0, 1);
    }
}

void TitleScreen::UpdateIntro(ScreenContext& context, FrameSnapshot& snapshot) {
    if (!world_) {
        if (!intro_->ready.load(std::memory_order_acquire) || intro_->keys.empty()) {
            context.holdCapture = true;
            return;
        }
        const assets::CameraKey& first = intro_->keys.front();
        world_ = std::make_unique<WorldScreen>(WorldScreen::Backdrop{first.sectorX, first.sectorY});
        world_->Enter(context);
        introTime_ = first.time + float(context.config.GetDouble("intro-at", 0.0)); // --intro-at S: captures further in
    }
    const std::vector<assets::CameraKey>& keys = intro_->keys;
    if (shown_) {
        // Real time from the first frame shown (no pause while a region or model streams in): the
        // keys' times are seconds (0, 5, 9 ... 53 in Constantinople's); held on the last key
        // (inferred; the original's pace is not known yet).
        introTime_ = std::min(keys.back().time, introTime_ + float(context.dt));
    }
    // The key looks at its point from `distance` behind it; positive pitch looks down. Our fly
    // camera is the eye, relative to the first key's region, with positive pitch looking up.
    const assets::CameraKey k = assets::SampleCameraTrack(keys, introTime_);
    const assets::CameraKey& origin = keys.front();
    const float px = k.position[0] + float(k.sectorX - origin.sectorX) * assets::kRegionSize;
    const float pz = k.position[2] + float(k.sectorY - origin.sectorY) * assets::kRegionSize;
    const float pitch = k.rotation[0], yaw = k.rotation[1];
    const float fx = std::cos(pitch) * std::sin(yaw), fy = -std::sin(pitch), fz = std::cos(pitch) * std::cos(yaw);
    world_->SetView(px - fx * k.distance, k.position[1] - fy * k.distance, pz - fz * k.distance, yaw, -pitch);
    world_->Update(context, snapshot);
}

void TitleScreen::DrawOutdated(ScreenContext& context) {
    // A box in the middle: the words, the download page (the address in them) and Exit.
    const float w = 460.0f, h = 190.0f;
    const float x = std::floor((core::DesignGrid::kWidth - w) / 2.0f), y = std::floor((core::DesignGrid::kHeight - h) / 2.0f);
    kit_->Fill(Rect(x, y, w, h), ui::Rgba(10, 8, 6, 235));
    kit_->List().Outline(kit_->Screen(Rect(x, y, w, h)), ui::Rgba(150, 128, 82, 230), std::max(1.0f, kit_->grid().scale()));
    kit_->Text("A new version is out", Rect(x, y + 14.0f, w, 20), ui::Rgba(255, 228, 160), 0, 1);
    kit_->TextBox(outdated_, Rect(x + 24.0f, y + 44.0f, w - 48.0f, 80.0f), ui::Rgba(240, 230, 200));
    std::string url;
    if (const std::size_t at = outdated_.find("https://"); at != std::string::npos) url = outdated_.substr(at, outdated_.find_first_of(" \n", at) - at);
    while (!url.empty() && (url.back() == '.' || url.back() == ',')) url.pop_back();
    const float by = y + h - 44.0f;
    if (!url.empty() && kit_->TextButton("outdated-download", "Open the download page", Rect(x + 24.0f, by, 220.0f, 28.0f))) {
        if (!platform::OpenUrl(url)) core::LogWarning("title: could not open {}", url);
    }
    if (kit_->TextButton("outdated-exit", "Exit", Rect(x + w - 24.0f - 120.0f, by, 120.0f, 28.0f))) context.quit = true;
}

void TitleScreen::DrawLoginWindow(const assets::UiLayout& layout, float cx, float cy) {
    const core::DesignRect window = Rect(cx - 144.0f, cy + 6.0f, 288.0f, 140.0f);
    if (textures_->Exists(kLoginWindow)) {
        kit_->Image(kLoginWindow, window);
    } else if (const auto* frame = layout.Find("GDR_STA_LOGINWINDOW")) {
        kit_->Image(frame->ddj, window);
    }
    for (const char* name : {"GDR_STATIC1", "GDR_STATIC2", "GDR_STATIC3"}) {
        if (const auto* label = layout.Find(name)) kit_->NodeText(*label, shared_->data.Text(label->text), UiKit::Local(*label, window));
    }
    bool submit = false;
    if (const auto* edit = layout.Find("GDR_EDIT_ID")) {
        submit |= kit_->Edit("##account", *edit, UiKit::Local(*edit, window), account_, 32, false, focusAccount_ && !busy_);
        focusAccount_ = false;
    }
    if (const auto* edit = layout.Find("GDR_EDIT_PASS")) {
        // Tab moves from the account to the password.
        const bool tab = ui_.HasFocus(ui_.GetId("##account")) && ui_.input().KeyPressed(ui::Key::Tab);
        submit |= kit_->Edit("##password", *edit, UiKit::Local(*edit, window), password_, 64, true, tab);
    }
    if (const auto* button = layout.Find("GDR_BTN_SERVER")) {
        if (kit_->Button("##servers", *button, UiKit::Local(*button, window), !busy_)) {
            serverListOpen_ = true;
            draftServer_ = shared_->server;
            shared_->session->RequestServerList();
        }
    }
    if (const auto* label = layout.Find("GDR_STA_SERVER")) kit_->NodeText(*label, shared_->serverName, UiKit::Local(*label, window));
    if (const auto* ok = layout.Find("GDR_BTN_OK")) submit |= kit_->Button("##ok", *ok, Rect(cx - 100.0f, cy + 156.0f, ok->rect.w, ok->rect.h), !busy_);
    if (const auto* cancel = layout.Find("GDR_BTN_CANCEL")) {
        if (kit_->Button("##cancel", *cancel, Rect(cx + 9.0f, cy + 156.0f, cancel->rect.w, cancel->rect.h))) next = "logo";
    }
    if (submit && !busy_) Submit();
}

void TitleScreen::DrawServerList(const assets::UiLayout& layout, float cx, float cy) {
    const core::DesignRect box = Rect(std::max(0.0f, cx - 120.0f), std::max(0.0f, cy - 175.0f), 240.0f, 340.0f);
    if (const auto* frame = layout.Find("GDR_STA_SERVERWINDOW")) kit_->Image(frame->ddj, box);
    const auto* listNode = layout.Find("GDR_LIST_SERVER");
    const core::DesignRect list = listNode ? UiKit::Local(*listNode, box) : Rect(box.x + 10, box.y + 40, 204, 260);
    const proto::ServerList* servers = shared_->session->LastServerList();
    int row = 0;
    if (servers) {
        for (const proto::ServerEntry& s : servers->servers) {
            if (row >= kServerRows) break;
            const core::DesignRect r = Rect(list.x, list.y + float(row) * kServerRowHeight, 204.0f, kServerRowHeight);
            ui_.PushId(row++);
            const ui::Context::ItemState state = kit_->Item("##server", r);
            ui_.PopId();
            if (state.clicked && s.state != proto::ServerState::Offline) draftServer_ = s.id;
            if (draftServer_ == s.id || state.hovered) kit_->Image(draftServer_ == s.id ? kServerSelect : kServerRollover, r);
            kit_->Text(s.name, Rect(r.x + 15, r.y, 130, r.h), ui::kWhite);
            ui::Color color = ui::kWhite;
            const char* key = LoadKey(s, color);
            kit_->Text(shared_->data.Text(key), Rect(r.x + 152, r.y, 45, r.h), color);
        }
    }
    if (row == 0) kit_->Text(connected_ ? "No servers." : "Requesting the server list...", Rect(list.x + 10, list.y, 200, 20), ui::kWhite);
    bool draftOpen = false;
    if (servers) {
        for (const proto::ServerEntry& s : servers->servers) draftOpen |= s.id == draftServer_ && s.state != proto::ServerState::Offline;
    }
    if (const auto* accept = layout.Find("GDR_BTN_SACCEPT")) {
        if (kit_->Button("##saccept", *accept, Rect(box.x + 21, box.y + 350, 91, 40), draftOpen)) {
            shared_->server = draftServer_;
            for (const proto::ServerEntry& s : servers->servers) {
                if (s.id == draftServer_) shared_->serverName = s.name;
            }
            serverListOpen_ = false;
        }
    }
    if (const auto* cancel = layout.Find("GDR_BTN_SCANCEL")) {
        if (kit_->Button("##scancel", *cancel, Rect(box.x + 130, box.y + 350, 91, 40))) serverListOpen_ = false;
    }
    if (ui_.KeyPressed(ui::Key::Escape)) serverListOpen_ = false;
}

} // namespace sr::client
