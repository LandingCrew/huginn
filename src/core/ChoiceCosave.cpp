#include "core/ChoiceCosave.h"

#include <cmath>
#include <cstring>
#include <format>
#include <unordered_map>

namespace Huginn::Core::ChoiceCosave
{
    namespace
    {
        class Writer
        {
        public:
            template <class T>
            void Put(T v)
            {
                const auto at = out.size();
                out.resize(at + sizeof(T));
                std::memcpy(out.data() + at, &v, sizeof(T));
            }
            void Text(const std::string& s)
            {
                const auto n = static_cast<std::uint16_t>(s.size() > kMaxText ? kMaxText : s.size());
                Put(n);
                const auto at = out.size();
                out.resize(at + n);
                if (n) std::memcpy(out.data() + at, s.data(), n);
            }
            std::vector<std::byte> out;
        };

        class Reader
        {
        public:
            explicit Reader(std::span<const std::byte> b) : blob(b) {}
            template <class T>
            bool Get(T& v)
            {
                if (blob.size() - pos < sizeof(T)) return false;
                std::memcpy(&v, blob.data() + pos, sizeof(T));
                pos += sizeof(T);
                return true;
            }
            bool Text(std::string& s)
            {
                std::uint16_t n = 0;
                if (!Get(n) || n > kMaxText || blob.size() - pos < n) return false;
                s.assign(reinterpret_cast<const char*>(blob.data() + pos), n);
                pos += n;
                return true;
            }
            [[nodiscard]] std::size_t Left() const noexcept { return blob.size() - pos; }
            std::span<const std::byte> blob;
            std::size_t pos = 0;
        };

        void Say(std::string* why, std::string text)
        {
            if (why) *why = std::move(text);
        }

        bool Usable(double mu, double var) noexcept
        {
            return std::isfinite(mu) && std::isfinite(var) && var > 0.0;
        }
    }

    std::vector<std::byte> EncodeTheta(const ThetaRecord& r)
    {
        Writer w;
        w.out.reserve(4 + 4 + r.createdUtc.size() + r.sha256.size() + 4 + r.entries.size() * kThetaStride);
        w.Put(kThetaVersion);
        w.Text(r.createdUtc);
        w.Text(r.sha256);
        w.Put(static_cast<std::uint32_t>(r.entries.size()));
        for (const auto& e : r.entries) {
            w.Put(e.key);
            w.Put(e.mu);
            w.Put(e.var);
        }
        return std::move(w.out);
    }

    std::vector<std::byte> EncodeBias(const BiasRecord& r)
    {
        Writer w;
        w.out.reserve(8 + r.entries.size() * kBiasStride);
        w.Put(kBiasVersion);
        w.Put(static_cast<std::uint32_t>(r.entries.size()));
        for (const auto& e : r.entries) {
            w.Put(e.form);
            w.Put(e.mu);
            w.Put(e.var);
            w.Put(e.n);
            w.Put(e.minutes);
            w.Put(e.flags);
        }
        return std::move(w.out);
    }

    std::optional<ThetaRecord> DecodeTheta(std::span<const std::byte> blob, std::uint32_t headerVersion, std::string* why)
    {
        Reader rd(blob);
        std::uint32_t version = 0;
        if (!rd.Get(version)) {
            Say(why, "THTA: shorter than its version");
            return std::nullopt;
        }
        if (version != kThetaVersion || headerVersion != kThetaVersion) {
            Say(why, std::format("THTA: version {} (record header {}), this build reads {}", version, headerVersion, kThetaVersion));
            return std::nullopt;
        }
        ThetaRecord r;
        if (!rd.Text(r.createdUtc) || !rd.Text(r.sha256)) {
            Say(why, "THTA: bad bootstrap identity");
            return std::nullopt;
        }
        std::uint32_t count = 0;
        if (!rd.Get(count)) {
            Say(why, "THTA: no entry count");
            return std::nullopt;
        }
        if (count > kMaxThetaEntries) {
            Say(why, std::format("THTA: {} entries, above the cap {}", count, kMaxThetaEntries));
            return std::nullopt;
        }
        if (rd.Left() != static_cast<std::size_t>(count) * kThetaStride) {
            Say(why, std::format("THTA: {} bytes for {} entries of {}", rd.Left(), count, kThetaStride));
            return std::nullopt;
        }
        r.entries.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            ThetaEntry e;
            rd.Get(e.key);
            rd.Get(e.mu);
            rd.Get(e.var);
            if (Usable(e.mu, e.var)) r.entries.push_back(e);
            else ++r.dropped;
        }
        return r;
    }

    std::optional<BiasRecord> DecodeBias(std::span<const std::byte> blob, std::uint32_t headerVersion, std::string* why)
    {
        Reader rd(blob);
        std::uint32_t version = 0;
        if (!rd.Get(version)) {
            Say(why, "BIAS: shorter than its version");
            return std::nullopt;
        }
        if (version != kBiasVersion || headerVersion != kBiasVersion) {
            Say(why, std::format("BIAS: version {} (record header {}), this build reads {}", version, headerVersion, kBiasVersion));
            return std::nullopt;
        }
        std::uint32_t count = 0;
        if (!rd.Get(count)) {
            Say(why, "BIAS: no entry count");
            return std::nullopt;
        }
        if (count > kMaxBiasEntries) {
            Say(why, std::format("BIAS: {} entries, above the cap {}", count, kMaxBiasEntries));
            return std::nullopt;
        }
        if (rd.Left() != static_cast<std::size_t>(count) * kBiasStride) {
            Say(why, std::format("BIAS: {} bytes for {} entries of {}", rd.Left(), count, kBiasStride));
            return std::nullopt;
        }
        BiasRecord r;
        r.entries.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            BiasEntry e;
            rd.Get(e.form);
            rd.Get(e.mu);
            rd.Get(e.var);
            rd.Get(e.n);
            rd.Get(e.minutes);
            rd.Get(e.flags);
            if (Usable(e.mu, e.var) && std::isfinite(e.n) && e.n >= 0.0f && e.form != 0) r.entries.push_back(e);
            else ++r.dropped;
        }
        return r;
    }

    std::vector<std::byte> EncodeDiscovery(const DiscoveryRecord& r)
    {
        Writer w;
        w.out.reserve(12 + r.seen.size() * kSeenStride + r.boosted.size() * kBoostStride);
        w.Put(kDiscoveryVersion);
        w.Put(static_cast<std::uint32_t>(r.seen.size()));
        for (const auto f : r.seen) w.Put(f);
        w.Put(static_cast<std::uint32_t>(r.boosted.size()));
        for (const auto& [f, sec] : r.boosted) {
            w.Put(f);
            w.Put(sec);
        }
        return std::move(w.out);
    }

    std::optional<DiscoveryRecord> DecodeDiscovery(std::span<const std::byte> blob, std::uint32_t headerVersion,
                                                   std::string* why)
    {
        Reader rd(blob);
        std::uint32_t version = 0;
        if (!rd.Get(version)) {
            Say(why, "DISC: shorter than its version");
            return std::nullopt;
        }
        if (version != kDiscoveryVersion || headerVersion != kDiscoveryVersion) {
            Say(why, std::format("DISC: version {} (record header {}), this build reads {}", version, headerVersion,
                                 kDiscoveryVersion));
            return std::nullopt;
        }
        std::uint32_t seenCount = 0;
        if (!rd.Get(seenCount)) {
            Say(why, "DISC: no seen count");
            return std::nullopt;
        }
        if (seenCount > kMaxSeenEntries) {
            Say(why, std::format("DISC: {} seen forms, above the cap {}", seenCount, kMaxSeenEntries));
            return std::nullopt;
        }
        // The seen list and the boost count must fit before either is read.
        if (rd.Left() < static_cast<std::size_t>(seenCount) * kSeenStride + 4) {
            Say(why, std::format("DISC: {} bytes for {} seen forms and a boost count", rd.Left(), seenCount));
            return std::nullopt;
        }
        DiscoveryRecord r;
        r.seen.reserve(seenCount);
        for (std::uint32_t i = 0; i < seenCount; ++i) {
            std::uint32_t f = 0;
            rd.Get(f);
            if (f != 0) r.seen.push_back(f);
            else ++r.dropped;
        }
        std::uint32_t boostCount = 0;
        rd.Get(boostCount);
        if (boostCount > kMaxBoostEntries) {
            Say(why, std::format("DISC: {} boosts, above the cap {}", boostCount, kMaxBoostEntries));
            return std::nullopt;
        }
        if (rd.Left() != static_cast<std::size_t>(boostCount) * kBoostStride) {
            Say(why, std::format("DISC: {} bytes for {} boosts of {}", rd.Left(), boostCount, kBoostStride));
            return std::nullopt;
        }
        r.boosted.reserve(boostCount);
        for (std::uint32_t i = 0; i < boostCount; ++i) {
            std::uint32_t f = 0, sec = 0;
            rd.Get(f);
            rd.Get(sec);
            if (f != 0) r.boosted.emplace_back(f, sec);
            else ++r.dropped;
        }
        return r;
    }

    std::vector<ThetaEntry> MergeTheta(std::span<const std::uint64_t> buildKeys, std::span<const ThetaEntry> saved,
                                       MergeReport& rep)
    {
        rep = MergeReport{};
        std::unordered_map<std::uint64_t, std::size_t> savedAt;   // key -> the last saved entry with it
        savedAt.reserve(saved.size());
        for (std::size_t i = 0; i < saved.size(); ++i) {
            const auto [it, fresh] = savedAt.try_emplace(saved[i].key, i);
            if (!fresh) {
                ++rep.duplicate;
                it->second = i;
            }
        }
        std::vector<ThetaEntry> out;
        out.reserve(buildKeys.size());
        std::unordered_set<std::uint64_t> known;
        known.reserve(buildKeys.size());
        for (const auto key : buildKeys) {
            if (!known.insert(key).second) continue;
            const auto it = savedAt.find(key);
            if (it == savedAt.end()) {
                ++rep.fromFile;
                continue;
            }
            out.push_back(saved[it->second]);
            ++rep.applied;
        }
        for (const auto& [key, i] : savedAt) {
            (void)i;
            if (!known.contains(key)) ++rep.unknown;
        }
        return out;
    }

    void ResetCarried(std::optional<ThetaRecord>& theta, std::optional<BiasRecord>& bias, bool& resetHolds)
    {
        if (theta) {
            theta->entries.clear();
            theta->dropped = 0;
        }
        bias = BiasRecord{};
        resetHolds = true;
    }

    CarryReport CarryLoaded(LoadDecision decision, std::optional<ThetaRecord>& carriedTheta,
        std::optional<BiasRecord>& carriedBias, bool& resetHolds, std::optional<ThetaRecord> savedTheta,
        std::optional<BiasRecord> savedBias)
    {
        switch (decision) {
        case LoadDecision::Failed:
            // Nothing was restored: the carried records and the reset bit stay
            // as they were, so a reset before it still holds at a reload.
            return {};
        case LoadDecision::NewGame:
            resetHolds = false;
            carriedTheta.reset();
            carriedBias.reset();
            return {};
        case LoadDecision::Reload: {
            // The bit stays set (0.24.23): every reload after a reset keeps it.
            if (!resetHolds) break;   // as a load: the save's records
            CarryReport rep{ .keptCarried = true };
            if (!carriedBias || !savedBias) return rep;
            auto dynamic = [](std::uint32_t form) { return (form >> 24) == 0xFF; };
            auto& kept = carriedBias->entries;
            rep.dynamicDropped = std::erase_if(kept, [&](const BiasEntry& e) { return dynamic(e.form); });
            for (const auto& e : savedBias->entries) {
                if (!dynamic(e.form) || !(e.var > 0.0) || !std::isfinite(e.mu)) continue;
                kept.push_back(e);
                ++rep.dynamicTaken;
            }
            return rep;
        }
        case LoadDecision::Replace:
            resetHolds = false;   // the save's records adopted: the reset's hold ends
            break;
        }
        carriedTheta = std::move(savedTheta);
        carriedBias = std::move(savedBias);
        return {};
    }
}
