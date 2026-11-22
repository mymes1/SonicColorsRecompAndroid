#include "music_mod.h"
#include "music_paths.h"
#include "music_manifest.h"
#include "music_tool.h"
#include "../config.h"
#include "../ios/disc.h"
#include "../log.h"
#include "../platform/platform.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{

std::string s_dirOverride;
std::string s_dir;
bool s_debug = false;
bool s_active = false;       // patched CPK registered
bool s_useOriginal = true;
std::string s_patchedCpk;
std::string s_statusJson;
std::string s_patchedSha;    // from current.json / status (hex)
bool s_loggedCpkOpen = false;

struct ProofEntry
{
    std::string path;
    uint64_t offset = 0;
    uint64_t size = 0;
    std::string sha256;
    uint8_t first16[16]{};
    bool haveFirst16 = false;
    bool matched = false;
};
std::vector<ProofEntry> s_proof;
std::string s_proofTsv;

static fs::path FindMusicAssetTool() { return MusicTool_Find(); }

#ifdef SCR_ANDROID
// The music-mod path resolver (audio/music_paths.cpp) pulls in nlohmann/json for the Music
// Library tab's status query, which the APK does not build; on Android the cache lives under the
// Music Library directory the manifest already points at.
static fs::path DefaultCacheRoot() { return MusicManifest_DefaultDir() / "cache"; }
#else
static fs::path DefaultCacheRoot() { return MusicPaths_CacheRoot(); }
#endif

static std::string JsonGetString(const std::string& json, const char* key)
{
    const std::string pat = std::string("\"") + key + "\"";
    auto pos = json.find(pat);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos);
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos);
    if (pos == std::string::npos) return {};
    auto end = json.find('"', pos + 1);
    if (end == std::string::npos) return {};
    return json.substr(pos + 1, end - pos - 1);
}

static bool JsonGetBool(const std::string& json, const char* key, bool def)
{
    const std::string pat = std::string("\"") + key + "\"";
    auto pos = json.find(pat);
    if (pos == std::string::npos) return def;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return def;
    while (pos < json.size() && (json[pos] == ':' || json[pos] == ' ')) pos++;
    if (json.compare(pos, 4, "true") == 0) return true;
    if (json.compare(pos, 5, "false") == 0) return false;
    return def;
}

static int HexNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool ParseHex16(const std::string& hex, uint8_t out[16])
{
    if (hex.size() < 32) return false;
    for (int i = 0; i < 16; i++)
    {
        int a = HexNibble(hex[i * 2]);
        int b = HexNibble(hex[i * 2 + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (uint8_t)((a << 4) | b);
    }
    return true;
}

static void LoadProofTsv(const fs::path& tsv)
{
    s_proof.clear();
    s_proofTsv = tsv.string();
    std::ifstream in(tsv);
    if (!in)
    {
        LOG_WARN("[MUSIC] payload proof TSV missing: {}", tsv.string());
        return;
    }
    std::string line;
    bool header = true;
    while (std::getline(in, line))
    {
        if (header) { header = false; continue; }
        if (line.empty()) continue;
        // path \t offset \t size \t sha \t first16
        ProofEntry e;
        size_t p0 = 0;
        auto next = [&](std::string& out) -> bool {
            size_t p1 = line.find('\t', p0);
            if (p1 == std::string::npos)
            {
                out = line.substr(p0);
                p0 = line.size();
                return !out.empty();
            }
            out = line.substr(p0, p1 - p0);
            p0 = p1 + 1;
            return true;
        };
        std::string offS, sizeS, sha, hex16;
        if (!next(e.path) || !next(offS) || !next(sizeS) || !next(sha)) continue;
        next(hex16);
        e.offset = std::strtoull(offS.c_str(), nullptr, 10);
        e.size = std::strtoull(sizeS.c_str(), nullptr, 10);
        e.sha256 = sha;
        e.haveFirst16 = ParseHex16(hex16, e.first16);
        s_proof.push_back(std::move(e));
    }
    LOG_INFO("[MUSIC] Loaded {} payload proof entries from {}", s_proof.size(), tsv.string());
}

static bool RunPrepare(const fs::path& manifest, const fs::path& musicDir, const fs::path& cpk,
                       const fs::path& cache, std::string& outJson)
{
    if (FindMusicAssetTool().empty())
    {
        LOG_WARN("[MUSIC] tools/music_asset not found — cannot build patched CPK");
        return false;
    }
    const fs::path outFile = cache / "prepare_last.json";
    const fs::path errFile = cache / "prepare_last.err";
    fs::create_directories(cache);
    // Default: do not replace FX variants (main BGM only).
    LOG_INFO("[MUSIC] Preparing assets via music_asset prepare");
    int rc = -1;
    if (!MusicTool_Run({ "prepare", "--manifest", manifest.string(), "--music-dir", musicDir.string(),
                         "--cpk", cpk.string(), "--cache", cache.string(), "--no-fx" },
                       outFile, errFile, rc))
    {
        LOG_WARN("[MUSIC] prepare could not start");
        return false;
    }
    std::ifstream in(outFile);
    if (!in)
    {
        LOG_WARN("[MUSIC] prepare produced no JSON (rc={})", rc);
        return false;
    }
    outJson.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (rc != 0)
    {
        LOG_WARN("[MUSIC] prepare failed (rc={}): {}", rc, outJson.empty() ? "(see prepare_last.err)" : outJson);
        return false;
    }
    return true;
}

} // namespace

void MusicMod_SetDir(const std::string& dir) { s_dirOverride = dir; }
void MusicMod_SetDebug(bool on) { s_debug = on; }
void MusicMod_ForceStage(const std::string&) { /* overlay removed */ }
bool MusicMod_Debug() { return s_debug; }

void MusicMod_Init()
{
#ifdef SCR_ANDROID
    // Music Library replacement needs tools/music_asset and a desktop process to drive it, and the
    // APK ships neither; the original soundtrack is the only option here (see music_paths.h).
    s_active = false;
    s_useOriginal = true;
    s_patchedCpk.clear();
    s_statusJson.clear();
    s_patchedSha.clear();
    s_proof.clear();
    s_loggedCpkOpen = false;
    if (g_config.musicModsEnabled)
        LOG_INFO("[MUSIC] replacement tracks are not available on Android; using the original soundtrack");
    return;
#else
    s_active = false;
    s_useOriginal = true;
    s_patchedCpk.clear();
    s_statusJson.clear();
    s_patchedSha.clear();
    s_proof.clear();
    s_loggedCpkOpen = false;

    if (!g_config.musicModsEnabled)
    {
        LOG_INFO("[MUSIC] Music mods disabled in settings — original CPK");
        return;
    }
    if (!s_dirOverride.empty())
        s_dir = s_dirOverride;
    else if (!g_config.musicModsDir.empty())
        s_dir = g_config.musicModsDir;
    else
        s_dir = MusicManifest_DefaultDir().string();

    const fs::path musicDir = fs::path(s_dir);
    const fs::path manifest = musicDir / "manifest.toml";
    if (!fs::exists(manifest))
    {
        LOG_INFO("[MUSIC] No manifest at {} — original soundtrack", manifest.string());
        return;
    }

    fs::path cpk = MusicPaths_OriginalCpk();
    if (!fs::exists(cpk))
    {
        LOG_WARN("[MUSIC] sonic2010_0.cpk not found — cannot patch (tried {})", cpk.string());
        return;
    }
    LOG_INFO("[MUSIC] original CPK → {}", cpk.string());

    const fs::path cache = DefaultCacheRoot();
    LOG_INFO("[MUSIC] cache root → {}", cache.string());
    std::string json;
    if (!RunPrepare(manifest, musicDir, cpk, cache, json))
    {
        LOG_WARN("[MUSIC] Falling back to original CPK");
        return;
    }
    s_statusJson = json;
    const bool useOrig = JsonGetBool(json, "use_original", true);
    const bool ok = JsonGetBool(json, "ok", false);
    if (!ok)
    {
        LOG_WARN("[MUSIC] Asset prep reported failure — original CPK");
        return;
    }
    if (useOrig)
    {
        LOG_INFO("[MUSIC] {}", JsonGetString(json, "message").empty()
                                   ? "Using original CPK (no active mappings)"
                                   : JsonGetString(json, "message"));
        return;
    }

    s_patchedCpk = JsonGetString(json, "patched_cpk");
    s_patchedSha = JsonGetString(json, "patched_sha256");
    if (s_patchedCpk.empty() || !fs::exists(s_patchedCpk))
    {
        const fs::path cur = cache / "current.json";
        if (fs::exists(cur))
        {
            std::ifstream in(cur);
            std::string cj((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            s_patchedCpk = JsonGetString(cj, "patched_cpk");
            if (s_patchedSha.empty()) s_patchedSha = JsonGetString(cj, "patched_sha256");
            std::string tsv = JsonGetString(cj, "payload_proof_tsv");
            if (!tsv.empty()) LoadProofTsv(tsv);
        }
    }
    if (s_patchedCpk.empty() || !fs::exists(s_patchedCpk))
    {
        LOG_WARN("[MUSIC] Patched CPK path missing — original CPK");
        return;
    }

    // Prefer TSV path from prepare JSON
    {
        std::string tsv = JsonGetString(json, "payload_proof_tsv");
        if (tsv.empty())
        {
            fs::path guess = fs::path(s_patchedCpk).parent_path() / "payload_proof.tsv";
            if (fs::exists(guess)) tsv = guess.string();
        }
        if (!tsv.empty() && s_proof.empty())
            LoadProofTsv(tsv);
    }

    // Register every patched archive listed in prepare status (SNCP8P: one CPK today).
    int registered = 0;
    {
        const std::string& js = s_statusJson;
        size_t pos = js.find("\"overrides\"");
        if (pos != std::string::npos)
        {
            size_t arr = js.find('[', pos);
            size_t end = arr == std::string::npos ? std::string::npos : js.find(']', arr);
            if (arr != std::string::npos && end != std::string::npos)
            {
                std::string block = js.substr(arr, end - arr + 1);
                size_t i = 0;
                while ((i = block.find("\"disc_name\"", i)) != std::string::npos)
                {
                    auto pull = [&](const char* key, size_t from) -> std::string {
                        auto p = block.find(std::string("\"") + key + "\"", from);
                        if (p == std::string::npos || p > from + 200) return {};
                        p = block.find('"', block.find(':', p));
                        if (p == std::string::npos) return {};
                        auto e = block.find('"', p + 1);
                        if (e == std::string::npos) return {};
                        return block.substr(p + 1, e - p - 1);
                    };
                    std::string disc = pull("disc_name", i);
                    std::string host = pull("patched_cpk", i);
                    i += 10;
                    if (disc.empty() || host.empty() || !fs::exists(host)) continue;
                    Disc_RegisterHostOverride(disc, host);
                    registered++;
                    LOG_INFO("[MUSIC] Host override {} → {}", disc, host);
                }
            }
        }
    }
    if (registered == 0)
    {
        Disc_RegisterHostOverride("sonic2010_0.cpk", s_patchedCpk);
        registered = 1;
        LOG_INFO("[MUSIC] Host override sonic2010_0.cpk → {}", s_patchedCpk);
    }
    s_active = true;
    s_useOriginal = false;
    LOG_INFO("[MUSIC] Manifest: {}", manifest.string());
    LOG_INFO("[MUSIC] Original CPK unchanged: {}", cpk.string());
    LOG_INFO("[MUSIC] Patched archive(s): {}  primary={} sha256={}",
             registered, s_patchedCpk,
             s_patchedSha.empty() ? "(see status.json)" : s_patchedSha);
    LOG_INFO("[MUSIC] Proof entries: {} (CRI loads replacement AAX; no host overlay)", s_proof.size());
    if (s_debug)
        LOG_INFO("[MUSIC][dbg] status={}", s_statusJson.size() > 400 ? s_statusJson.substr(0, 400) + "…" : s_statusJson);
#endif
}

void MusicMod_Shutdown()
{
    s_active = false;
    s_patchedCpk.clear();
    s_proof.clear();
}

void MusicMod_Tick() {}
void MusicMod_OnStreamActivity(bool active, int voices, int streaming)
{
    if (!s_debug) return;
    static int last = -1;
    const int key = (active ? 1 : 0) | (voices << 1) | (streaming << 8);
    if (key == last) return;
    last = key;
    LOG_INFO("[MUSIC][dbg] AX stream activity active={} voices={} streaming={}", active, voices, streaming);
}
void MusicMod_OnAxVoiceStats(int total, int running, int streaming)
{
    if (!s_debug) return;
    static int lastT = -1, lastR = -1, lastS = -1;
    if (total == lastT && running == lastR && streaming == lastS) return;
    lastT = total; lastR = running; lastS = streaming;
    LOG_INFO("[MUSIC][dbg] AX voices total={} running={} streaming={}", total, running, streaming);
}
void MusicMod_OnStageName(const char* stage, const char* via)
{
    // Natural/stage coverage: observe stage transitions without requiring music mods.
    extern void A2gNat_OnStageHint(const char*, const char*);
    A2gNat_OnStageHint(stage, via);
    if (!s_debug && !s_active) return;
    if (stage && stage[0])
        LOG_INFO("[MUSIC] stage context '{}' via {}", stage, via ? via : "?");
}
void MusicMod_OnGuestString(const char* s)
{
    if (!s_debug || !s) return;
    if (std::strncmp(s, "bgm_", 4) == 0)
        LOG_INFO("[MUSIC][dbg] guest cue/string '{}'", s);
}

void MusicMod_OnDiscPath(const std::string& path)
{
    if (!s_active) return;
    std::string lower = path;
    for (char& c : lower) c = (char)tolower((unsigned char)c);
    if (lower.find("sonic2010_0.cpk") == std::string::npos) return;
    if (s_loggedCpkOpen) return;
    s_loggedCpkOpen = true;
    LOG_INFO("[MUSIC] Game opened CPK via disc path '{}' — host override {}",
             path, s_patchedCpk);
}

// Portable SHA-256 for music proof logs (no shell / popen — MSVC and clang-cl on Windows).
static std::string Sha256Hex(const void* data, size_t len)
{
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const uint8_t* p = (const uint8_t*)data;
    uint8_t block[64];
    size_t used = 0;
    uint64_t bits = (uint64_t)len * 8;
    auto compress = [&](const uint8_t* b) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)b[4 * i] << 24) | ((uint32_t)b[4 * i + 1] << 16) | ((uint32_t)b[4 * i + 2] << 8) | b[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b0 = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b0) ^ (a & c) ^ (b0 & c);
            uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b0; b0 = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b0; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    };
    for (size_t i = 0; i < len; i++) {
        block[used++] = p[i];
        if (used == 64) { compress(block); used = 0; }
    }
    block[used++] = 0x80;
    if (used > 56) {
        while (used < 64) block[used++] = 0;
        compress(block);
        used = 0;
    }
    while (used < 56) block[used++] = 0;
    for (int i = 0; i < 8; i++) block[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    compress(block);
    static const char* hexd = "0123456789abcdef";
    std::string out(64, '0');
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 4; j++) {
            uint8_t byte = (uint8_t)(h[i] >> (24 - 8 * j));
            out[i * 8 + j * 2] = hexd[byte >> 4];
            out[i * 8 + j * 2 + 1] = hexd[byte & 0xf];
        }
    return out;
}

static std::string Sha256FileRange(const fs::path& path, uint64_t off, uint64_t size)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    in.seekg((std::streamoff)off);
    std::vector<char> buf((size_t)size);
    in.read(buf.data(), (std::streamsize)size);
    if ((uint64_t)in.gcount() != size) return {};
    return Sha256Hex(buf.data(), buf.size());
}

void MusicMod_OnCpkHostRead(const std::string& hostPath, uint64_t fileOffset, uint32_t length, const uint8_t* data)
{
    if (!s_active || s_proof.empty() || !data || length == 0) return;
    if (!s_patchedCpk.empty())
    {
        if (hostPath.find("sonic2010_0.cpk") == std::string::npos && hostPath != s_patchedCpk)
            return;
    }
    for (auto& e : s_proof)
    {
        if (e.matched) continue;
        const uint64_t end = e.offset + e.size;
        const uint64_t r0 = fileOffset;
        const uint64_t r1 = fileOffset + length;
        if (r1 <= e.offset || r0 >= end) continue;

        if (r0 <= e.offset && r1 > e.offset && e.haveFirst16)
        {
            const uint32_t at = (uint32_t)(e.offset - r0);
            if (at + 16 <= length)
            {
                const bool ok = std::memcmp(data + at, e.first16, 16) == 0;
                if (ok)
                {
                    e.matched = true;
                    std::string liveSha;
                    if (!s_patchedCpk.empty())
                        liveSha = Sha256FileRange(s_patchedCpk, e.offset, e.size);
                    const bool shaOk = !liveSha.empty() && liveSha == e.sha256;
                    LOG_INFO("[MUSIC] PROOF: CPK read supplies replacement AAX '{}' "
                             "@ cpk+{:x} size={} first16=match full_sha256={} (expected {})",
                             e.path, e.offset, (unsigned)e.size,
                             shaOk ? "MATCH" : (liveSha.empty() ? "unverified" : liveSha),
                             e.sha256);
                    if (s_debug && !shaOk && !liveSha.empty())
                        LOG_WARN("[MUSIC][dbg] payload SHA mismatch vs proof TSV");
                }
                else
                {
                    LOG_WARN("[MUSIC] PROOF FAIL: expected AAX '{}' first16 mismatch at cpk+{:x}",
                             e.path, e.offset);
                }
            }
        }
        else if (s_debug)
        {
            LOG_INFO("[MUSIC][dbg] CPK read overlaps '{}' host_off={:x}+{:x} entry={:x}+{:x}",
                     e.path, fileOffset, length, e.offset, e.size);
        }
    }
}

bool MusicMod_SilenceStreams() { return false; }
bool MusicMod_SilenceVoice(bool, bool, uint32_t, int16_t) { return false; }
void MusicMod_MixInto(int16_t*, uint32_t, uint32_t) {}

bool MusicMod_Active() { return s_active; }
bool MusicMod_UsingOriginalCpk() { return s_useOriginal; }
const char* MusicMod_CurrentCue() { return ""; }
const char* MusicMod_CurrentStage() { return ""; }
const char* MusicMod_PatchedCpk() { return s_patchedCpk.c_str(); }
