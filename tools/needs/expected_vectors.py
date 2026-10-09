"""Independent reference (test oracle) for the need vector.

Reads need snapshots in the text record format and writes, per snapshot and
per need, the curve input x and the need value y = curve(x).

Written ONLY from:
  * docs/architecture/9-data/needs.csv  (id, curve_kind, curve_p1, curve_p2,
    r3_input -- the input formula of each need),
  * src/core/NeedSnapshot.h             (field names, types and defaults;
    NEVER = kNever = 1e6),
  * the written curve spec (below).
It was deliberately NOT derived from the C++ evaluator, the C++ curve code or
the C++ snapshot reader, so that it can be used to check them.

Curves (p1 = curve_p1, p2 = curve_p2; output clamped to [0,1]; NaN x -> 0):
  logistic    y = 1 / (1 + exp(-p2 * (x - p1)))      (p2 < 0 falls)
  linear      y = clamp((x - p1) / (p2 - p1), 0, 1)    (p2 < p1 falls)
  quadratic   y = linear(x) ** 2
  saturating  y = 0 if x <= 0 else min(ln(1 + p1*x) / ln(1 + p1), 1)
  gaussian    y = exp(-(x - p1)**2 / (2 * p2**2))
  step        y = 1 if x >= p1 else 0
  decay       y = 1 if x <= 0 else exp(-x / p1)
Arithmetic is Python double. Snapshot float fields are first rounded to
IEEE float32 (they are 32-bit in the C++ record); int and bool fields are
exact integers (bools 0/1); curve parameters are used as doubles.

Snapshot text format (one or more blocks):
  # comment            (lines starting with '#'; blank lines ignored)
  snapshot <label>
  <fieldName> <value>  (any field may be omitted -> NeedSnapshot.h default)
  end
Unknown fields, duplicate fields, duplicate labels, bad values and
unterminated blocks are errors.

Usage:
  python -I tools/needs/expected_vectors.py <snapshots.txt> [-o <out.csv>]
Writes CSV (LF line endings) with header `label,need,input,value`, one row per
snapshot per need in needs.csv order; input and value are repr() of the
Python float. Without -o the CSV goes to stdout.
"""
import csv
import io
import math
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NEEDS_CSV = os.path.join(ROOT, "docs", "architecture", "9-data", "needs.csv")

NEVER = 1.0e6  # kNever (exactly representable in float32)

# ---------------------------------------------------------------------------
# Snapshot fields, transcribed from src/core/NeedSnapshot.h:
# name -> (type, default). Types: 'f' float32, 'i' int, 'b' bool, 'u' uint32.
# ---------------------------------------------------------------------------
FIELDS = {
    # Vitals
    "health": ("f", 1.0), "magicka": ("f", 1.0), "stamina": ("f", 1.0),
    "magickaHeld": ("f", 1.0), "staminaHeld": ("f", 1.0),
    "maxHealth": ("f", 100.0), "maxMagicka": ("f", 100.0), "maxStamina": ("f", 100.0),
    "damageRate": ("f", 0.0), "healingRate": ("f", 0.0),
    "magickaUsageRate": ("f", 0.0), "magickaRegenRate": ("f", 0.0),
    "staminaUsageRate": ("f", 0.0), "staminaRegenRate": ("f", 0.0),
    "restoreHealthPending": ("f", 0.0), "restoreMagickaPending": ("f", 0.0),
    "restoreStaminaPending": ("f", 0.0),
    # Elemental threat
    "dmgFire": ("f", 0.0), "dmgFrost": ("f", 0.0), "dmgShock": ("f", 0.0),
    "dmgMagic": ("f", 0.0), "dmgPhysical": ("f", 0.0),
    "castFireAgo": ("f", NEVER), "castFrostAgo": ("f", NEVER), "castShockAgo": ("f", NEVER),
    # Status
    "poisoned": ("b", 0), "diseased": ("b", 0), "drainHealth": ("b", 0),
    "drainMagicka": ("b", 0), "drainStamina": ("b", 0), "regenSuppressed": ("b", 0),
    "encumbrance": ("f", 0.0), "vampireStage": ("i", 0),
    # Survival
    "survival": ("b", 0), "hungerStage": ("i", 0), "coldStage": ("i", 0),
    "fatigueStage": ("i", 0), "hungerRaw": ("f", -1.0), "coldRaw": ("f", -1.0),
    "fatigueRaw": ("f", -1.0), "warmth": ("f", 0.0),
    # Combat
    "inCombat": ("b", 0), "combatStartAgo": ("f", NEVER), "combatEndAgo": ("f", NEVER),
    "enemyCount": ("i", 0), "enemiesNear256": ("i", 0), "closestEnemy": ("f", -1.0),
    "anyCasting": ("b", 0), "targetCaster": ("b", 0), "targetArcher": ("b", 0),
    "targetHealth": ("f", -1.0), "soleHostileTtk": ("f", -1.0), "families": ("u", 0),
    "hostileSummoned": ("b", 0),
    # Ally
    "followerPresent": ("b", 0), "followerBleedout": ("b", 0),
    # Environment
    "light": ("f", 1.0), "openDaylight": ("b", 0), "underwater": ("b", 0),
    "submergedFor": ("f", 0.0), "swimming": ("b", 0), "fallDepth": ("f", 0.0),
    "dropAhead": ("f", -1.0), "lock": ("b", 0), "workstation": ("i", 0),
    "oreVein": ("b", 0), "merchant": ("b", 0),
    # Equipment
    "enchantedWeapon": ("b", 0), "weaponCharge": ("f", 1.0), "bow": ("b", 0),
    "crossbow": ("b", 0), "arrows": ("i", 0), "bolts": ("i", 0),
    "melee": ("b", 0), "spell": ("b", 0), "staff": ("b", 0), "shield": ("b", 0),
    "oneHanded": ("b", 0), "twoHanded": ("b", 0), "handSchools": ("u", 0),
    # Activity
    "sneaking": ("b", 0), "mounted": ("b", 0),
}

INT32_MIN, INT32_MAX = -(2 ** 31), 2 ** 31 - 1
UINT32_MAX = 2 ** 32 - 1


def f32(v):
    """Round a Python float to IEEE float32 (raises on overflow)."""
    return struct.unpack("<f", struct.pack("<f", v))[0]


def parse_value(name, kind, text, where):
    try:
        if kind == "f":
            return f32(float(text))
        if kind == "b":
            if text not in ("0", "1"):
                raise ValueError("bool must be 0 or 1")
            return int(text)
        v = int(text, 10)
        if kind == "i" and not INT32_MIN <= v <= INT32_MAX:
            raise ValueError("out of int32 range")
        if kind == "u" and not 0 <= v <= UINT32_MAX:
            raise ValueError("out of uint32 range")
        return v
    except (ValueError, OverflowError) as e:
        raise SystemExit(f"{where}: field {name}: bad value {text!r} ({e})")


def default_snapshot():
    return {name: (f32(d) if kind == "f" else d) for name, (kind, d) in FIELDS.items()}


def read_snapshots(path):
    snaps = []  # list of (label, dict)
    labels = set()
    cur = None
    cur_label = None
    seen = None
    with open(path, encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            line = raw.strip()
            where = f"{path}:{n}"
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if parts[0] == "snapshot":
                if cur is not None:
                    raise SystemExit(f"{where}: 'snapshot' inside block {cur_label!r} (missing 'end')")
                if len(parts) != 2:
                    raise SystemExit(f"{where}: expected 'snapshot <label>'")
                cur_label = parts[1]
                if cur_label in labels:
                    raise SystemExit(f"{where}: duplicate label {cur_label!r}")
                labels.add(cur_label)
                cur = default_snapshot()
                seen = set()
            elif parts[0] == "end":
                if cur is None or len(parts) != 1:
                    raise SystemExit(f"{where}: stray 'end'")
                snaps.append((cur_label, cur))
                cur = None
            else:
                if cur is None:
                    raise SystemExit(f"{where}: field line outside a snapshot block")
                if len(parts) != 2:
                    raise SystemExit(f"{where}: expected '<field> <value>'")
                name, text = parts
                if name not in FIELDS:
                    raise SystemExit(f"{where}: unknown field {name!r}")
                if name in seen:
                    raise SystemExit(f"{where}: duplicate field {name!r}")
                seen.add(name)
                cur[name] = parse_value(name, FIELDS[name][0], text, where)
    if cur is not None:
        raise SystemExit(f"{path}: block {cur_label!r} has no 'end'")
    return snaps


# ---------------------------------------------------------------------------
# r3_input formulas, one per needs.csv id (transcribed from the r3_input
# column). s is the snapshot dict; every result is a Python float.
# ---------------------------------------------------------------------------
def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def bit(word, n):
    return float((word >> n) & 1)


def per_max(num, den):
    """'<num> / <den>; 0 if <den> <= 0'."""
    return 0.0 if den <= 0 else num / den


def pending_ratio(pending, frac, maxv):
    """'0 if pending <= 0 or (1 - frac) * max < 1; else min(pending / ((1 - frac) * max), 10)'
    (0.23.16: under one point missing there is nothing to cover)."""
    deficit = (1.0 - frac) * maxv
    if pending <= 0 or not deficit >= 1.0:
        return 0.0
    return min(pending / deficit, 10.0)


def survival_meter(s, raw, stage):
    """'0 if not survival; else raw / 1000 if raw >= 0; else clamp(stage, 0, 5) / 5'."""
    if not s["survival"]:
        return 0.0
    if s[raw] >= 0:
        return s[raw] / 1000.0
    return clamp(s[stage], 0, 5) / 5.0


def enemy_distance(s):
    """'4096 if closestEnemy < 0 else closestEnemy'."""
    return 4096.0 if s["closestEnemy"] < 0 else s["closestEnemy"]


def enemy_far_distance(s):
    """'0 if closestEnemy < 0 else closestEnemy' (no hostile is not far)."""
    return 0.0 if s["closestEnemy"] < 0 else s["closestEnemy"]


FORMULAS = {
    # Vitals
    "health_deficit": lambda s: 1.0 - s["health"],
    "magicka_deficit": lambda s: 1.0 - s["magickaHeld"],
    "stamina_deficit": lambda s: 1.0 - s["staminaHeld"],
    "health_falling": lambda s: per_max(s["damageRate"] - s["healingRate"], s["maxHealth"]),
    "magicka_burn": lambda s: per_max(s["magickaUsageRate"] - s["magickaRegenRate"], s["maxMagicka"]),
    "stamina_burn": lambda s: per_max(s["staminaUsageRate"] - s["staminaRegenRate"], s["maxStamina"]),
    "restore_pending_health": lambda s: pending_ratio(s["restoreHealthPending"], s["health"], s["maxHealth"]),
    "restore_pending_magicka": lambda s: pending_ratio(s["restoreMagickaPending"], s["magicka"], s["maxMagicka"]),
    "restore_pending_stamina": lambda s: pending_ratio(s["restoreStaminaPending"], s["stamina"], s["maxStamina"]),
    # Elemental threat
    "fire_damage_rate": lambda s: per_max(s["dmgFire"], s["maxHealth"]),
    "frost_damage_rate": lambda s: per_max(s["dmgFrost"], s["maxHealth"]),
    "shock_damage_rate": lambda s: per_max(s["dmgShock"], s["maxHealth"]),
    "magic_damage_rate": lambda s: per_max(s["dmgMagic"], s["maxHealth"]),
    "physical_damage_rate": lambda s: per_max(s["dmgPhysical"], s["maxHealth"]),
    "enemy_casting_fire": lambda s: s["castFireAgo"],
    "enemy_casting_frost": lambda s: s["castFrostAgo"],
    "enemy_casting_shock": lambda s: s["castShockAgo"],
    # Status
    "poisoned": lambda s: float(s["poisoned"]),
    "diseased": lambda s: float(s["diseased"]),
    "drain_health": lambda s: float(s["drainHealth"]),
    "drain_magicka": lambda s: float(s["drainMagicka"]),
    "drain_stamina": lambda s: float(s["drainStamina"]),
    "regen_suppressed": lambda s: float(s["regenSuppressed"]),
    "healing_blocked": lambda s: 0.0,  # deferred
    "encumbrance": lambda s: s["encumbrance"],
    "vampire_sun_exposure": lambda s: 1.0 if (s["vampireStage"] >= 3 and s["openDaylight"]) else 0.0,
    # Survival
    "hunger": lambda s: survival_meter(s, "hungerRaw", "hungerStage"),
    "thirst": lambda s: 0.0,  # deferred
    "cold": lambda s: survival_meter(s, "coldRaw", "coldStage"),
    "fatigue": lambda s: survival_meter(s, "fatigueRaw", "fatigueStage"),
    "warmth_deficit": lambda s: clamp(1.0 - s["warmth"] / 200.0, 0.0, 1.0) if s["survival"] else 0.0,
    # Enemy
    "in_combat": lambda s: float(s["inCombat"]),
    "combat_onset": lambda s: s["combatStartAgo"] if s["inCombat"] else NEVER,
    "enemy_count": lambda s: s["enemyCount"] / 6.0,
    "enemy_close": enemy_distance,
    "enemy_mid": enemy_distance,
    "enemy_far": enemy_far_distance,
    "surrounded": lambda s: s["enemiesNear256"] / 3.0,
    "enemy_casting": lambda s: float(s["anyCasting"]),
    "target_caster": lambda s: float(s["targetCaster"]),
    "target_archer": lambda s: float(s["targetArcher"]),
    "target_health_low": lambda s: 0.0 if s["targetHealth"] < 0 else 1.0 - s["targetHealth"],
    "boss_fight": lambda s: s["soleHostileTtk"] if (s["enemyCount"] == 1 and s["soleHostileTtk"] >= 0) else 0.0,
    "target_magicka_low": lambda s: 0.0,  # deferred
    "target_stamina_low": lambda s: 0.0,  # deferred
    # Target type (families bits, Family enum order)
    "target_humanoid": lambda s: bit(s["families"], 0),
    "target_undead": lambda s: bit(s["families"], 1),
    "target_daedra": lambda s: bit(s["families"], 2),
    "target_dragon": lambda s: bit(s["families"], 3),
    "target_construct": lambda s: bit(s["families"], 4),
    "target_animal": lambda s: bit(s["families"], 5),
    "target_arthropod": lambda s: bit(s["families"], 6),
    "target_troll": lambda s: bit(s["families"], 7),
    "target_giant": lambda s: bit(s["families"], 8),
    "target_werebeast": lambda s: bit(s["families"], 9),
    "target_monster": lambda s: bit(s["families"], 10),
    "target_spectral": lambda s: bit(s["families"], 11),
    "target_element_fire": lambda s: bit(s["families"], 12),
    "target_element_frost": lambda s: bit(s["families"], 13),
    "target_element_shock": lambda s: bit(s["families"], 14),
    "target_summoned": lambda s: float(s["hostileSummoned"]),
    # Ally
    "ally_injured": lambda s: float(s["followerBleedout"]),
    "follower_present": lambda s: float(s["followerPresent"]),
    # Environment
    "darkness": lambda s: 0.0 if s["openDaylight"] else 1.0 - s["light"],
    "underwater": lambda s: s["submergedFor"] / 20.0 if s["underwater"] else 0.0,
    "swimming": lambda s: float(s["swimming"]),
    "falling": lambda s: s["fallDepth"],
    "drop_ahead": lambda s: 0.0 if s["dropAhead"] < 0 else s["dropAhead"],
    "lock_in_crosshair": lambda s: float(s["lock"]),
    "workstation_smithing": lambda s: 1.0 if s["workstation"] == 1 else 0.0,
    "workstation_enchanting": lambda s: 1.0 if s["workstation"] == 2 else 0.0,
    "workstation_alchemy": lambda s: 1.0 if s["workstation"] == 3 else 0.0,
    "ore_vein_in_crosshair": lambda s: float(s["oreVein"]),
    "merchant_in_crosshair": lambda s: float(s["merchant"]),
    # Equipment
    "weapon_charge_deficit": lambda s: 1.0 - s["weaponCharge"] if s["enchantedWeapon"] else 0.0,
    "ammo_low": lambda s: (max(0.0, 1.0 - s["arrows"] / 25.0) if s["bow"]
                           else max(0.0, 1.0 - s["bolts"] / 25.0) if s["crossbow"]
                           else 0.0),
    "hands_empty": lambda s: 0.0 if (s["melee"] or s["bow"] or s["crossbow"] or s["spell"] or s["staff"]) else 1.0,
    # Loadout
    "loadout_one_handed": lambda s: float(s["oneHanded"]),
    "loadout_two_handed": lambda s: float(s["twoHanded"]),
    "loadout_archery": lambda s: 1.0 if (s["bow"] or s["crossbow"]) else 0.0,
    "loadout_staff": lambda s: float(s["staff"]),
    "loadout_shield": lambda s: float(s["shield"]),
    "loadout_destruction": lambda s: bit(s["handSchools"], 0),
    "loadout_conjuration": lambda s: bit(s["handSchools"], 1),
    "loadout_restoration": lambda s: bit(s["handSchools"], 2),
    "loadout_alteration": lambda s: bit(s["handSchools"], 3),
    "loadout_illusion": lambda s: bit(s["handSchools"], 4),
    # Activity
    "sneaking": lambda s: float(s["sneaking"]),
    "sneak_detected": lambda s: 0.0,  # deferred
    "mounted": lambda s: float(s["mounted"]),
    "downtime": lambda s: s["combatEndAgo"] if (not s["inCombat"] and s["enemyCount"] == 0) else 0.0,
    "combat_ended_recent": lambda s: s["combatEndAgo"] if not s["inCombat"] else NEVER,
}

DEFERRED = {"healing_blocked", "thirst", "target_magicka_low", "target_stamina_low", "sneak_detected"}


# ---------------------------------------------------------------------------
# Curves
# ---------------------------------------------------------------------------
def _clamp01(y):
    if math.isnan(y):
        return 0.0
    return clamp(y, 0.0, 1.0)


def _linear(x, p1, p2):
    return clamp((x - p1) / (p2 - p1), 0.0, 1.0)


def _logistic(x, p1, p2):
    z = -p2 * (x - p1)
    if z > 709.0:  # exp overflows double: 1 / (1 + inf) = 0
        return 0.0
    return 1.0 / (1.0 + math.exp(z))


def curve(kind, p1, p2, x):
    if math.isnan(x):
        return 0.0
    if kind == "logistic":
        y = _logistic(x, p1, p2)
    elif kind == "linear":
        y = _linear(x, p1, p2)
    elif kind == "quadratic":
        y = _linear(x, p1, p2) ** 2
    elif kind == "saturating":
        y = 0.0 if x <= 0 else min(math.log(1.0 + p1 * x) / math.log(1.0 + p1), 1.0)
    elif kind == "gaussian":
        y = math.exp(-((x - p1) ** 2) / (2.0 * p2 * p2))
    elif kind == "step":
        y = 1.0 if x >= p1 else 0.0
    elif kind == "decay":
        y = 1.0 if x <= 0 else math.exp(-x / p1)
    else:
        raise SystemExit(f"unknown curve kind {kind!r}")
    return _clamp01(y)


def read_needs():
    with open(NEEDS_CSV, encoding="utf-8", newline="") as f:
        rows = list(csv.DictReader(f))
    needs = []
    for r in rows:
        nid = r["id"]
        kind = r["curve_kind"]
        p1, p2 = float(r["curve_p1"]), float(r["curve_p2"])
        if kind in ("linear", "quadratic") and p1 == p2:
            raise SystemExit(f"{nid}: {kind} with p1 == p2")
        if kind in ("gaussian",) and p2 == 0:
            raise SystemExit(f"{nid}: gaussian with p2 == 0")
        if kind in ("decay",) and p1 == 0:
            raise SystemExit(f"{nid}: decay with p1 == 0")
        if kind == "saturating" and p1 <= 0:
            raise SystemExit(f"{nid}: saturating with p1 <= 0")
        deferred = r["r3_input"].strip().startswith("0 (deferred")
        if deferred != (nid in DEFERRED):
            raise SystemExit(f"{nid}: deferred status disagrees with the formula table")
        needs.append((nid, kind, p1, p2))
    ids = [n[0] for n in needs]
    if len(set(ids)) != len(ids):
        raise SystemExit("needs.csv: duplicate id")
    missing = [i for i in ids if i not in FORMULAS]
    extra = [i for i in FORMULAS if i not in set(ids)]
    if missing or extra:
        raise SystemExit(f"formula table mismatch: no formula for {missing}; not in csv {extra}")
    return needs


def evaluate(needs, snap):
    out = []
    for nid, kind, p1, p2 in needs:
        x = float(FORMULAS[nid](snap))
        out.append((nid, x, curve(kind, p1, p2, x)))
    return out


def main(argv):
    args = argv[1:]
    out_path = None
    if "-o" in args:
        i = args.index("-o")
        if i + 1 >= len(args):
            raise SystemExit("-o needs a path")
        out_path = args[i + 1]
        del args[i:i + 2]
    if len(args) != 1:
        raise SystemExit(__doc__)
    needs = read_needs()
    snaps = read_snapshots(args[0])
    buf = io.StringIO()
    w = csv.writer(buf, lineterminator="\n")
    w.writerow(["label", "need", "input", "value"])
    for label, snap in snaps:
        for nid, x, y in evaluate(needs, snap):
            w.writerow([label, nid, repr(x), repr(y)])
    if out_path:
        with open(out_path, "w", encoding="utf-8", newline="") as f:
            f.write(buf.getvalue())
    else:
        sys.stdout.buffer.write(buf.getvalue().encode("utf-8"))  # LF, no newline translation


if __name__ == "__main__":
    main(sys.argv)
