# Unknown Spell Patterns (v0.7.9)

This document tracks spell patterns that are currently classified as `Unknown` by the automatic classifier. These patterns are scheduled for implementation in v0.7.9 (Mod Compatibility).

---

## Pattern Analysis Summary

From stress testing with 236 spells (Lorerim + spell mod stack):

| Metric | Count |
|--------|-------|
| Total Spells | 236 |
| Successfully Classified | 178 (75%) |
| Unknown | 58 (25%) |

---

## Identified Patterns

### High Priority (Common Vanilla/DLC Spells)

| Pattern | Example Spells | Suggested Type | Suggested Tags |
|---------|----------------|----------------|----------------|
| `reanimate` | Reanimate Corpse, Reanimate Zombie, Dread Zombie | `Summon` | `Summon`, `Undead` |
| `soul trap` | Soul Trap, Soul Split | `Utility` | `Soul` |
| `turn undead` | Turn Lesser Undead, Turn Greater Undead | `Debuff` | `AntiUndead`, `Fear` |
| `clairvoyance` | Clairvoyance | `Utility` | `Detection` |
| `courage` | Courage, Rally | `Buff` | `Illusion`, `Fear` |

### Medium Priority (Common Mod Spells)

| Pattern | Example Spells | Suggested Type | Suggested Tags |
|---------|----------------|----------------|----------------|
| `feather` | Feather on Self, Feather on Target, Featherfall | `Buff` | `Movement` |
| `night` + `eye/vision` | Transmute Night Eye, Darkvision, Night Eye | `Buff` | `Vision` |
| `knock` | Knock | `Utility` | `Alteration` |
| `levitate` | Levitate | `Buff` | `Movement` |
| `slowfall` | Slowfall | `Buff` | `Movement` |

### Lower Priority (Mod-Specific)

| Pattern | Example Spells | Suggested Type | Suggested Tags |
|---------|----------------|----------------|----------------|
| `absorb` | Absorb Health, Absorb Magicka, Absorb Stamina | `Damage` | `Drain`, `Restoration` |
| `transmute` | Transmute (ore), Transmute Muscles | varies | context-dependent |
| `detect` | Detect Life, Detect Dead, Detect Undead | `Utility` | `Detection` |
| `reflect` | Reflect Damage, Spell Reflection | `Buff` | `Defensive` |
| `dispel` | Dispel Magic | `Utility` | `Debuff` |
| `silence` | Silence | `Debuff` | `CrowdControl` |

---

## Implementation Notes

### Tag-Based Derivation Pattern

Following the established pattern from v0.7.2, new patterns should:

1. **Add name detection** in `DetermineSpellTags()`:
   ```cpp
   if (NameContains(name, "reanimate")) {
       tags |= SpellTag::Summon;  // Reuse existing Summon tag
   }
   ```

2. **Leverage existing derivation** in `DeriveSpellTypeFromTags()` where possible

3. **Only add new tags** if no existing tag maps to the correct SpellType

### Recommended New Tags (v0.7.9)

| New Tag | Bit Position | Derives To |
|---------|--------------|------------|
| `SpellTag::Soul` | TBD | `SpellType::Utility` |
| `SpellTag::Detection` | TBD | `SpellType::Utility` |
| `SpellTag::Movement` | TBD | `SpellType::Buff` |

### Reusable Existing Tags

| Pattern | Reuse Tag | Rationale |
|---------|-----------|-----------|
| `reanimate` | `SpellTag::Summon` | Already derives to `SpellType::Summon` |
| `feather`/`slowfall` | `SpellTag::Shield` | Environmental protection buffs |
| `night eye`/`darkvision` | `SpellTag::Invisibility` | Vision-related buffs |
| `courage`/`rally` | (new) | Fear/illusion control |

---

## Testing Checklist

After implementing patterns, verify with stress test log:

- [ ] Reanimate Corpse → `type=Summon`
- [ ] Soul Trap → `type=Utility`
- [ ] Turn Lesser Undead → `type=Debuff`
- [ ] Clairvoyance → `type=Utility`
- [ ] Courage → `type=Buff`
- [ ] Feather on Self → `type=Buff`
- [ ] Night Eye variants → `type=Buff`
- [ ] Knock → `type=Utility`

---

## Related Files

| File | Purpose |
|------|---------|
| `src/spell/SpellClassifier.cpp` | Pattern detection and derivation |
| `src/spell/SpellData.h` | `SpellTag` enum definition |
| `docs/architecture/2-classifiers.md` | Classification system documentation |

---

## Version History

| Version | Changes |
|---------|---------|
| v0.7.9 | Initial pattern documentation (this file) |
| v0.7.2 | Added `waterbreath`, `open`, Sun override patterns |

---

## See Also

- [mod-compatibility.md](mod-compatibility.md) - Override INI format for manual classification
- [../architecture/classifiers.md](../architecture/2-classifiers.md) - Classifier architecture
- [../ROADMAP.md](../ROADMAP.md) - v0.7.9 milestone details
