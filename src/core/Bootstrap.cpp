#include "Bootstrap.h"

#include "core/ObviousPairs.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <unordered_map>

namespace Huginn::Core::Bootstrap
{
    namespace
    {
        using Json::Value;

        constexpr std::array<std::string_view, 3> kGradings{ "graded", "presence", "presence+strength" };
        constexpr std::array<std::string_view, 2> kOutcomes{ "picks", "three" };

        bool IsNum(const Value* v) noexcept { return v && v->IsNumber() && std::isfinite(v->AsNumber()); }

        bool IsHex6(std::string_view s) noexcept
        {
            if (s.size() != 6) return false;
            for (const char c : s) {
                if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
            }
            return true;
        }

        std::string Lower(std::string_view s)
        {
            std::string o(s);
            for (char& c : o) {
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            }
            return o;
        }

        // bootstrap._matrix: K rows of J finite numbers (precisions above 0).
        // Appends the flat values on success.
        void Matrix(const Value& doc, std::string_view key, std::size_t rows, std::size_t cols, bool allowNone,
                    bool positive, std::vector<std::string>& errs, std::vector<double>& out)
        {
            const Value* m = doc.Find(key);
            if (!m || m->IsNull()) {
                if (!allowNone) errs.push_back(std::format("{}: missing", key));
                return;
            }
            if (!m->IsArray() || m->Size() != rows) {
                errs.push_back(std::format("{}: {} rows expected", key, rows));
                return;
            }
            std::vector<double> flat;
            flat.reserve(rows * cols);
            for (std::size_t i = 0; i < rows; ++i) {
                const Value& r = m->Items()[i];
                if (!r.IsArray() || r.Size() != cols) {
                    errs.push_back(std::format("{}[{}]: {} numbers expected", key, i, cols));
                    return;
                }
                for (const Value& x : r.Items()) {
                    if (!IsNum(&x)) {
                        errs.push_back(std::format("{}[{}]: not all finite numbers", key, i));
                        return;
                    }
                }
                if (positive) {
                    for (const Value& x : r.Items()) {
                        if (!(x.AsNumber() > 0.0)) {
                            errs.push_back(std::format("{}[{}]: a precision must be above 0 (sigma^2 = 1 / precision)", key, i));
                            return;
                        }
                    }
                }
                for (const Value& x : r.Items()) flat.push_back(x.AsNumber());
            }
            out = std::move(flat);
        }

        // A list of exactly n finite numbers.
        bool Numbers(const Value* v, std::size_t n, std::vector<double>& out)
        {
            if (!v || !v->IsArray() || v->Size() != n) return false;
            std::vector<double> o;
            o.reserve(n);
            for (const Value& x : v->Items()) {
                if (!IsNum(&x)) return false;
                o.push_back(x.AsNumber());
            }
            out = std::move(o);
            return true;
        }

        std::string StrOr(const Value* v, std::string_view dflt = {})
        {
            return v && v->IsString() ? v->AsString() : std::string(dflt);
        }

        void Validate(const Value& doc, std::vector<std::string>& errs, Document& d)
        {
            if (!doc.IsObject()) {
                errs.push_back("not an object");
                return;
            }
            // 1. format and version
            const Value* fmt = doc.Find("format");
            if (!fmt || !fmt->IsString() || fmt->AsString() != kFormat) {
                errs.push_back(std::format("format: '{}' expected", kFormat));
            }
            const Value* ver = doc.Find("version");
            if (!ver || !ver->IsNumber() || ver->AsNumber() != static_cast<double>(kVersion)) {
                errs.push_back(std::format("version: {} expected (a reader rejects any other)", kVersion));
                return;
            }
            d.createdUtc = StrOr(doc.Find("created_utc"));
            if (const Value* src = doc.Find("source"); src && src->IsObject()) {
                d.sourceChar = StrOr(src->Find("char"));
            }
            // 2. model.grading and model.outcomes
            const Value* model = doc.Find("model");
            const std::string grading = model ? StrOr(model->Find("grading")) : std::string{};
            const std::string outcomes = model ? StrOr(model->Find("outcomes")) : std::string{};
            if (std::find(kGradings.begin(), kGradings.end(), grading) == kGradings.end()) {
                errs.push_back("model.grading: one of ('graded', 'presence', 'presence+strength')");
            }
            if (std::find(kOutcomes.begin(), kOutcomes.end(), outcomes) == kOutcomes.end()) {
                errs.push_back("model.outcomes: one of ('picks', 'three')");
            }
            d.spec = model ? StrOr(model->Find("spec")) : std::string{};
            d.outcomes = outcomes;
            d.grading = grading == "presence"            ? Choice::Grading::Presence
                      : grading == "presence+strength"   ? Choice::Grading::PresenceStrength
                                                         : Choice::Grading::Graded;
            const bool strength = grading == "presence+strength";
            const bool three = outcomes == "three";
            // 3. needs and cols
            for (const auto& [key, dst] : { std::pair{ std::string_view("needs"), &d.needs },
                                            std::pair{ std::string_view("cols"), &d.cols } }) {
                const Value* lst = doc.Find(key);
                bool ok = lst && lst->IsArray();
                if (ok) {
                    for (const Value& x : lst->Items()) ok = ok && x.IsString() && !x.AsString().empty();
                }
                if (!ok) {
                    errs.push_back(std::format("{}: a list of names expected", key));
                    return;
                }
                dst->clear();
                for (const Value& x : lst->Items()) dst->push_back(x.AsString());
                std::vector<std::string> sorted = *dst;
                std::sort(sorted.begin(), sorted.end());
                if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
                    errs.push_back(std::format("{}: duplicate names", key));
                }
            }
            const std::size_t K = d.needs.size(), J = d.cols.size();
            // 4. theta, theta_precision, strength, bias
            Matrix(doc, "theta", K, J, false, false, errs, d.theta);
            Matrix(doc, "theta_precision", K, J, false, true, errs, d.precision);
            Matrix(doc, "theta_strength", K, J, !strength, false, errs, d.strength);
            Matrix(doc, "theta_strength_precision", K, J, !strength, true, errs, d.strengthPrecision);
            const Value* ts = doc.Find("theta_strength");
            const Value* tsp = doc.Find("theta_strength_precision");
            if (!strength && ((ts && !ts->IsNull()) || (tsp && !tsp->IsNull()))) {
                errs.push_back("theta_strength: only with grading presence+strength");
            }
            const Value* tb = doc.Find("theta_bias");
            if (tb && !tb->IsNull()) {
                if (!Numbers(tb, J, d.bias)) errs.push_back(std::format("theta_bias: null or {} numbers", J));
                if (strength) errs.push_back("theta_bias: not with grading presence+strength");
            }
            // 5. kappa
            const Value* kappa = doc.Find("kappa");
            if (!IsNum(kappa)) errs.push_back("kappa: a number");
            else d.kappa = kappa->AsNumber();
            // 6. u0
            const Value* u0 = doc.Find("u0");
            if (three) {
                const Value* bias = u0 && u0->IsObject() ? u0->Find("bias") : nullptr;
                const Value* need = u0 && u0->IsObject() ? u0->Find("need") : nullptr;
                if (!u0 || !u0->IsObject() || !IsNum(bias) || !need || !need->IsObject()) {
                    errs.push_back("u0: {bias, need} expected with outcomes three");
                }
                else {
                    bool ok = true;
                    for (const auto& [n, w] : need->Members()) {
                        ok = ok && IsNum(&w) && std::find(d.needs.begin(), d.needs.end(), n) != d.needs.end();
                    }
                    if (!ok) errs.push_back("u0.need: names from needs, numbers");
                    else {
                        d.hasU0 = true;
                        d.u0Bias = bias->AsNumber();
                        for (const auto& [n, w] : need->Members()) d.u0Need.emplace_back(n, w.AsNumber());
                    }
                }
            }
            else if (u0 && !u0->IsNull()) {
                errs.push_back("u0: null unless outcomes three");
            }
            // 7. recent
            const Value* rec = doc.Find("recent");
            if (rec && !rec->IsNull()) {
                const Value* window = rec->IsObject() ? rec->Find("window_s") : nullptr;
                const Value* taus = rec->IsObject() ? rec->Find("taus_s") : nullptr;
                if (!IsNum(window) || !taus || !taus->IsArray()) {
                    errs.push_back("recent: window_s and taus_s expected");
                }
                else {
                    d.window = window->AsNumber();
                    for (const Value& t : taus->Items()) {
                        if (IsNum(&t)) d.taus.push_back(t.AsNumber());
                    }
                }
                const Value* feats = rec->IsObject() ? rec->Find("features") : nullptr;
                if (feats && feats->IsArray()) {
                    std::size_t i = 0;
                    for (const Value& f : feats->Items()) {
                        RecentFeature rf;
                        const Value* fam = f.IsObject() ? f.Find("family") : nullptr;
                        if (!fam || !fam->IsString() ||
                            std::find(d.cols.begin(), d.cols.end(), fam->AsString()) == d.cols.end()) {
                            errs.push_back(std::format("recent.features[{}].family: a name from cols", i));
                        }
                        else {
                            rf.family = fam->AsString();
                        }
                        const Value* tau = f.IsObject() ? f.Find("tau_s") : nullptr;
                        bool tauOk = IsNum(tau) && taus && taus->IsArray();
                        if (tauOk) {
                            tauOk = false;
                            for (const Value& t : taus->Items()) tauOk |= t.IsNumber() && t.AsNumber() == tau->AsNumber();
                        }
                        if (!tauOk) errs.push_back(std::format("recent.features[{}].tau_s: one of taus_s", i));
                        else rf.tau = tau->AsNumber();
                        auto row = [&](std::string_view key, std::vector<double>& dst, bool positive) {
                            const Value* v = f.IsObject() ? f.Find(key) : nullptr;
                            if (!Numbers(v, J, dst)) {
                                errs.push_back(std::format("recent.features[{}].{}: {} numbers", i, key, J));
                            }
                            else if (positive && !std::all_of(dst.begin(), dst.end(), [](double x) { return x > 0.0; })) {
                                errs.push_back(std::format("recent.features[{}].precision: above 0", i));
                            }
                        };
                        row("theta", rf.theta, false);
                        row("precision", rf.precision, true);
                        if (strength) row("theta_strength", rf.strength, false);
                        const Value* fu0 = f.IsObject() ? f.Find("u0") : nullptr;
                        if (three != (fu0 != nullptr)) {
                            errs.push_back(std::format("recent.features[{}].u0: present exactly when outcomes three", i));
                        }
                        if (fu0 && IsNum(fu0)) {
                            rf.hasU0 = true;
                            rf.u0 = fu0->AsNumber();
                        }
                        d.recent.push_back(std::move(rf));
                        ++i;
                    }
                }
                d.hasRecent = true;
            }
            // 8. features
            if (const Value* feats = doc.Find("features"); feats && feats->IsArray()) {
                std::size_t i = 0;
                for (const Value& f : feats->Items()) {
                    const Value* name = f.IsObject() ? f.Find("name") : nullptr;
                    const Value* w = f.IsObject() ? f.Find("w") : nullptr;
                    const bool known = name && name->IsString() &&
                                       std::find(Choice::kFeatureNames.begin(), Choice::kFeatureNames.end(),
                                                 name->AsString()) != Choice::kFeatureNames.end();
                    if (!known || !IsNum(w)) {
                        errs.push_back(std::format("features[{}]: name one of the format's list, w a number", i));
                    }
                    else {
                        d.features.emplace_back(name->AsString(), w->AsNumber());
                    }
                    ++i;
                }
            }
            // 9. items
            if (const Value* items = doc.Find("items"); items && items->IsArray()) {
                std::vector<std::string> seen;
                std::size_t i = 0;
                for (const Value& it : items->Items()) {
                    Item item;
                    const Value* p = it.IsObject() ? it.Find("plugin") : nullptr;
                    const Value* loc = it.IsObject() ? it.Find("local") : nullptr;
                    const Value* b = it.IsObject() ? it.Find("b") : nullptr;
                    const Value* prec = it.IsObject() ? it.Find("precision") : nullptr;
                    bool ok = true;
                    if (!p || !p->IsString() || p->AsString().empty()) {
                        errs.push_back(std::format("items[{}].plugin: a plugin file name", i));
                        ok = false;
                    }
                    if (!loc || !loc->IsString() || !IsHex6(loc->AsString())) {
                        errs.push_back(std::format("items[{}].local: 6 upper-case hex digits", i));
                        ok = false;
                    }
                    if (!IsNum(b) || !IsNum(prec)) {
                        errs.push_back(std::format("items[{}]: b and precision numbers", i));
                        ok = false;
                    }
                    else if (prec->AsNumber() <= 0.0) {
                        errs.push_back(std::format("items[{}].precision: above 0", i));
                        ok = false;
                    }
                    // Python keys duplicates on (str(plugin).lower(), local) even
                    // when either is malformed; only well-formed ones matter here.
                    const std::string key = (p && p->IsString() ? Lower(p->AsString()) : std::string("?")) + "|" +
                                            (loc && loc->IsString() ? loc->AsString() : std::string("?"));
                    if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
                        errs.push_back(std::format("items[{}]: duplicate plugin + local", i));
                        ok = false;
                    }
                    seen.push_back(key);
                    if (ok) {
                        item.plugin = p->AsString();
                        item.local = loc->AsString();
                        const auto parsedLocal = std::from_chars(item.local.data(), item.local.data() + item.local.size(), item.localId, 16);
                        (void)parsedLocal;   // IsHex6 checked it
                        item.name = StrOr(it.Find("name"));
                        item.b = b->AsNumber();
                        item.precision = prec->AsNumber();
                        if (const Value* pk = it.Find("picks"); IsNum(pk)) item.picks = static_cast<int>(pk->AsNumber());
                        d.items.push_back(std::move(item));
                    }
                    ++i;
                }
            }
            // 10. curves
            if (const Value* curves = doc.Find("curves"); curves && curves->IsObject()) {
                for (const auto& [n, c] : curves->Members()) {
                    const bool needOk = std::find(d.needs.begin(), d.needs.end(), n) != d.needs.end();
                    const bool shape = c.IsArray() && c.Size() == 3 && c.Items()[0].IsString() &&
                                       c.Items()[0].AsString() == "logistic" && IsNum(&c.Items()[1]) &&
                                       IsNum(&c.Items()[2]) && c.Items()[2].AsNumber() != 0.0;
                    if (!needOk || !shape) {
                        errs.push_back(std::format("curves.{}: [\"logistic\", centre, slope != 0] for a need in needs", n));
                    }
                    else {
                        d.curves.push_back(CurveSpec{ n, c.Items()[1].AsNumber(), c.Items()[2].AsNumber() });
                    }
                }
            }
        }

        // The prior of every (need, column) pair of the build: theta0 and variance.
        struct PairPrior
        {
            std::array<std::int8_t, Choice::K * Choice::J> sign{};
            std::array<bool, Choice::K * Choice::J> obvious{};
        };

        const PairPrior& Priors()
        {
            static const PairPrior p = [] {
                PairPrior out{};
                for (const auto& pr : Obvious::kPairs) {
                    const std::size_t i = static_cast<std::size_t>(pr.need) * Choice::J + static_cast<std::size_t>(pr.col);
                    out.sign[i] = pr.sign;
                    out.obvious[i] = true;
                }
                return out;
            }();
            return p;
        }

        Choice::Weight PriorWeight(std::size_t k, std::size_t j)
        {
            const auto& p = Priors();
            const std::size_t i = k * Choice::J + j;
            if (p.obvious[i]) return Choice::Weight{ static_cast<double>(p.sign[i]), 1.0 / kPrecisionObvious };
            return Choice::Weight{ 0.0, 1.0 / kPrecisionDense };
        }

        constexpr Choice::Weight kDenseRecent{ 0.0, 1.0 / kPrecisionDense };

        std::optional<std::size_t> NeedIndex(std::string_view name)
        {
            for (std::size_t k = 0; k < Needs::kNeedCount; ++k) {
                if (Needs::kNeeds[k].id == name) return k;
            }
            return std::nullopt;
        }

        std::optional<std::size_t> ColIndex(std::string_view name)
        {
            for (std::size_t j = 0; j < Effect::kColumnCount; ++j) {
                if (Effect::kColumns[j].id == name) return j;
            }
            return std::nullopt;
        }
    }

    std::uint64_t Document::SourceCharId() const noexcept
    {
        if (sourceChar.size() != 16) return 0;
        std::uint64_t v = 0;
        const auto r = std::from_chars(sourceChar.data(), sourceChar.data() + sourceChar.size(), v, 16);
        return (r.ec == std::errc{} && r.ptr == sourceChar.data() + sourceChar.size()) ? v : 0;
    }

    ReadResult Read(std::string_view text)
    {
        ReadResult r;
        auto parsed = Json::Parse(text);
        if (!parsed.Ok()) {
            r.errors.push_back("not JSON: " + parsed.error);
            return r;
        }
        Document d;
        Validate(*parsed.value, r.errors, d);
        if (r.errors.empty()) r.doc = std::move(d);
        return r;
    }

    std::vector<std::uint16_t> RecentFamilyColumns()
    {
        std::vector<std::uint16_t> out;
        for (std::size_t j = 0; j < Effect::kColumnCount; ++j) {
            if (Effect::kColumns[j].level == Effect::Level::Family) out.push_back(static_cast<std::uint16_t>(j));
        }
        for (std::size_t j = 0; j < Effect::kColumnCount; ++j) {
            if (Effect::kColumns[j].id.starts_with("kind_")) out.push_back(static_cast<std::uint16_t>(j));
        }
        return out;
    }

    Choice::Model PriorModel(const MapOptions& opt)
    {
        Choice::Model m;
        m.theta.resize(Choice::K * Choice::J);
        for (std::size_t k = 0; k < Choice::K; ++k) {
            for (std::size_t j = 0; j < Choice::J; ++j) m.Th(k, j) = PriorWeight(k, j);
        }
        m.covered = Choice::Weight{ opt.coveredPrior, 1.0 / opt.coveredPrecision };
        return m;
    }

    Mapped MapToBuild(const Document& doc, const MapOptions& opt)
    {
        Mapped out;
        auto& m = out.model;
        auto& rep = out.report;
        m = PriorModel(opt);
        m.grading = doc.grading;
        m.kappa = doc.kappa;
        m.three = doc.outcomes == "three";
        const bool st = doc.grading == Choice::Grading::PresenceStrength;
        if (st) m.strength.assign(Choice::K * Choice::J, kDenseRecent);   // strength pairs have no obvious prior

        // Names -> the build's indices.
        std::vector<std::optional<std::size_t>> needOf(doc.needs.size()), colOf(doc.cols.size());
        std::vector<bool> needSeen(Choice::K), colSeen(Choice::J);
        for (std::size_t a = 0; a < doc.needs.size(); ++a) {
            needOf[a] = NeedIndex(doc.needs[a]);
            if (needOf[a]) {
                ++rep.needsMapped;
                needSeen[*needOf[a]] = true;
            }
            else {
                rep.needsUnknown.push_back(doc.needs[a]);
            }
        }
        for (std::size_t b = 0; b < doc.cols.size(); ++b) {
            colOf[b] = ColIndex(doc.cols[b]);
            if (colOf[b]) {
                ++rep.colsMapped;
                colSeen[*colOf[b]] = true;
            }
            else {
                rep.colsUnknown.push_back(doc.cols[b]);
            }
        }
        rep.needsPrior = static_cast<std::size_t>(std::count(needSeen.begin(), needSeen.end(), false));
        rep.colsPrior = static_cast<std::size_t>(std::count(colSeen.begin(), colSeen.end(), false));
        const std::size_t Jf = doc.cols.size();
        for (std::size_t a = 0; a < doc.needs.size(); ++a) {
            if (!needOf[a]) continue;
            const std::size_t k = *needOf[a];
            for (std::size_t b = 0; b < Jf; ++b) {
                if (!colOf[b]) continue;
                const std::size_t j = *colOf[b];
                m.Th(k, j) = Choice::Weight{ doc.theta[a * Jf + b], 1.0 / doc.precision[a * Jf + b] };
                if (st) {
                    m.strength[k * Choice::J + j] =
                        Choice::Weight{ doc.strength[a * Jf + b], 1.0 / doc.strengthPrecision[a * Jf + b] };
                }
            }
        }
        if (!doc.bias.empty()) {
            m.bias.assign(Choice::J, 0.0);
            for (std::size_t b = 0; b < Jf; ++b) {
                if (colOf[b]) m.bias[*colOf[b]] = doc.bias[b];
            }
        }
        if (doc.hasU0) {
            m.u0Bias = doc.u0Bias;
            for (const auto& [n, w] : doc.u0Need) {
                if (const auto k = NeedIndex(n)) m.u0Need[*k] = w;
            }
        }

        // recent_<family>: the file's rows by (family, tau), then the build's
        // families the file lacks at each of its time constants, at the prior.
        if (doc.hasRecent) {
            m.window = doc.window;
            m.taus = doc.taus;
            const auto fams = RecentFamilyColumns();
            for (const auto& f : doc.recent) {
                const auto col = ColIndex(f.family);
                if (!col) {
                    ++rep.recentUnknown;
                    continue;
                }
                Choice::RecentRow rr;
                rr.col = static_cast<std::uint16_t>(*col);
                rr.tau = f.tau;
                rr.theta.assign(Choice::J, kDenseRecent);
                if (st) rr.strength.assign(Choice::J, kDenseRecent);
                for (std::size_t b = 0; b < Jf; ++b) {
                    if (!colOf[b]) continue;
                    rr.theta[*colOf[b]] = Choice::Weight{ f.theta[b], 1.0 / f.precision[b] };
                    // format v1 carries no strength precision for a recent row:
                    // the strength weights keep the dense prior's variance.
                    if (st) rr.strength[*colOf[b]].mu = f.strength[b];
                }
                rr.u0 = f.hasU0 ? f.u0 : 0.0;
                m.recent.push_back(std::move(rr));
                ++rep.recentMapped;
            }
            for (const double tau : doc.taus) {
                for (const std::uint16_t c : fams) {
                    const bool have = std::any_of(m.recent.begin(), m.recent.end(),
                                                  [&](const Choice::RecentRow& r) { return r.col == c && r.tau == tau; });
                    if (have) continue;
                    Choice::RecentRow rr;
                    rr.col = c;
                    rr.tau = tau;
                    rr.theta.assign(Choice::J, kDenseRecent);
                    if (st) rr.strength.assign(Choice::J, kDenseRecent);
                    m.recent.push_back(std::move(rr));
                    ++rep.recentPrior;
                }
            }
        }

        for (const auto& [name, w] : doc.features) {
            for (std::size_t i = 0; i < Choice::kFeatureCount; ++i) {
                if (Choice::kFeatureNames[i] == name) {
                    m.w[i] = w;
                    ++rep.featuresWeighted;
                }
            }
        }

        for (const auto& c : doc.curves) {
            if (const auto k = NeedIndex(c.need)) {
                out.curves.push_back(BuildCurve{ *k, c.centre, c.slope });
                ++rep.curvesMapped;
            }
            else {
                ++rep.curvesUnknown;
            }
        }
        return out;
    }

    bool NamesAgree(std::string_view file, std::string_view game) noexcept
    {
        if (file.empty() || file == game) return true;
        auto high = [](std::string_view s) {
            return std::any_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x80; });
        };
        if (!high(file) && !high(game)) return false;
        auto next = [](std::string_view s, std::size_t& i) -> int {
            while (i < s.size() && static_cast<unsigned char>(s[i]) >= 0x80) ++i;
            return i < s.size() ? static_cast<unsigned char>(s[i++]) : -1;
        };
        std::size_t i = 0, j = 0;
        while (true) {
            const int a = next(file, i);
            const int b = next(game, j);
            if (a != b) return false;
            if (a < 0) return true;
        }
    }

    BiasMode ParseBiasMode(std::string_view text) noexcept
    {
        auto eq = [text](std::string_view w) {
            if (text.size() != w.size()) return false;
            for (std::size_t i = 0; i < w.size(); ++i) {
                char c = text[i];
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                if (c != w[i]) return false;
            }
            return true;
        };
        if (eq("always")) return BiasMode::Always;
        if (eq("never")) return BiasMode::Never;
        return BiasMode::Match;
    }

    std::string_view BiasModeName(BiasMode m) noexcept
    {
        switch (m) {
        case BiasMode::Always: return "always";
        case BiasMode::Never: return "never";
        case BiasMode::Match: return "match";
        }
        return "match";
    }

    std::uint64_t StableKey(std::string_view kind, std::string_view a, std::string_view b) noexcept
    {
        std::uint64_t h = 1469598103934665603ull;
        auto mix = [&h](std::string_view s) {
            for (const char c : s) {
                h ^= static_cast<unsigned char>(c);
                h *= 1099511628211ull;
            }
        };
        mix(kind);
        h ^= 0x1F;
        h *= 1099511628211ull;
        mix(a);
        h ^= 0x1F;
        h *= 1099511628211ull;
        mix(b);
        return h;
    }

    std::string TauText(double tau)
    {
        char buf[32];
        const auto r = std::to_chars(buf, buf + sizeof(buf), tau, std::chars_format::general, 6);
        return std::string(buf, r.ptr);
    }
}
