RecFlare Launcher
=================

hi! this gets rec room (build 20260323.14) onto RecFlare. no injector, no messing around
just hit play [:

how to use
----------
1. open "RecFlare Launcher.exe" (you can put it anywhere). this zip doesn't include the game,
   you get that from steam, your login stays in steam the whole time
2. it looks for rec room in your steam libraries by itself
   - no rec room? press INSTALL VIA STEAM and let steam install it (it's free)
   - different build? press UPDATE VIA STEAM
   - already got rec room somewhere else? press USE MY OWN INSTALL and pick its folder
   leave the launcher open, it picks the game up when steam is done
3. press PLAY (desktop) or PLAY IN VR
   the first time it checks your game files which takes a few secs
   if it's your normal steam install it asks if you want a separate copy first, so a future
   steam update can't break RecFlare (6.8 GB, totally optional)

steam's current rec room is the right build (20260323.14). if that ever changes, click
"steam console download" at the bottom: it copies a download command and opens steam's console, you paste
it with ctrl+v and steam downloads the exact build (manifest 8867414017562658247).

what it does to your game folder
--------------------------------
- adds 2026loader.dll, 2026patch.dll and 2026patch.ini
  (if you already have a 2026patch.ini it keeps yours, that's where the server settings live)
- backs up RecRoom.exe as RecRoom.original.exe, then adds one import to RecRoom.exe so the
  patch loads by itself. after that you can even launch RecRoom.exe directly
  (use +forcemode:screen or +forcemode:vr, or just use the launcher)
- GameAssembly.dll / Referee.dll / UnityPlayer.dll are never touched

to undo: press "undo recflare" in the launcher. heads up: if you set up your normal steam install,
launching rec room from steam also loads RecFlare until you undo (or steam verifies the files)

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
