#include <windows.h>
#include <winhttp.h>

#include <cctype>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kClass[]   = L"OjbsDgbMiner";
constexpr wchar_t kTitle[]   = L"Minero visible de DigiByte (DGB)";
constexpr wchar_t kCfgPath[] = L".\\miner.cfg";

struct Config {
    std::wstring pool;
    std::wstring wallet;
    std::wstring worker;
    std::wstring miner;
    std::wstring args;
    std::wstring server;
    std::wstring localApi;
    int threads;
};

HWND g_start{};
HWND g_stop{};
HWND g_accept{};
HWND g_walletView{};
HWND g_status{};
HANDLE g_stopEvent{};
HANDLE g_statsThread{};
PROCESS_INFORMATION g_pi{};
DWORD g_startTick = 0;
volatile bool g_running = false;

std::wstring replaceAll(std::wstring s, const std::wstring& from, const std::wstring& to) {
    if (from.empty()) return s;
    size_t p = 0;
    while ((p = s.find(from, p)) != std::wstring::npos) {
        s.replace(p, from.size(), to);
        p += to.size();
    }
    return s;
}

Config loadConfig() {
    wchar_t buf[1024]{};
    Config c;

    GetPrivateProfileStringW(L"miner", L"pool", L"", buf, 1024, kCfgPath);
    c.pool = buf;

    GetPrivateProfileStringW(L"miner", L"wallet", L"", buf, 1024, kCfgPath);
    c.wallet = buf;

    GetPrivateProfileStringW(L"miner", L"worker", L"worker01", buf, 1024, kCfgPath);
    c.worker = buf;

    GetPrivateProfileStringW(L"miner", L"miner", L"dgb-miner.exe", buf, 1024, kCfgPath);
    c.miner = buf;

    GetPrivateProfileStringW(L"miner", L"args",
        L"%MINER% -o %POOL% -u %WALLET%.%WORKER% -t %THREADS%", buf, 1024, kCfgPath);
    c.args = buf;

    GetPrivateProfileStringW(L"miner", L"server",
        L"http://127.0.0.1:8765/report", buf, 1024, kCfgPath);
    c.server = buf;

    GetPrivateProfileStringW(L"stats", L"local_api", L"", buf, 1024, kCfgPath);
    c.localApi = buf;

    GetPrivateProfileStringW(L"miner", L"threads", L"2", buf, 1024, kCfgPath);
    c.threads = _wtoi(buf);
    if (c.threads < 1) c.threads = 1;
    if (c.threads > 256) c.threads = 256;

    return c;
}

std::wstring utf8ToWide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string wideToUtf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char ch : s) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (ch < 0x20) {
                static const char hex[] = "0123456789abcdef";
                out += "\\u00";
                out += hex[(ch >> 4) & 0xF];
                out += hex[ch & 0xF];
            } else {
                out += static_cast<char>(ch);
            }
        }
    }
    return out;
}

bool httpRequest(const std::wstring& url, const std::string& method,
                 const std::string& body, std::string& out) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);

    wchar_t host[256]{};
    wchar_t path[2048]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = static_cast<DWORD>(sizeof(host) / sizeof(host[0]));
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;

    HINTERNET session = WinHttpOpen(L"OjbsDgbMiner/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!session) return false;

    HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (!conn) {
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    std::wstring reqPath = path[0] ? path : L"/";
    HINTERNET req = WinHttpOpenRequest(conn, utf8ToWide(method).c_str(), reqPath.c_str(),
        nullptr, nullptr, nullptr, flags);
    if (!req) {
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return false;
    }

    WinHttpSetTimeouts(req, 3000, 3000, 3000, 3000);

    LPCWSTR headers = body.empty() ? nullptr : L"Content-Type: application/json\r\n";
    BOOL ok = WinHttpSendRequest(req, headers, static_cast<DWORD>(-1),
        body.empty() ? nullptr : const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);

    if (ok) ok = WinHttpReceiveResponse(req, nullptr);

    out.clear();
    if (ok) {
        char chunk[4096];
        DWORD read = 0;
        while (WinHttpReadData(req, chunk, sizeof(chunk), &read) && read > 0) {
            out.append(chunk, read);
        }
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return ok != FALSE;
}

bool jsonNumber(const std::string& json, const std::vector<std::string>& keys, double& out) {
    for (const auto& key : keys) {
        const std::string pat = "\"" + key + "\":";
        size_t p = json.find(pat);
        if (p == std::string::npos) continue;
        p += pat.size();
        while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
        size_t e = p;
        while (e < json.size() &&
               (std::isdigit(static_cast<unsigned char>(json[e])) ||
                json[e] == '-' || json[e] == '+' || json[e] == '.' ||
                json[e] == 'e' || json[e] == 'E')) ++e;
        if (e == p) continue;
        out = std::atof(json.substr(p, e - p).c_str());
        return true;
    }
    return false;
}

bool jsonText(const std::string& json, const std::vector<std::string>& keys, std::string& out) {
    for (const auto& key : keys) {
        const std::string pat = "\"" + key + "\":\"";
        size_t p = json.find(pat);
        if (p == std::string::npos) continue;
        p += pat.size();
        size_t e = json.find('"', p);
        if (e == std::string::npos) continue;
        out = json.substr(p, e - p);
        return true;
    }
    return false;
}

std::string localComputerName() {
    wchar_t name[256]{};
    DWORD size = static_cast<DWORD>(sizeof(name) / sizeof(name[0]));
    if (GetComputerNameW(name, &size)) return wideToUtf8(name);
    return "pc";
}

DWORD WINAPI statsThread(LPVOID) {
    while (g_running) {
        if (WaitForSingleObject(g_stopEvent, 5000) == WAIT_OBJECT_0) break;
        if (!g_running) break;

        Config c = loadConfig();

        double hr = 0.0;
        double hashes = 0.0;
        double shares = 0.0;
        double uptime = 0.0;
        double paidDgb = 0.0;
        std::string rig = localComputerName();

        if (!c.localApi.empty()) {
            std::string stats;
            if (httpRequest(c.localApi, "GET", "", stats)) {
                jsonNumber(stats, {"hashrate", "hashrate_60s", "speed", "hr"}, hr);
                jsonNumber(stats, {"hashes_total", "hashes", "total_hashes", "accepted"}, hashes);
                jsonNumber(stats, {"shares_good", "shares_accepted", "shares", "accepted_shares"}, shares);
                jsonNumber(stats, {"uptime", "uptime_seconds", "runtime"}, uptime);
                jsonNumber(stats, {"paid_dgb", "paid", "earned_dgb", "balance_dgb", "amount", "total_dgb"}, paidDgb);
                jsonText(stats, {"worker_id", "worker", "rig", "miner"}, rig);
            }
        }

        if (uptime <= 0.0 && g_startTick) {
            uptime = static_cast<double>(GetTickCount64() - g_startTick) / 1000.0;
        }

        std::string payload = "{\"coin\":\"DGB\",";
        payload += "\"rig\":\"" + jsonEscape(rig) + "\",";
        payload += "\"hashrate\":" + std::to_string(hr) + ",";
        payload += "\"hashes\":" + std::to_string(static_cast<long long>(hashes)) + ",";
        payload += "\"shares\":" + std::to_string(static_cast<long long>(shares)) + ",";
        payload += "\"uptime\":" + std::to_string(static_cast<long long>(uptime)) + ",";
        payload += "\"mined_dgb\":" + std::to_string(paidDgb) + ",";
        payload += "\"wallet\":\"" + jsonEscape(wideToUtf8(c.wallet)) + "\",";
        payload += "\"ts\":" + std::to_string(static_cast<long long>(std::time(nullptr))) + "}";

        std::string ignored;
        httpRequest(c.server, "POST", payload, ignored);
    }
    return 0;
}

void stopMiner() {
    if (!g_running) return;

    SetEvent(g_stopEvent);
    if (g_pi.hProcess) {
        TerminateProcess(g_pi.hProcess, 0);
        WaitForSingleObject(g_pi.hProcess, 5000);
    }

    if (g_statsThread) {
        WaitForSingleObject(g_statsThread, 6000);
        CloseHandle(g_statsThread);
        g_statsThread = nullptr;
    }

    if (g_pi.hThread) CloseHandle(g_pi.hThread);
    if (g_pi.hProcess) CloseHandle(g_pi.hProcess);
    g_pi = PROCESS_INFORMATION{};
    g_running = false;
    g_startTick = 0;

    EnableWindow(g_start, TRUE);
    EnableWindow(g_stop, FALSE);
    SetWindowTextW(g_status, L"Detenido. No queda ningún proceso minando DGB.");
}

std::wstring buildCommand(const Config& c) {
    std::wstring exe = L"\"" + c.miner + L"\"";
    std::wstring cmd = c.args;
    if (cmd.find(L"%MINER%") == std::wstring::npos) {
        cmd = exe + L" " + cmd;
    }
    cmd = replaceAll(cmd, L"%MINER%", exe);
    cmd = replaceAll(cmd, L"%POOL%", c.pool);
    cmd = replaceAll(cmd, L"%WALLET%", c.wallet);
    cmd = replaceAll(cmd, L"%WORKER%", c.worker);
    cmd = replaceAll(cmd, L"%THREADS%", std::to_wstring(c.threads));
    return cmd;
}

void startMiner() {
    if (g_running) return;

    if (SendMessageW(g_accept, BM_GETCHECK, 0, 0) != BST_CHECKED) {
        SetWindowTextW(g_status, L"Marcá la casilla de consentimiento antes de iniciar.");
        return;
    }

    Config c = loadConfig();

    if (c.wallet.empty() || c.wallet.rfind(L"FILL:", 0) == 0) {
        SetWindowTextW(g_status, L"Configurá tu wallet DGB real en miner.cfg antes de iniciar.");
        return;
    }
    if (c.pool.empty() || c.pool.rfind(L"FILL:", 0) == 0 ||
        c.pool.find(L"example.invalid") != std::wstring::npos) {
        SetWindowTextW(g_status, L"Configurá el pool DGB real en miner.cfg antes de iniciar.");
        return;
    }
    if (c.miner.empty() || c.miner.rfind(L"FILL:", 0) == 0) {
        SetWindowTextW(g_status, L"Configurá el ejecutable del minero DGB en miner.cfg.");
        return;
    }
    if (GetFileAttributesW(c.miner.c_str()) == INVALID_FILE_ATTRIBUTES) {
        SetWindowTextW(g_status, L"No se encontró el minero. Poné el .exe indicado junto a esta app.");
        return;
    }

    std::wstring cmd = buildCommand(c);
    STARTUPINFOW si{};
    si.cb = sizeof(si);

    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &g_pi)) {
        SetWindowTextW(g_status, L"No se pudo iniciar el minero DGB. Revisá la ruta y los argumentos.");
        return;
    }

    ResetEvent(g_stopEvent);
    g_startTick = static_cast<DWORD>(GetTickCount64());
    g_running = true;
    g_statsThread = CreateThread(nullptr, 0, statsThread, nullptr, 0, nullptr);

    EnableWindow(g_start, FALSE);
    EnableWindow(g_stop, TRUE);
    SetWindowTextW(g_status, L"Minando DGB. Consola visible. Panel: http://127.0.0.1:8765");
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE: {
        CreateWindowExW(0, L"STATIC",
            L"Minero visible de DigiByte (DGB). Inicia el programa que vos instales, "
            L"muestra su consola y envía las estadísticas al panel local. "
            L"Podés detenerlo cuando quieras.",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 20, 450, 60,
            hwnd, nullptr, nullptr, nullptr);

        g_walletView = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL,
            20, 90, 450, 24, hwnd, nullptr, nullptr, nullptr);

        g_accept = CreateWindowExW(0, L"BUTTON",
            L"Acepto minar DGB y enviar todo el resultado a la wallet indicada",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            20, 128, 450, 24, hwnd, nullptr, nullptr, nullptr);

        g_start = CreateWindowExW(0, L"BUTTON", L"Iniciar",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            20, 170, 110, 32, hwnd, reinterpret_cast<HMENU>(1), nullptr, nullptr);

        g_stop = CreateWindowExW(0, L"BUTTON", L"Detener",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            145, 170, 110, 32, hwnd, reinterpret_cast<HMENU>(2), nullptr, nullptr);

        g_status = CreateWindowExW(0, L"STATIC", L"Listo. Revisá miner.cfg antes de empezar.",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 222, 450, 28,
            hwnd, nullptr, nullptr, nullptr);

        g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        EnableWindow(g_stop, FALSE);

        Config c = loadConfig();
        SetWindowTextW(g_walletView,
            (L"DGB wallet: " + c.wallet + L"    Pool: " + c.pool +
             L"    Minero: " + c.miner).c_str());
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case 1: startMiner(); break;
        case 2: stopMiner();  break;
        }
        return 0;

    case WM_CLOSE:
        stopMiner();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_stopEvent) CloseHandle(g_stopEvent);
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    WNDCLASSW wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = kClass;
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    if (!RegisterClassW(&wc)) return 1;

    CreateWindowExW(0, kClass, kTitle,
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 300,
        nullptr, nullptr, hInstance, nullptr);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return 0;
}
