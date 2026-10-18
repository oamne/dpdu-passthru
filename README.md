# dpdu-passthru
Fork of [JohnJocke/dpdu-passthru](https://github.com/JohnJocke/dpdu-passthru) with GMLAN CAN support for Saab 9-3 NG (9440).
D-PDU API driver for J2534 compatible devices. Currently supports only GM Tech2Win software.

## Changes in this fork
* **Dual-Architecture Installer**: Support for both 32-bit and 64-bit Windows in `install.bat`.
* **Uninstaller**: Added `uninstall.bat` for clean driver and registry cleanup.
* **Linux Cross-Compilation**: Build 32-bit Win32 DLL directly on Linux using MinGW-w64 (`./build_linux.sh`).
* **Early GMLAN Support**: Initial dual-bus CAN implementation (SW-CAN + HS-CAN) with dynamic bus switching for Saab 9-3 NG (9440).

# Currently working
* Any J2534 compatible interface with K-Line support (pin 7)
* Tested with:
  * XHorse Mini-VCI J2534
  * Mongoose Pro JLR
  * Mongoose Pro GM II
  * Scanmatik 2 Pro
  * Mongoose adapters are recommended
* Tech2Win
  * Saab OG9-5 with K-Line diagnostics (pre-2006 year models).
  * Saab OG9-3 2002
  * Saab 9-3 NG (9440) 2003-2011: GMLAN support (tested only with Scanmatik 2 Pro).
  * NOTE: The pins 7 and 8 of the DLC need to be shorted in order to access all diagnostic units in Saab OG9-5/OG9-3
* Protocols
  * ISO14230: Almost all Saab OG9-5/OG9-3 modules use this protocol
  * KW82: OG9-3 and old OG9-5 (pre MY2000?) SRS modules use this protocol
    * NOTE: So far tested only with OG9-3 2002 and Mongoose Pro JLR. XHorse Mini-VCI does not work. 
  * GMLAN dual-bus: SW-CAN (Pin 1 @ 33.3 kbps) & HS-CAN (Pins 6/14 @ 500 kbps) with dynamic bus switching for single-channel multiplexers (Scanmatik 2 Pro).

# Todo
* Test more with real cars and other J2534 adapters
* Find out why Saab 9-5 post-2006 fails communication with DICE through the K-Line 
* Test and validate stability on complex procedures (add/remove) on GMLAN

# Known issues
* T2Win will sometimes freeze if several T2Win menus are accessed in one session. There is no known workaround other than restarting T2Win each time when different menus are accessed.

# Instructions
## Prerequisities
* Install latest Microsoft Visual C++ Redistributable (x86): https://aka.ms/vs/17/release/vc_redist.x86.exe

## Install
1. Download latest release: https://github.com/oamne/dpdu-passthru/releases
1. Unzip package and run install.bat as administrator

## Configuration
* Installer uses recommended settings for Saab OG9-5/OG9-3
* Settings file can be found from C:\Users\Public\dpdu_settings.ini
*  Settings:
   * DisableTesterpresent: Set to 1 to disable sending testerpresent messages to the ECU
   * FixTesterpresentDestination: Set to 1 if D-PDU host sends testerpresent messages to wrong destination
   * AutoRestartComm: Set to 1 to automatically restart communication to correct destination
    
## Build
* **Linux (Cross-compilation)**:
  1. Install MinGW: `sudo apt-get install g++-mingw-w64-i686`
  2. Run `./build_linux.sh` to compile `driver.dll` and generate the deliverable zip.
* **Windows**:
  1. Build the project with Visual Studio 2022, using the Release x86 build configuration.
  2. Install the driver by running `installer/install.bat` as administrator.

---

## Credits & Pair-Programming

* **Original author & K-Line engine**: [JohnJocke](https://github.com/JohnJocke/dpdu-passthru).
* **GMLAN dual-bus extensions & Linux toolchain**: Built with AI Pair-Programming (**Antigravity / Google DeepMind AI**) for the international Saab community — [saabiste.net](https://saabiste.net).
