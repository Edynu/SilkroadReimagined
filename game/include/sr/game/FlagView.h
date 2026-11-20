#pragma once

// Capture the Flag as the client sees it (Event So-Ok's desk too): what the 1.188 client does with each notice, as data,
// and what it keeps: the event timer (CIFTimerWnd), the door map (zone -> open), every player's flag team, the flag
// "a registration is expected" (set when So-Ok's row was drawn) and So-Ok's desk as the server told it.
// Headless: the game screen shows the lines, the timer, the boxes and the bars.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "sr/game/EventProtocol.h"
#include "sr/game/FlagWorld.h"

namespace sr::game {

class FlagView {
public:
    static constexpr std::uint8_t kNoTeam = 0xFF;
    static constexpr std::int64_t kTimerMs = 1'200'000; // the event timer's 20:00 (state 8, FF 0E)
    static constexpr int kActionSeconds = 5;           // state 0x0A's bar
    static constexpr int kCampSeconds = 10;            // the camp countdown ((10000, ..))

    // A line: on the green banner (CIFNotify 42) and in the system messages (category 0), or the system line alone.
    struct Line {
        std::string key;  // UIIT_MSG_FLAGWAR_*
        std::string name; // its %s (a carrier, a key's taker, the flag's placer); empty: none
        bool banner = true;
        bool operator==(const Line&) const = default;
    };
    // What a notice asks of the screen.
    struct Outcome {
        std::optional<Line> line;
        bool cardBox = false;        // FF 0C: the Subscription Card's box
        std::uint32_t actionBar = 0; // state 0x0A: that player's 5 s bar under the head
    };

    // A notice (FlagWorldNotice: the original's 0x34B1), for the player `self`, at `now` (the screen's clock, seconds).
    Outcome Apply(const proto::FlagWorldNotice& n, std::uint32_t self, double now);
    // A player's flag team (0 red, 1 blue, 0xFF none).
    void Apply(const proto::EntityFlagTeam& t);
    // So-Ok's desk after her NpcMenu; cleared by any NpcMenu (the next one's comes after it).
    void Apply(const proto::EventDesk& d) { desk_ = d; }
    void ClearDesk() { desk_ = {}; }
    // One gone from sight; the world entered anew as `self` (a teleport, a login, another game server): the others' teams
    // forgotten (told again with their appearance), the player's own kept (ours tells it apart from the entry: a teleport
    // into a match tells it before its WorldEntered comes, the way home tells 0xFF before), and the timer hidden when the
    // player has no flag team (from OnMyCharacterEntered); state 8 or FF 0E shows it again.
    void Forget(std::uint32_t entity) { teams_.erase(entity); }
    void Entered(std::uint32_t self);

    std::uint8_t TeamOf(std::uint32_t entity) const;
    bool InMatch(std::uint32_t self) const { return TeamOf(self) != kNoTeam; }
    // In a match an enemy is exactly another flag team.
    bool Enemy(std::uint32_t self, std::uint32_t other) const;
    // The door map (FF 0B opens a zone for everyone; state 4 closes them all again).
    bool DoorOpen(std::uint32_t zone) const;

    // The event timer: shown, and the seconds left (0 once run out; it hides a second after: the stock client's "end"
    // picture, interface\gngwc\end.ddj, is absent from 1.188's Media.pk2).
    bool TimerShown(double now) const { return timerShown_ && now < timerEnd_ + 1.0; }
    double TimerLeft(double now) const { return timerEnd_ > now ? timerEnd_ - now : 0.0; }
    // MM:SS of the seconds left, as the timer counts them (a whole second shown until it is gone).
    static std::string TimerText(double secondsLeft);

    // So-Ok's desk: the registration row's text key by her state (1 not registered: the registration, 2 registered: its
    // cancel, else none), drawn (the expected answer's flag), pressed (level
    // under 20: its line and nothing sent).
    const proto::EventDesk& Desk() const { return desk_; }
    static std::string_view DeskRowKey(std::uint8_t flagState);
    void RowDrawn(std::uint8_t flagState) { registering_ = flagState == 1; }
    bool Registering() const { return registering_; }
    static std::optional<Line> RowRefusal(int level);

private:
    std::unordered_map<std::uint32_t, std::uint8_t> teams_;
    std::unordered_map<std::uint32_t, bool> doors_;
    bool timerShown_ = false;
    double timerEnd_ = 0.0;
    bool registering_ = true;
    proto::EventDesk desk_;
};

} // namespace sr::game
