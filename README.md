# AWvoice — an RJ11 phone port for the Arcadyan AW1000 router

Plug an ordinary desk phone into the AW1000's RJ11 socket and make and take the SIM's calls on it. The router drives the line itself: a kernel module reaches the line chip, a daemon serves the hook, the tones and the calls over the modem's AT port, and a LuCI page shows the state and the settings.

No SIP, no Asterisk, no second number — the cellular network is the exchange and the SIM's number is the line's number. Everything it needs ships in the packages: install, enable, dial. The daemon talks to the modem's AT port directly, with no extra modem software in between, and there is no cloud and no account — everything runs on the router.

## What's new

- Call recordings: answered calls land on the router as WAV files and the call log links each call to its own recording — play, save or delete it from the call's card on the Calls tab. Stereo (the caller and the handset on separate channels), 16 kHz with HD voice, opt-in from the Settings tab with a size budget for the storage. The files never carry a phone number.

## What it does

- Calls both ways: dial (tone), ring, answer, hang-up, hook-flash (R), call waiting with hold and swap.
- Three-way join: after a flash, press `3` and both calls talk together (turn it off in Settings if you don't want it).
- Recall: put the handset down while another call waits on hold and the held call rings the phone back — lift the handset to take it.
- Progress tones and a ringback you can keep or hand to the network.
- Caller id on the handset's display: FSK (Bellcore or ETSI) or the DTMF variant, sent before or after the first ring.
- Calling-line restriction, network service codes, a hotline number.
- Call log with search, filters and CSV export, phonebook with speed dial, alarm (wake-up) calls, a line test.
- Call recording (off by default — a recording may need an announcement where you live; turn it on in Settings): answered calls land on the router as WAV files, stereo — the caller and the handset on separate channels — at 16 kHz when HD voice is on. The call's own card in the log plays, saves or deletes its recording.
- The recordings never carry a phone number — a file's name is the call's time and id — and a size budget (50 MB by default) removes the oldest when the storage passes it.
- Emergency numbers always ring through: nothing on the router blocks or drops them.
- HD voice: a 16 kHz wideband path from the handset to the network, end to end (off by default; turn it on in Settings).
- The phone LED shows the line: solid while ready, a fast blink while a call rings, a slow blink while off hook — each rate is settable.

## The three packages

| package | it is |
| --- | --- |
| `kmod-awvoice-fxs` | the kernel module: the line chip's register bus, the PCM block, the audio bridge between the modem and the line chip, the capture tap for the call recordings, and the ring watchdog |
| `awvoice-phoned` | the phone daemon: the hook, the tones, dialling, ringing, caller id, the call log, the call recordings and the `ubus` service `awvoice` |
| `luci-app-awvoice` | the LuCI page, **Services → AWvoice(RJ11)** |

## Hardware

- Arcadyan AW1000 (Qualcomm IPQ807x router side, Qualcomm 5G modem side) with the RJ11 phone socket.
- A standards-compliant FXS line chip on the board, driven over its register bus.
- An ordinary analog phone (tone dial); a handset with a display and the FSK or DTMF caller-id standard of your region also shows who calls.
- A SIM with a voice plan.

## The board file

The line chip needs this board's own settings (line voltages, ringing, tones, impedances, the init sequence). A ready board file ships with the `awvoice-phoned` package and the first service start installs it at `/etc/awvoice/board.bin` — a file already there (yours, kept across upgrades) is never overwritten. If you prefer to derive your own from your router's original firmware, the same file format is produced by `tools/provision/awvoice-board-extract.py` with the section map in `tools/provision/aw1000.map`.

Note on the chip's firmware: the chip runs on the firmware built into it. This port uses **no DSP patch file** — none is needed, and none ships.

## Building

The packages build inside an OpenWrt build tree, target `qualcommax/ipq807x` (the module builds only there). The repository has the standard feed layout:

```
cd openwrt
echo 'src-git awvoice https://github.com/Pr0Luci/openwrt-aw1000-voice-awvoice' >> feeds.conf
scripts/feeds update awvoice
scripts/feeds install kmod-awvoice-fxs awvoice-phoned luci-app-awvoice
make menuconfig   # select the three packages
make -j$(nproc)
```

Or copy the three package directories at the repository root into the build tree's `package/` directory instead.

## Installing

Install the three packages on the router, then enable the service and turn the phone on:

```
uci set awvoice.main.enabled=1
uci commit awvoice
/etc/init.d/awvoice start
```

Or simply reboot instead: at the next boot everything starts by itself — the board file is installed and the phone line comes ready.

Defaults fit the AW1000 — the board file ships with the package and the rest is automatic; the settings the page does not show (the modem's AT interface number, the LED name) live in `/etc/config/awvoice` with sensible values.

## Host tests

Each of `kmod-awvoice-fxs` and `awvoice-phoned` has a host test suite that builds and runs without the hardware:

```
cd packages/awvoice-phoned && make -f Makefile.host test
```

## Under the hood

A thin kernel module owns the line chip's register bus, the PCM block and a hardware ring watchdog: if the phone service ever stops, the chip is back in reset within milliseconds — the phone never rings forever and the line never stays live. All the line logic sits in one daemon on a single 5 millisecond clock, and the whole core builds and runs on an ordinary computer without the hardware (`make -f Makefile.host test` in each package).

## Licence

GPL-2.0-only — see [LICENSE](LICENSE).
