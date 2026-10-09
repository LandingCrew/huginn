"""Shared loader + player-facing filter for the three hg dump all CSVs.
Data files are untrusted: read as strings, parsed numerically here."""
import re
import pandas as pd

# Callers fill DUMPS {name: path to an hg dump all CSV} before load(name).
DUMPS = {}

ARCH = {-1:'None',0:'ValueModifier',1:'Script',2:'Dispel',3:'CureDisease',4:'Absorb',5:'DualValueModifier',6:'Calm',7:'Demoralize',8:'Frenzy',9:'Disarm',10:'CommandSummoned',11:'Invisibility',12:'Light',13:'Darkness',14:'NightEye',15:'Lock',16:'Open',17:'BoundWeapon',18:'SummonCreature',19:'DetectLife',20:'Telekinesis',21:'Paralysis',22:'Reanimate',23:'SoulTrap',24:'TurnUndead',25:'Guide',26:'WerewolfFeed',27:'CureParalysis',28:'CureAddiction',29:'CurePoison',30:'Concussion',31:'ValueAndParts',32:'AccumulateMagnitude',33:'Stagger',34:'PeakValueModifier',35:'Cloak',36:'Werewolf',37:'SlowTime',38:'Rally',39:'EnhanceWeapon',40:'SpawnHazard',41:'Etherealize',42:'Banish',43:'SpawnScriptedRef',44:'Disguise',45:'GrabActor',46:'VampireLord'}

NUM = ['playable','value','weight','playerCount','spellType','castingType','delivery','magickaCost','weaponType',
       'twoHanded','damage','speed','reach','critDamage','armorRating','soulCapacity','soulContained','lightRadius',
       'enchantmentCharge','effectIndex','archetype','effectDelivery','effectCasting','magnitude','duration','area',
       'effectCost','detrimental','hostile']
BAD = re.compile(r'\b(dummy|test|testing|donotuse|do not use|unused|placeholder|deleted)\b|^zz|DONOTUSE|TAKEME', re.I)


def load(lst):
    d = pd.read_csv(DUMPS[lst], dtype=str, keep_default_na=False, encoding='utf-8', encoding_errors='replace')
    if 'effectBaseCost' in d.columns and 'effectCost' not in d.columns:
        d = d.rename(columns={'effectBaseCost': 'effectCost'})
    for c in NUM:
        d[c + '_n'] = pd.to_numeric(d[c].replace('', None), errors='coerce')
    d['hasEff'] = d.effectFormID != ''
    d['fl'] = d.effectFlags.map(lambda s: int(s, 16) if s else 0)
    d['hide'] = (d.fl & 0x8000) > 0
    d['recover'] = (d.fl & 0x2) > 0
    d['det'] = d.detrimental_n == 1
    d['arch'] = d.archetype_n.map(lambda x: ARCH.get(int(x), '?') if pd.notna(x) else '')
    if 'effectDescription' not in d.columns:
        d['effectDescription'] = ''
    return d


def player_facing(d, spells='old'):
    """Union of the three analyses' filters, kept simple and per kind.
    Spells: player-castable types (Spell, Power, LesserPower, Voice), F&F or concentration,
    cost > 0 for plain spells, at least one visible effect. Scrolls: value > 0.
    Alchemy (ingredients dropped): playable. Gear: playable, value > 0, not HandToHand.
    v2: spells='tome' replaces the cost test for plain spells (type 0) with taughtByTome == 1;
    powers, lesser powers and shouts (2, 3, 11) keep their own rule (F&F/concentration,
    a visible effect, cost < 5000), since no tome teaches them. Abilities stay out."""
    d = d[d.kind != 'Ingredient']
    g = d.groupby(['kind', 'formID'])
    vis_any = g['hide'].transform(lambda h: (~h).any())
    k = d.kind
    keep = ~d.name.str.contains(BAD)
    sp = k == 'Spell'
    st = d.spellType_n
    base = st.isin([0, 2, 3, 11]) & d.castingType_n.isin([1, 2]) & vis_any & (d.magickaCost_n < 5000)
    if spells == 'tome' and 'taughtByTome' in d.columns:
        plain = (st == 0) & (d.taughtByTome == '1')
    else:
        plain = (st == 0) & ~(d.magickaCost_n <= 0.5)
    keep &= ~sp | (base & (plain | st.isin([2, 3, 11])))
    keep &= ~(k == 'Scroll') | (d.value_n > 0)
    keep &= ~k.isin(['Potion', 'Poison', 'Food']) | (d.playable_n == 1)
    keep &= ~k.isin(['Weapon', 'Armor', 'Ammo']) | ((d.playable_n == 1) & (d.value_n > 0))
    keep &= ~(k == 'Weapon') | (d.weaponType_n != 0)
    return d[keep].copy()
