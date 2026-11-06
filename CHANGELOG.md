# Changelog

## 1.0.1 (2026-11)
- Fixed: pressing **STOP** or **Overdub** while the MPC was playing could reload the preset and undo your DRIVE / TONE / MIX tweaks. The MPC's host briefly changes a plugin's first parameter when it re-prepares audio, and that was PRESET. That first slot now does nothing; PRESET is a new parameter at the end (same menu, same Q-Link). Saved projects open with their preset and settings as before.

## 1.0.0 (2026-11)
- First release: DRIVE, three CHARACTERs (soft, tape, hard), TONE, CEILING, LEVEL MATCH, MIX and OUTPUT, 2x oversampling, a live meter and 12 one-tap presets.
