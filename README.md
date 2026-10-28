# AWvoice — an RJ11 phone port for the Arcadyan AW1000 router

Plug an ordinary desk phone into the AW1000's RJ11 socket and make and take the SIM's calls on it. The router drives the line itself: a kernel module reaches the line chip, a daemon serves the hook, the tones and the calls over the modem's AT port, and a LuCI page shows the state and the settings.

No SIP, no Asterisk, no second number — the cellular network is the exchange and the SIM's number is the line's number. Everything it needs ships in the packages: install, enable, dial. The daemon talks to the modem's AT port directly, with no extra modem software in between, and there is no cloud and no account — everything runs on the router.

## What's new

- Answering machine: after a delay you choose, an unanswered call on the SIM's line hears your greeting and leaves a message on the router. The call log links each call to its own message — play, save or delete it from the call's card on the Calls tab — and the Settings tab shows how much the messages take and deletes them all. Off by default; the greeting is your own WAV file.
- Call recordings: calls you answer land on the router as WAV files and the call log links each call to its own recording — play, save or delete it from the call's card on the Calls tab. Stereo (the caller and the handset on separate channels), 16 kHz with HD voice, opt-in from the Settings tab with a size budget for the storage. The files never carry a phone number. A call the answering machine answered is kept as its message instead.

## What it does

- Calls both ways: dial (tone), ring, answer, hang-up, hook-flash (R), call waiting with hold and swap.
- Three-way join: after a flash, press `3` and both calls talk together (turn it off in Settings if you don't want it).
- Recall: put the handset down while another call waits on hold and the held call rings the phone back — lift the handset to take it.
- Progress tones and a ringback you can keep or hand to the network.
- Caller id on the handset's display: FSK (Bellcore or ETSI) or the DTMF variant, sent before or after the first ring.
- Calling-line restriction, network service codes, a hotline number.
- Call log with search, filters and CSV export, phonebook with speed dial, alarm (wake-up) calls, a line test.
- Call recording (off by default — a recording may need an announcement where you live; turn it on in Settings): calls you answer land on the router as WAV files, stereo — the caller and the handset on separate channels — at 16 kHz when HD voice is on. The call's own card in the log plays, saves or deletes its recording. A call the answering machine answered is kept as its message instead: when you pick the handset up, the message ends at the lift and the rest of the conversation is not recorded.
- The recordings never carry a phone number — a file's name is the call's time and id — and a size budget (50 MB by default) removes the oldest when the storage passes it.
- Both storage folders (the recordings and the messages) are made and checked when the service starts or reloads, and again before a call writes its first file: a missing folder on a mounted drive is created, and a folder that cannot be written — or a drive with almost no space — is reported on the Health tab instead of failing silently. The answering machine does not answer a call it could not keep a message from, so the handset still rings. A folder under `/mnt` is only used once its drive is mounted — never created on the router's own flash under an unmounted mount point — and the two folders are always kept apart.
- Answering machine (off by default; only calls on the cellular line are answered): after a settable delay — zero answers at once — the greeting you put on the router is played to the caller and what they say is kept as a message. The call log links each call to its message, played, saved or deleted in that call's card; the Settings tab shows the storage the messages take and deletes the whole folder. The greeting must be a 16-bit PCM WAV at 8 or 16 kHz; a file the machine cannot read switches it off rather than answer with noise. Messages get their own size budget and a longest-message limit, and their names carry no phone number. Picking up the handset during the machine's call hands the call over; the part of the message already recorded is kept — the classic answering-machine (TAD) behaviour, the same in standard voicemail systems.
- Emergency numbers always ring through: nothing on the router blocks or drops them.

**A note on safety and emergencies:** the RJ11 contacts sit at about 46 V, and higher while the phone rings. This is not certified telephone equipment — do not rely on it for emergency calls, and check the rules that apply to you before connecting anything to the telephone network.
- HD voice: a 16 kHz wideband path from the handset to the network, end to end (off by default; turn it on in Settings).
- The phone LED shows the line: solid while ready, a fast blink while a call rings, a slow blink while off hook — each rate is settable.

## The three packages

| package | it is |
| --- | --- |
| `kmod-awvoice-fxs` | the kernel module: the line chip's register bus, the PCM block, the audio bridge between the modem and the line chip, the capture tap for the recordings and messages, the playback ring that carries a greeting to the caller, and the ring watchdog |
| `awvoice-phoned` | the phone daemon: the hook, the tones, dialling, ringing, caller id, the call log, the call recordings, the answering machine (its greeting and messages) and the `ubus` service `awvoice` |
| `luci-app-awvoice` | the LuCI page, **Services → AWvoice(RJ11)** |

## Hardware

- Arcadyan AW1000 (Qualcomm IPQ807x router side, Qualcomm 5G modem side) with the RJ11 phone socket.
- A standards-compliant FXS line chip on the board, driven over its register bus.
- An ordinary analog phone (tone dial); a handset with a display and the FSK or DTMF caller-id standard of your region also shows who calls.
- A SIM with a voice plan.

## The board file

The line chip needs this board's own settings (line voltages, ringing, tones, impedances, the init sequence). A ready board file ships with the `awvoice-phoned` package and the first service start installs it at `/etc/awvoice/board.bin` — a file already there (yours, kept across upgrades) is never overwritten. If you prefer to derive your own from your router's original firmware, the same file format is produced by `tools/provision/awvoice-board-extract.py` with the section map in `tools/provision/aw1000.map`.

Why the board file ships here: its contents are the line chip's operating parameters — voltages, ringing and audio presets, impedances — functional values the hardware requires, not the chip maker's executable code. The DSP patch never ships and never runs from this port (see the note on the chip's firmware above), the values were read from the board's own stock firmware, the tool and the section map to derive them from your own firmware ship here, and the same operating values are already public in source form in other open-source projects.

Note on the chip's firmware: the chip runs on the firmware built into it. This port uses **no DSP patch file** — none is needed, and none ships. The port never uploads or redistributes a proprietary binary: the line chip's ready state is checked at every service start, and if a unit's chip did not come up the service stops with that cause in the log instead of a silent dead line.

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

## Prebuilt packages

Every release carries the three packages, their checksums and a short build record. Install all three from the same release, then reboot — the module and the daemon are a matched pair, and a module that is already loaded stays loaded until a reboot:

```
apk add --allow-untrusted awvoice-phoned-*.apk kmod-awvoice-fxs-*.apk luci-app-awvoice-*.apk
reboot
```

The module is built for kernel 6.12.94 (OpenWrt 25.12.5) on aarch64; its package depends on that exact kernel build, so `apk` refuses it on another kernel instead of leaving a module behind that cannot load. `SHA256SUMS` in the release has the checksums and `build-info.txt` the commit and the SDK the packages came from. The packages are not signed, hence `--allow-untrusted`.

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

The caller's voice reaches the daemon through a lock-free capture ring, and an answering-machine greeting goes back through a matching playback ring — the first locally generated voice this port sends toward the caller. An empty ring is silence with a counted underrun, never a stale frame from an earlier call; the bridges' stop and start reset both rings.

## Licence

GPL-2.0-only — see [LICENSE](LICENSE).
