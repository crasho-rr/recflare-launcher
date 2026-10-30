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

enum : UINT { WM_APP_STATUS = WM_APP + 1, WM_APP_STATE, WM_APP_DONE };
enum Tone { Info, Good, Bad };
enum : int { IDC_PLAY = 201, IDC_VR, IDC_CHANGE, IDC_FOLDER };

HWND g_window;
HWND g_play, g_vr, g_change, g_open;
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

void PostState(bool ready, bool busy) { PostMessageW(g_window, WM_APP_STATE, ready, busy); }

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

void CheckFolder(std::wstring folder) {
    PostState(false, true);
    if (!LooksLikeGame(folder)) {
        PostStatus(Bad, folder.empty() ? L"pick your rec room folder (the one with RecRoom.exe)"
                                       : L"no rec room here. hit change and pick the folder with RecRoom.exe");
        PostState(false, false);
        return;
    }
    const std::wstring stamp = Stamp(folder);
    if (stamp.empty() || stamp != Setting(L"VerifiedStamp")) {
        PostStatus(Info, L"checking game files (only the first time)...");
        for (const auto& module : kModules) {
            if (Sha256File(folder + L"\\" + module.name) != module.sha256) {
                PostStatus(Bad, L"wrong rec room version. RecFlare needs build 20260323.14");
                PostState(false, false);
                return;
            }
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

    if (id == IDC_CHANGE || id == IDC_FOLDER) {
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
    const bool canPlay = g_ready && !g_busy;
    EnableWindow(g_play, canPlay);
    EnableWindow(g_vr, canPlay);
    EnableWindow(g_change, !g_busy);
    EnableWindow(g_open, !g_folder.empty());
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
        g_open = MakeButton(hwnd, IDC_FOLDER, L"open game folder", 120, 520, 180, 26);
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
            if (g_ready && !g_busy) {
                g_busy = true;
                UpdateButtons();
                std::thread(Launch, g_folder, LOWORD(wp) == IDC_VR).detach();
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
        g_busy = lp != 0;
        UpdateButtons();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
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

    // Folder: --folder, then the saved one, then the launcher's own folder.
    std::wstring folder = folderArg;
    if (folder.empty()) {
        folder = Setting(L"GameFolder");
    }
    if (!LooksLikeGame(folder) && LooksLikeGame(ExeFolder())) {
        folder = ExeFolder();
    }
    if (!folderArg.empty() || LooksLikeGame(folder)) {
        SetFolder(folder);
    } else {
        g_status = L"pick your rec room folder (the one with RecRoom.exe)";
        g_tone = Info;
        InvalidateRect(g_window, nullptr, FALSE);
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
