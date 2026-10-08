#include "ScriptNames.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <unordered_set>

namespace Huginn::Effect
{
    namespace
    {
        constexpr std::uint32_t Tag(const char (&s)[5]) noexcept
        {
            return static_cast<std::uint32_t>(static_cast<unsigned char>(s[0])) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(s[1])) << 8) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(s[2])) << 16) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(s[3])) << 24);
        }

        constexpr std::uint32_t kCompressed = 0x00040000;
        constexpr std::uint32_t kLightFile = 0x00000200;

#pragma pack(push, 1)
        struct RecordHeader
        {
            std::uint32_t type;
            std::uint32_t size;   // data size (record) or whole group size (GRUP)
            std::uint32_t flags;  // record flags, or the group label
            std::uint32_t formID; // record FormID, or the group type
            std::uint32_t vc;
            std::uint16_t version;
            std::uint16_t unknown;
        };
#pragma pack(pop)
        static_assert(sizeof(RecordHeader) == 24);

        std::string LowerAscii(std::string_view s)
        {
            std::string out(s);
            for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        /// Walks a subrecord block (with XXXX handling); calls fn(type, data, size).
        template <class Fn>
        void ForEachSubrecord(const std::vector<char>& data, Fn&& fn)
        {
            std::size_t pos = 0;
            std::uint32_t nextSize = 0;
            while (pos + 6 <= data.size()) {
                std::uint32_t type = 0;
                std::uint16_t size16 = 0;
                std::memcpy(&type, data.data() + pos, 4);
                std::memcpy(&size16, data.data() + pos + 4, 2);
                pos += 6;
                std::uint32_t size = nextSize ? nextSize : size16;
                nextSize = 0;
                if (type == Tag("XXXX") && size16 == 4 && pos + 4 <= data.size()) {
                    std::memcpy(&nextSize, data.data() + pos, 4);
                    pos += 4;
                    continue;
                }
                if (pos + size > data.size()) return;
                if (!fn(type, data.data() + pos, size)) return;
                pos += size;
            }
        }

        /// Script names from a VMAD subrecord; stops (keeping what it has) at
        /// anything it does not understand.
        std::vector<std::string> ParseVmad(const char* p, std::uint32_t size)
        {
            std::vector<std::string> names;
            std::size_t pos = 0;
            auto need = [&](std::size_t n) { return pos + n <= size; };
            auto u8 = [&]() { std::uint8_t v = 0; std::memcpy(&v, p + pos, 1); pos += 1; return v; };
            auto u16 = [&]() { std::uint16_t v = 0; std::memcpy(&v, p + pos, 2); pos += 2; return v; };
            auto u32 = [&]() { std::uint32_t v = 0; std::memcpy(&v, p + pos, 4); pos += 4; return v; };
            auto wstr = [&](std::string* out) {
                if (!need(2)) return false;
                const std::uint16_t len = u16();
                if (!need(len)) return false;
                if (out) out->assign(p + pos, len);
                pos += len;
                return true;
            };
            if (!need(6)) return names;
            const std::int16_t version = static_cast<std::int16_t>(u16());
            const std::int16_t objFormat = static_cast<std::int16_t>(u16());
            (void)objFormat;
            const std::uint16_t scripts = u16();
            for (std::uint16_t s = 0; s < scripts; ++s) {
                std::string name;
                if (!wstr(&name)) return names;
                names.push_back(name);
                if (version >= 4) {
                    if (!need(1)) return names;
                    u8();
                }
                if (!need(2)) return names;
                const std::uint16_t props = u16();
                for (std::uint16_t i = 0; i < props; ++i) {
                    if (!wstr(nullptr) || !need(1)) return names;
                    const std::uint8_t type = u8();
                    if (version >= 4) {
                        if (!need(1)) return names;
                        u8();
                    }
                    switch (type) {
                        case 1: if (!need(8)) return names; pos += 8; break;  // object
                        case 2: if (!wstr(nullptr)) return names; break;      // string
                        case 3:
                        case 4: if (!need(4)) return names; pos += 4; break;  // int, float
                        case 5: if (!need(1)) return names; pos += 1; break;  // bool
                        case 11:
                        case 12:
                        case 13:
                        case 14:
                        case 15: {
                            if (!need(4)) return names;
                            const std::uint32_t n = u32();
                            for (std::uint32_t k = 0; k < n; ++k) {
                                if (type == 11) { if (!need(8)) return names; pos += 8; }
                                else if (type == 12) { if (!wstr(nullptr)) return names; }
                                else if (type == 15) { if (!need(1)) return names; pos += 1; }
                                else { if (!need(4)) return names; pos += 4; }
                            }
                            break;
                        }
                        default: return names;  // unknown type: the rest cannot be walked
                    }
                }
            }
            return names;
        }

        struct Want
        {
            RE::FormID runtime = 0;
            std::uint32_t local = 0;  // the ID as the winning file writes it
        };

        /// Reads one plugin's top-level MGEF group; fills names for `wanted`
        /// (local ID -> runtime ID). False if the file could not be read.
        bool ScanPlugin(const std::string& fileName, const std::map<std::uint32_t, RE::FormID>& wanted,
                        std::unordered_map<RE::FormID, std::string>& out, ScriptNameStats& stats)
        {
            std::ifstream in(std::filesystem::path("Data") / fileName, std::ios::binary);
            if (!in) return false;
            RecordHeader h{};
            if (!in.read(reinterpret_cast<char*>(&h), sizeof(h)) || h.type != Tag("TES4")) return false;
            in.seekg(h.size, std::ios::cur);  // the TES4 record's data
            while (in.read(reinterpret_cast<char*>(&h), sizeof(h))) {
                if (h.type != Tag("GRUP") || h.size < sizeof(h)) return false;
                const std::uint32_t body = h.size - static_cast<std::uint32_t>(sizeof(h));
                if (h.flags != Tag("MGEF") || h.formID != 0) {
                    in.seekg(body, std::ios::cur);
                    continue;
                }
                std::uint32_t done = 0;
                while (done < body) {
                    RecordHeader r{};
                    if (!in.read(reinterpret_cast<char*>(&r), sizeof(r))) return false;
                    done += static_cast<std::uint32_t>(sizeof(r));
                    const bool isGroup = r.type == Tag("GRUP");
                    const std::uint32_t dataSize = isGroup ? r.size - static_cast<std::uint32_t>(sizeof(r)) : r.size;
                    const auto it = isGroup ? wanted.end() : wanted.find(r.formID);
                    if (it == wanted.end() || (r.flags & kCompressed)) {
                        in.seekg(dataSize, std::ios::cur);
                    }
                    else {
                        std::vector<char> data(dataSize);
                        if (!in.read(data.data(), dataSize)) return false;
                        ++stats.found;
                        ForEachSubrecord(data, [&](std::uint32_t type, const char* p, std::uint32_t size) {
                            if (type != Tag("VMAD")) return true;
                            const auto names = ParseVmad(p, size);
                            std::string joined;
                            for (const auto& n : names) {
                                if (!joined.empty()) joined += ';';
                                joined += n;
                            }
                            if (!joined.empty()) {
                                ++stats.withScripts;
                                out[it->second] = std::move(joined);
                            }
                            return false;
                        });
                    }
                    done += dataSize;
                }
                return true;  // one top-level MGEF group per plugin
            }
            return true;
        }
    }

    std::unordered_map<RE::FormID, std::string> ReadScriptNames(const std::vector<const RE::EffectSetting*>& wanted,
                                                               ScriptNameStats* statsOut)
    {
        ScriptNameStats stats;
        std::unordered_map<RE::FormID, std::string> out;
        // Group by the winning file, with each ID as that file writes it: the
        // top byte is the defining plugin's position in the winner's master
        // list (or the master count, for the winner's own records).
        std::map<std::string, std::map<std::uint32_t, RE::FormID>> byFile;
        for (const auto* m : wanted) {
            if (!m) continue;
            const auto* origin = m->GetFile(0);
            const auto* winner = m->GetFile(-1);
            if (!origin || !winner) continue;
            ++stats.wanted;
            std::uint32_t index = winner->masterCount;
            if (origin != winner) {
                const std::string originName = LowerAscii(origin->fileName);
                bool found = false;
                for (std::uint32_t i = 0; i < winner->masterCount; ++i) {
                    const auto* master = winner->masterPtrs ? winner->masterPtrs[i] : nullptr;
                    if (master && LowerAscii(master->fileName) == originName) {
                        index = i;
                        found = true;
                        break;
                    }
                }
                if (!found) continue;
            }
            const std::uint32_t low = origin->IsLight() ? (m->GetFormID() & 0xFFFu) : (m->GetFormID() & 0xFFFFFFu);
            byFile[winner->fileName][(index << 24) | low] = m->GetFormID();
        }
        for (const auto& [file, ids] : byFile) {
            if (ScanPlugin(file, ids, out, stats)) ++stats.filesRead;
            else ++stats.filesFailed;
        }
        (void)kLightFile;
        if (statsOut) *statsOut = stats;
        return out;
    }
}
