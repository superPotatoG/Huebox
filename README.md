# Huebox — *A Little Departure*

Single-player 3D physics puzzle game (Unreal Engine 5.8, C++). Coloured light pulses are routed through modules
(turn, colour shift, delay, mixer) to push, pull and brake a small carriage carrying a mannequin.

- Design: [ProjectManagement/Huebox_GDD.docx](ProjectManagement/Huebox_GDD.docx)
- Plan & checklist (deadline 2026-12-14): [ProjectManagement/CHECKLIST.md](ProjectManagement/CHECKLIST.md)
- C++ / memory-management notes: [Docs/Huebox_CPP_Notes.md](Docs/Huebox_CPP_Notes.md)

## Build & run
1. Install UE 5.8 and Visual Studio 2026 (Game development with C++).
2. Right-click `Huebox.uproject` → *Generate Visual Studio project files*, or just open the `.uproject` and let it build.
3. Default map: `/Game/Huebox/Maps/Lvl_Tutorial` (recreate with `Scripts/create_tutorial_map.py` if needed).

Controls: WASD move · mouse look · LMB interact (collect / place) · Q/E rotate · Z/X delay ∓0.25 s · wheel/1-6 hotbar ·
G drop · Space run/pause · R reset · H help · Esc/P pause.

## Tests
```
UnrealEditor-Cmd.exe Huebox.uproject -ExecCmds="Automation RunTests Huebox;Quit" -nullrhi -unattended -TestExit="Automation Test Queue Empty"
UnrealEditor-Cmd.exe Huebox.uproject /Game/Huebox/Maps/Lvl_Tutorial -game -nullrhi -HueSelfTest -unattended
```
