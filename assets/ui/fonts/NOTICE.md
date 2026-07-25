# Font licensing NOTICE — bundled OFL fonts (safe to distribute)

Every `.ttf` in this directory is an open-source font released under the
**SIL Open Font License 1.1** (OFL). They are safe to redistribute with the game,
including in commercial builds, provided the OFL terms are met (keep this notice,
do not sell the fonts on their own, and do not use the Reserved Font Names for
modified versions). The full licence text is available from each project below.

The in-game UI (RmlUi) ships **three switchable font "kits"**. Each kit fills three
type roles — **display** (titles/headings/tabs), **body** (general UI text) and
**mono** (numeric / technical readouts). The active kit is chosen live from the
editor panel's *UI FONT KIT* combo (see `assets/ui/theme.rcss` → FONT KITS section).

| Kit         | Display        | Body          | Mono              |
|-------------|----------------|---------------|-------------------|
| Neon        | Orbitron       | Rajdhani      | Share Tech Mono   |
| Clean (def) | Chakra Petch   | Inter         | JetBrains Mono    |
| Minimal     | Exo 2          | Exo 2         | JetBrains Mono    |

## Bundled families (all SIL OFL 1.1)

- **Orbitron** — © The League of Moveable Type / Matt McInerney
- **Rajdhani** — © Indian Type Foundry
- **Share Tech Mono** — © Carrois Apostrophe
- **Chakra Petch** — © Cadson Demak
- **Inter** — © The Inter Project Authors (rsms)
- **JetBrains Mono** — © JetBrains s.r.o.
- **Exo 2** — © Natanael Gama (Ndiscover)

Weight-specific static instances are named `<family>-<weight>.ttf` (e.g.
`inter-600.ttf`). A few mid-weights advertise a distinct family name in their
metadata (e.g. `rajdhani-500.ttf` → "Rajdhani Medium", `chakra-petch-600.ttf`
→ "Chakra Petch SemiBold"); reference those names directly in RCSS if needed.

## How fonts are loaded

The loader `bs_rml_load_fonts("assets/ui/fonts")` enumerates **every** `.ttf`/`.otf`
in this folder at startup and registers each with RmlUi, reading the family name
from the font file itself. To add or swap a face, drop the `.ttf` here and reference
its family name via `font-family` in `assets/ui/*.rcss` — no code change required.
