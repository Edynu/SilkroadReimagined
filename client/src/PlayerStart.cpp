#include "PlayerStart.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "sr/core/Log.h"

#ifdef _WIN32
#include <windows.h>
#include <shobjidl.h>
#endif

namespace sr::client {
namespace {

constexpr const char* kSettingsFile = "SroReimagined.cfg";
// The archives the client reads; all five must be in the game folder.
constexpr std::array<const char*, 5> kArchives = {"Media.pk2", "Map.pk2", "Data.pk2", "Music.pk2", "Particles.pk2"};

#ifdef _WIN32
std::wstring Wide(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    std::wstring out(std::size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), n);
    return out;
}

std::string Utf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(std::size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// A folder the player picks (Windows' own picker); empty when cancelled.
std::wstring PickFolder(const std::wstring& title) {
    std::wstring chosen;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(title.c_str());
        if (SUCCEEDED(dialog->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    chosen = path;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return chosen;
}

bool AskYesNo(std::string_view text) {
    return MessageBoxW(nullptr, Wide(text).c_str(), L"SroReimagined", MB_OKCANCEL | MB_ICONINFORMATION) == IDOK;
}
#endif

// What is wrong with a game folder (empty: it has every archive the client reads).
std::string FolderProblem(const std::filesystem::path& dir) {
    std::error_code ec;
    std::string missing;
    for (const char* name : kArchives) {
        if (!std::filesystem::is_regular_file(dir / name, ec)) missing += std::string(missing.empty() ? "" : ", ") + name;
    }
    return missing.empty() ? std::string() : "it has no " + missing;
}

// SroReimagined.cfg with its `game` line set (the other lines kept as they are).
void RememberGameFolder(const std::string& folder) {
    std::vector<std::string> lines;
    {
        std::ifstream in(kSettingsFile, std::ios::binary);
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }
    bool replaced = false;
    for (std::string& line : lines) {
        const std::size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line.compare(start, 4, "game") != 0) continue;
        const std::size_t after = line.find_first_not_of(" \t", start + 4);
        if (after == std::string::npos || line[after] != '=') continue;
        line = "game = " + folder;
        replaced = true;
    }
    if (!replaced) lines.push_back("game = " + folder);
    std::ofstream out(kSettingsFile, std::ios::binary | std::ios::trunc);
    for (const std::string& line : lines) out << line << "\r\n";
    if (!out) core::LogWarning("cannot write {}: the game folder is asked for again next time", kSettingsFile);
}

} // namespace

void UseExeFolder() {
#ifdef _WIN32
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
        if (n == 0) return;
        if (n < path.size()) {
            path.resize(n);
            break;
        }
        path.resize(path.size() * 2);
    }
    std::error_code ec;
    std::filesystem::current_path(std::filesystem::path(path).parent_path(), ec);
#endif
}

void ShowStartError(std::string_view text) {
    core::LogError("{}", text);
#ifdef _WIN32
    MessageBoxW(nullptr, Wide(text).c_str(), L"SroReimagined", MB_OK | MB_ICONERROR);
#endif
}

bool PreparePlayerStart(core::Config& config) {
    const auto fallback = [&config](const char* key, const char* value) {
        if (!config.Has(key)) config.Set(key, value);
    };
    fallback("log", "SroReimagined.log");
    fallback("cursor-dir", "cursors");
    // Straight to the title screen, as 1.188 starts (its logo is on the title): the developers' screens (the viewer, the
    // benchmarks, the gallery) are not a player's, whatever the command line says.
    config.Set("screen", "title");
    // The shard's gateway and certificate are built in (PlayerServer.h): nothing to set here.

    // The game folder: the one remembered when it is still whole, else the player picks one. Holding Shift while the
    // game starts (or --choose-game) asks again, to move to another folder.
    bool choose = config.Has("choose-game");
#ifdef _WIN32
    choose = choose || (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
#endif
    std::string problem;
    if (config.Has("game")) problem = FolderProblem(config.GetString("game"));
    // Asked again with a folder that still works: Cancel keeps it.
    const bool keep = config.Has("game") && problem.empty();
    if (keep && !choose) return true;
#ifdef _WIN32
    std::string text = "SroReimagined plays on the files of your own Silkroad Online (vSRO 1.188) client.\n\n"
                       "Choose the folder it is installed in (the one with Media.pk2).";
    if (!problem.empty()) text = "The game folder " + config.GetString("game") + " cannot be used: " + problem + ".\n\n" + text;
    else if (keep) text = "The game folder is " + config.GetString("game") + ".\n\nChoose another one, or Cancel to keep it.";
    for (;;) {
        if (!AskYesNo(text)) return keep;
        const std::wstring picked = PickFolder(L"The Silkroad Online (vSRO 1.188) folder");
        if (picked.empty()) return keep;
        problem = FolderProblem(std::filesystem::path(picked));
        if (problem.empty()) {
            // The client opens its files through narrow paths: a folder name outside ASCII goes by its short (8.3) name.
            std::wstring usable = picked;
            bool ascii = true;
            for (const wchar_t c : picked) ascii = ascii && c < 0x80;
            if (!ascii) {
                std::wstring shortPath(MAX_PATH, L'\0');
                const DWORD n = GetShortPathNameW(picked.c_str(), shortPath.data(), DWORD(shortPath.size()));
                if (n > 0 && n < shortPath.size()) {
                    shortPath.resize(n);
                    usable = shortPath;
                }
            }
            const std::string folder = Utf8(usable);
            config.Set("game", folder);
            RememberGameFolder(folder);
            core::LogInfo("the game folder: {}", folder);
            return true;
        }
        text = "That folder cannot be used: " + problem + ".\n\nChoose the folder your vSRO 1.188 client is installed in.";
    }
#else
    if (!problem.empty()) ShowStartError("The game folder " + config.GetString("game") + " cannot be used: " + problem);
    else ShowStartError("No game folder: add game = <the vSRO 1.188 client's folder> to SroReimagined.cfg");
    return false;
#endif
}

} // namespace sr::client
