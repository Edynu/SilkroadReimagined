#pragma once

// The interface textures a player's runs drew, kept in a small text file next to the options (ours, not the
// original's): the next run asks for them at its start, so the HUD, the windows the player opens, the bag's icons and
// the minimap tiles of where they play are in before they are first shown (UiWarmup).
//
// One line a texture: the runs since it was last drawn (0: the last run), a tab, its path (normalized). A run that
// draws it again makes it 0; one that does not ages it, and after kForgetAfter runs without it it goes. The file keeps
// at most kMaxEntries, the most recently drawn first. Only what was drawn counts (a map's look-ahead does not), and
// nothing on it is held once loaded: the store keeps them under its budget like any texture.

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sr::client {

class UiWarmupList {
public:
    static constexpr int kForgetAfter = 4;
    static constexpr std::size_t kMaxEntries = 6000;

    // The file's lines (a line that does not read is skipped); replaces the list.
    void Decode(std::string_view text);
    // The paths to warm up, the most recently drawn first.
    std::vector<std::string> Paths() const;
    // Drawn this run (any spelling of the path: it is normalized).
    void Note(std::string_view path);
    // The file for the next run: this run's paths at 0, the others a run older, the oldest past kForgetAfter gone,
    // at most kMaxEntries.
    std::string Encode() const;

    std::size_t size() const { return entries_.size(); }
    std::size_t drawnThisRun() const { return drawn_; }

private:
    struct Entry {
        int age = 0;
        bool drawn = false; // this run
    };
    std::unordered_map<std::string, Entry> entries_;
    std::size_t drawn_ = 0;
};

} // namespace sr::client
