// RecFlare Launcher: sets up a Rec Room 20260323.14 install for RecFlare and
// starts it. The loader, the patch and its default ini are embedded resources.
//
// Setup, done before every launch:
//   - checks the build (SHA-256 of the three modules the patch fingerprints,
//     cached by size + write time so it only hashes once),
//   - writes 2026loader.dll / 2026patch.dll when they differ, 2026patch.ini
//     only when missing (it holds the player's settings),
//   - adds the 2026loader.dll import to RecRoom.exe once, keeping the stock
//     exe as RecRoom.original.exe.
// Command line: --play / --vr start right away, --folder <path> picks the install.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>
#include "peimport.h"
#include "resource.h"

namespace {

using Bytes = std::vector<unsigned char>;

struct Module {
    const wchar_t* name;
    const char* sha256;
};
const Module kModules[] = {
    {L"GameAssembly.dll", "A6D90770CDFDA4043EADB278A16198ED0907C3BA2AC09BC37D06CD4DD22A1D84"},
    {L"Referee.dll", "5868C90A7C67757F3B678AA356CBD933D646C9749E0ACAA31B7DCC9EAEE12EAD"},
    {L"UnityPlayer.dll", "30F4A99C49566B2C5045469B5A39572DD2298ADC31F7FE2BC39D8BB8FC9D75BB"},
};
// Stock RecRoom.exe of 20260323.14, before the loader import is added.
const char kStockExe[] = "3BFDDBD82167A7FD3042ABADDD39B6ECAF2B7149407DF606EB6AF0E798A57A70";

enum : UINT { WM_APP_STATUS = WM_APP + 1, WM_APP_STATE, WM_APP_DONE, WM_APP_FOLDER };
enum Tone { Info, Good, Bad };
enum : int { IDC_PLAY = 201, IDC_VR, IDC_CHANGE, IDC_FOLDER, IDC_UNDO };

HWND g_window;
HWND g_play, g_vr, g_change, g_open, g_undo;
// No usable 20260323.14 install: the two big buttons become the steam steps.
bool g_needGame = false;
volatile LONG g_watching = 0;
std::wstring g_folder;
std::wstring g_status = L"looking for rec room...";
Tone g_tone = Info;
bool g_ready = false;
bool g_busy = false;
HWND g_hot;
float g_scale = 1.0f;
Gdiplus::Bitmap* g_logo;

int S(int value) { return static_cast<int>(value * g_scale + 0.5f); }

// ---- small helpers ----------------------------------------------------------

void PostStatus(Tone tone, const std::wstring& text) {
    PostMessageW(g_window, WM_APP_STATUS, tone, reinterpret_cast<LPARAM>(new std::wstring(text)));
}

void PostState(bool ready, bool busy, bool needGame = false) {
    PostMessageW(g_window, WM_APP_STATE, ready, (busy ? 1 : 0) | (needGame ? 2 : 0));
}

bool ReadAll(const std::wstring& path, Bytes& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart < (1ll << 31);
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = out.empty() || (ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) &&
                             read == out.size());
    }
    CloseHandle(file);
    return ok;
}

// Writes next to the target and renames over it, so a half-written file never
// replaces a good one (and hard links elsewhere keep their old contents).
bool WriteAll(const std::wstring& path, const void* data, size_t size) {
    const std::wstring temp = path + L".rfltmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) && written == size;
    CloseHandle(file);
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string Hex(const unsigned char* data, size_t size) {
    static const char digits[] = "0123456789ABCDEF";
    std::string out;
    for (size_t i = 0; i < size; ++i) {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 15];
    }
    return out;
}

// Streams the file through SHA-256 so big modules never sit in memory whole.
std::string Sha256File(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[32]{};
    std::string result;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
        std::vector<unsigned char> buffer(1 << 20);
        DWORD read = 0;
        bool ok = true;
        while ((ok = ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) != 0) && read) {
            BCryptHashData(hash, buffer.data(), read, 0);
        }
        if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0) {
            result = Hex(digest, sizeof(digest));
        }
    }
    if (hash) {
        BCryptDestroyHash(hash);
    }
    if (algorithm) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    CloseHandle(file);
    return result;
}

std::string Sha256(const Bytes& data) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    unsigned char digest[32]{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        return {};
    }
    const bool ok = BCryptHash(algorithm, nullptr, 0, const_cast<unsigned char*>(data.data()),
                               static_cast<ULONG>(data.size()), digest, sizeof(digest)) >= 0;
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok ? Hex(digest, sizeof(digest)) : std::string();
}

bool Resource(int id, const void*& data, DWORD& size) {
    HRSRC info = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL handle = info ? LoadResource(nullptr, info) : nullptr;
    data = handle ? LockResource(handle) : nullptr;
    size = info ? SizeofResource(nullptr, info) : 0;
    return data && size;
}

// ---- settings ---------------------------------------------------------------

std::wstring SettingsPath() {
    PWSTR base = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        path = std::wstring(base) + L"\\RecFlare Launcher";
        CreateDirectoryW(path.c_str(), nullptr);
        path += L"\\settings.ini";
    }
    CoTaskMemFree(base);
    return path;
}

std::wstring Setting(const wchar_t* key) {
    wchar_t value[1024]{};
    GetPrivateProfileStringW(L"launcher", key, L"", value, 1024, SettingsPath().c_str());
    return value;
}

void SaveSetting(const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(L"launcher", key, value.c_str(), SettingsPath().c_str());
}

// Size + last write time of the fingerprinted modules: if unchanged, the
// earlier hash check still holds.
std::wstring Stamp(const std::wstring& folder) {
    std::wstring stamp = folder;
    for (const auto& module : kModules) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW((folder + L"\\" + module.name).c_str(), GetFileExInfoStandard, &data)) {
            return {};
        }
        wchar_t part[96];
        swprintf_s(part, L"|%lu:%lu:%lu:%lu", data.nFileSizeHigh, data.nFileSizeLow, data.ftLastWriteTime.dwHighDateTime,
                   data.ftLastWriteTime.dwLowDateTime);
        stamp += part;
    }
    return stamp;
}

// ---- game folder checks -----------------------------------------------------

bool LooksLikeGame(const std::wstring& folder) {
    if (folder.empty() || !Exists(folder + L"\\RecRoom.exe")) {
        return false;
    }
    for (const auto& module : kModules) {
        if (!Exists(folder + L"\\" + module.name)) {
            return false;
        }
    }
    return true;
}

std::wstring ExeFolder() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return path;
}

// ---- steam ------------------------------------------------------------------

// 20260323.14 is an old build: Steam's normal install moved on, so players get
// it with Steam's own console (download_depot), which keeps their login in Steam.
const wchar_t kDepotCommand[] = L"download_depot 471710 471711 8867414017562658247";
constexpr unsigned long long kDepotBytes = 6775968514ull;
constexpr unsigned long long kGameAssemblyBytes = 287903264ull;

std::wstring Widen(const std::string& text) {
    std::wstring out(MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), static_cast<int>(out.size()));
    return out;
}

std::wstring SteamRoot() {
    wchar_t path[MAX_PATH]{};
    DWORD size = sizeof(path);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, path, &size) !=
        ERROR_SUCCESS) {
        return {};
    }
    std::wstring root = path;
    std::replace(root.begin(), root.end(), L'/', L'\\');
    return root;
}

// Values of every "key" "value" pair with this key in a Valve KeyValues file.
std::vector<std::wstring> VdfValues(const std::wstring& file, const char* key) {
    Bytes text;
    std::vector<std::wstring> values;
    if (!ReadAll(file, text)) {
        return values;
    }
    const std::string body(text.begin(), text.end());
    const std::string token = std::string("\"") + key + "\"";
    for (size_t at = body.find(token); at != std::string::npos; at = body.find(token, at + 1)) {
        const size_t open = body.find('"', at + token.size());
        std::string value;
        for (size_t i = open + 1; open != std::string::npos && i < body.size() && body[i] != '"'; ++i) {
            if (body[i] == '\\' && i + 1 < body.size()) {
                ++i;
            }
            value += body[i];
        }
        values.push_back(Widen(value));
    }
    return values;
}

// Steam's regular install of Rec Room (whatever build Steam put there).
std::wstring SteamInstall(const std::wstring& root) {
    std::vector<std::wstring> libraries{root};
    for (const auto& path : VdfValues(root + L"\\steamapps\\libraryfolders.vdf", "path")) {
        libraries.push_back(path);
    }
    for (const auto& library : libraries) {
        const auto dirs = VdfValues(library + L"\\steamapps\\appmanifest_471710.acf", "installdir");
        if (!dirs.empty() && Exists(library + L"\\steamapps\\common\\" + dirs[0])) {
            return library + L"\\steamapps\\common\\" + dirs[0];
        }
    }
    return {};
}

std::wstring DepotFolder(const std::wstring& root) {
    return root.empty() ? std::wstring() : root + L"\\steamapps\\content\\app_471710\\depot_471711";
}

unsigned long long FileBytes(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return 0;
    }
    return (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

unsigned long long FolderBytes(const std::wstring& folder) {
    unsigned long long total = 0;
    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileExW((folder + L"\\*").c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L"..")) {
            continue;
        }
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            total += FolderBytes(folder + L"\\" + entry.cFileName);
        } else {
            total += (static_cast<unsigned long long>(entry.nFileSizeHigh) << 32) | entry.nFileSizeLow;
        }
    } while (FindNextFileW(find, &entry));
    FindClose(find);
    return total;
}

bool ModulesMatch(const std::wstring& folder) {
    for (const auto& module : kModules) {
        if (Sha256File(folder + L"\\" + module.name) != module.sha256) {
            return false;
        }
    }
    return true;
}

bool SteamManaged(const std::wstring& folder) {
    std::wstring lower = folder;
    CharLowerBuffW(lower.data(), static_cast<DWORD>(lower.size()));
    return lower.find(L"\\steamapps\\common\\") != std::wstring::npos;
}

void CheckFolder(std::wstring folder) {
    PostState(false, true);
    if (!LooksLikeGame(folder)) {
        PostStatus(Bad, L"no rec room here. install it via steam, or use your own install");
        PostState(false, false, true);
        return;
    }
    const std::wstring stamp = Stamp(folder);
    if (stamp.empty() || stamp != Setting(L"VerifiedStamp")) {
        PostStatus(Info, L"checking game files (only the first time)...");
        if (!ModulesMatch(folder)) {
            PostStatus(Bad, L"that's a different rec room build. RecFlare needs 20260323.14");
            PostState(false, false, true);
            return;
        }
        SaveSetting(L"VerifiedStamp", stamp);
    }
    PostStatus(Good, L"ready! rec room 20260323.14 found");
    PostState(true, false);
}

bool GameRunning(const std::wstring& exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool running = false;
    if (snap != INVALID_HANDLE_VALUE && Process32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"RecRoom.exe")) {
                continue;
            }
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (process) {
                wchar_t path[MAX_PATH]{};
                DWORD length = MAX_PATH;
                running = QueryFullProcessImageNameW(process, 0, path, &length) && !_wcsicmp(path, exe.c_str());
                CloseHandle(process);
            }
        } while (!running && Process32NextW(snap, &entry));
    }
    if (snap != INVALID_HANDLE_VALUE) {
        CloseHandle(snap);
    }
    return running;
}

// ---- setup + launch ---------------------------------------------------------

// Writes an embedded file unless the folder already has identical bytes.
bool InstallResource(int id, const std::wstring& path, bool keepExisting) {
    const void* data = nullptr;
    DWORD size = 0;
    if (!Resource(id, data, size)) {
        return false;
    }
    Bytes current;
    if (ReadAll(path, current) &&
        (keepExisting || (current.size() == size && !memcmp(current.data(), data, size)))) {
        return true;
    }
    return WriteAll(path, data, size);
}

bool PrepareExe(const std::wstring& folder, std::wstring& error) {
    const std::wstring exe = folder + L"\\RecRoom.exe";
    Bytes image;
    if (!ReadAll(exe, image)) {
        error = L"couldn't read RecRoom.exe";
        return false;
    }
    if (PeImport::HasImportSection(image)) {
        return true;
    }
    if (Sha256(image) != kStockExe) {
        error = L"RecRoom.exe isn't the 20260323.14 one. reinstall that build";
        return false;
    }
    const std::wstring backup = folder + L"\\RecRoom.original.exe";
    if (!Exists(backup) && !WriteAll(backup, image.data(), image.size())) {
        error = L"couldn't back up RecRoom.exe (try running the launcher as admin)";
        return false;
    }
    if (!PeImport::AddImport(image, "2026loader.dll", "RecFlareLoader") || !WriteAll(exe, image.data(), image.size())) {
        error = L"couldn't patch RecRoom.exe (try running the launcher as admin)";
        return false;
    }
    return true;
}

// Last matching line of 2026patch.log for any of the given prefixes.
std::string FindLogLine(const std::wstring& path, std::initializer_list<const char*> prefixes) {
    Bytes text;
    if (!ReadAll(path, text)) {
        return {};
    }
    std::string found;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = start;
        while (end < text.size() && text[end] != '\n') {
            ++end;
        }
        std::string line(text.begin() + start, text.begin() + end);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        for (const char* prefix : prefixes) {
            if (!line.compare(0, strlen(prefix), prefix)) {
                found = line;
            }
        }
        start = end + 1;
    }
    return found;
}

void Launch(std::wstring folder, bool vr) {
    PostState(true, true);
    const std::wstring exe = folder + L"\\RecRoom.exe";
    if (GameRunning(exe)) {
        PostStatus(Bad, L"rec room is already running");
        PostState(true, false);
        return;
    }
    PostStatus(Info, L"setting things up...");
    std::wstring error;
    if (!InstallResource(IDR_LOADER, folder + L"\\2026loader.dll", false) ||
        !InstallResource(IDR_PATCH, folder + L"\\2026patch.dll", false) ||
        !InstallResource(IDR_INI, folder + L"\\2026patch.ini", true)) {
        error = L"couldn't copy the patch files (try running the launcher as admin)";
    } else {
        PrepareExe(folder, error);
    }
    if (!error.empty()) {
        PostStatus(Bad, error);
        PostState(true, false);
        return;
    }

    const std::wstring log = folder + L"\\2026patch.log";
    DeleteFileW(log.c_str());
    std::wstring command = L"\"" + exe + L"\" " + (vr ? L"+forcemode:vr" : L"+forcemode:screen");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &startup,
                        &process)) {
        PostStatus(Bad, L"couldn't start RecRoom.exe");
        PostState(true, false);
        return;
    }
    CloseHandle(process.hThread);
    PostStatus(Info, vr ? L"starting rec room in vr..." : L"starting rec room...");

    // Watch the patch come up; the loader writes 2026patch.log via the patch.
    const ULONGLONG deadline = GetTickCount64() + 90000;
    bool finished = false;
    while (!finished && GetTickCount64() < deadline) {
        if (WaitForSingleObject(process.hProcess, 250) == WAIT_OBJECT_0) {
            PostStatus(Bad, L"rec room closed while starting. check 2026loader.log in the game folder");
            finished = true;
            break;
        }
        const std::string line = FindLogLine(log, {"[Build] hooks enabled", "[Build] initialization failed",
                                                   "[Build] fingerprint mismatch", "[Config]"});
        if (line.empty()) {
            continue;
        }
        finished = true;
        if (!line.compare(0, 21, "[Build] hooks enabled") && line.find("status=0") != std::string::npos) {
            PostStatus(Good, L"patched and running. have fun [:");
            CloseHandle(process.hProcess);
            Sleep(2500);
            PostMessageW(g_window, WM_APP_DONE, 0, 0);
            return;
        }
        PostStatus(Bad, L"patch problem: " + std::wstring(line.begin(), line.end()));
    }
    if (!finished) {
        PostStatus(Info, L"still loading... if it won't connect, check 2026patch.log");
    }
    CloseHandle(process.hProcess);
    PostState(true, false);
}

// ---- getting the game via steam ---------------------------------------------

std::wstring Gigabytes(unsigned long long bytes) {
    wchar_t text[32];
    swprintf_s(text, L"%.1f", bytes / 1e9);
    return text;
}

// A Steam install that has finished downloading (StateFlags 4 = fully installed).
bool SteamInstallReady(const std::wstring& root, std::wstring& folder) {
    folder = SteamInstall(root);
    if (folder.empty() || !LooksLikeGame(folder) || FileBytes(folder + L"\\GameAssembly.dll") != kGameAssemblyBytes) {
        return false;
    }
    std::vector<std::wstring> libraries{root};
    for (const auto& path : VdfValues(root + L"\\steamapps\\libraryfolders.vdf", "path")) {
        libraries.push_back(path);
    }
    for (const auto& library : libraries) {
        const auto flags = VdfValues(library + L"\\steamapps\\appmanifest_471710.acf", "StateFlags");
        if (!flags.empty()) {
            return flags[0] == L"4";
        }
    }
    return false;
}

// Follows Steam while it installs/updates Rec Room or runs download_depot, and
// hands over whichever folder ends up holding 20260323.14.
void WatchSteam(std::wstring root) {
    if (InterlockedExchange(&g_watching, 1)) {
        return;
    }
    const std::wstring depot = DepotFolder(root);
    unsigned long long last = 0;
    int stable = 0;
    ULONGLONG nextHash = 0;
    while (g_watching) {
        // Normal install / update route.
        std::wstring install;
        if (GetTickCount64() >= nextHash && SteamInstallReady(root, install)) {
            PostStatus(Info, L"steam is done, checking the game...");
            if (ModulesMatch(install)) {
                g_watching = 0;
                PostMessageW(g_window, WM_APP_FOLDER, 0, reinterpret_cast<LPARAM>(new std::wstring(install)));
                return;
            }
            nextHash = GetTickCount64() + 30000;
        }
        // Console download_depot route; Steam can preallocate, so wait for the size to settle.
        const unsigned long long bytes = FolderBytes(depot);
        stable = bytes && bytes == last ? stable + 1 : 0;
        last = bytes;
        if (bytes >= kDepotBytes && stable >= 2 && GetTickCount64() >= nextHash && LooksLikeGame(depot) &&
            FileBytes(depot + L"\\GameAssembly.dll") == kGameAssemblyBytes) {
            PostStatus(Info, L"download looks done, checking it...");
            if (ModulesMatch(depot)) {
                g_watching = 0;
                PostMessageW(g_window, WM_APP_FOLDER, 0, reinterpret_cast<LPARAM>(new std::wstring(depot)));
                return;
            }
            nextHash = GetTickCount64() + 30000;
        }
        const unsigned long long downloading = FolderBytes(root + L"\\steamapps\\downloading\\471710");
        if (bytes) {
            const int percent = static_cast<int>(std::min<unsigned long long>(99, bytes * 100 / kDepotBytes));
            PostStatus(Info, L"steam is downloading rec room... " + std::to_wstring(percent) + L"% (" + Gigabytes(bytes) +
                                 L" / " + Gigabytes(kDepotBytes) + L" GB)");
        } else if (downloading) {
            PostStatus(Info, L"steam is downloading rec room... " + Gigabytes(downloading) + L" GB so far");
        } else {
            PostStatus(Info, L"waiting for steam... this picks the game up by itself when steam is done");
        }
        Sleep(3000);
    }
}

// Main route: Steam's normal install, or a verify/update if a different build is there.
void InstallViaSteam(HWND owner) {
    const std::wstring root = SteamRoot();
    if (root.empty()) {
        g_status = L"couldn't find steam. install steam first, then try again";
        g_tone = Bad;
        InvalidateRect(owner, nullptr, FALSE);
        return;
    }
    const bool installed = !SteamInstall(root).empty();
    ShellExecuteW(owner, L"open", installed ? L"steam://validate/471710" : L"steam://install/471710", nullptr, nullptr,
                  SW_SHOWNORMAL);
    std::thread(WatchSteam, root).detach();
}

void StartSteamDownload(HWND owner) {
    const std::wstring root = SteamRoot();
    if (root.empty()) {
        g_status = L"couldn't find steam. install steam first, then try again";
        g_tone = Bad;
        InvalidateRect(owner, nullptr, FALSE);
        return;
    }
    if (OpenClipboard(owner)) {
        EmptyClipboard();
        const size_t size = sizeof(kDepotCommand);
        if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size)) {
            memcpy(GlobalLock(memory), kDepotCommand, size);
            GlobalUnlock(memory);
            if (!SetClipboardData(CF_UNICODETEXT, memory)) {
                GlobalFree(memory);
            }
        }
        CloseClipboard();
    }
    ShellExecuteW(owner, L"open", L"steam://open/console", nullptr, nullptr, SW_SHOWNORMAL);
    MessageBoxW(owner,
                L"the steam console is opening (steam has to be running and signed in).\n\n"
                L"1. click the text box at the bottom of the console\n"
                L"2. press ctrl+v, the download command is already copied\n"
                L"3. press enter\n\n"
                L"steam downloads rec room 20260323.14 (about 6.8 GB). your login never leaves steam.\n"
                L"leave this launcher open, it picks the game up by itself when the download is done.\n\n"
                L"rec room has to be in your steam library for this. if it isn't, press \"install via steam\" once.",
                L"RecFlare Launcher", MB_OK | MB_ICONINFORMATION);
    std::thread(WatchSteam, root).detach();
}

// Copies a Steam-managed install aside so Steam updates can't replace the build.
void CopyGame(std::wstring from, std::wstring to) {
    PostState(false, true);
    const unsigned long long total = FolderBytes(from);
    unsigned long long done = 0;
    std::vector<std::wstring> pending{L""};
    bool ok = true;
    while (ok && !pending.empty()) {
        const std::wstring relative = pending.back();
        pending.pop_back();
        CreateDirectoryW((to + relative).c_str(), nullptr);
        WIN32_FIND_DATAW entry;
        HANDLE find = FindFirstFileExW((from + relative + L"\\*").c_str(), FindExInfoBasic, &entry,
                                       FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) {
            continue;
        }
        do {
            const std::wstring name = relative + L"\\" + entry.cFileName;
            if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L"..")) {
                continue;
            }
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                pending.push_back(name);
                continue;
            }
            ok = CopyFileExW((from + name).c_str(), (to + name).c_str(), nullptr, nullptr, nullptr, 0) != 0;
            done += (static_cast<unsigned long long>(entry.nFileSizeHigh) << 32) | entry.nFileSizeLow;
            PostStatus(Info, L"copying rec room... " +
                                 std::to_wstring(total ? static_cast<int>(done * 100 / total) : 0) + L"%");
        } while (ok && FindNextFileW(find, &entry));
        FindClose(find);
    }
    if (!ok) {
        PostStatus(Bad, L"copy failed (out of space?). nothing was changed in your steam install");
        PostState(true, false);
        return;
    }
    PostMessageW(g_window, WM_APP_FOLDER, 1, reinterpret_cast<LPARAM>(new std::wstring(to)));
}

// Puts the stock RecRoom.exe back and removes the loader and patch.
void Undo(std::wstring folder) {
    PostState(g_ready, true, g_needGame);
    if (GameRunning(folder + L"\\RecRoom.exe")) {
        PostStatus(Bad, L"close rec room first");
        PostState(g_ready, false, g_needGame);
        return;
    }
    const std::wstring exe = folder + L"\\RecRoom.exe", backup = folder + L"\\RecRoom.original.exe";
    bool ok = true;
    if (Exists(backup)) {
        ok = MoveFileExW(backup.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    }
    DeleteFileW((folder + L"\\2026loader.dll").c_str());
    DeleteFileW((folder + L"\\2026patch.dll").c_str());
    PostStatus(ok ? Good : Bad, ok ? L"RecFlare removed, RecRoom.exe is stock again (play puts it back)"
                                   : L"couldn't restore RecRoom.exe (try running the launcher as admin)");
    PostState(g_ready, false, g_needGame);
}

// ---- UI ---------------------------------------------------------------------

bool PickFolder(HWND owner, std::wstring& folder) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return false;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"pick your rec room folder (the one with RecRoom.exe)");
    bool picked = false;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        PWSTR path = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            folder = path;
            picked = true;
        }
        CoTaskMemFree(path);
        if (item) {
            item->Release();
        }
    }
    dialog->Release();
    return picked;
}

void SetFolder(const std::wstring& folder) {
    g_folder = folder;
    SaveSetting(L"GameFolder", folder);
    InvalidateRect(g_window, nullptr, FALSE);
    std::thread(CheckFolder, folder).detach();
}

Gdiplus::Color Rgb(BYTE r, BYTE g, BYTE b, BYTE a = 255) { return Gdiplus::Color(a, r, g, b); }

const Gdiplus::Color kBack = Rgb(0x14, 0x12, 0x11);
const Gdiplus::Color kOrangeA = Rgb(0xFF, 0x52, 0x00);
const Gdiplus::Color kOrangeB = Rgb(0xFF, 0x9A, 0x00);
const Gdiplus::Color kText = Rgb(0xF5, 0xF1, 0xEC);
const Gdiplus::Color kMuted = Rgb(0x9A, 0x92, 0x8A);

void RoundRect(Gdiplus::GraphicsPath& path, Gdiplus::RectF r, float radius) {
    const float d = radius * 2;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    path.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    path.CloseFigure();
}

Gdiplus::Font* MakeFont(const wchar_t* family, float pixels, int style) {
    Gdiplus::FontFamily fam(family);
    if (fam.GetLastStatus() != Gdiplus::Ok) {
        return new Gdiplus::Font(L"Segoe UI", pixels, style, Gdiplus::UnitPixel);
    }
    return new Gdiplus::Font(&fam, pixels, style, Gdiplus::UnitPixel);
}

void DrawLabel(Gdiplus::Graphics& g, const std::wstring& text, Gdiplus::Font* font, Gdiplus::Color color,
              Gdiplus::RectF rect, Gdiplus::StringAlignment align, bool ellipsisPath = false) {
    Gdiplus::StringFormat format;
    format.SetAlignment(align);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    format.SetTrimming(ellipsisPath ? Gdiplus::StringTrimmingEllipsisPath : Gdiplus::StringTrimmingEllipsisCharacter);
    format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    Gdiplus::SolidBrush brush(color);
    g.DrawString(text.c_str(), -1, font, rect, &format, &brush);
}

void DrawButton(const DRAWITEMSTRUCT* item) {
    const RECT& rc = item->rcItem;
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    Gdiplus::Bitmap buffer(w, h, PixelFormat32bppPARGB);
    Gdiplus::Graphics g(&buffer);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    g.Clear(kBack);

    const int id = static_cast<int>(item->CtlID);
    const bool enabled = !(item->itemState & ODS_DISABLED);
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    const bool hot = item->hwndItem == g_hot && enabled;
    wchar_t label[64]{};
    GetWindowTextW(item->hwndItem, label, 64);

    if (id == IDC_CHANGE || id == IDC_FOLDER || id == IDC_UNDO) {
        Gdiplus::Font* font = MakeFont(L"Segoe UI Semibold", static_cast<float>(S(13)),
                                       hot ? Gdiplus::FontStyleUnderline : Gdiplus::FontStyleRegular);
        DrawLabel(g, label, font, enabled ? (hot ? kOrangeB : Rgb(0xFF, 0x7A, 0x1A)) : kMuted,
                 Gdiplus::RectF(0, 0, static_cast<float>(w), static_cast<float>(h)),
                 id == IDC_CHANGE ? Gdiplus::StringAlignmentFar : Gdiplus::StringAlignmentCenter);
        delete font;
    } else {
        const float inset = pressed ? 2.0f : 0.0f;
        Gdiplus::RectF r(inset + 1, inset + 1, w - 2 - inset * 2, h - 2 - inset * 2);
        Gdiplus::GraphicsPath path;
        RoundRect(path, r, static_cast<float>(S(12)));
        const BYTE alpha = enabled ? 255 : 90;
        if (id == IDC_PLAY) {
            Gdiplus::Color a = hot ? Rgb(0xFF, 0x66, 0x10, alpha) : Rgb(0xFF, 0x52, 0x00, alpha);
            Gdiplus::Color b = hot ? Rgb(0xFF, 0xB0, 0x20, alpha) : Rgb(0xFF, 0x9A, 0x00, alpha);
            Gdiplus::LinearGradientBrush fill(Gdiplus::PointF(r.X, r.Y), Gdiplus::PointF(r.GetRight(), r.GetBottom()), a, b);
            g.FillPath(&fill, &path);
            Gdiplus::Font* font = MakeFont(L"Segoe UI Black", static_cast<float>(S(24)), Gdiplus::FontStyleRegular);
            DrawLabel(g, label, font, Rgb(0xFF, 0xFF, 0xFF, enabled ? 255 : 150), r, Gdiplus::StringAlignmentCenter);
            delete font;
        } else {
            Gdiplus::SolidBrush fill(hot ? Rgb(0x2A, 0x22, 0x1C) : Rgb(0x1E, 0x1A, 0x17));
            g.FillPath(&fill, &path);
            Gdiplus::Pen pen(Rgb(0xFF, 0x7A, 0x1A, enabled ? 255 : 80), static_cast<float>(S(2)));
            g.DrawPath(&pen, &path);
            Gdiplus::Font* font = MakeFont(L"Segoe UI Black", static_cast<float>(S(17)), Gdiplus::FontStyleRegular);
            DrawLabel(g, label, font, enabled ? kText : kMuted, r, Gdiplus::StringAlignmentCenter);
            delete font;
        }
    }
    Gdiplus::Graphics screen(item->hDC);
    screen.DrawImage(&buffer, rc.left, rc.top);
}

void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);
    const int w = client.right, h = client.bottom;
    Gdiplus::Bitmap buffer(w, h, PixelFormat32bppPARGB);
    Gdiplus::Graphics g(&buffer);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    g.Clear(kBack);

    // Soft orange glow behind the logo.
    Gdiplus::GraphicsPath glow;
    glow.AddEllipse(static_cast<float>(w / 2 - S(220)), static_cast<float>(S(-40)), static_cast<float>(S(440)),
                    static_cast<float>(S(380)));
    Gdiplus::PathGradientBrush glowBrush(&glow);
    glowBrush.SetCenterColor(Rgb(0xFF, 0x6A, 0x00, 46));
    Gdiplus::Color edge = Rgb(0xFF, 0x6A, 0x00, 0);
    int edges = 1;
    glowBrush.SetSurroundColors(&edge, &edges);
    g.FillPath(&glowBrush, &glow);

    if (g_logo) {
        const float logoW = static_cast<float>(S(330));
        const float logoH = logoW * g_logo->GetHeight() / g_logo->GetWidth();
        g.DrawImage(g_logo, (w - logoW) / 2, static_cast<float>(S(26)), logoW, logoH);
    }

    Gdiplus::Font* caption = MakeFont(L"Segoe UI Semibold", static_cast<float>(S(12)), Gdiplus::FontStyleRegular);
    Gdiplus::Font* body = MakeFont(L"Segoe UI", static_cast<float>(S(13)), Gdiplus::FontStyleRegular);
    DrawLabel(g, L"GAME FOLDER", caption, kMuted,
             Gdiplus::RectF(static_cast<float>(S(32)), static_cast<float>(S(300)), static_cast<float>(S(200)),
                            static_cast<float>(S(18))),
             Gdiplus::StringAlignmentNear);
    DrawLabel(g, g_folder.empty() ? L"not picked yet" : g_folder, body, g_folder.empty() ? kMuted : kText,
             Gdiplus::RectF(static_cast<float>(S(32)), static_cast<float>(S(320)), static_cast<float>(S(286)),
                            static_cast<float>(S(24))),
             Gdiplus::StringAlignmentNear, true);

    const Gdiplus::Color tone = g_tone == Good ? Rgb(0x7C, 0xE0, 0x8A) : g_tone == Bad ? Rgb(0xFF, 0x6B, 0x5E) : kMuted;
    DrawLabel(g, g_status, body, tone,
             Gdiplus::RectF(static_cast<float>(S(20)), static_cast<float>(S(488)), static_cast<float>(w - S(40)),
                            static_cast<float>(S(24))),
             Gdiplus::StringAlignmentCenter);
    delete caption;
    delete body;

    Gdiplus::Graphics screen(dc);
    screen.DrawImage(&buffer, 0, 0);
    EndPaint(hwnd, &ps);
}

void UpdateButtons() {
    const bool steam = g_needGame && !g_ready;
    const bool installed = steam && !SteamInstall(SteamRoot()).empty();
    SetWindowTextW(g_play, !steam ? L"PLAY" : installed ? L"UPDATE VIA STEAM" : L"INSTALL VIA STEAM");
    SetWindowTextW(g_vr, steam ? L"USE MY OWN INSTALL" : L"PLAY IN VR");
    SetWindowTextW(g_undo, steam ? L"steam console download" : L"undo recflare");
    const bool canUse = (g_ready || g_needGame) && !g_busy;
    EnableWindow(g_play, canUse);
    EnableWindow(g_vr, canUse);
    EnableWindow(g_change, !g_busy);
    EnableWindow(g_open, !g_folder.empty());
    EnableWindow(g_undo, !g_busy && (steam || (!g_folder.empty() && Exists(g_folder + L"\\RecRoom.original.exe"))));
    InvalidateRect(g_play, nullptr, FALSE);
    InvalidateRect(g_vr, nullptr, FALSE);
    InvalidateRect(g_undo, nullptr, FALSE);
}

LRESULT CALLBACK ButtonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_MOUSEMOVE && g_hot != hwnd) {
        HWND old = g_hot;
        g_hot = hwnd;
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&track);
        InvalidateRect(hwnd, nullptr, FALSE);
        if (old) {
            InvalidateRect(old, nullptr, FALSE);
        }
    } else if (msg == WM_MOUSELEAVE && g_hot == hwnd) {
        g_hot = nullptr;
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (msg == WM_SETCURSOR) {
        SetCursor(LoadCursorW(nullptr, IsWindowEnabled(hwnd) ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

HWND MakeButton(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h) {
    HWND button = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, S(x), S(y), S(w), S(h), parent,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SetWindowSubclass(button, ButtonProc, 0, 0);
    return button;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_play = MakeButton(hwnd, IDC_PLAY, L"PLAY", 32, 362, 356, 60);
        g_vr = MakeButton(hwnd, IDC_VR, L"PLAY IN VR", 32, 432, 356, 46);
        g_change = MakeButton(hwnd, IDC_CHANGE, L"change", 318, 318, 70, 28);
        g_open = MakeButton(hwnd, IDC_FOLDER, L"open game folder", 30, 520, 170, 26);
        g_undo = MakeButton(hwnd, IDC_UNDO, L"undo recflare", 210, 520, 180, 26);
        UpdateButtons();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint(hwnd);
        return 0;
    case WM_DRAWITEM:
        DrawButton(reinterpret_cast<DRAWITEMSTRUCT*>(lp));
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_PLAY:
        case IDC_VR:
            if (g_needGame && !g_busy) {
                if (LOWORD(wp) == IDC_PLAY) {
                    InstallViaSteam(hwnd);
                } else {
                    // A copy they already have somewhere else; it gets the same build check.
                    std::wstring folder;
                    if (PickFolder(hwnd, folder)) {
                        SetFolder(folder);
                    }
                }
                return 0;
            }
            if (g_ready && !g_busy) {
                // A Steam-managed install changes when Steam updates it; offer a separate copy once.
                if (SteamManaged(g_folder) && Setting(L"CopyOffered") != g_folder) {
                    SaveSetting(L"CopyOffered", g_folder);
                    const int answer = MessageBoxW(
                        hwnd,
                        L"this is steam's own rec room install. if steam ever updates it, RecFlare stops working "
                        L"until you get 20260323.14 again.\n\n"
                        L"make a separate copy for RecFlare first? (about 6.8 GB)\n\n"
                        L"yes = copy it (you pick where)\nno = use the steam install as it is",
                        L"RecFlare Launcher", MB_YESNOCANCEL | MB_ICONQUESTION);
                    if (answer == IDCANCEL) {
                        return 0;
                    }
                    std::wstring parent;
                    if (answer == IDYES) {
                        if (PickFolder(hwnd, parent)) {
                            std::thread(CopyGame, g_folder, parent + L"\\RecFlare 2026").detach();
                        }
                        return 0;
                    }
                }
                g_busy = true;
                UpdateButtons();
                std::thread(Launch, g_folder, LOWORD(wp) == IDC_VR).detach();
            }
            return 0;
        case IDC_UNDO:
            if (g_needGame && !g_ready && !g_busy) {
                // Fallback for when Steam's own build isn't 20260323.14 any more.
                StartSteamDownload(hwnd);
                return 0;
            }
            if (!g_busy && !g_folder.empty() &&
                MessageBoxW(hwnd,
                            L"put RecRoom.exe back to stock and remove the RecFlare files from this folder?\n"
                            L"(your 2026patch.ini stays. pressing play later sets it up again)",
                            L"RecFlare Launcher", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                std::thread(Undo, g_folder).detach();
            }
            return 0;
        case IDC_CHANGE: {
            std::wstring folder;
            if (!g_busy && PickFolder(hwnd, folder)) {
                SetFolder(folder);
            }
            return 0;
        }
        case IDC_FOLDER:
            ShellExecuteW(hwnd, L"open", g_folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        break;
    case WM_APP_STATUS: {
        auto* text = reinterpret_cast<std::wstring*>(lp);
        g_status = *text;
        g_tone = static_cast<Tone>(wp);
        delete text;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_APP_STATE:
        g_ready = wp != 0;
        g_busy = (lp & 1) != 0;
        g_needGame = (lp & 2) != 0;
        if (g_ready) {
            g_watching = 0;
        }
        UpdateButtons();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_APP_FOLDER: {
        // A finished steam download (wp 0) or a finished copy (wp 1).
        auto* folder = reinterpret_cast<std::wstring*>(lp);
        SetFolder(*folder);
        delete folder;
        return 0;
    }
    case WM_APP_DONE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

Gdiplus::Bitmap* LoadLogo() {
    const void* data = nullptr;
    DWORD size = 0;
    if (!Resource(IDR_LOGO, data, size)) {
        return nullptr;
    }
    IStream* stream = SHCreateMemStream(static_cast<const BYTE*>(data), size);
    if (!stream) {
        return nullptr;
    }
    Gdiplus::Bitmap source(stream);
    Gdiplus::Bitmap* copy = nullptr;
    if (source.GetLastStatus() == Gdiplus::Ok) {
        // Copy out of the stream-backed bitmap so the stream can go.
        copy = new Gdiplus::Bitmap(source.GetWidth(), source.GetHeight(), PixelFormat32bppPARGB);
        Gdiplus::Graphics g(copy);
        g.DrawImage(&source, 0, 0, source.GetWidth(), source.GetHeight());
    }
    stream->Release();
    return copy;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    // Command line: --folder <path>, --play, --vr
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring folderArg;
    int autoStart = 0;
    for (int i = 1; i < argc; ++i) {
        if (!_wcsicmp(argv[i], L"--folder") && i + 1 < argc) {
            folderArg = argv[++i];
        } else if (!_wcsicmp(argv[i], L"--play")) {
            autoStart = IDC_PLAY;
        } else if (!_wcsicmp(argv[i], L"--vr")) {
            autoStart = IDC_VR;
        }
    }
    LocalFree(argv);

    HDC screen = GetDC(nullptr);
    g_scale = GetDeviceCaps(screen, LOGPIXELSY) / 96.0f;
    ReleaseDC(nullptr, screen);
    g_logo = LoadLogo();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"RecFlareLauncher";
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rect{0, 0, S(420), S(564)};
    AdjustWindowRect(&rect, style, FALSE);
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    g_window = CreateWindowExW(0, wc.lpszClassName, L"RecFlare Launcher", style,
                               (GetSystemMetrics(SM_CXSCREEN) - width) / 2, (GetSystemMetrics(SM_CYSCREEN) - height) / 2,
                               width, height, nullptr, nullptr, instance, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    ShowWindow(g_window, show);
    UpdateWindow(g_window);

    // Folder: --folder, else the first right-sized install among the saved one,
    // the launcher's own folder, a steam download_depot and steam's install.
    const std::wstring steamRoot = SteamRoot();
    const std::wstring depot = DepotFolder(steamRoot);
    const std::wstring steamInstall = steamRoot.empty() ? std::wstring() : SteamInstall(steamRoot);
    std::wstring folder = folderArg;
    if (folder.empty()) {
        const std::wstring candidates[] = {Setting(L"GameFolder"), ExeFolder(), depot, steamInstall};
        for (const auto& candidate : candidates) {
            if (LooksLikeGame(candidate) && FileBytes(candidate + L"\\GameAssembly.dll") == kGameAssemblyBytes) {
                folder = candidate;
                break;
            }
        }
        for (const auto& candidate : candidates) {
            if (folder.empty() && LooksLikeGame(candidate)) {
                folder = candidate;
            }
        }
    }
    if (!folder.empty()) {
        SetFolder(folder);
        // Picked a different build while a download_depot sits half done: follow it.
        if (FileBytes(folder + L"\\GameAssembly.dll") != kGameAssemblyBytes && !depot.empty() && Exists(depot) &&
            folder != depot) {
            std::thread(WatchSteam, steamRoot).detach();
        }
    } else {
        g_needGame = true;
        g_status = L"no rec room found. install it via steam, or use your own install";
        g_tone = Info;
        UpdateButtons();
        InvalidateRect(g_window, nullptr, FALSE);
        // A download_depot that's already under way: keep following it.
        if (!depot.empty() && Exists(depot)) {
            std::thread(WatchSteam, steamRoot).detach();
        }
    }

    MSG msg;
    bool started = false;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        // --play / --vr: press the button as soon as the folder check passes.
        if (autoStart && !started && g_ready && !g_busy) {
            started = true;
            SendMessageW(g_window, WM_COMMAND, autoStart, 0);
        }
    }
    delete g_logo;
    Gdiplus::GdiplusShutdown(gdiplusToken);
    CoUninitialize();
    return 0;
}
