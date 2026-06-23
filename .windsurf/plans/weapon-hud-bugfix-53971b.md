# Weapon HUD Bugfix Plan

Fix layout bugs in the WEAPONS HUD panel: overlapping weapon name / status text, and invisible slot numbers.

## Root Causes

- `label_x + 110.0f` is too narrow for long names like "Ballistic Cannon", causing the status text to overlap.
- Three-column `same_line` + `set_cursor_pos_x` layout is fragile with proportional fonts; the slot number "1" is tiny and easily clipped by panel padding.

## Fix

- Collapse slot + name into a single `bs_ui_text_colored` call (`"1  Ballistic Cannon"`), eliminating the first `same_line` / `set_cursor_pos_x` pair.
- Keep status on the same line via one remaining `same_line` + `set_cursor_pos_x(label_x + 160.0f)` call, giving long names ~100 extra pixels of breathing room.
- Adjust colours so the combined entry still shows selection state clearly.
