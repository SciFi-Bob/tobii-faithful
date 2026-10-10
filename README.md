# tobii-faithful

Linux runtime for the Tobii Eye Tracker 5: head tracking and gaze for games
that ship with Tobii's own game integration DLL.

I have created this so I can use my beloved ET5 on Linux having finally made the
jump for my main PC this year.  It was developed on Nobara and tested against
Star Citizen (Wine) and MS Flightsim 2020 (Steam).

I credit njmill and their repo https://github.com/njmill/tobii-linux for getting me started, I was going to make this a pull request
but over the last couple of weeks I have changed just about everything I think :-)

The aim, that I think that I have managed, was to build as faithfully to the official software
as I possibly can.

## Installing

Download the package for your distribution from the latest release and install it.
Fedora:

    sudo dnf install ./tobii-faithful-*.rpm

Arch Linux and Garuda:

    sudo pacman -U ./tobii-faithful-*.pkg.tar.zst

After the installation you should have a 'Faithful Tobii' tray icon (if you do not, start Faithful Tobii from the program menu) that will show you a closed
eye (no clients) and right clicking that will offer you the option to "Open Faithful Tobii" where
you can select your screen, calibrate and make sure that your games were detected.  It is critical that
any game you want to use is detected.  Only games that ship with the official Tobii integration will be
detected.
Once you are done you can close the 'Faithful Tobii' window and the device should go back to sleep.  You can
tell the link to a game is working if the device wakes when you start the game.

## Upgrading

Download the newer package and upgrade in place:

    sudo dnf upgrade ./tobii-faithful-*.rpm             # Fedora
    sudo pacman -U ./tobii-faithful-*.pkg.tar.zst       # Arch, Garuda

Your calibration and settings are kept. The tracker service and game adapter
restart on their own; the tray and the settings window use the new version the
next time they start, so log out and back in, or quit and reopen them.

## What the package puts on your system

| Path | What it is |
|---|---|
| `/usr/bin/tobiid` | the service that owns the tracker; opens it only while a client needs it |
| `/usr/bin/tobii-adapter` | answers games that ship Tobii's integration DLL |
| `/usr/bin/tobii-manager` | calibration and live view |
| `/usr/bin/tobii-tray` | tray icon |
| `/usr/bin/tobiictl` | command-line client |
| `/usr/lib64/tobii-faithful/tobii-sesp-bridge.exe` | run inside the game's Wine prefix for head pose |
| `/usr/lib64/tobii-faithful/libonnxruntime.so.1` | ONNX Runtime, used only by these programs |
| `/usr/share/tobii-faithful/models/` | face detection models |
| `/usr/lib/udev/rules.d/60-tobii-eyetracker5.rules` | gives the logged-in user access to the tracker |
| `/usr/lib/systemd/user/tobiid.service`, `tobii-adapter.service` | user services |
| `/usr/lib/systemd/user-preset/80-tobii-faithful.preset` | enables those services (Fedora; on Arch the install enables them) |
| `/etc/xdg/autostart/tobii-tray.desktop` | starts the tray with your session |
| `/usr/share/applications/tobii-faithful.desktop`, icon | menu entry; starts the tray |
| `/usr/share/applications/tobii-manager.desktop` | hidden; gives the settings window its icon |

On Arch, `/usr/lib64/` is `/usr/lib/`. Your settings are kept in `~/.config/tobii-faithful/`. Removing the package
removes everything in the table and stops the running programs; it leaves your
settings, which you can delete by hand.

## This source

This repository is the source of the programs in the package, published so you
can review what they do. It is generated from the project's development tree
for each release and contains exactly the files the shipped programs are
compiled from. The vendor handshake constants are not
included; the stub in their place builds, and a build from this tree cannot
open the tracker's features correctly.

`packaging/tobii-faithful.spec` is the spec the release RPM is built with, and
`packaging/PKGBUILD` with `packaging/tobii-faithful.install` the Arch package,
including what each runs when you install and remove it.
`packaging/build-deps.sh` builds the OpenCV and ONNX Runtime both packages carry.

## Licence

Apache Licence 2.0; see `LICENSE`. The package also contains MediaPipe face
models and data, Dear ImGui, Eigen, OpenCV, ONNX Runtime and the mingw-w64
runtime under their own
terms: see `THIRD-PARTY-NOTICES.md` and `licenses/`. The package installs the
same files under `/usr/share/licenses/tobii-faithful/`.

## Disclaimer

### Not affiliated with Tobii

This project is not affiliated with, endorsed by, sponsored by or supported by
Tobii AB or any of its subsidiaries. "Tobii" and related marks are trademarks of
their respective owners, used here only to identify the hardware this software
works with.

### No warranty

This software is provided "as is", without warranty of any kind, express or
implied, including but not limited to the warranties of merchantability, fitness
for a particular purpose and non-infringement. In no event shall the authors or
contributors be liable for any claim, damages or other liability, whether in an
action of contract, tort or otherwise, arising from or in connection with this
software or its use.

It communicates with your hardware directly and emulates interfaces that game
software expects to find. It may stop working at any time if the hardware, its
firmware, a driver or a game changes. Use it at your own risk.
