# AWvoice — an RJ11 phone port for the Arcadyan AW1000 router

Plug an ordinary desk phone into the AW1000's RJ11 socket and make and take the SIM's calls on it. The router drives the line itself: a kernel module reaches the line chip, a daemon serves the hook, the tones and the calls over the modem's AT port, and a LuCI page shows the state and the settings.

No SIP, no Asterisk, no second number — the cellular network is the exchange and the SIM's number is the line's number. Everything it needs ships in the packages: install, enable, dial. The daemon talks to the modem's AT port directly, with no extra modem software in between, and there is no cloud and no account — everything runs on the router.

## What's new

- Quiet hours: the phone's own do-not-disturb. Windows of the day you write on the Rules tab, three answers for a call that arrives inside one (silent, the answering machine, or released), callers that always ring through — a list you keep and the contacts you mark for it — the same caller ringing twice getting through, and `*78` / `*79` from the handset or the page's dialler for a quiet asked for by hand. Off until you write a window; nothing about the line changes by itself.
- HD voice is now honest about the hardware: the setting asks the modem which rate it actually puts on the bus, and the phone line is brought up at that rate — when nobody is using it, so it never cuts a call. The recordings, the answering machine and the page's audio all follow the same layout, and the service's health reply reports the setting, the modem's own answer and the rate the audio path came up at. See *Verified on the hardware* for the measured numbers.
- Answering machine: after a delay you choose, an unanswered call on the SIM's line hears your greeting and leaves a message on the router. The call log links each call to its own message — play, save or delete it from the call's card on the Calls tab — and the Settings tab shows how much the messages take and deletes them all. Off by default. The greeting is one file the Answering machine card manages: it ships with the package, plays in the browser, and takes an upload or a recording of your own voice — the service trims the silence and adds the tone that tells the caller to start talking, so a greeting needs no editing by hand.
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
- Answering machine (off by default; only calls on the cellular line are answered): after a settable delay — zero answers at once — the caller hears your greeting and what they say is kept as a message. The greeting is one file at a fixed place on the router (`/etc/awvoice/greeting.wav`), managed from the Answering machine card rather than by a path in the settings: the package ships a short default, the first start copies it in when the router has none, and nothing after that ever writes over what you put there. The card plays the file in the browser (a file the machine cannot use too, so you can hear what is wrong with it), takes an upload of any usual WAV — the browser resamples it to 8 kHz mono — and records your own voice, with the silence trimmed off both ends and the "leave a message" tone added by the service, which plays no tone of its own. A file the machine cannot read switches it off rather than answer with noise, and the card says which it found. The call log links each call to its message, played, saved or deleted in that call's card; the Settings tab shows the storage the messages take and deletes the whole folder. Messages get their own size budget and a longest-message limit, and their names carry no phone number. Picking up the handset during the machine's call hands the call over; the part of the message already recorded is kept — the classic answering-machine (TAD) behaviour, the same in standard voicemail systems.
- Emergency numbers always ring through: nothing on the router blocks or drops them.
- Quiet hours: the windows of your own day when the phone must not ring. A window is a start and an end — 22:00 to 07:00 is tonight through midnight, not nothing — a list of weekdays, or all day, and the first window that covers the moment decides, in the order you wrote them on the Rules tab (eight of them at most). What happens to a call inside one is that window's own choice: the line stays silent, the call goes to the answering machine, or the caller is released. A silenced call is not lost — the page says it is held quiet and answers it, and lifting the handset answers it too; the machine's answer needs the answering machine switched on, and with it off the caller simply hears nothing from this side until the network ends the call. Two kinds of caller reach you whatever the window says: an emergency number, and a caller you marked — a number written on the Rules tab (twenty at most), or any number of a contact marked *Ring through quiet hours*, recognised however the network happened to write it. A caller who tries again inside the window's repeat time is let through as well; a new window starts at 15 minutes and zero turns that answer off. `*78` and `*79` — with or without the send key — typed on the handset or in the page's dialler put the line on quiet by hand and take it off again, and that ask outlives a save of anything else, while a restart of the service falls back to the windows written down. Nothing is honoured from a schedule before the router knows what time it is: a clock still at 1970 would match every window or none, so an unknown time never quiet the line (a quiet asked for by hand needs no clock at all). The Health tab says what the line answered a moment ago and what decided it.

**A note on safety and emergencies:** the RJ11 contacts sit at about 46 V, and higher while the phone rings. This is not certified telephone equipment — do not rely on it for emergency calls, and check the rules that apply to you before connecting anything to the telephone network.
- HD voice: a 16 kHz wideband path from the handset to the network, end to end (off by default; turn it on in Settings). The setting is a wish rather than a command: the service asks the modem which rate it actually puts on the bus and only when the modem answers with the rate you chose is the phone line brought up at it. The line chip's band, the PCM slots and the audio bridge are one layout and the slots can only change while the bus is down, so that bring-up runs when nobody is using the line — it never cuts a call, and an owe raised while the handset is up waits for the line to clear. Changing the rate does restart the modem, so the mobile data drops for about half a minute; a modem that keeps answering the other rate is asked a bounded number of times and the line then keeps working at the rate it has, with the reason in the log. The health reply carries three separate facts — `hdVoice` for the setting (the page's word), `audioRate` for the modem's own answer and `voiceLayout` for the layout the audio path actually came up at — so a mismatch is visible rather than inferred.
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

### The greeting the answering machine plays

The machine plays one file, always at `/etc/awvoice/greeting.wav` — its place is not a setting. The package carries a
short default greeting and the service puts it there the first time it starts on a router that has none; after that the
file is yours and an upgrade never writes over it.

The Answering machine card on the Settings tab manages it:

- **Play** hears the file in the browser, exactly as it stands on the router — including a file the machine cannot use,
  which is how you recognise one. The button is off when there is nothing there to read.
- **Upload WAV…** sends a file of yours. The browser decodes it and resamples it to 8 kHz mono, so a recording from a
  phone or a computer in any usual rate works; the daemon checks the result again and only then puts it in place, writing
  to a temporary file and moving it into position, so an interrupted upload can never leave a half greeting.
- **Record** takes your voice from the computer's microphone, and the service trims the silence off both ends and adds
  the short tone that tells the caller to start talking — a greeting of your own, with nothing to convert by hand.

A greeting is a 16-bit PCM WAV at 8 or 16 kHz, at most 2 MB; a file the machine cannot read is reported on the card and
the machine stays off rather than answer with noise. Keeping your own recording is what the Record button is for, and
the file lives on the router at the path above — you can still copy one there yourself if you prefer:

```
scp greeting.wav root@router:/etc/awvoice/greeting.wav
```

The service reads the file when it starts and whenever the settings are saved, and the card says what it found: how long
the greeting is, or why it could not be used.

## Verified on the hardware

The host suites below prove the logic on an ordinary computer. This section is the runs on the router itself — an AW1000 with a live SIM, OpenWrt 25.12.5, kernel 6.12.94, aarch64 — with what each one measured. Nothing is called working here before a call has gone through the actual hardware.

**HD voice (16 kHz), 2 November 2026.**

- Turning HD on while the modem already keeps 16 kHz: the line came up narrowband first, the modem's answer to the read-only `AT+QDAI?` question arrived 0.5 s later, one bring-up followed, and the log ends `the line is in service at 16000 Hz` about 4 s after the setting. The modem was not restarted and the mobile data did not drop.
- HD off and then HD on again: the rate was written, the modem restarted (`AT+CFUN=1,1`), its AT port came back 14 s and 16 s later, the read answered 8 kHz and then 16 kHz, and the line was in service at 8000 Hz 47 s after the setting and at 16000 Hz 46 s after the next one. Both directions with `modemTimeouts: 0` and no faults.
- Three calls at 16 kHz, recorded: a 66 s call's file is **65.573 s** and a 58 s call's is **57.541 s** — 16 kHz stereo, caller and handset on separate channels, both carrying voice, **no clipped 20 ms window in either file**. The answering machine's 15 s message is **14.766 s** and its greeting played its own length (4.43 s), so the rate is right in the capture and in the playback.
- The band, measured from the recording (195 windows of 2048 samples over the first 20 s): the caller's channel holds **4.68 % of its energy between 3.4 and 4 kHz and 4.07 % between 4 and 5 kHz**. A narrowband call upsampled to 16 kHz is empty above 4 kHz, so the file's header and its data agree.
- Not claimed: the handset's own transmit band measured 0.02–0.08 % above 4 kHz in the same files, which is what the line chip's transmit filter gives. The receive direction is proven wideband; a tone test in the 4–5 kHz band is owed before the transmit width is claimed.

**Answering machine and recordings, 30 October – 2 November 2026.** An unanswered call was answered 10.7–10.8 s after it came, the greeting went out with no frame dropped (one counted underrun — a single late frame), and the message was kept and linked in that call's card. Picking the handset up during the machine's call hands the conversation over: a take-over five seconds into the greeting left no message, because none had begun. At 8 kHz the recordings are stereo with both channels carrying voice and the file's length matching the call log — a 407 s call's file is 13,044,540 bytes, which is exactly 407 s of 8 kHz stereo.

**The page's phone, 31 October – 1 November 2026.** An incoming call answered with the page's Answer button ran two-way audio for 64 s with both channels of its recording carrying a voice, and a call dialled from the page's keypad reached the network and both sides talked. Hold and resume (`flash: the call is held`, `flash: the held call is taken back`), a waiting call taken and then swapped, two calls joined after a flash and `3`, and a page call taken back by a handset flash are all in the log — the page's flash is a 100 ms break inside the 80–300 ms hook window, with the 3 s code window kept separate from it.

**Caller id, 29 October 2026.** The number and the phonebook's name reached a handset display, and the transmission was decoded from a recording of the line: 304 alternating bits, 186 mark bits and the framed message with a good check.

**The line and the ring.** One phone's ringer draws more than the chip's AC ring detector tolerates, and the supervision shows that rather than a dead line: the burst is cut and the reason is logged (`the chip's ring trip fired before any lift`) instead of repeated short chirps. With a second phone, ten back-to-back ring tests held 57–96 V on the ring rail at ±3–6 mA and every call rang. An outgoing call the network released before the answer plays the busy tone.

**Install and boot.** The three packages upgrade in place with `apk` and the delivered defaults are off — the recordings, the answering machine and the SIP side. After a reboot the service comes by itself: board file installed, line ready and calibrated, the greeting loaded, and every stored file hash-identical to before.

## Host tests

Each of `kmod-awvoice-fxs` and `awvoice-phoned` has a host test suite that builds and runs without the hardware:

```
cd awvoice-phoned && make -f Makefile.host test
```

Every fix's test is first seen to fail with that fix reverted, then green with it restored — a test that survives the reverted fix proves nothing.

## Under the hood

A thin kernel module owns the line chip's register bus, the PCM block and a hardware ring watchdog: if the phone service ever stops, the chip is back in reset within milliseconds — the phone never rings forever and the line never stays live. All the line logic sits in one daemon on a single 5 millisecond clock, and the whole core builds and runs on an ordinary computer without the hardware (`make -f Makefile.host test` in each package).

The caller's voice reaches the daemon through a lock-free capture ring, and an answering-machine greeting goes back through a matching playback ring — the first locally generated voice this port sends toward the caller. An empty ring is silence with a counted underrun, never a stale frame from an earlier call; the bridges' stop and start reset both rings.

## Licence

GPL-2.0-only — see [LICENSE](LICENSE).
