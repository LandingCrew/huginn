#pragma once

// =============================================================================
// FACETS OF A CANDIDATE (R11) -- the game side of core/CapFacets.h
// =============================================================================
// Looks a form up in the effect catalog (EffectCatalog::FindOrMapId) and
// answers core/CapFacets.h's questions for it. The slot classes
// (slot/SlotClassifier), the candidate filters, the override finders and the
// explanation label read an item through this, never through the old
// classifiers' tags, types or elements: those feed only the D2 legacy feature
// now (learning/legacy/; tools/prune/r8_check.py confines them).
//
// Threads: the catalog is immutable once Ready() and FindOrMapId locks its own
// late table, so any thread may call these. They read the form only on a late
// mapping (FindOrMapId), so call them where a form read is allowed: the update
// loop, the main thread, the console.
// =============================================================================

#include "candidate/CandidateTypes.h"
#include "core/CapFacets.h"

#include <functional>
#include <optional>

namespace Huginn::Effect
{
    /// What the catalog says of one form.
    struct FormFacets
    {
        bool known = false;   // the catalog has an entry (else kind and facets say nothing)
        Core::Effect::Kind kind = Core::Effect::Kind::Potion;
        Core::Facets::Set facets;
        int unlockReach = -1;   // 0.24.29: CatalogEntry::unlockReach (core/LockReach.h)
    };

    /// The catalog's answer for a form; known = false before the catalog is
    /// ready or for a form it does not map.
    [[nodiscard]] FormFacets FacetsOfForm(RE::FormID formID);

    /// The slot classes' subject for a candidate: its source, its school, its
    /// weapon reach (form facts the candidate carries) and the catalog's
    /// facets for its form.
    [[nodiscard]] Core::Facets::Subject SubjectOf(const Candidate::CandidateVariant& candidate);

#ifndef NDEBUG
    /// The Debug test suites (Tests.cpp) build candidates with made-up FormIDs
    /// the catalog does not hold. A resolver set here answers for a candidate
    /// the catalog has no entry for; nothing in the plugin sets one. Pass an
    /// empty function to clear it. It answers only on the thread that set it
    /// (the suites run synchronously on it): a pipeline tick on another
    /// thread beside an after-load suite still sees a real form the catalog
    /// does not map as unknown, not as a synthetic one. Guarded by a mutex.
    using TestResolver = std::function<std::optional<FormFacets>(const Candidate::CandidateVariant&)>;
    void SetTestResolver(TestResolver resolver);
#endif
}
