# Huebox — *A Little Departure*

Single-player 3D physics puzzle game (Unreal Engine 5.8, C++). Coloured light pulses are routed through modules
(turn, colour shift, delay, mixer) to push, pull and brake a dancer on a small stage, and to break the coloured locks
on her way.

- Design: [ProjectManagement/Huebox_GDD.docx](ProjectManagement/Huebox_GDD.docx)
- Plan & checklist (deadline 2026-12-14): [ProjectManagement/CHECKLIST.md](ProjectManagement/CHECKLIST.md)
- C++ / memory-management notes: [Docs/Huebox_CPP_Notes.md](Docs/Huebox_CPP_Notes.md)

## Build & run
1. Install UE 5.8 and Visual Studio 2026 (Game development with C++).
2. Right-click `Huebox.uproject` → *Generate Visual Studio project files*, or just open the `.uproject` and let it build.
3. Maps (recreate with `Scripts/create_tutorial_map.py` and `Scripts/create_maps.py` if needed):
   - `/Game/Huebox/Maps/Lvl_MainMenu` — title screen; the packaged game starts here
   - `/Game/Huebox/Maps/Lvl_Tutorial` — Level 1, four exercises (the editor's startup map)
   - `/Game/Huebox/Maps/Lvl_Exhibit` — Level 2, the exhibition box
   - `/Game/Huebox/Maps/Lvl_Sandbox` — free building in first person

Controls (build view): mouse/WASD cell cursor · LMB place/collect · RMB collect · Q/E rotate · Z/X delay ∓0.25 s ·
wheel/number keys tray · Space run/pause · R reset · Tab/[ ] switch board · H help · Esc/P pause menu.
Sandbox: WASD move · mouse look · same build keys, Z/X also change a source's or brick's colour · K next skin.

## Tests
```
UnrealEditor-Cmd.exe Huebox.uproject -ExecCmds="Automation RunTests Huebox;Quit" -nullrhi -unattended -TestExit="Automation Test Queue Empty"
UnrealEditor-Cmd.exe Huebox.uproject /Game/Huebox/Maps/Lvl_Tutorial -game -nullrhi -HueSelfTest -unattended
UnrealEditor-Cmd.exe Huebox.uproject /Game/Huebox/Maps/Lvl_Exhibit -game -nullrhi -HueSelfTest -HueSelfTestSweep=3 -UseFixedTimeStep -FPS=60 -unattended
```
`-HueSelfTestSweep=N` also runs every solution delay N steps either side in real physics and logs the timing windows.
