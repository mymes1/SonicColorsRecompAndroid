// Reads a Wii ISO and writes the runtime's game-data layout (see iso_install.h).
//
// Only plain ISO is read: RVZ/WIA/WBFS/CISO/GCZ need whole decoders for formats a player can
// convert once on a PC (Dolphin's "Convert to ISO", or `nodtool convert`). The ISO path is the
// common one and keeps this file to the parts that matter: the partition table, the ticket,
// AES-128-CBC sector decryption, and the FST walk.
//
// Disc layout (offsets from the start of the image):
//   0x00000  disc header      game ID at 0, Wii magic 0x5D1C9EA3 at 0x18
//   0x60     no_partition_hashes (1 = sectors have no 0x400 hash block)
//   0x61     no_partition_encryption (1 = sector data is plain)
//   0x40000  partition table: 4 groups of {count, entryOffset<<2}
//   0x40020  group 0's entries: {offset<<2, type}   type 0 = DATA
//   at a partition's offset: 0x2A4 ticket, then 0x2C0 header
//   header+0x2A4 tmd size, +0x2A8 tmd offset<<2, +0x2AC cert size, +0x2B0 cert offset<<2,
//   +0x2B4 h3 offset<<2, +0x2B8 data offset<<2, +0x2BC data size<<2
// The DATA partition's decrypted stream is boot.bin (0x440), bi2.bin (0x2000),
// apploader.img, then the FST and main.dol at the offsets in boot.bin's boot block.
#include "iso_install.h"
#include <array>
#include "deflate.h"   // Inflate_Raw: ZIP entries

#include "../../install/releases.h"
#include "../../platform/sha1.h"
#include "../aes128.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace
{
constexpr uint64_t kSectorSize = 0x8000;
constexpr uint64_t kHashesSize = 0x400;
constexpr uint64_t kSectorDataSize = kSectorSize - kHashesSize;   // 0x7C00
constexpr uint64_t kUnencryptedSize = 0x50000;
constexpr uint64_t kBootSize = 0x440;
constexpr uint64_t kBi2Size = 0x2000;
constexpr uint64_t kDiskMargin = 256ull << 20;

#if defined(SCR_ISO_INSTALL_TEST)
const char* g_expectedDolHashOverride = nullptr;
#endif

uint32_t BE32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
std::string Lower(std::string s)
{
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string Trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
bool ParseHex16(const std::string& hex, uint8_t out[16])
{
    if (hex.size() != 32) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (int i = 0; i < 16; i++)
    {
        const int hi = nib(hex[2 * i]), lo = nib(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}
std::string Hex(const uint8_t* d, size_t n)
{
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) { s += digits[d[i] >> 4]; s += digits[d[i] & 15]; }
    return s;
}

namespace
{
// A disc image inside a ZIP is unpacked into RAM, so the ceiling has to cover a real Wii image
// (single-layer 4.7 GB, dual-layer 8.5 GB) without letting a corrupt entry size become a wild
// allocation. Entry sizes in this reader are 32-bit (ZIP64 is rejected above), so the ceiling is
// capped at what that can express.
constexpr uint32_t kMaxZipEntry = 0xFFFFFFFFu;
}   // namespace

// ------------------------------------------------------------------ ZIP entry extraction
// Players hand over whatever the rip site gave them, which is usually a ZIP around the ISO. The
// player is copied into app storage before this point, so a byte range of the archive can be
// inflated on demand: the entry becomes a random-access image of its own.
class ZipImage
{
public:
    ~ZipImage() { free(data_); }

    // Whether the file even ends with a ZIP end-of-central-directory record. Only then is it
    // treated as an archive; everything else goes down the plain-ISO path, which reports its own
    // errors.
    static bool LooksLikeZip(const fs::path& path)
    {
        FILE* f = fopen(path.string().c_str(), "rb");
        if (!f) return false;
        fseeko(f, 0, SEEK_END);
        const uint64_t size = (uint64_t)ftello(f);
        if (size < 22) { fclose(f); return false; }
        uint8_t sig[4] = {};
        fseeko(f, (off_t)(size - 22), SEEK_SET);
        const bool ok = fread(sig, 1, 4, f) == 4;
        fclose(f);
        return ok && sig[0] == 'P' && sig[1] == 'K' && sig[2] == 5 && sig[3] == 6;
    }

    // Opens the archive and unpacks its one Wii image, or explains in player terms why not.
    bool Open(const fs::path& path, std::string& error)
    {
        FILE* f = fopen(path.string().c_str(), "rb");
        if (!f) return false;
        fseeko(f, 0, SEEK_END);
        const uint64_t size = (uint64_t)ftello(f);
        fclose(f);

        // The end-of-central-directory record sits in the last 64 KiB that may also hold a
        // comment, so it has to be searched for rather than read at a fixed offset.
        const uint64_t tailSize = std::min<uint64_t>(size, 0x10000 + 22);
        std::vector<uint8_t> tail((size_t)tailSize);
        if (!ReadFileRange(path, size - tailSize, tail.data(), tail.size())) return false;
        for (size_t i = tailSize >= 22 ? tailSize - 22 : 0;; i--)
        {
            if (tail[i] == 'P' && tail[i + 1] == 'K' && tail[i + 2] == 5 && tail[i + 3] == 6)
            {
                const uint8_t* eocd = tail.data() + i;
                const uint32_t entries = Read16(eocd + 10);
                const uint64_t cdOffset = Read32(eocd + 16);
                const uint32_t cdSize = Read32(eocd + 12);
                if (entries == 0xFFFF || cdOffset == 0xFFFFFFFF) return NotIso(error);
                return OpenEntry(path, cdOffset, cdSize, entries, error);
            }
            if (i == 0) break;
        }
        return NotIso(error);
    }

    uint64_t Size() const { return data_ ? size_ : 0; }
    bool Read(uint64_t offset, void* out, size_t len) const
    {
        if (!data_ || offset + len > size_) return false;
        memcpy(out, data_ + offset, len);
        return true;
    }
    bool IsZip() const { return data_ != nullptr; }
    const std::string& ExtractedName() const { return name_; }

private:
    static bool NotIso(std::string& error)
    {
        error = "That ZIP does not contain a Wii disc image; put the ISO in the ZIP (or pick the ISO).";
        return false;
    }
    static uint32_t Crc32(const uint8_t* data, size_t size)
    {
        static const auto table = [] {
            std::array<uint32_t, 256> t{};
            for (uint32_t n = 0; n < 256; n++)
            {
                uint32_t c = n;
                for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[n] = c;
            }
            return t;
        }();
        uint32_t c = 0xFFFFFFFFu;
        for (size_t i = 0; i < size; i++) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }
    static uint16_t Read16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
    static uint32_t Read32(const uint8_t* p) { return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
    static bool ReadFileRange(const fs::path& path, uint64_t offset, void* out, size_t len)
    {
        FILE* f = fopen(path.string().c_str(), "rb");
        if (!f) return false;
        const bool ok = fseeko(f, (off_t)offset, SEEK_SET) == 0 && fread(out, 1, len, f) == len;
        fclose(f);
        return ok;
    }

    bool OpenEntry(const fs::path& path, uint64_t cdOffset, uint32_t cdSize, uint32_t entries, std::string& error)
    {
        std::vector<uint8_t> cd(cdSize);
        if (!ReadFileRange(path, cdOffset, cd.data(), cd.size())) return NotIso(error);
        const uint8_t* p = cd.data();
        const uint8_t* end = cd.data() + cd.size();
        int found = 0;
        for (uint32_t n = 0; n < entries; n++)
        {
            if (p + 46 > end || Read32(p) != 0x02014B50) return NotIso(error);
            const uint16_t method = Read16(p + 10);
            const uint32_t csize = Read32(p + 20), usize = Read32(p + 24);
            const uint16_t nameLen = Read16(p + 28), extraLen = Read16(p + 30), commentLen = Read16(p + 32);
            const uint32_t localOffset = Read32(p + 42);
            const uint16_t flags = Read16(p + 8);
            if (p + 46 + nameLen + extraLen + commentLen > end) return NotIso(error);
            std::string name((const char*)p + 46, nameLen);
            if (!name.empty() && name.back() != '/' && LooksLikeDisc(name))
            {
                found++;
                if (found > 1)
                {
                    error = "That ZIP contains more than one disc image; keep only your PAL SNCP8P ISO in it.";
                    return false;
                }
                if (!Extract(path, method, Read32(p + 16), localOffset, csize, usize, flags, name, error)) return false;
            }
            p += 46 + nameLen + extraLen + commentLen;
        }
        if (found != 1)
        {
            error = "That ZIP does not contain a Wii disc image; put the ISO in the ZIP (or pick the ISO).";
            return false;
        }
        return true;
    }

    static bool LooksLikeDisc(const std::string& name)
    {
        std::string lower;
        for (char c : name) lower += (char)std::tolower((unsigned char)c);
        for (const char* ext : { ".iso", ".wbfs", ".wia", ".gcz", ".rvz" })
            if (lower.size() > strlen(ext) && lower.compare(lower.size() - strlen(ext), strlen(ext), ext) == 0) return true;
        return false;
    }

    bool Extract(const fs::path& path, uint16_t method, uint32_t crc, uint32_t localOffset, uint32_t csize,
                 uint32_t usize, uint16_t flags, const std::string& name, std::string& error)
    {
        if (flags & 1)
        {
            error = "That ZIP is encrypted; re-create it without a password and try again.";
            return false;
        }
        if (method != 0 && method != 8)
        {
            error = "That ZIP uses an unsupported compression method (stored and deflate work).";
            return false;
        }
        if (usize == 0 || usize > kMaxZipEntry)
        {
            error = "That ZIP's disc image is a suspicious size; extract the ISO and pick it instead.";
            return false;
        }
        // The local header repeats the name and extra-field lengths, which differ from the
        // central directory's, so the data offset has to come from there.
        uint8_t local[30];
        if (!ReadFileRange(path, localOffset, local, sizeof(local)) || Read32(local) != 0x04034B50)
        {
            error = "That ZIP could not be unpacked (it may be damaged); extract the ISO and pick it instead.";
            return false;
        }
        const uint64_t dataOffset = (uint64_t)localOffset + 30 + Read16(local + 26) + Read16(local + 28);

        std::vector<uint8_t> deflated(csize);
        if (csize && !ReadFileRange(path, dataOffset, deflated.data(), deflated.size()))
        {
            error = "That ZIP could not be unpacked (it may be damaged); extract the ISO and pick it instead.";
            return false;
        }

        data_ = (uint8_t*)malloc(usize);
        if (!data_)
        {
            error = "Not enough memory to unpack the ZIP. Extract the ISO and pick it instead.";
            return false;
        }
        size_ = usize;
        name_ = name;
        if (method == 0)
        {
            if (csize != usize || Crc32(deflated.data(), usize) != crc)
            {
                error = "That ZIP could not be unpacked (it may be damaged); extract the ISO and pick it instead.";
                return false;
            }
            memcpy(data_, deflated.data(), usize);
            return true;
        }
        std::vector<uint8_t> inflated;
        inflated.reserve(usize);
        // A stream damaged inside its payload still inflates, to the right length, as garbage; only
        // the CRC catches that, and catching it here turns a confusing "not a disc" into "damaged".
        if (!Inflate_Raw(deflated.data(), deflated.size(), inflated) || inflated.size() != usize ||
            Crc32(inflated.data(), inflated.size()) != crc)
        {
            error = "That ZIP could not be unpacked (it may be damaged); extract the ISO and pick it instead.";
            return false;
        }
        memcpy(data_, inflated.data(), usize);
        return true;
    }

    uint8_t* data_ = nullptr;
    uint64_t size_ = 0;
    std::string name_;
};

// ------------------------------------------------------------------ the image
// A disc image as the rest of the installer sees it: random access, but it may be a plain file or
// a ZIP entry unpacked in memory.
class Image
{
public:
    // A ZIP is detected by its end record rather than by extension: downloads get renamed, and a
    // plain ISO whose name ends in .zip must still work. Only a ZIP that fails to yield an image
    // sets `error`; a plain file leaves it alone so the caller can report the usual message.
    bool Open(const fs::path& path, std::string& error)
    {
        if (ZipImage::LooksLikeZip(path))
        {
            if (!zip_.Open(path, error)) return false;
            size_ = zip_.Size();
            return size_ >= kUnencryptedSize;
        }
        f_ = fopen(path.string().c_str(), "rb");
        if (!f_) return false;
        fseeko(f_, 0, SEEK_END);
        size_ = (uint64_t)ftello(f_);
        return size_ >= kUnencryptedSize;
    }
    ~Image()
    {
        if (f_) fclose(f_);
    }
    Image() = default;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    uint64_t Size() const { return size_; }
    bool Read(uint64_t offset, void* out, size_t len)
    {
        if (offset + len > size_) return false;
        if (zip_.IsZip()) return zip_.Read(offset, out, len);
        if (fseeko(f_, (off_t)offset, SEEK_SET) != 0) return false;
        return fread(out, 1, len, f_) == len;
    }
    bool IsZip() const { return zip_.IsZip(); }

private:
    FILE* f_ = nullptr;
    uint64_t size_ = 0;
    ZipImage zip_;
};

// Decrypted view of a Wii partition: presents the partition's data as if the hashes were not
// there and the sectors were plain, which is what everything above this class expects.
class Partition
{
public:
    // One 32 KiB sector of raw partition data, decrypted on demand and reused for the next
    // read in the same sector (the FST walk reads small blobs).
    bool Read(Image& image, uint64_t pos, void* out, size_t len)
    {
        uint8_t* dst = (uint8_t*)out;
        while (len)
        {
            const uint64_t sectorIndex = pos / size_;
            const uint64_t offsetInSector = pos % size_;
            if (!LoadSector(image, sectorIndex)) return false;
            const uint64_t take = std::min<uint64_t>(len, size_ - offsetInSector);
            memcpy(dst, base_ + offsetInSector, (size_t)take);
            dst += take;
            pos += take;
            len -= (size_t)take;
        }
        return true;
    }

    void Setup(uint64_t dataStart, uint64_t dataSize, bool hashed, bool encrypted, const Aes128& aes,
               bool haveKey)
    {
        dataStart_ = dataStart;
        dataSize_ = dataSize;
        hashed_ = hashed;
        encrypted_ = encrypted;
        aes_ = aes;
        haveKey_ = haveKey;
        size_ = hashed ? kSectorDataSize : kSectorSize;
        loadedSector_ = UINT64_MAX;
    }
    uint64_t Size() const { return dataSize_; }

private:
    bool LoadSector(Image& image, uint64_t sectorIndex)
    {
        if (sectorIndex == loadedSector_) return true;
        const uint64_t rawBytes = hashed_ ? kSectorSize : kSectorSize;
        if (!image.Read(dataStart_ + sectorIndex * rawBytes, buffer_, (size_t)rawBytes)) return false;
        if (encrypted_)
        {
            if (!haveKey_) return false;
            // Each sector carries its own IVs: the hash block decrypts with a zero IV, and
            // the data block's IV is stored at 0x3D0 of the encrypted hash block.
            if (hashed_)
            {
                uint8_t iv[16];
                memcpy(iv, buffer_ + 0x3D0, 16);
                Aes128CbcDecrypt(aes_, zeroIv_, buffer_, kHashesSize);
                Aes128CbcDecrypt(aes_, iv, buffer_ + kHashesSize, kSectorSize - kHashesSize);
            }
            else
            {
                uint8_t iv[16];
                memcpy(iv, buffer_ + 0x3D0, 16);
                Aes128CbcDecrypt(aes_, iv, buffer_, kSectorSize);
            }
        }
        // For a hashed partition the decrypted data starts after the (now useless) hash block;
        // reads are relative to it.
        base_ = hashed_ ? buffer_ + kHashesSize : buffer_;
        loadedSector_ = sectorIndex;
        return true;
    }

    uint64_t dataStart_ = 0, dataSize_ = 0, size_ = kSectorSize;
    bool hashed_ = true, encrypted_ = true, haveKey_ = false;
    Aes128 aes_{};
    const uint8_t zeroIv_[16] = {};
    uint8_t buffer_[kSectorSize] = {};
    const uint8_t* base_ = buffer_;
    uint64_t loadedSector_ = UINT64_MAX;
};

bool WriteAll(const fs::path& path, const void* data, size_t len)
{
    FILE* f = fopen(path.string().c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(data, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}

bool EnoughSpace(const fs::path& dest, uint64_t need, std::string& error)
{
    std::error_code ec;
    fs::path probe = dest;
    while (!probe.empty() && !fs::exists(probe, ec))
    {
        if (!probe.has_parent_path() || probe.parent_path() == probe) break;
        probe = probe.parent_path();
    }
    const fs::space_info si = fs::space(probe.empty() ? fs::path(".") : probe, ec);
    if (ec) return true;   // unknown: let the writes fail with a real error
    if (si.available >= need + kDiskMargin) return true;
    error = "Not enough free space: the extracted game needs about " +
            std::to_string((need + kDiskMargin) >> 20) + " MB, and " + std::to_string(si.available >> 20) +
            " MB are free.";
    return false;
}

struct FstEntry
{
    std::string name;
    bool dir = false;
    uint32_t offset = 0;    // << 2, relative to the partition
    uint32_t size = 0;
    uint32_t endIndex = 0;  // directories only
};

// Walks the FST: entry 0 is the root, a directory owns the range up to its end index.
bool ParseFst(const std::vector<uint8_t>& fst, std::vector<std::string>& paths, std::vector<FstEntry>& out)
{
    if (fst.size() < 12) return false;
    const uint32_t count = BE32(fst.data() + 8);
    if (!count || count > (1u << 20)) return false;
    const size_t strings = (size_t)count * 12;
    if (fst.size() < strings) return false;
    std::vector<std::pair<uint32_t, std::string>> stack{ { count, "" } };
    for (uint32_t i = 0; i < count; i++)
    {
        while (stack.size() > 1 && i >= stack.back().first) stack.pop_back();
        const uint8_t* e = fst.data() + (size_t)i * 12;
        const uint32_t typeName = BE32(e);
        const uint32_t nameOff = typeName & 0xFFFFFF;
        if (strings + nameOff >= fst.size()) return false;
        const char* name = (const char*)fst.data() + strings + nameOff;
        const size_t nameLen = strnlen(name, fst.size() - strings - nameOff);
        if (strings + nameOff + nameLen >= fst.size()) return false;
        FstEntry entry;
        entry.name = std::string(name, nameLen);
        entry.dir = (typeName >> 24) != 0;
        const std::string prefix = stack.back().second;
        if (i != 0) entry.name = prefix + entry.name;
        if (entry.dir)
        {
            entry.endIndex = BE32(e + 8);
            if (i != 0) stack.push_back({ entry.endIndex, entry.name + "/" });
        }
        else
        {
            entry.offset = BE32(e + 4);   // << 2, applied at the 64-bit read below
            entry.size = BE32(e + 8);
            paths.push_back(entry.name);
            out.push_back(entry);
        }
    }
    return true;
}

struct Headers
{
    uint8_t boot[kBootSize]{};
    uint8_t bi2[kBi2Size]{};
    std::vector<uint8_t> apploader;
    std::vector<uint8_t> dol;
    std::vector<uint8_t> fst;
};
}   // namespace

// ================================================================== keys.txt
std::filesystem::path Keys_Find(const std::string& preferred, const std::string& filesDir)
{
    std::error_code ec;
    std::vector<fs::path> candidates;
    if (!preferred.empty()) candidates.emplace_back(preferred);
    if (!filesDir.empty())
    {
        candidates.emplace_back(fs::path(filesDir) / "keys.txt");
        candidates.emplace_back(fs::path(filesDir).parent_path() / "keys.txt");
        candidates.emplace_back(fs::path(filesDir).parent_path().parent_path() / "keys.txt");
    }
    for (const auto& c : candidates)
        if (!c.empty() && fs::is_regular_file(c, ec)) return c;
    return {};
}

bool Keys_ReadCommonKey(const std::filesystem::path& keysFile, int index, uint8_t key[16])
{
    if (keysFile.empty()) return false;
    std::ifstream in(keysFile);
    if (!in) return false;
    const std::vector<std::string> names[2] = {
        { "common-key-0", "common0", "common-key", "rvl-key-retail", "retail-common-key" },
        { "common-key-1", "common1", "rvl-key-korean", "korean-common-key" },
    };
    const int which = index == 1 ? 1 : 0;
    std::string line;
    while (std::getline(in, line))
    {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string name = Lower(Trim(line.substr(0, eq)));
        if (std::find(names[which].begin(), names[which].end(), name) == names[which].end()) continue;
        std::string value = Trim(line.substr(eq + 1));
        // Tolerate "0x", quotes and spaces/dashes inside the hex.
        if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) value.erase(0, 2);
        // Strip separators, then require *exactly* 32 digits: silently taking the first 32 of
        // a longer string would accept a typo as a valid key.
        std::string hex;
        for (char c : value)
            if (std::isxdigit((unsigned char)c)) hex += c;
        if (ParseHex16(hex, key)) return true;
    }
    return false;
}

// ================================================================== install
std::string IsoInstall_GameId(const std::filesystem::path& image, bool* needsKey)
{
    Image img;
    std::string ignored;   // a ZIP that cannot be unpacked is simply not a readable disc here
    if (!img.Open(image, ignored)) return {};
    uint8_t header[0x20];
    if (!img.Read(0, header, sizeof(header))) return {};
    if (needsKey)
    {
        uint8_t flags[2] = {};
        // The flags live at 0x60 (hashes) and 0x61 (encryption), after the 64-byte title.
        if (img.Read(0x60, flags, sizeof(flags))) *needsKey = flags[1] == 0;
    }
    return std::string((const char*)header, 6);
}

IsoInstallResult IsoInstall_Run(const IsoInstallOptions& options, IsoInstallProgress& progress,
                                std::string& error)
{
    auto status = [&](const std::string& text) { if (options.status) options.status(text); };
    auto report = [&](int percent, const std::string& text) { if (options.progress) options.progress(percent, text); };
    auto fail = [&](IsoInstallResult r) { return r; };

    Image image;
    if (!image.Open(options.image, error))
    {
        // Image::Open sets `error` when it understood the input but could not use it (a ZIP without
        // exactly one image, or one it could not unpack); otherwise the input is not a disc image.
        if (error.empty()) error = "The disc image could not be opened. Use your own PAL copy as an ISO or ZIP.";
        return fail(IsoInstallResult::OpenFailed);
    }
    status("Checking the disc header…");

    uint8_t* raw = (uint8_t*)malloc((size_t)kUnencryptedSize);
    if (!raw || !image.Read(0, raw, (size_t)kUnencryptedSize))
    {
        free(raw);
        error = "The disc image is too small to be a Wii disc.";
        return fail(IsoInstallResult::OpenFailed);
    }
    const std::string gameId((const char*)raw, 6);
    const GameRelease* release = Release_Find(gameId);
    if (!release)
    {
        free(raw);
        error = Release_UnsupportedMessage(gameId);
        return fail(IsoInstallResult::WrongGame);
    }
    const bool hasMagic = BE32(raw + 0x18) == 0x5D1C9EA3;
    if (!hasMagic)
    {
        free(raw);
        error = "This is not a Wii disc image (the disc header has no Wii magic).";
        return fail(IsoInstallResult::OpenFailed);
    }
    const bool hashed = raw[0x60] == 0;        // no_partition_hashes
    const bool encrypted = raw[0x61] == 0;     // no_partition_encryption

    // Partition table → the DATA partition (type 0).
    uint64_t partOffset = 0;
    {
        const uint8_t* table = raw + 0x40000;
        for (int g = 0; g < 4 && !partOffset; g++)
        {
            const uint32_t count = BE32(table + g * 8);
            const uint32_t entryOff = BE32(table + g * 8 + 4) << 2;
            if (!count || entryOff + count * 8 > kUnencryptedSize) continue;
            for (uint32_t p = 0; p < count; p++)
            {
                const uint8_t* e = table + entryOff + p * 8;
                if (BE32(e + 4) != 0) continue;   // type 0 = DATA
                partOffset = (uint64_t)BE32(e) << 2;
                break;
            }
        }
    }
    if (!partOffset)
    {
        free(raw);
        error = "The disc has no data partition; is it a Wii disc?";
        return fail(IsoInstallResult::OpenFailed);
    }

    // Partition header: 0x2A4 ticket + 0x2C0 of offsets/sizes.
    std::vector<uint8_t> partHead(0x2A4 + 0x2C0);
    if (!image.Read(partOffset, partHead.data(), partHead.size()))
    {
        free(raw);
        error = "The data partition's header could not be read.";
        return fail(IsoInstallResult::OpenFailed);
    }
    const uint8_t* ticket = partHead.data();
    const uint8_t* ph = partHead.data() + 0x2A4;
    const uint32_t tmdSize = BE32(ph + 0x00), tmdOff = BE32(ph + 0x04) << 2;
    const uint32_t certSize = BE32(ph + 0x08), certOff = BE32(ph + 0x0C) << 2;
    const uint32_t h3Off = BE32(ph + 0x10) << 2;
    const uint32_t dataOff = BE32(ph + 0x14) << 2;
    const uint32_t dataSize = BE32(ph + 0x18) << 2;
    if (!dataOff || !dataSize)
    {
        free(raw);
        error = "The data partition is empty or malformed.";
        return fail(IsoInstallResult::OpenFailed);
    }

    // The title key is the ticket's AES key, encrypted with a common key: decrypt it with the
    // player's keys.txt. A decrypted disc needs neither.
    Aes128 aes;
    bool haveKey = false;
    if (encrypted)
    {
        uint8_t wrapped[16];
        memcpy(wrapped, ticket + 0x1BF, 16);
        const int keyIndex = (ticket[0x1F1] & 1);   // common key index; 0 retail, 1 Korean
        // Some tools copy the retail index while the disc is Korean; try both when the first
        // one is missing rather than failing with "wrong key".
        for (int attempt = 0; attempt < 2 && !haveKey; attempt++)
        {
            uint8_t common[16];
            const int idx = attempt == 0 ? keyIndex : 1 - keyIndex;
            if (!Keys_ReadCommonKey(options.keysFile, idx, common)) continue;
            Aes128 commonAes;
            if (!commonAes.SetKey(common)) continue;
            uint8_t iv[16] = {};
            uint8_t titleKey[16];
            memcpy(titleKey, wrapped, 16);
            memcpy(iv, ticket + 0x1DC, 8);   // title ID is the first half of the IV
            Aes128CbcDecrypt(commonAes, iv, titleKey, 16);
            if (!aes.SetKey(titleKey)) continue;
            haveKey = true;
        }
        if (!haveKey)
        {
            free(raw);
            error = options.keysFile.empty()
                        ? "This ISO is encrypted, so the installer needs your keys.txt (the Wii "
                          "common key). Put keys.txt in the app's files folder, or install from a "
                          "decrypted ISO."
                        : "The key in " + options.keysFile.string() +
                              " does not decrypt this disc. Check the keys.txt line for your disc's "
                              "region (common-key-0 for PAL/NTSC).";
            return fail(IsoInstallResult::OpenFailed);
        }
    }

    // Decrypted stream of the partition. boot.bin, bi2.bin and the apploader are at fixed
    // positions; the FST and main.dol offsets come from boot.bin's boot block.
    Partition part;
    part.Setup(partOffset + dataOff, dataSize, hashed, encrypted, aes, haveKey);

    Headers head;
    if (!part.Read(image, 0, head.boot, kBootSize) || !part.Read(image, kBootSize, head.bi2, kBi2Size))
    {
        free(raw);
        error = "The partition's boot data could not be decrypted.";
        return fail(IsoInstallResult::OpenFailed);
    }
    // boot.bin's copy of the disc header must match the disc itself: a wrong key usually
    // shows up here as garbage rather than as a read error.
    if (memcmp(head.boot, raw, 6) != 0 || BE32(head.boot + 0x18) != 0x5D1C9EA3)
    {
        free(raw);
        error = "The decrypted boot data does not match the disc header (wrong key?).";
        return fail(IsoInstallResult::OpenFailed);
    }
    {
        uint8_t aplHead[0x20];
        if (!part.Read(image, kBootSize + kBi2Size, aplHead, sizeof(aplHead)))
        {
            free(raw);
            error = "The apploader could not be read.";
            return fail(IsoInstallResult::OpenFailed);
        }
        const uint32_t code = BE32(aplHead + 0x14), trailer = BE32(aplHead + 0x18);
        const uint64_t aplSize = sizeof(aplHead) + code + trailer;
        if (aplSize > (4u << 20))
        {
            free(raw);
            error = "The apploader size is invalid (wrong key?).";
            return fail(IsoInstallResult::OpenFailed);
        }
        head.apploader.resize((size_t)aplSize);
        if (!part.Read(image, kBootSize + kBi2Size, head.apploader.data(), head.apploader.size()))
        {
            free(raw);
            error = "The apploader could not be read.";
            return fail(IsoInstallResult::OpenFailed);
        }
    }

    const uint64_t dolOff = (uint64_t)BE32(head.boot + 0x420) << 2;
    const uint64_t fstOff = (uint64_t)BE32(head.boot + 0x424) << 2;
    const uint64_t fstSize = (uint64_t)BE32(head.boot + 0x428) << 2;
    if (!fstOff || !fstSize || fstSize > (32u << 20) || !dolOff)
    {
        free(raw);
        error = "The file system table's position in boot.bin is invalid.";
        return fail(IsoInstallResult::OpenFailed);
    }
    head.fst.resize((size_t)fstSize);
    if (!part.Read(image, fstOff, head.fst.data(), head.fst.size()))
    {
        free(raw);
        error = "The file system table could not be read.";
        return fail(IsoInstallResult::OpenFailed);
    }
    {
        // main.dol: its size is the end of its last section, as in the DOL header.
        uint8_t dolHead[0x100];
        if (!part.Read(image, dolOff, dolHead, sizeof(dolHead)))
        {
            free(raw);
            error = "main.dol could not be read.";
            return fail(IsoInstallResult::OpenFailed);
        }
        // Section offset[i] + size[i], all 32-bit fields read as unsigned 64-bit.
        uint64_t dolSize = 0x100;
        for (int i = 0; i < 18; i++)
            dolSize = std::max(dolSize, (uint64_t)BE32(dolHead + i * 4) + (uint64_t)BE32(dolHead + 0x90 + i * 4));
        if (dolSize > (32u << 20))
        {
            free(raw);
            error = "main.dol looks damaged.";
            return fail(IsoInstallResult::OpenFailed);
        }
        head.dol.resize((size_t)dolSize);
        if (!part.Read(image, dolOff, head.dol.data(), head.dol.size()))
        {
            free(raw);
            error = "main.dol could not be read.";
            return fail(IsoInstallResult::OpenFailed);
        }
    }

    // The recompiled code is generated from exactly this main.dol; refuse early otherwise.
    {
        uint8_t sha[20];
        if (!Sha1_Hash(head.dol.data(), head.dol.size(), sha))
        {
            free(raw);
            error = "Cannot hash main.dol.";
            return fail(IsoInstallResult::OpenFailed);
        }
        const KnownFile* expected = nullptr;
        for (size_t i = 0; i < release->fileCount; i++)
            if (strcmp(release->files[i].path, "sys/main.dol") == 0) expected = &release->files[i];
        const char* want = expected ? expected->sha1 : nullptr;
#if defined(SCR_ISO_INSTALL_TEST)
        if (g_expectedDolHashOverride) want = g_expectedDolHashOverride;
#endif
        if (!want || Hex(sha, 20) != want)
        {
            free(raw);
            error = "This release's main.dol is not the one this build was made from (" +
                    std::string(release->name) + ").";
            return fail(IsoInstallResult::WrongVersion);
        }
    }

    std::vector<std::string> paths;
    std::vector<FstEntry> files;
    if (!ParseFst(head.fst, paths, files))
    {
        free(raw);
        error = "The file system table is damaged.";
        return fail(IsoInstallResult::OpenFailed);
    }
    uint64_t payload = 0;
    for (auto& f : files) payload += f.size;
    progress.total = payload;
    if (!EnoughSpace(options.dest, payload, error))
    {
        free(raw);
        return fail(IsoInstallResult::WriteFailed);
    }

    // Everything is written to <dest>.partial and renamed at the end, so an interrupted
    // install is never mistaken for a finished one.
    const fs::path stage = options.dest.string() + ".partial";
    std::error_code ec;
    fs::remove_all(stage, ec);
    fs::create_directories(stage / "sys", ec);
    fs::create_directories(stage / "files", ec);
    fs::create_directories(stage / "disc", ec);
    if (ec)
    {
        free(raw);
        error = "Cannot write to app storage: " + ec.message();
        return fail(IsoInstallResult::WriteFailed);
    }
    struct Guard
    {
        fs::path p;
        bool keep = false;
        ~Guard() { if (!keep) { std::error_code e; fs::remove_all(p, e); } }
    } guard{ stage };

    bool ok = WriteAll(stage / "sys" / "boot.bin", head.boot, head.boot[0] ? kBootSize : 0) &&
              WriteAll(stage / "sys" / "bi2.bin", head.bi2, kBi2Size) &&
              WriteAll(stage / "sys" / "apploader.img", head.apploader.data(), head.apploader.size()) &&
              WriteAll(stage / "sys" / "main.dol", head.dol.data(), head.dol.size()) &&
              WriteAll(stage / "sys" / "fst.bin", head.fst.data(), head.fst.size()) &&
              WriteAll(stage / "disc" / "header.bin", raw, 0x100) &&
              WriteAll(stage / "disc" / "region.bin", raw + 0x4E000, 0x20) &&
              WriteAll(stage / "disc" / "unencrypted.bin", raw, (size_t)kUnencryptedSize);

    // ticket/tmd/cert/h3 as they sit in the partition header (dev_es reads tmd and ticket).
    auto copyBlob = [&](const char* name, uint32_t off, uint32_t size) {
        if (!size) return true;
        std::vector<uint8_t> buf(size);
        if (!image.Read(partOffset + off, buf.data(), size)) return false;
        return WriteAll(stage / name, buf.data(), size);
    };
    ok = ok && WriteAll(stage / "ticket.bin", ticket, 0x2A4) && copyBlob("tmd.bin", tmdOff, tmdSize) &&
         copyBlob("cert.bin", certOff, certSize) && copyBlob("h3.bin", h3Off, 0x18000);
    free(raw);
    if (!ok)
    {
        error = "Cannot write to app storage (out of space?).";
        return fail(IsoInstallResult::WriteFailed);
    }

    // files/: the FST walk order gives each directory before its children, so creating the
    // parent of every file is enough (directories with no files are recreated below).
    report(0, "Extracting game data…");
    std::vector<uint8_t> buf(1 << 20);
    for (auto& f : files)
    {
        if (progress.cancel) return fail(IsoInstallResult::Canceled);
        const fs::path out = stage / "files" / fs::path(f.name);
        fs::create_directories(out.parent_path(), ec);
        FILE* fp = fopen(out.string().c_str(), "wb");
        if (!fp)
        {
            error = "Cannot write files/" + f.name + " (out of space?).";
            return fail(IsoInstallResult::WriteFailed);
        }
        uint64_t left = f.size, pos = (uint64_t)f.offset << 2;
        bool good = true;
        while (left)
        {
            const size_t n = (size_t)std::min<uint64_t>(left, buf.size());
            if (!part.Read(image, pos, buf.data(), n))
            {
                good = false;
                error = "Cannot read files/" + f.name + " from the image (damaged or wrong key).";
                break;
            }
            if (fwrite(buf.data(), 1, n, fp) != n)
            {
                good = false;
                error = "Cannot write files/" + f.name + " (out of space?).";
                break;
            }
            pos += n;
            left -= n;
            progress.done += n;
            if (progress.cancel) { fclose(fp); return fail(IsoInstallResult::Canceled); }
        }
        if (fclose(fp) != 0 && good)
        {
            good = false;
            error = "Cannot write files/" + f.name + " (out of space?).";
        }
        if (!good) return fail(IsoInstallResult::WriteFailed);
        const uint64_t total = progress.total.load(), done = progress.done.load();
        if (total) report((int)(done * 100 / total), "Extracting " + f.name);
    }
    fs::create_directories(options.dest.parent_path(), ec);
    const fs::path old = options.dest.string() + ".old";
    bool replaced = false;
    if (fs::exists(options.dest, ec))
    {
        fs::remove_all(old, ec);
        fs::rename(options.dest, old, ec);
        if (ec)
        {
            error = "Cannot replace the existing install: " + ec.message();
            return fail(IsoInstallResult::WriteFailed);
        }
        replaced = true;
    }
    fs::rename(stage, options.dest, ec);
    if (ec)
    {
        error = "Cannot move the extracted data into place: " + ec.message();
        if (replaced) { std::error_code e2; fs::rename(old, options.dest, e2); }
        return fail(IsoInstallResult::WriteFailed);
    }
    guard.keep = true;
    if (replaced) fs::remove_all(old, ec);
    report(100, "Game data installed.");
    return IsoInstallResult::Ok;
}

#if defined(SCR_ISO_INSTALL_TEST)
void IsoInstall_SetExpectedDolHashForTest(const char* sha1Hex) { g_expectedDolHashOverride = sha1Hex; }
#endif

int IsoInstallResultCode(IsoInstallResult r)
{
    switch (r)
    {
    case IsoInstallResult::Ok: return 0;
    case IsoInstallResult::OpenFailed: return 1;
    case IsoInstallResult::WrongGame: return 2;
    case IsoInstallResult::WrongVersion: return 3;
    case IsoInstallResult::WriteFailed: return 4;
    case IsoInstallResult::Canceled: return 5;
    }
    return 1;
}
