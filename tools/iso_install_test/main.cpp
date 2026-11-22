// Builds a synthetic Wii disc image and installs it with the Android installer.
//
// runtime/platform/android/iso_install.cpp is the one part of the port that reads the player's
// disc, so it is checked here against an image built to the same on-disc layout the console
// uses: partition table, ticket with a wrapped title key, AES-128-CBC sectors with hash blocks,
// FST, DOL. The test verifies the extracted tree byte for byte and the failure paths (no key,
// wrong key, foreign disc, damaged header). Real game data cannot be used in CI, and the
// release's main.dol SHA-1 would reject a synthetic disc, so the test sets that hash to the
// one it built (IsoInstall_SetExpectedDolHashForTest).
#include "../../runtime/platform/android/iso_install.h"
#include "../../runtime/platform/aes128.h"
#include "../../runtime/platform/sha1.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <cstddef>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_fail = 0;
// The test hook keeps a pointer, so the string has to outlive it.
static std::string g_dolHash;
#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                            \
            g_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)

namespace
{
constexpr uint64_t kSector = 0x8000;
constexpr uint64_t kHashes = 0x400;
constexpr uint64_t kData = kSector - kHashes;
constexpr uint64_t kPartOffset = 0x100000;   // sector aligned, comfortably past the header

void PutBE32(std::vector<uint8_t>& v, size_t at, uint32_t value)
{
    v[at + 0] = (uint8_t)(value >> 24);
    v[at + 1] = (uint8_t)(value >> 16);
    v[at + 2] = (uint8_t)(value >> 8);
    v[at + 3] = (uint8_t)value;
}

std::string Hex(const uint8_t* d, size_t n)
{
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) { s += digits[d[i] >> 4]; s += digits[d[i] & 15]; }
    return s;
}

std::vector<uint8_t> ReadAll(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteAll(const fs::path& p, const std::vector<uint8_t>& v)
{
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out.write((const char*)v.data(), (std::streamsize)v.size());
}

// The decrypted contents of the DATA partition.
struct Disc
{
    std::vector<uint8_t> boot = std::vector<uint8_t>(0x440);
    std::vector<uint8_t> bi2 = std::vector<uint8_t>(0x2000);
    std::vector<uint8_t> apploader;
    std::vector<uint8_t> dol;
    std::vector<uint8_t> fst;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> files;   // relative to files/

    // boot, bi2, apploader, then main.dol and the FST on sector boundaries.
    std::vector<uint8_t> Stream(uint64_t& dolOff, uint64_t& fstOff) const
    {
        std::vector<uint8_t> s = boot;
        s.insert(s.end(), bi2.begin(), bi2.end());
        s.insert(s.end(), apploader.begin(), apploader.end());
        dolOff = (s.size() + kData - 1) / kData * kData;
        s.resize((size_t)dolOff, 0);
        s.insert(s.end(), dol.begin(), dol.end());
        fstOff = (s.size() + kData - 1) / kData * kData;
        s.resize((size_t)fstOff, 0);
        s.insert(s.end(), fst.begin(), fst.end());
        return s;
    }
};

// FST for root + the files/dirs in `paths`, in the order the console uses (a directory comes
// before its children and carries the index one past its last child).
// `base` is the position of the FST inside the partition: the file offsets it stores are
// relative to the partition, not to the FST, so the caller passes where the FST will land.
void BuildFst(Disc& d, uint32_t base = 0)
{
    struct Node { std::string name; bool dir; std::vector<uint8_t> data; std::vector<Node> kids; };
    Node root{ "", true, {}, {} };
    for (auto& [path, data] : d.files)
    {
        Node* cur = &root;
        size_t start = 0;
        while (true)
        {
            const size_t slash = path.find('/', start);
            const std::string part =
                path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            auto it = std::find_if(cur->kids.begin(), cur->kids.end(),
                                   [&](const Node& n) { return n.name == part; });
            if (slash == std::string::npos)
            {
                if (it == cur->kids.end()) cur->kids.push_back({ part, false, data, {} });
                else it->data = data;
                break;
            }
            if (it == cur->kids.end())
            {
                cur->kids.push_back({ part, true, {}, {} });
                it = std::prev(cur->kids.end());
            }
            cur = &*it;
            start = slash + 1;
        }
    }

    // Depth-first numbering: a directory owns the range up to its end index, which is what the
    // console's FST walk relies on.
    struct Flat { Node* node; uint32_t end = 0; };
    std::vector<Flat> flat;
    std::function<void(Node&)> number = [&](Node& n) {
        const size_t index = flat.size();
        flat.push_back({ &n, 0 });
        if (n.dir)
        {
            for (auto& k : n.kids) number(k);
            flat[index].end = (uint32_t)flat.size();
        }
    };
    number(root);

    // The entry table is fixed size; names follow it and the file contents follow them.
    size_t total = flat.size() * 12;
    for (auto& f : flat) total += f.node->name.size() + 1;
    uint32_t dataOffset = base + (uint32_t)((total + 0x1F) & ~0x1Fu);

    std::vector<uint8_t> out(flat.size() * 12, 0);
    PutBE32(out, 0x08, (uint32_t)flat.size());
    std::vector<uint8_t> names, blob;
    for (size_t i = 0; i < flat.size(); i++)
    {
        const uint32_t nameOff = (uint32_t)names.size();
        names.insert(names.end(), flat[i].node->name.begin(), flat[i].node->name.end());
        names.push_back(0);
        if (flat[i].node->dir)
        {
            PutBE32(out, i * 12, (1u << 24) | nameOff);
            PutBE32(out, i * 12 + 8, flat[i].end);
        }
        else
        {
            PutBE32(out, i * 12, nameOff);
            // Offsets are stored divided by four, so every file starts word aligned.
            PutBE32(out, i * 12 + 4, dataOffset >> 2);
            PutBE32(out, i * 12 + 8, (uint32_t)flat[i].node->data.size());
            blob.insert(blob.end(), flat[i].node->data.begin(), flat[i].node->data.end());
            dataOffset += (uint32_t)flat[i].node->data.size();
            const uint32_t padded = (dataOffset + 3) & ~3u;
            blob.resize(blob.size() + (padded - dataOffset), 0);
            dataOffset = padded;
        }
    }
    out.insert(out.end(), names.begin(), names.end());
    out.resize(std::max<size_t>(out.size(), (size_t)(dataOffset - base - blob.size())), 0);
    out.insert(out.end(), blob.begin(), blob.end());
    d.fst = out;
}

struct BuiltImage
{
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> dol;   // copy of main.dol, for the SHA-1 the test allows
};

// Lays out a full disc: header, partition table, ticket, encrypted sectors.
BuiltImage BuildImage(const Disc& d, const uint8_t titleKey[16], const uint8_t commonKey[16],
                      bool encrypt, const char* gameId)
{
    uint64_t dolOff = 0, fstOff = 0;
    Disc head = d;
    // The FST records partition-relative file offsets, so its position has to be known before
    // it is written: lay the stream out once to find it, then rebuild the FST at that base.
    head.Stream(dolOff, fstOff);
    BuildFst(head, (uint32_t)fstOff);
    std::vector<uint8_t> stream = head.Stream(dolOff, fstOff);
    PutBE32(head.boot, 0x420, (uint32_t)(dolOff >> 2));
    PutBE32(head.boot, 0x424, (uint32_t)(fstOff >> 2));
    PutBE32(head.boot, 0x428, (uint32_t)((head.fst.size() + 3) & ~3u) >> 2);
    // boot.bin's own bytes are part of the stream, so lay it out once more with the offsets in.
    stream = head.Stream(dolOff, fstOff);

    const uint64_t sectors = (stream.size() + kData - 1) / kData;
    const uint64_t dataSize = sectors * kSector;
    BuiltImage built;
    built.dol = head.dol;
    built.bytes.assign((size_t)(kPartOffset + 0x8000 + dataSize), 0);
    std::vector<uint8_t>& image = built.bytes;

    memcpy(image.data(), head.boot.data(), 0x400);
    // The Wii magic sits at 0x18; the hash/encryption flags follow the 64-byte title at 0x60.
    image[0x60] = 0;                     // partition hashes present
    image[0x61] = encrypt ? 0 : 1;       // partition encryption

    PutBE32(image, 0x40000, 1);          // one partition group
    PutBE32(image, 0x40004, 0x20);       // its entry table at 0x40000 + 0x80
    PutBE32(image, 0x40080, (uint32_t)(kPartOffset >> 2));
    PutBE32(image, 0x40084, 0);          // type 0 = DATA

    // Ticket: wrap the title key with the common key, IV = title ID.
    Aes128 commonAes;
    commonAes.SetKey(commonKey);
    uint8_t iv[16] = {};
    memcpy(iv, head.boot.data(), 8);   // the title ID starts with the game ID
    std::vector<uint8_t> wrapped(titleKey, titleKey + 16);
    Aes128CbcEncrypt(commonAes, iv, wrapped.data(), 16);
    memcpy(image.data() + kPartOffset + 0x1BF, wrapped.data(), 16);
    memcpy(image.data() + kPartOffset + 0x1DC, iv, 8);
    image[kPartOffset + 0x1F1] = 0;      // retail common key
    memcpy(image.data() + kPartOffset + 0x100, "Root-CA00000001-XS00000003", 26);

    const size_t ph = (size_t)kPartOffset + 0x2A4;
    PutBE32(image, ph + 0x14, (uint32_t)(0x8000 >> 2));            // data offset
    PutBE32(image, ph + 0x18, (uint32_t)(dataSize >> 2));          // data size

    // Sectors: 0x400 hash block + 0x7C00 data, each with the IVs the console uses.
    Aes128 title;
    title.SetKey(titleKey);
    const size_t dataStart = (size_t)kPartOffset + 0x8000;
    for (uint64_t s = 0; s < sectors; s++)
    {
        uint8_t sector[kSector];
        memset(sector, 0, sizeof(sector));
        const uint64_t pos = s * kData;
        const size_t n = (size_t)std::min<uint64_t>(kData, stream.size() - pos);
        memcpy(sector + kHashes, stream.data() + pos, n);
        // The hash block is arbitrary here, but it must be encrypted *first*: on a real disc
        // the data block's IV is the ciphertext at 0x3D0 of the encrypted hash block (that is
        // what Dolphin and nod use), not the plaintext.
        for (size_t i = 0; i < kHashes; i++) sector[i] = (uint8_t)(i * 13 + s * 5);
        if (encrypt)
        {
            const uint8_t zeroIv[16] = {};
            Aes128CbcEncrypt(title, zeroIv, sector, kHashes);
            uint8_t dataIv[16];
            memcpy(dataIv, sector + 0x3D0, 16);
            Aes128CbcEncrypt(title, dataIv, sector + kHashes, kSector - kHashes);
        }
        memcpy(image.data() + dataStart + s * kSector, sector, kSector);
    }
    (void)gameId;
    return built;
}

std::string DolSha1(const std::vector<uint8_t>& dol)
{
    uint8_t sha[20];
    Sha1_Hash(dol.data(), dol.size(), sha);
    return Hex(sha, 20);
}

// The progress counters are atomics, so they live with the caller that wants to inspect them
// rather than inside the result (which would make it non-copyable for no reason).
struct Run
{
    IsoInstallResult result = IsoInstallResult::Ok;
    std::string error;
    std::vector<std::string> status;
};

Run Install(const fs::path& image, const fs::path& dest, const fs::path& keys,
            IsoInstallProgress* progress = nullptr)
{
    IsoInstallOptions options;
    options.image = image;
    options.dest = dest;
    options.keysFile = keys;
    IsoInstallProgress scratch;
    Run run;
    options.status = [&](const std::string& s) { run.status.push_back(s); };
    run.result = IsoInstall_Run(options, progress ? *progress : scratch, run.error);
    return run;
}

Disc MakeDisc(const char* gameId, const char* dolSeed)
{
    Disc d;
    for (int i = 0; i < 6; i++) d.boot[i] = (uint8_t)gameId[i];
    d.boot[0x18] = 0x5D; d.boot[0x19] = 0x1C; d.boot[0x1A] = 0x9E; d.boot[0x1B] = 0xA3;
    for (size_t i = 0; i < d.bi2.size(); i++) d.bi2[i] = (uint8_t)(i ^ 0x5A);
    // apploader: 0x20-byte header with code size, then the code and trailer.
    const uint32_t code = 0x100, trailer = 0x20;
    d.apploader.assign(0x20 + code + trailer, 0xEE);
    PutBE32(d.apploader, 0x14, code);
    PutBE32(d.apploader, 0x18, trailer);
    // main.dol: 0x100-byte header, code from 0x100 to 0x900, data from 0x900 to 0xB00.
    // Everything else in the header stays zero (unused sections), as a real DOL has it.
    d.dol.assign(0xB00, 0);
    for (size_t i = 0x100; i < d.dol.size(); i++) d.dol[i] = (uint8_t)(i * 3 + dolSeed[0]);
    PutBE32(d.dol, 0x00, 0x100);      // text 0 offset
    PutBE32(d.dol, 0x90, 0x800);      // text 0 size
    PutBE32(d.dol, 0x1C, 0x900);      // data 0 offset
    PutBE32(d.dol, 0xAC, 0x200);      // data 0 size
    d.files = {
        { "sonic2010_0.cpk", { 'c', 'p', 'k' } },
        { "movie/ev110.sfd", std::vector<uint8_t>(0x1000, 0x7F) },
        { "sub/dir/deep.bin", { 1, 2, 3, 4, 5 } },
        { "empty-dir-placeholder.txt", { 'x' } },
    };
    BuildFst(d);
    return d;
}

// ---------------------------------------------------------------- ZIP container for the tests
// A ZIP with the entries given, stored (method 0) or deflated (method 8). Only what the installer
// reads is written: local headers, one central directory, the end record, no data descriptors.
// Building the archive here keeps the test independent of a system zip tool, and lets it produce
// the malformed cases (two images, no image, a damaged payload) a player can run into.
struct ZipEntry
{
    std::string name;
    std::vector<uint8_t> data;
    bool deflate = false;
};

std::vector<uint8_t> DeflateStored(const std::vector<uint8_t>& data)
{
    // Stored (uncompressed) DEFLATE blocks: no Huffman coding, so a decoder must handle them. The
    // length is 16-bit, so anything larger has to be split across blocks.
    std::vector<uint8_t> out;
    for (size_t pos = 0; pos < data.size();)
    {
        const size_t n = std::min<size_t>(data.size() - pos, 65535);
        const bool last = pos + n == data.size();
        out.push_back(last ? 0x01 : 0x00);   // BFINAL, BTYPE=00
        out.push_back((uint8_t)(n & 0xFF));
        out.push_back((uint8_t)(n >> 8));
        out.push_back((uint8_t)(~n & 0xFF));
        out.push_back((uint8_t)((~n >> 8) & 0xFF));
        out.insert(out.end(), data.begin() + pos, data.begin() + pos + n);
        pos += n;
    }
    if (data.empty()) out.insert(out.end(), { 0x01, 0, 0, 0xFF, 0xFF });
    return out;
}

uint32_t Crc32Of(const std::vector<uint8_t>& data)
{
    // zlib's CRC-32, as every ZIP stores it; the installer verifies it on the way out.
    static uint32_t table[256];
    static bool built = false;
    if (!built)
    {
        for (uint32_t n = 0; n < 256; n++)
        {
            uint32_t c = n;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        built = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (uint8_t b : data) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void PutLE16(std::vector<uint8_t>& v, uint16_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
void PutLE32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)(x >> 16)); v.push_back((uint8_t)(x >> 24));
}

std::vector<uint8_t> MakeZip(const std::vector<ZipEntry>& entries)
{
    std::vector<uint8_t> out, central;
    for (const ZipEntry& e : entries)
    {
        std::vector<uint8_t> body = e.deflate ? DeflateStored(e.data) : e.data;
        const uint32_t offset = (uint32_t)out.size();
        PutLE32(out, 0x04034B50);
        PutLE16(out, 20);                       // version needed
        PutLE16(out, 0);                        // flags
        PutLE16(out, e.deflate ? 8 : 0);        // method
        PutLE16(out, 0); PutLE16(out, 0);       // time, date
        PutLE32(out, Crc32Of(e.data));
        PutLE32(out, (uint32_t)body.size());
        PutLE32(out, (uint32_t)e.data.size());
        PutLE16(out, (uint16_t)e.name.size());
        PutLE16(out, 0);                        // extra
        out.insert(out.end(), e.name.begin(), e.name.end());
        out.insert(out.end(), body.begin(), body.end());

        PutLE32(central, 0x02014B50);
        PutLE16(central, 20); PutLE16(central, 20);   // made by, version needed
        PutLE16(central, 0);                          // flags
        PutLE16(central, e.deflate ? 8 : 0);
        PutLE16(central, 0); PutLE16(central, 0);
        PutLE32(central, Crc32Of(e.data));
        PutLE32(central, (uint32_t)body.size());
        PutLE32(central, (uint32_t)e.data.size());
        PutLE16(central, (uint16_t)e.name.size());
        PutLE16(central, 0); PutLE16(central, 0);     // extra, comment
        PutLE16(central, 0);                          // disk
        PutLE16(central, 0);                          // internal attrs
        PutLE32(central, 0);                          // external attrs
        PutLE32(central, offset);
        central.insert(central.end(), e.name.begin(), e.name.end());
    }
    const uint32_t cdOffset = (uint32_t)out.size();
    out.insert(out.end(), central.begin(), central.end());
    PutLE32(out, 0x06054B50);
    PutLE16(out, 0); PutLE16(out, 0);
    PutLE16(out, (uint16_t)entries.size());
    PutLE16(out, (uint16_t)entries.size());
    PutLE32(out, (uint32_t)central.size());
    PutLE32(out, cdOffset);
    PutLE16(out, 0);   // comment length
    return out;
}

void CheckTree(const fs::path& dest, const Disc& d)
{
    CHECK(fs::exists(dest / "sys" / "boot.bin"));
    CHECK(fs::file_size(dest / "sys" / "boot.bin") == 0x440);
    CHECK(fs::file_size(dest / "sys" / "bi2.bin") == 0x2000);
    CHECK(fs::file_size(dest / "sys" / "apploader.img") == d.apploader.size());
    CHECK(fs::file_size(dest / "sys" / "main.dol") == d.dol.size());
    CHECK(fs::file_size(dest / "disc" / "unencrypted.bin") == 0x50000);
    CHECK(fs::file_size(dest / "disc" / "header.bin") == 0x100);
    CHECK(fs::file_size(dest / "disc" / "region.bin") == 0x20);
    CHECK(ReadAll(dest / "sys" / "main.dol") == d.dol);
    CHECK(ReadAll(dest / "sys" / "bi2.bin") == d.bi2);
    for (auto& [path, data] : d.files) CHECK(ReadAll(dest / "files" / path) == data);
    // Nothing may be left behind from a previous or interrupted attempt.
    CHECK(!fs::exists(fs::path(dest.string() + ".partial")));
    CHECK(!fs::exists(fs::path(dest.string() + ".old")));
}
}   // namespace

int main()
{
    const fs::path dir = fs::temp_directory_path() / "scr_iso_install_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    // ---- keys.txt parsing: names, comments, 0x, spaces, and index fallback
    const fs::path keys = dir / "keys.txt";
    {
        std::ofstream out(keys);
        out << "# my keys\n";
        out << "common-key-0 = 0xA0A1A2A3A4A5A6A7A8A9AAABACADAEAF\n";
        out << "common-key-1=A0B1B2B3B4B5B6B7B8B9BABBBCBDBEBF  # korean\n";
    }
    uint8_t key[16] = {};
    CHECK(Keys_ReadCommonKey(keys, 0, key) && key[0] == 0xA0 && key[15] == 0xAF);
    CHECK(Keys_ReadCommonKey(keys, 1, key) && key[1] == 0xB1 && key[15] == 0xBF);
    CHECK(Keys_ReadCommonKey(keys, 2, key) && key[0] == 0xA0);   // unknown index → retail
    CHECK(!Keys_ReadCommonKey(dir / "nope.txt", 0, key));
    {
        // Alternate spelling, as printed by most key lists.
        const fs::path alt = dir / "keys2.txt";
        std::ofstream out(alt);
        out << "rvl-key-retail=C0C1C2C3C4C5C6C7C8C9CACBCCCDCECF\n";
        out.flush();
        CHECK(Keys_ReadCommonKey(alt, 0, key) && key[0] == 0xC0);
        // Discovery order: an explicit path wins, then <filesDir>/keys.txt.
        CHECK(Keys_Find(keys.string(), "") == keys);
        CHECK(Keys_Find("", dir.string()) == keys);
        CHECK(Keys_Find((dir / "absent.txt").string(), "").empty());
    }

    uint8_t titleKey[16], commonKey[16];
    for (int i = 0; i < 16; i++) { titleKey[i] = (uint8_t)(0x11 * (i + 1)); commonKey[i] = (uint8_t)(0xA0 + i); }

    // ---- an encrypted retail disc installs and extracts byte for byte
    {
        const Disc disc = MakeDisc("SNCP8P", "A");
        const BuiltImage image = BuildImage(disc, titleKey, commonKey, true, "SNCP8P");
        g_dolHash = DolSha1(disc.dol);
        IsoInstall_SetExpectedDolHashForTest(g_dolHash.c_str());
        const fs::path iso = dir / "encrypted.iso";
        WriteAll(iso, image.bytes);

        bool needsKey = false;
        CHECK(IsoInstall_GameId(iso, &needsKey) == "SNCP8P");
        CHECK(needsKey);

        const fs::path dest = dir / "data-enc";
        IsoInstallProgress progress;
        Run run = Install(iso, dest, keys, &progress);
        if (run.result != IsoInstallResult::Ok)
            fprintf(stderr, "encrypted install failed: %s\n", run.error.c_str());
        CHECK(run.result == IsoInstallResult::Ok);
        CHECK(progress.total > 0);
        CHECK(progress.done == progress.total);
        CheckTree(dest, disc);
        // The boot copy inside the partition kept the game ID and magic.
        const std::vector<uint8_t> boot = ReadAll(dest / "sys" / "boot.bin");
        CHECK(boot.size() == 0x440 && memcmp(boot.data(), "SNCP8P", 6) == 0 && boot[0x18] == 0x5D);
        CHECK(fs::file_size(dest / "ticket.bin") == 0x2A4);
    }

    // ---- a decrypted ("Systems") image needs no key at all
    {
        const Disc disc = MakeDisc("SNCP8P", "B");
        const BuiltImage image = BuildImage(disc, titleKey, commonKey, false, "SNCP8P");
        g_dolHash = DolSha1(disc.dol);
        IsoInstall_SetExpectedDolHashForTest(g_dolHash.c_str());
        const fs::path iso = dir / "decrypted.iso";
        WriteAll(iso, image.bytes);
        bool needsKey = true;
        CHECK(IsoInstall_GameId(iso, &needsKey) == "SNCP8P");
        CHECK(!needsKey);
        const fs::path dest = dir / "data-dec";
        Run run = Install(iso, dest, {});
        if (run.result != IsoInstallResult::Ok)
            fprintf(stderr, "decrypted install failed: %s\n", run.error.c_str());
        CHECK(run.result == IsoInstallResult::Ok);
        CheckTree(dest, disc);
    }

    // ---- encrypted without a key file: a clear error, and nothing written
    {
        const Disc disc = MakeDisc("SNCP8P", "C");
        const BuiltImage image = BuildImage(disc, titleKey, commonKey, true, "SNCP8P");
        g_dolHash = DolSha1(disc.dol);
        IsoInstall_SetExpectedDolHashForTest(g_dolHash.c_str());
        const fs::path iso = dir / "nokey.iso";
        WriteAll(iso, image.bytes);
        const fs::path dest = dir / "data-nokey";
        Run run = Install(iso, dest, {});
        CHECK(run.result == IsoInstallResult::OpenFailed);
        CHECK(run.error.find("keys.txt") != std::string::npos);
        CHECK(!fs::exists(dest));
        CHECK(!fs::exists(fs::path(dest.string() + ".partial")));
    }

    // ---- encrypted with the wrong key: detected, not silently filled with garbage
    {
        const Disc disc = MakeDisc("SNCP8P", "D");
        const BuiltImage image = BuildImage(disc, titleKey, commonKey, true, "SNCP8P");
        g_dolHash = DolSha1(disc.dol);
        IsoInstall_SetExpectedDolHashForTest(g_dolHash.c_str());
        const fs::path iso = dir / "wrongkey.iso";
        WriteAll(iso, image.bytes);
        const fs::path badKeys = dir / "badkeys.txt";
        { std::ofstream out(badKeys); out << "common-key-0 = " << std::string(32, '0') << "\n"; }
        const fs::path dest = dir / "data-wrongkey";
        Run run = Install(iso, dest, badKeys);
        CHECK(run.result == IsoInstallResult::OpenFailed);
        CHECK(!run.error.empty());
        CHECK(!fs::exists(dest));
    }

    // ---- a disc of another region is refused with the release's own message
    {
        const Disc disc = MakeDisc("SNCE8P", "E");
        const BuiltImage image = BuildImage(disc, titleKey, commonKey, false, "SNCE8P");
        const fs::path iso = dir / "usa.iso";
        WriteAll(iso, image.bytes);
        const fs::path dest = dir / "data-usa";
        Run run = Install(iso, dest, {});
        CHECK(run.result == IsoInstallResult::WrongGame);
        CHECK(run.error.find("USA") != std::string::npos);
        CHECK(!fs::exists(dest));
    }

    // ---- a disc with no partition table, and a file that is not a disc at all
    {
        std::vector<uint8_t> junk(0x60000, 0);
        memcpy(junk.data(), "SNCP8P", 6);
        junk[0x18] = 0x5D; junk[0x19] = 0x1C; junk[0x1A] = 0x9E; junk[0x1B] = 0xA3;
        junk[0x60] = 0; junk[0x61] = 1;
        const fs::path iso = dir / "notable.iso";
        WriteAll(iso, junk);
        Run run = Install(iso, dir / "data-notable", keys);
        CHECK(run.result == IsoInstallResult::OpenFailed);
        CHECK(IsoInstall_GameId(dir / "absent.iso", nullptr).empty());
        const fs::path text = dir / "readme.txt";
        { std::ofstream out(text); out << "not a disc\n"; }
        CHECK(IsoInstall_GameId(text, nullptr).empty());
    }

    // ---- a canceled install leaves nothing behind and reports Canceled
    {
        const Disc disc = MakeDisc("SNCP8P", "F");
        const BuiltImage image = BuildImage(disc, titleKey, commonKey, false, "SNCP8P");
        g_dolHash = DolSha1(disc.dol);
        IsoInstall_SetExpectedDolHashForTest(g_dolHash.c_str());
        const fs::path iso = dir / "cancel.iso";
        WriteAll(iso, image.bytes);
        const fs::path dest = dir / "data-cancel";
        IsoInstallOptions options;
        options.image = iso;
        options.dest = dest;
        IsoInstallProgress progress;
        progress.cancel = true;   // already asked to stop: the first file loop breaks out
        std::string error;
        const IsoInstallResult result = IsoInstall_Run(options, progress, error);
        CHECK(result == IsoInstallResult::Canceled);
        CHECK(!fs::exists(dest));
        CHECK(!fs::exists(fs::path(dest.string() + ".partial")));
    }


    // ---- a ZIP around the ISO installs the same way (what a player usually has)
    {
        const Disc disc = MakeDisc("SNCP8P", "F");
        const BuiltImage image = BuildImage(disc, titleKey, commonKey, false, "SNCP8P");
        g_dolHash = DolSha1(disc.dol);
        IsoInstall_SetExpectedDolHashForTest(g_dolHash.c_str());
        for (bool deflate : { false, true })
        {
            const fs::path zip = dir / (deflate ? "disc-deflate.zip" : "disc-stored.zip");
            WriteAll(zip, MakeZip({ { "Sonic Colors (PAL)/disc.iso", image.bytes, deflate } }));
            bool needsKey = true;
            CHECK(IsoInstall_GameId(zip, &needsKey) == "SNCP8P");
            CHECK(!needsKey);
            const fs::path dest = dir / (deflate ? "data-zip-deflate" : "data-zip-stored");
            Run run = Install(zip, dest, {});
            if (run.result != IsoInstallResult::Ok)
                fprintf(stderr, "zip install failed (%s): %s\n", deflate ? "deflate" : "stored", run.error.c_str());
            CHECK(run.result == IsoInstallResult::Ok);
            CheckTree(dest, disc);
        }
    }

    // ---- ZIPs that cannot be used are refused with a player-readable reason, and write nothing
    {
        const Disc disc = MakeDisc("SNCP8P", "G");
        const BuiltImage image = BuildImage(disc, titleKey, commonKey, false, "SNCP8P");
        g_dolHash = DolSha1(disc.dol);
        IsoInstall_SetExpectedDolHashForTest(g_dolHash.c_str());

        // Two images in one archive: the installer would have to guess which one is the disc.
        const fs::path two = dir / "two-isos.zip";
        WriteAll(two, MakeZip({ { "a.iso", image.bytes }, { "b.iso", image.bytes } }));
        Run run = Install(two, dir / "data-zip-two", {});
        CHECK(run.result == IsoInstallResult::OpenFailed);
        CHECK(run.error.find("more than one") != std::string::npos);
        CHECK(!fs::exists(dir / "data-zip-two"));

        // No disc image at all.
        const fs::path empty = dir / "no-iso.zip";
        WriteAll(empty, MakeZip({ { "readme.txt", { 'h', 'i' } } }));
        run = Install(empty, dir / "data-zip-none", {});
        CHECK(run.result == IsoInstallResult::OpenFailed);
        CHECK(run.error.find("does not contain") != std::string::npos);

        // A damaged stream: the headers claim the right sizes, the stream still decodes to that
        // length, but its contents no longer match the stored CRC.
        std::vector<uint8_t> broken = DeflateStored(image.bytes);
        for (size_t i = 64; i < std::min<size_t>(broken.size(), 5000); i++) broken[i] ^= 0xFF;
        std::vector<uint8_t> zipBytes;
        PutLE32(zipBytes, 0x04034B50);
        PutLE16(zipBytes, 20); PutLE16(zipBytes, 0); PutLE16(zipBytes, 8);
        PutLE16(zipBytes, 0); PutLE16(zipBytes, 0);
        PutLE32(zipBytes, Crc32Of(image.bytes));   // the CRC of what the archive claims to hold
        PutLE32(zipBytes, (uint32_t)broken.size());
        PutLE32(zipBytes, (uint32_t)image.bytes.size());
        PutLE16(zipBytes, 10); PutLE16(zipBytes, 0);   // "broken.iso"
        zipBytes.insert(zipBytes.end(), { 'b', 'r', 'o', 'k', 'e', 'n', '.', 'i', 's', 'o' });
        zipBytes.insert(zipBytes.end(), broken.begin(), broken.end());
        const uint32_t cdOffset = (uint32_t)zipBytes.size();
        PutLE32(zipBytes, 0x02014B50);
        PutLE16(zipBytes, 20); PutLE16(zipBytes, 20); PutLE16(zipBytes, 0); PutLE16(zipBytes, 8);
        PutLE16(zipBytes, 0); PutLE16(zipBytes, 0); PutLE32(zipBytes, Crc32Of(image.bytes));
        PutLE32(zipBytes, (uint32_t)broken.size());
        PutLE32(zipBytes, (uint32_t)image.bytes.size());
        PutLE16(zipBytes, 10); PutLE16(zipBytes, 0); PutLE16(zipBytes, 0);   // name, extra, comment
        PutLE16(zipBytes, 0); PutLE16(zipBytes, 0); PutLE32(zipBytes, 0); PutLE32(zipBytes, 0);
        zipBytes.insert(zipBytes.end(), { 'b', 'r', 'o', 'k', 'e', 'n', '.', 'i', 's', 'o' });
        PutLE32(zipBytes, 0x06054B50);
        PutLE16(zipBytes, 0); PutLE16(zipBytes, 0); PutLE16(zipBytes, 1); PutLE16(zipBytes, 1);
        PutLE32(zipBytes, (uint32_t)(zipBytes.size() - cdOffset));
        PutLE32(zipBytes, cdOffset);
        PutLE16(zipBytes, 0);
        const fs::path bad = dir / "broken.zip";
        WriteAll(bad, zipBytes);
        run = Install(bad, dir / "data-zip-broken", {});
        CHECK(run.result == IsoInstallResult::OpenFailed);
        CHECK(run.error.find("could not be unpacked") != std::string::npos);
        CHECK(!fs::exists(dir / "data-zip-broken"));
        CHECK(!fs::exists(fs::path((dir / "data-zip-broken").string() + ".partial")));
    }

    if (g_fail)
    {
        fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("iso_install_test: all checks passed\n");
    fs::remove_all(dir, ec);
    return 0;
}
