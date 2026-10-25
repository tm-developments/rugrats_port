// RUGUPDT.EXE - update checker for the Rugrats Adventure Game 64-bit port.
// GPL-3.0-or-later, (C) 2026 T.M. DEVELOPMENTS.
//
// Reads a small update file (WiseUpdate's [WiseUpdate] .ini format) over HTTPS, compares its
// Version with the installed RUGRATS.EXE beside this program, and if it is newer downloads the
// new installer, checks it (Size, and SHA256 when given) and starts it. Uses Windows' own WinHTTP
// (current TLS, the system proxy) and task dialogs with the current visual style. Runs as the
// user: the installer it starts asks for administrator rights itself when it installs.
//
// Update file, e.g. https://raw.githubusercontent.com/<owner>/<repo>/main/update/rugrats_update.ini
//   [WiseUpdate]
//   Version=1.1
//   Size=50405376
//   Install=https://github.com/<owner>/<repo>/releases/download/v1.1/RRATADV64.EXE
//   ReadMe=https://.../whatsnew.txt  (optional: fetched with the check into %TEMP%, opened by
//                                "See what's new", deleted when the updater closes)
//   SHA256=<64 hex digits>      (optional: checked before the installer is started)
//
// Settings (all optional, for testing or forks): command line  /url <update file URL>
// or registry HKLM\SOFTWARE\T.M. DEVELOPMENTS\Rugrats Adventure Game (x64): UpdateURL, WebPage.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

namespace {

const wchar_t* kTitle = L"Update Rugrats Adventure Game (x64)";
const wchar_t* kDefaultUrl = L"https://raw.githubusercontent.com/tm-developments/rugrats_port/main/update/rugrats_update.ini";
const wchar_t* kDefaultWebPage = L"https://github.com/tm-developments/rugrats_port/releases";
const wchar_t* kSettingsKey = L"SOFTWARE\\T.M. DEVELOPMENTS\\Rugrats Adventure Game (x64)";
const wchar_t* kGameMutex = L"Local\\RugratsAdventureGame_x64_TMDevelopments";
const wchar_t* kUpdaterMutex = L"Local\\RugratsAdventureGame_x64_Updater";
const wchar_t* kAgent = L"RugratsAdventureGameUpdater/1.0";
const int kBtnDownload = 100, kBtnNotes = 101, kBtnWebPage = 102, kBtnRetry = 103;

HINSTANCE g_inst;
HWND g_host = nullptr;   // an invisible window that stays open for the whole run (see CreateHost)
std::wstring g_webPage = kDefaultWebPage;
std::wstring g_readmeFile;   // the update's notes, fetched with the check into %TEMP% (deleted on exit)

// ---- small helpers --------------------------------------------------------------------------
std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
std::wstring Trim(std::wstring s) {
    const wchar_t* ws = L" \t\r\n\"";
    s.erase(0, s.find_first_not_of(ws));
    const size_t e = s.find_last_not_of(ws);
    s.erase(e == std::wstring::npos ? 0 : e + 1);
    return s;
}
std::wstring Lower(std::wstring s) { for (wchar_t& c : s) c = towlower(c); return s; }
std::wstring ExeDir() {
    wchar_t p[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, p, DWORD(std::size(p)));
    std::wstring s(p, n);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}
std::wstring RegString(const wchar_t* value) {
    wchar_t buf[2048]; DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kSettingsKey, value, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buf, &size) == ERROR_SUCCESS)
        return Trim(buf);
    return {};
}
void OpenInBrowser(const std::wstring& url) {
    if (url.rfind(L"https://", 0) == 0 || url.rfind(L"http://", 0) == 0)
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// "12.3 of 48.1 MB downloaded" (GB once the whole download is 1 GB or more)
std::wstring Progress(uint64_t done, uint64_t total) {
    const bool gb = total >= (uint64_t(1) << 30);
    const double unit = gb ? 1073741824.0 : 1048576.0;
    wchar_t s[96];
    swprintf(s, std::size(s), gb ? L"%.2f of %.2f GB downloaded" : L"%.1f of %.1f MB downloaded", double(done) / unit, double(total) / unit);
    return s;
}
std::wstring SizeText(uint64_t bytes) {
    wchar_t s[64];
    if (bytes >= (uint64_t(1) << 30)) swprintf(s, std::size(s), L"%.2f GB", double(bytes) / 1073741824.0);
    else swprintf(s, std::size(s), L"%.1f MB", double(bytes) / 1048576.0);
    return s;
}

// "1.1" / "1.1.0.0" -> four numbers
struct Version { unsigned v[4] = {0, 0, 0, 0}; };
Version ParseVersion(const std::wstring& s) {
    Version r; size_t i = 0;
    for (int k = 0; k < 4 && i < s.size(); ++k) {
        unsigned n = 0; bool any = false;
        while (i < s.size() && iswdigit(s[i])) { n = n * 10 + unsigned(s[i] - L'0'); ++i; any = true; }
        if (!any) break;
        r.v[k] = n;
        if (i < s.size() && s[i] == L'.') ++i; else break;
    }
    return r;
}
int Compare(const Version& a, const Version& b) {
    for (int k = 0; k < 4; ++k) if (a.v[k] != b.v[k]) return a.v[k] < b.v[k] ? -1 : 1;
    return 0;
}
std::wstring Show(const Version& v) {   // 1.1.0.0 -> "1.1", 1.2.3.0 -> "1.2.3"
    int last = 1;
    for (int k = 2; k < 4; ++k) if (v.v[k]) last = k;
    std::wstring s;
    for (int k = 0; k <= last; ++k) { if (k) s += L'.'; s += std::to_wstring(v.v[k]); }
    return s;
}
// The installed game's version: RUGRATS.EXE beside this program.
bool InstalledVersion(Version& out) {
    const std::wstring exe = ExeDir() + L"RUGRATS.EXE";
    DWORD h = 0; const DWORD n = GetFileVersionInfoSizeW(exe.c_str(), &h);
    if (!n) return false;
    std::vector<BYTE> buf(n);
    if (!GetFileVersionInfoW(exe.c_str(), 0, n, buf.data())) return false;
    VS_FIXEDFILEINFO* fi = nullptr; UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&fi), &len) || !fi) return false;
    out.v[0] = HIWORD(fi->dwFileVersionMS); out.v[1] = LOWORD(fi->dwFileVersionMS);
    out.v[2] = HIWORD(fi->dwFileVersionLS); out.v[3] = LOWORD(fi->dwFileVersionLS);
    return true;
}

// ---- HTTPS ----------------------------------------------------------------------------------
struct Transfer {
    std::atomic<uint64_t> done{0}, total{0};
    std::atomic<bool> cancel{false}, finished{false};
    bool ok = false;
    std::wstring error;
};

// GET url (https only) into memory or a file. Follows redirects (GitHub release downloads do).
bool HttpGet(const std::wstring& url, Transfer& t, std::string* toMemory, HANDLE toFile, size_t maxMemory = 64 * 1024) {
    URL_COMPONENTS uc{}; uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[4096];
    uc.lpszHostName = host; uc.dwHostNameLength = DWORD(std::size(host));
    uc.lpszUrlPath = path; uc.dwUrlPathLength = DWORD(std::size(path));
    wchar_t extra[2048]; uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = DWORD(std::size(extra));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { t.error = L"Illegal address (" + url + L")"; return false; }
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) { t.error = L"The updater is only taking https:// addresses (address used: " + url + L")."; return false; }
    const std::wstring object = std::wstring(path) + extra;

    HINTERNET s = WinHttpOpen(kAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(kAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { t.error = L"Windows' internet functions could not be started."; return false; }
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000 /* TLS 1.3 */;
    if (!WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
    }
    WinHttpSetTimeouts(s, 15000, 15000, 30000, 60000);
    bool ok = false;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (r) {
        const wchar_t* headers = L"Cache-Control: no-cache\r\nPragma: no-cache\r\n";
        if (WinHttpSendRequest(r, headers, DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
            DWORD status = 0, sz = sizeof status;
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
            if (status != 200) {
                t.error = status == 404 ? std::wstring(L"Couldn't find the update file from the server (404 Not Found)")
                                        : L"The update server answered with an error (" + std::to_wstring(status) + L").";
            } else {
                wchar_t len[32]; DWORD lsz = sizeof len;
                if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len, &lsz, WINHTTP_NO_HEADER_INDEX))
                    t.total = _wcstoui64(len, nullptr, 10);
                std::vector<char> buf(256 * 1024);
                ok = true;
                for (;;) {
                    if (t.cancel) { ok = false; t.error = L"You have chosen to stop the download."; break; }
                    DWORD got = 0;
                    if (!WinHttpReadData(r, buf.data(), DWORD(buf.size()), &got)) { ok = false; break; }
                    if (!got) break;
                    if (toMemory) { toMemory->append(buf.data(), got); if (toMemory->size() > maxMemory) { ok = false; t.error = L"The update file seems to be larger than usual."; break; } }
                    if (toFile) { DWORD w = 0; if (!WriteFile(toFile, buf.data(), got, &w, nullptr) || w != got) { ok = false; t.error = L"Download failed to save (is it locked or the disk full?)."; break; } }
                    t.done += got;
                }
            }
        }
    }
    if (!ok && t.error.empty()) {
        const DWORD e = GetLastError();
        wchar_t msg[512] = L"";
        FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS, GetModuleHandleW(L"winhttp.dll"), e, 0, msg, DWORD(std::size(msg)), nullptr);
        t.error = L"Failed to connect to update server (error " + std::to_wstring(e) + L"). " + Trim(msg) +
                  L"\n\nPlease verify your internet connection and restart the update process.";
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

// ---- the update file ------------------------------------------------------------------------
struct UpdateInfo { std::wstring version, install, readme, sha256; uint64_t size = 0; bool hasSize = false; };
bool ParseUpdateFile(const std::string& text, UpdateInfo& u) {
    std::wstring all = Widen(text), section;
    if (!all.empty() && all[0] == 0xFEFF) all.erase(0, 1);
    size_t p = 0;
    while (p <= all.size()) {
        size_t e = all.find(L'\n', p); if (e == std::wstring::npos) e = all.size();
        std::wstring line = Trim(all.substr(p, e - p));
        p = e + 1;
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line[0] == L'[') { section = Lower(Trim(line.substr(1, line.find(L']') - 1))); continue; }
        if (section != L"wiseupdate") continue;
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        const std::wstring key = Lower(Trim(line.substr(0, eq))), val = Trim(line.substr(eq + 1));
        if (key == L"version") u.version = val;
        else if (key == L"install") u.install = val;
        else if (key == L"readme") u.readme = val;
        else if (key == L"sha256") u.sha256 = Lower(val);
        else if (key == L"size") { u.size = _wcstoui64(val.c_str(), nullptr, 10); u.hasSize = true; }
    }
    return !u.version.empty() && !u.install.empty();
}

std::wstring Sha256File(const std::wstring& file) {
    std::wstring hex;
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return hex;
    HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f != INVALID_HANDLE_VALUE && BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<UCHAR> buf(1 << 20); DWORD got = 0; bool ok = true;
        while (ReadFile(f, buf.data(), DWORD(buf.size()), &got, nullptr) && got)
            if (BCryptHashData(h, buf.data(), got, 0) != 0) { ok = false; break; }
        UCHAR d[32];
        if (ok && BCryptFinishHash(h, d, sizeof d, 0) == 0)
            for (UCHAR b : d) { wchar_t x[3]; swprintf(x, 3, L"%02x", b); hex += x; }
        BCryptDestroyHash(h);
    }
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

// ---- dialogs ---------------------------------------------------------------------------------
HRESULT CALLBACK LinkCallback(HWND, UINT msg, WPARAM, LPARAM lp, LONG_PTR) {
    if (msg == TDN_HYPERLINK_CLICKED) {
        const std::wstring href = reinterpret_cast<const wchar_t*>(lp);
        if (href == L"whatsnew" && !g_readmeFile.empty())          // the notes fetched with the check
            ShellExecuteW(nullptr, L"open", g_readmeFile.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else OpenInBrowser(href);
    }
    return S_OK;
}
int Message(PCWSTR icon, const std::wstring& main, const std::wstring& content, bool offerWebPage, bool offerRetry = false,
            const wchar_t* retryText = L"Restart download\nWe'll try to redownload the update again") {
    TASKDIALOGCONFIG tc{}; tc.cbSize = sizeof tc;
    // the standard icons (TD_INFORMATION_ICON...) are Windows' own: with hInstance set, Windows
    // looks for them in this program instead and the dialog fails to open
    tc.hInstance = nullptr; tc.hwndParent = g_host;
    tc.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION;
    tc.pszWindowTitle = kTitle; tc.pszMainIcon = icon;
    tc.pszMainInstruction = main.c_str(); tc.pszContent = content.c_str();
    TASKDIALOG_BUTTON b[2]; UINT n = 0;
    if (offerRetry) b[n++] = {kBtnRetry, retryText};
    if (offerWebPage) b[n++] = {kBtnWebPage, L"Open GitHub page\nOpens your default web browser"};
    tc.pButtons = n ? b : nullptr; tc.cButtons = n;
    if (n) tc.dwFlags |= TDF_USE_COMMAND_LINKS;
    tc.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    int pressed = 0;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, nullptr, nullptr))) {   // never leave the player with nothing
        const UINT type = icon == TD_ERROR_ICON ? MB_ICONERROR : icon == TD_WARNING_ICON ? MB_ICONWARNING : MB_ICONINFORMATION;
        if (offerRetry) {
            const int r = MessageBoxW(g_host, (main + L"\n\n" + content + L"\n\nTry again?").c_str(), kTitle, type | MB_RETRYCANCEL);
            return r == IDRETRY ? kBtnRetry : IDCLOSE;
        }
        MessageBoxW(g_host, (main + L"\n\n" + content).c_str(), kTitle, type | MB_OK);
        return IDCLOSE;
    }
    if (pressed == kBtnWebPage) OpenInBrowser(g_webPage);
    return pressed;
}

// A task dialog with a progress bar while `work` runs on another thread.
struct ProgressCtx { Transfer* t; bool marquee; bool userCancelled = false; };
HRESULT CALLBACK ProgressCallback(HWND h, UINT msg, WPARAM wp, LPARAM, LONG_PTR ref) {
    auto* ctx = reinterpret_cast<ProgressCtx*>(ref);
    switch (msg) {
    case TDN_CREATED:
        if (ctx->marquee) SendMessageW(h, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 30);
        else SendMessageW(h, TDM_SET_PROGRESS_BAR_RANGE, 0, MAKELPARAM(0, 1000));
        break;
    case TDN_TIMER:
        if (ctx->t->finished) { SendMessageW(h, TDM_CLICK_BUTTON, IDCANCEL, 0); break; }
        if (!ctx->marquee) {
            const uint64_t total = ctx->t->total, done = ctx->t->done;
            if (total) {
                SendMessageW(h, TDM_SET_PROGRESS_BAR_POS, WPARAM(done * 1000 / total), 0);
                const std::wstring line = Progress(done, total);
                SendMessageW(h, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, LPARAM(line.c_str()));
            }
        }
        break;
    case TDN_BUTTON_CLICKED:
        if (wp == IDCANCEL && !ctx->t->finished) { ctx->userCancelled = true; ctx->t->cancel = true; return S_FALSE; }   // wait for the worker
        break;
    }
    return S_OK;
}
template <class Work>
bool RunWithProgress(const std::wstring& main, const std::wstring& content, bool marquee, Transfer& t, Work work, bool& cancelled) {
    std::thread worker([&] { t.ok = work(); t.finished = true; });
    ProgressCtx ctx{&t, marquee};
    TASKDIALOGCONFIG tc{}; tc.cbSize = sizeof tc;
    tc.hInstance = g_inst; tc.hwndParent = g_host;
    tc.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SHOW_PROGRESS_BAR | TDF_CALLBACK_TIMER | TDF_ALLOW_DIALOG_CANCELLATION | (marquee ? TDF_SHOW_MARQUEE_PROGRESS_BAR : 0);
    tc.pszWindowTitle = kTitle; tc.pszMainIcon = MAKEINTRESOURCEW(1);
    tc.pszMainInstruction = main.c_str(); tc.pszContent = content.c_str();
    tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tc.pfCallback = ProgressCallback; tc.lpCallbackData = reinterpret_cast<LONG_PTR>(&ctx);
    TaskDialogIndirect(&tc, nullptr, nullptr, nullptr);
    worker.join();
    cancelled = ctx.userCancelled;
    return t.ok;
}

bool GameRunning() {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, kGameMutex);
    if (m) { CloseHandle(m); return true; }
    return GetLastError() == ERROR_ACCESS_DENIED;   // started as administrator
}

std::wstring TempDir() {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"RugratsUpdate\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

int Run(const std::wstring& url) {
    // the notes fetched with the check are only kept while this program runs
    struct DropReadme { ~DropReadme() { if (!g_readmeFile.empty()) DeleteFileW(g_readmeFile.c_str()); } } dropReadme;
    // 1. the update file (and the update's notes, when it names them: ReadMe=)
    std::string text, notes;
    UpdateInfo u;
    for (;;) {
        Transfer t; bool cancelled = false;
        text.clear(); notes.clear(); u = UpdateInfo{};
        const bool ok = RunWithProgress(L"Checking for updates...", L"Contacting the update server.", true, t,
                                        [&] {
                                            if (!HttpGet(url, t, &text, nullptr)) return false;
                                            if (ParseUpdateFile(text, u) && u.readme.rfind(L"https://", 0) == 0) {
                                                Transfer n;
                                                if (!HttpGet(u.readme, n, &notes, nullptr, 512 * 1024)) notes.clear();
                                            }
                                            return true;
                                        }, cancelled);
        if (cancelled) return 1;
        if (ok) break;
        if (Message(TD_ERROR_ICON, L"Failed to fetch updates", t.error, true, true) != kBtnRetry) return 1;
    }
    if (u.version.empty() || u.install.empty()) {
        Message(TD_ERROR_ICON, L"Failed to fetch updates", L"Failed to gather update information from server.", true);
        return 1;
    }
    Version installed;
    const bool haveInstalled = InstalledVersion(installed);
    const Version latest = ParseVersion(u.version);
    if (haveInstalled && Compare(latest, installed) <= 0) {
        Message(TD_INFORMATION_ICON, L"Rugrats Adventure Game (x64) is up to date!",
                L"You're running the latest version! There is no need to update.", false);
        return 0;
    }

    // 2. offer it. The notes go into %TEMP% for "See what's new" (a .txt or .rtf file).
    if (!notes.empty()) {
        std::wstring ext = Lower(u.readme.substr(0, u.readme.find_first_of(L"?#")));
        ext = ext.size() > 4 ? ext.substr(ext.size() - 4) : L"";
        if (ext != L".rtf") ext = L".txt";
        const std::wstring f = TempDir() + L"whatsnew" + ext;
        HANDLE h = CreateFileW(f.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            if (WriteFile(h, notes.data(), DWORD(notes.size()), &w, nullptr) && w == notes.size()) g_readmeFile = f;
            CloseHandle(h);
            if (g_readmeFile.empty()) DeleteFileW(f.c_str());
        }
    }
    std::wstring content = L"New version: " + Show(latest);
    if (!haveInstalled) content += L"\nFailed to fetch version info.";
    if (u.hasSize) content += L"\nSize: " + SizeText(u.size);
    if (!g_readmeFile.empty()) content += L"\n\n<a href=\"whatsnew\">See what's new</a>";
    TASKDIALOG_BUTTON buttons[] = {{kBtnDownload, L"Download and install\nDownloads from GitHub"}};
    TASKDIALOGCONFIG tc{}; tc.cbSize = sizeof tc;
    tc.hInstance = g_inst; tc.hwndParent = g_host;
    tc.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_COMMAND_LINKS | TDF_ENABLE_HYPERLINKS;
    tc.pszWindowTitle = kTitle; tc.pszMainIcon = MAKEINTRESOURCEW(1);
    const std::wstring main = L"New update available!";
    tc.pszMainInstruction = main.c_str(); tc.pszContent = content.c_str();
    tc.pButtons = buttons; tc.cButtons = 1;
    tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tc.pfCallback = LinkCallback;
    int pressed = 0;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, nullptr, nullptr))) {
        std::wstring plain = content.substr(0, content.find(L"\n\n<a "));
        if (MessageBoxW(g_host, (main + L"\n\n" + plain + L"\n\nDownload and install now?").c_str(), kTitle, MB_ICONINFORMATION | MB_YESNO) == IDYES)
            pressed = kBtnDownload;
    }
    if (pressed != kBtnDownload) return 0;
    while (GameRunning()) {
        if (Message(TD_WARNING_ICON, L"Before we can continue...",
                    L"You need to close Rugrats Adventure Game before proceeding.", false, true, L"Try again\nOnce fully closed, the update will continue.") != kBtnRetry) return 0;
    }

    // 3. download into the user's temporary folder
    std::wstring name = u.install.substr(u.install.find_last_of(L'/') + 1);
    name = name.substr(0, name.find_first_of(L"?#"));
    if (name.empty() || Lower(name).size() < 5 || Lower(name).substr(Lower(name).size() - 4) != L".exe") name = L"RRATADV64.EXE";
    const std::wstring dir = TempDir();
    const std::wstring file = dir + name;
    for (;;) {
        HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) { Message(TD_ERROR_ICON, L"Failed to download update", L"Failed to download to " + file + L" (is it locked or in use?).", true); return 1; }
        Transfer t; bool cancelled = false;
        if (u.hasSize) t.total = u.size;
        const bool ok = RunWithProgress(L"Downloading...", u.hasSize ? Progress(0, u.size) : std::wstring(L"Starting download."), false, t,
                                        [&] { return HttpGet(u.install, t, nullptr, f); }, cancelled);
        CloseHandle(f);
        if (cancelled) {
            DeleteFileW(file.c_str());
            if (Message(TD_INFORMATION_ICON, L"Failed to download update", L"You have chosen to stop the download.", true, true) != kBtnRetry) return 1;
            continue;
        }
        if (ok) {
            std::wstring heading, problem;
            if (u.hasSize && t.done != u.size) { heading = L"Corrupted download."; problem = L"File size does not match compared to what was declared."; }
            else if (!u.sha256.empty() && Sha256File(file) != u.sha256) { heading = L"Invalid checksum"; problem = L"The update retrieved does not correspond to the expected checksum."; }
            if (problem.empty()) break;
            DeleteFileW(file.c_str());
            if (Message(TD_ERROR_ICON, heading, problem + L"\n\nThe file has been removed. Please try again.", true, true) != kBtnRetry) return 1;
            continue;
        }
        DeleteFileW(file.c_str());
        if (Message(TD_ERROR_ICON, L"Failed to download update", t.error, true, true) != kBtnRetry) return 1;
    }

    // 4. start the installer and get out of its way (this program's folder is being updated)
    SHELLEXECUTEINFOW sei{}; sei.cbSize = sizeof sei;
    sei.lpVerb = L"open"; sei.lpFile = file.c_str(); sei.lpDirectory = dir.c_str(); sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        Message(TD_ERROR_ICON, L"Failed to open installer", L"We've downloaded the update to:\n" + file + L"\n\nPlease run the installer manually.", false);
        return 1;
    }
    return 0;
}

}  // namespace

// The updater shows one dialog after another. Between two of them it would have no window at
// all for a moment, and Windows then gives the foreground to another program and keeps the next
// dialog behind it (focus-stealing protection). So an invisible window - transparent, centred,
// with the updater's title and icon on the taskbar - stays open the whole time and owns every
// dialog: the updater keeps the foreground and each dialog opens in front, centred on screen.
void CreateHost() {
    WNDCLASSEXW wc{}; wc.cbSize = sizeof wc;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = g_inst;
    wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = static_cast<HICON>(LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wc.lpszClassName = L"RugratsUpdaterHost";
    RegisterClassExW(&wc);
    const int x = GetSystemMetrics(SM_CXSCREEN) / 2, y = GetSystemMetrics(SM_CYSCREEN) / 2;
    g_host = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_LAYERED, wc.lpszClassName, kTitle, WS_POPUP,
                             x, y, 1, 1, nullptr, nullptr, g_inst, nullptr);
    if (!g_host) return;
    SetLayeredWindowAttributes(g_host, 0, 0, LWA_ALPHA);   // fully transparent
    ShowWindow(g_host, SW_SHOW);
    SetForegroundWindow(g_host);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    g_inst = inst;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_LINK_CLASS};
    InitCommonControlsEx(&icc);
    HANDLE single = CreateMutexW(nullptr, TRUE, kUpdaterMutex);
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(single); return 0; }

    std::wstring url = RegString(L"UpdateURL");
    if (url.empty()) url = kDefaultUrl;
    if (const std::wstring w = RegString(L"WebPage"); !w.empty()) g_webPage = w;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i + 1 < argc; ++i)
        if (_wcsicmp(argv[i], L"/url") == 0 || _wcsicmp(argv[i], L"--url") == 0) url = argv[++i];
    if (argv) LocalFree(argv);

    CreateHost();
    const int rc = Run(url);
    if (g_host) DestroyWindow(g_host);
    if (single) CloseHandle(single);
    return rc;
}
