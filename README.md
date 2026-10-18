# symbos-teletekst
A Teletekst app for Symbos

<a href="https://msx.pics/images/2026/10/17/WhatsApp-Image-2026-10-17-at-20.15.41.md.jpeg "><img src="https://msx.pics/images/2026/10/17/WhatsApp-Image-2026-10-17-at-20.15.41.md.jpeg" border="0"></a>

TELETEKST 1.3 - NOS Teletekst for SymbOS
=======================================
SymbOS version of MSXTTPP (rubikonlab.com). Shows live NOS Teletekst pages in a
SymbOS window, with the same 6x8 font, colours and block graphics.

REQUIREMENTS
- SymbOS 4.x with a running Network Daemon for your hardware
  (DenYoNet, GR8NET, MSX-UART/ESP8266, MSXPico, M4 Board, ...)
- 16-colour screen mode recommended (4-colour works, with fewer colours)

INSTALL
Copy TELETEKS.EXE to any folder and start it from the SymbOS desktop.
On the first start (no TELETEKS.CFG yet) the Setup window opens before any
network access. Settings are saved as TELETEKS.CFG in the same folder.

LOG FILE (since 1.1)
TELETEKS.LOG is written next to TELETEKS.EXE, one line per step (network
daemon, DNS, connect, bytes received, HTTP status, render). Each line is
written to disk immediately, so after a freeze the last line shows how far
it got. Switch it off in Setup when everything works.

CONNECTION (click "Setup")
  Auto   Try HTTPS directly; if that fails, use the relay.   (default)
  HTTPS  Direct HTTPS only. Needs network hardware with TLS support
         (e.g. ESP8266-UNAPI based: MSX-UART, MSXPico, Picoverse).
  Relay  Plain HTTP via ttrelay.py on a PC in your own network.
         Needed for DenYoNet, GR8NET, openMSX networking: no TLS.
  Relay IP or name: the address of the PC running ttrelay.py,
         e.g. 192.168.1.20. Port: default 8080.

THE RELAY (ttrelay.py)
Runs on any PC / Mac / Raspberry Pi with Python 3.7+, no extra packages:
    python3 ttrelay.py            (listens on port 8080)
    python3 ttrelay.py 8000       (other port)
Test it from another computer: http://<relay-ip>:8080/json/100
The same relay also works for the MSX-DOS version (MSXTT.COM).
MSXTT's own tools/tt-proxy.py (port 8080) works as the relay as well.

USING IT
  <  >          previous / next page
  Sub< Sub>     previous / next sub page
  Reload        reload the current page
  page field    type a number, press Return or click Go
  Mouse         click any page number on the screen to go there
  Keys          Left/Right = page, Up/Down = sub page,
                F1-F4 = fast text links, R = reload,
                0-9 = type a page number directly

SOURCE
SRC\ contains the C and Z80 sources (SymbOS C Compiler). The page parser and
renderer are in ttcore.c; ttfast.s / ttcell.s are Z80 speed-ups for parsing
and drawing.
