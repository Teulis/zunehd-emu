param([string]$Cap = 'C:\rea-work\ape\cap1', [int]$First = 1, [int]$Last = 300)
& C:\rea-work\tools\apesim.exe run $Cap $First $Last C:\rea-work\ape\sim.raw
py -3 -I C:\rea-work\tools\raw2wav.py C:\rea-work\ape\sim.raw C:\rea-work\ape\sim.wav
py -3 -I C:\rea-work\tools\pcmcmp.py C:\rea-work\ape\sim.wav C:\rea-work\bse\ref.raw
