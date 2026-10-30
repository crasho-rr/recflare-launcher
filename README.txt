RecFlare Launcher
=================

hi! this gets rec room (build 20260323.14) onto RecFlare. no injector, no messing around
just hit play [:

how to use
----------
1. open "RecFlare Launcher.exe" (you can put it anywhere). this zip doesn't include the game
2. press DOWNLOAD and pick where it goes (it makes a "RecFlare 2026" folder there)
   it's a 4.5 GB download and about 6.8 GB unpacked, have 12 GB free
   if it stops, press download again and pick the same place, it picks up where it left off
3. press PLAY (desktop) or PLAY IN VR
   the first time it checks your game files which takes a few secs

what it does to your game folder
--------------------------------
- adds 2026loader.dll, 2026patch.dll and 2026patch.ini
  (if you already have a 2026patch.ini it keeps yours, that's where the server settings live)
- backs up RecRoom.exe as RecRoom.original.exe, then adds one import to RecRoom.exe so the
  patch loads by itself. after that you can even launch RecRoom.exe directly
  (use +forcemode:screen or +forcemode:vr, or just use the launcher)
- GameAssembly.dll / Referee.dll / UnityPlayer.dll are never touched

steam id: the little switch above the play button flips between spacewar (480, the default) and
rec room (471710). it writes steam_appid.txt in the game folder


shortcut tricks
---------------
"RecFlare Launcher.exe" --play       starts desktop mode straight away
"RecFlare Launcher.exe" --vr         starts vr straight away
"RecFlare Launcher.exe" --folder "D:\path\to\rec room"

if something breaks
-------------------
- "wrong rec room version": you need exactly 20260323.14
- "try running the launcher as admin": your game folder is somewhere windows protects
- game closes / won't connect: look at 2026loader.log and 2026patch.log in the game folder
  and send them to the RecFlare team
- windows smartscreen might complain since the exe isn't signed, click "more info" > "run anyway"

source: https://github.com/crasho-rr/recflare-launcher
made by crasho for RecFlare. not affiliated with rec room inc / against gravity
