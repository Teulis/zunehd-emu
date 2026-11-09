# Research scripts (as-is)

One-off scripts from the Session 9 GPU and audio work, kept for reference. They were written for the
original working folder and **still hard-code `C:\rea-work\...` paths** and the private capture folders
(`gui\`, `ape\`, `bse\`) that aren't in this repo. Expect to edit paths before running any of them.

- `gui/`: fragment-pipeline timing fits and register-dataflow tools for the 3D interpreter (`fpsym.py`,
  `fpfit*.py`, `fpdump.py`, `trscan.py`, `px.py`), the GUI session's run/build wrappers (`run.ps1`,
  `build.ps1`, `buildretry.ps1`), and the settings-hold repro steps (`holdsteps.txt`). See NOTES.md,
  "Session 9 (GUI vs device photos)".
- `ape/`: audio vector engine checks against reference IMDCT/DCT/synthesis (`chk_*.py`, `synref.py`),
  capture scanners, and the playback/simulator wrappers (`run.ps1`, `sim.ps1`, `build.ps1`). See NOTES.md,
  "Session 9: AVP audio vector engine". `ctlscan.py` and `scanconst.py` read AVP firmware dumps, which are
  not included.
