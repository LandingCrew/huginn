# Huginn docs

**Read the staleness note before trusting anything here.**

## Provenance

Most of this tree was recovered on 2026-08-29 from an untracked `docs copy/`
directory sitting beside `docs/` in the working tree. It had never been
committed — `git log --diff-filter=D` finds no trace of it — so CLAUDE.md had
been linking to files that existed on exactly one machine and nowhere in the
repository. That is what [roadmap.md](roadmap.md) #59 was about.

Two files in the snapshot were deliberately NOT imported:

| Snapshot file | Why |
|---|---|
| `architecture/0-pipeline.md` | Snapshot was v0.14.x; the tracked copy is v0.18.x and newer |
| `ROADMAP.md` | A v0.13.x roadmap dated 2026-02-27, superseded by [roadmap.md](roadmap.md). On a case-insensitive filesystem it would also have clobbered it |

Still missing, referenced but never found: `reviews/architecture-critique.md`.
The roadmap's whole "Architecture Critique" section points at it.

## Staleness

The snapshot describes **v0.13.x–v0.14.x** (roughly 2026-02). The plugin is at
**0.19.x**. Six months of pipeline, slot, Wheeler and display work are not
reflected here, and no accuracy pass has been done — importing these files put
them under version control so that drift becomes *visible*, which is not the
same as making them correct.

Treat every document here as a starting point to verify against `src/`, not as
a specification. Where a doc and the code disagree, the code is right.

Known-current, because they are maintained alongside the work:

- [roadmap.md](roadmap.md) and [roadmap-archive.md](roadmap-archive.md)
- [profiling/tracy-traces.md](profiling/tracy-traces.md)
- [playtest/LongPlaySoak.md](playtest/LongPlaySoak.md)
- [architecture/0-pipeline.md](architecture/0-pipeline.md) (v0.18.x)
- [refactor/wheeler-push-spikes.md](refactor/wheeler-push-spikes.md)

## Terminology

The learning system is a **contextual bandit**, not Q-learning — see
[architecture/4-contextual-bandits.md](architecture/4-contextual-bandits.md)
for the update rule that settles it. The docs were converted to bandit
vocabulary on 2026-08-29.

The **code identifiers were not renamed** and will not be: `FeatureQLearner`,
`QLearnerSerializer`, the `FQLW` cosave record and `hg reset qvalues` all keep
the historical name, because renaming them would break the cosave format and a
documented console command. Read "QLearner" in an identifier as "the learner".

## Map

| Path | Contents |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | Entry point — vision, system overview, doc map |
| [architecture/](architecture/) | Deep dives: pipeline, states, classifiers, candidate filtering, the bandit, slots, UI, dMenu, future work |
| [reference/](reference/) | Console commands, candidate system, performance, Scaleform build, Wheeler API headers |
| [compatibility/](compatibility/) | LoreRim, Survival Mode, unknown-spell handling, general mod compatibility |
| [changelog/](changelog/) | v0.5 through v0.13 release notes |
| [testing/](testing/) | Test index and profiling guide |
| [refactor/](refactor/) | Fix patterns, form registry, performance work, Wheeler push spikes |
| [limitations/](limitations/) | Known limits (soul gems) |
| [reviews/](reviews/) | Magic classification review |
| [profiling/](profiling/), [playtest/](playtest/) | Tracy captures, long-play soak protocol |
