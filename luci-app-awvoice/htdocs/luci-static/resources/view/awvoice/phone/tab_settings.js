// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
'use strict';
'require baseclass';
'require view.awvoice.phone.editors as editors';

/*
 * tab_settings.js - Phone settings tab: line, tones, audio, features, recording, answering machine, speed dial,
 * indicator and advanced options. Edits are held in the form and written with "Save and apply"; "Reset" discards
 * them. The machine card shows what the daemon actually loaded, so a refused greeting is not a silent failure.
 * Fields whose daemon support is a later milestone are shown disabled with a "Not available yet" tag
 * (api.settingsLive says which are live).
 */

var NUMBER_RE = editors.NUMBER_RE;
var EARPIECE_STEPS = [ 0, 1, 2, 3, 4, 5 ];     /* the range this modem reports for AT+CLVL; the daemon ignores a level outside it */
var REFRESH_STEPS = [ 1, 2, 3, 5, 10, 15, 30 ]; /* live view refresh, seconds; UCI page_refresh_s, read by the page only */

return baseclass.extend({
	render: function(ctx) {
		var u = ctx.ui, live = ctx.api.settingsLive, loaded = null, h = null;

		function num(id, min, max, label) {
			var i = u.text('', '', '', 'number');
			i.min = min; i.max = max;
			i.setAttribute('aria-label', label);
			return i;
		}

		var cid = u.select([ [ 'dtmf', _('DTMF') ], [ 'bellcore', _('FSK (Bellcore)') ], [ 'etsi', _('FSK (ETSI)') ], [ 'off', _('Off') ] ], 'dtmf');
	var cidWhen = u.select([ [ 'ring1', _('After the first ring burst') ], [ 'prering', _('Before the first ring') ] ], 'ring1');
		var myNo = u.text('', _('Number'), 'av-mono wide');
		var pause = num('pause', 3, 15, _('Dial pause in seconds'));
		var flashMin = num('fmin', 20, 500, _('Hook flash minimum in milliseconds'));
		var flashMax = num('fmax', 300, 3000, _('Hook flash maximum in milliseconds'));

		var ringback = u.select([ [ 'auto', _('Automatic') ], [ 'on', _('Always play') ], [ 'off', _('Never play') ] ], 'auto');
		var volume = u.select([ [ '', _('Modem default') ] ].concat(EARPIECE_STEPS.map(function(n) { return [ String(n), String(n) ]; })), '');
		var micGain = num('mic', -99, 99, _('Microphone gain in dB'));
		var earGain = num('ear', -99, 99, _('Earpiece gain in dB'));

		/* the caller-id restriction has no field: this network's registration carries no supplementary-service
		 * channel for it; the stored value is kept untouched until the network offers it again */
		var conf = u.checkbox(false, _('Allow a second call with the flash key'));
	var hd = u.checkbox(false, _('HD voice (wideband 16 kHz)'));
		var wait = u.checkbox(true, _('Call waiting'));
		var hotNo = u.text('', _('Number'), 'av-mono wide');
		var hotDelay = num('hot', 3, 99, _('Hotline delay in seconds'));
		var led = u.select([ [ '', _('None') ] ], '');

var rxGain = num('rx', 0, 65535, _('Modem receive gain'));
var usb = num('usb', 0, 9, _('Modem USB interface'));
var ringRate = num('rr', 100, 3000, _('Call-blink rate in milliseconds'));
var offhookRate = num('or', 200, 10000, _('Off-hook blink rate in milliseconds'));
var refresh = u.select(REFRESH_STEPS.map(function(n) { return [ String(n), n === 1 ? _('1 second (live)') : _('%d seconds').format(n) ]; }), '1');

/* the recordings: kept on the router's storage, listed on the Calls tab */
var recEnabled = u.checkbox(false, _('Record phone calls'));
var recPath = u.text('', '/etc/awvoice/recordings', 'av-mono wide');
var recMax = num('recmax', 1, 4095, _('Recording budget in megabytes'));
var recUsage = E('span', { 'class': 'av-mono' }, '');

/* the answering machine: its greeting, the wait before it answers and its message storage */
var machOn = u.checkbox(false, _('Answer the phone for me'), function() { machineNote(); });
var machGreeting = u.text('', '/etc/awvoice/greeting.wav', 'av-mono wide');
var machDelay = num('mdelay', 0, 60, _('Answering machine delay in seconds'));
var machPath = u.text('', '/etc/awvoice/messages', 'av-mono wide');
var machMax = num('mmax', 1, 4095, _('Message budget in megabytes'));
var machSecs = num('msecs', 10, 600, _('Longest message in seconds'));
var msgUsage = E('span', { 'class': 'av-mono' }, '');
var machNote = E('div', { 'class': 'av-note' });

	/* the FSK variants carry the timing choice; DTMF has its own placement (it goes out while the phone rings) */
	function fskPicked() {
		cidWhen.disabled = cid.value !== 'bellcore' && cid.value !== 'etsi';
	}
	function when() {
		return cid.value === 'bellcore' || cid.value === 'etsi' ? cidWhen.value : null;
	}
	cid.addEventListener('change', fskPicked);

		var err = E('div', { 'class': 'av-err-msg', 'role': 'alert' });

		function tag() { return u.tag(_('Not available yet')); }
		function offIf(key, kids) { return u.off(kids, !live[key]); }

		/* keeps a stored refresh that is not one of the offered steps as its own option */
		function setRefresh(sec) {
			if (REFRESH_STEPS.indexOf(sec) < 0)
				refresh.appendChild(E('option', { 'value': String(sec) }, _('%d seconds').format(sec)));
			refresh.value = String(sec);
		}

		/* whether the greeting the daemon holds matches the switch, and whether a message could be kept at all: the
		 * daemon answers only when both hold, and the health reply says what it found */
		function machineNote() {
			var g = h && h.machine, st = storeState('messages');
			var note;

			if (!machOn.get()) {
				machNote.textContent = _('Off: an unanswered call keeps ringing until the caller gives up.');
				return;
			}
			if (g && g.greetingLoaded === false)
				note = _('The greeting could not be loaded, so the machine will not answer. Check that the file exists and is a 16-bit PCM WAV at 8 or 16 kHz.');
			else if (g && g.greetingLoaded)
				note = _('The greeting is %s long.').format(u.fmtDuration(Math.round(g.greetingMs / 1000)));
			else
				note = _('The greeting\'s state is not known yet.');
			if (st && st.ready === false)
				note += ' ' + _('The machine will not answer until the message folder can be used: %s.').format(
					st.error || _('no reason given'));
			else if (st && st.ready === true && st.freeMb != null && st.freeMb < 1)
				note += ' ' + _('The machine will not answer until the message folder has room for a file.');
			else if (g && g.greetingLoaded)
				note += ' ' + _('It answers after %s seconds.').format(String(g.delaySec));
			machNote.textContent = note;
		}

		function fill() {
			led.textContent = '';
			led.appendChild(E('option', { 'value': '' }, _('None')));
			var names = (h && h.indicator && h.indicator.leds) || [];
			names.forEach(function(n) {
				led.appendChild(E('option', { 'value': n }, n));
			});
			if (loaded.indicator.led && names.indexOf(loaded.indicator.led) < 0)
				led.appendChild(E('option', { 'value': loaded.indicator.led }, loaded.indicator.led));
			led.value = loaded.indicator.led;
		}

		function load() {
			return ctx.api.getPhoneSettings().then(function(p) {
				loaded = p;
				cid.value = p.line.cid; myNo.value = p.line.myNumber; pause.value = p.line.dialPauseSec;
				cidWhen.value = p.line.cidWhen; fskPicked();
				flashMin.value = p.hookFlash.minMs; flashMax.value = p.hookFlash.maxMs;
				ringback.value = p.tones.ringback;
				volume.value = p.audio.earpieceVolume == null ? '' : String(p.audio.earpieceVolume);
				micGain.value = p.audio.micGainDb == null ? '' : p.audio.micGainDb;
				earGain.value = p.audio.earpieceGainDb == null ? '' : p.audio.earpieceGainDb;
				rxGain.value = p.audio.modemRxGain == null ? '' : p.audio.modemRxGain;
				conf.set(p.features.conference); wait.set(p.features.callWaiting); hd.set(p.audio.hd);
				hotNo.value = p.features.hotline.number; hotDelay.value = p.features.hotline.delaySec;
				ringRate.value = p.indicator.ringRateMs; offhookRate.value = p.indicator.offhookRateMs;
				fill();
				usb.value = p.modem.usbInterface;
				machOn.set(p.machine.enabled);
				machGreeting.value = p.machine.greeting;
				machDelay.value = p.machine.delaySec;
				machPath.value = p.machine.path;
				machMax.value = p.machine.maxMb;
				machSecs.value = p.machine.maxSec;
				setRefresh(p.page.refreshSec);
				err.textContent = '';
				return ctx.api.getHealth();
			}).then(function(health) {
				h = health;
				fill();
				machineNote();
				/* the usage lines carry the folders' state, so they are drawn again once it is known */
				recRefreshUsage();
				msgRefreshUsage();
			}, function(e) { err.textContent = e.message; });
		}

		function loadRec() {
			return ctx.api.getRecordingSettings().then(function(s) {
				recEnabled.set(s.enabled);
				recPath.value = s.path;
				recMax.value = String(s.maxMb);
			}, function(e) { err.textContent = err.textContent || e.message; });
		}

		/* one refusal must not hide the files that did go: every delete settles, then the count says how it went;
		 * the usage line is read again and the Calls tab is told, so no card keeps pointing at a gone file */
		function delAll(list, del, noun, done, usage, evt) {
			return Promise.all(list.map(function(f) {
				return del(f.name).then(function() { return null; }, function(e) { return e; });
			})).then(function(errs) {
				var bad = errs.filter(function(e) { return e; });

				ctx.toast(bad.length ? _('%d of %d %s could not be deleted: %s').format(bad.length, list.length, noun, bad[0].message) : done);
				ctx.emit(evt);
				return usage();
			});
		}

		/* the folder as the daemon reads it: no trailing slashes, and never the root (the daemon refuses both the
		 * root and a folder that is a file, and says why on the Health tab) */
		function dirPath(v) {
			v = String(v).trim().replace(/\/+$/, '');
			return /^\/./.test(v) ? v : null;
		}

		/* the folders' state from the health reply: a folder that cannot take a file is said here, before a call
		 * is lost to a path that only fails at write time */
		function storeState(which) {
			return h && h.storage ? h.storage[which] : null;
		}

		function storeTail(which) {
			var st = storeState(which);

			if (st && st.ready === false)
				return ' ' + _('The folder cannot be used: %s.').format(st.error || _('no reason given'));
			if (st && st.freeMb != null)
				return ' ' + (st.freeMb < 1 ? _('Nearly full: %s MB free.').format(st.freeMb) : _('%s MB free.').format(st.freeMb));
			return '';
		}

		function recRefreshUsage() {
			return ctx.api.listRecordings().then(function(list) {
				var bytes = list.reduce(function(a, r) { return a + r.bytes; }, 0);
				recUsage.textContent = (list.length ?
					_('%d recordings, %s MB on the storage').format(list.length, (bytes / 1048576).toFixed(1)) :
					_('No recordings on the storage.')) + storeTail('recordings');
				return list;
			}, function(e) { recUsage.textContent = e.message; return []; });
		}

		function recDeleteAll() {
			return recRefreshUsage().then(function(list) {
				if (!list.length)
					return;
				editors.confirm(ctx, _('Delete all recordings'),
					_('Delete all %d recordings? This cannot be undone.').format(list.length), _('Delete all'),
					function() { return delAll(list, ctx.api.deleteRecording, _('recordings'), _('Recordings deleted'), recRefreshUsage, 'recordings'); });
			});
		}

		function msgRefreshUsage() {
			return ctx.api.listMessages().then(function(list) {
				var bytes = list.reduce(function(a, m) { return a + m.bytes; }, 0);
				msgUsage.textContent = (list.length ?
					_('%d messages, %s MB on the storage').format(list.length, (bytes / 1048576).toFixed(1)) :
					_('No messages on the storage.')) + storeTail('messages');
				return list;
			}, function(e) { msgUsage.textContent = e.message; return []; });
		}

		function msgDeleteAll() {
			return msgRefreshUsage().then(function(list) {
				if (!list.length)
					return;
				editors.confirm(ctx, _('Delete all messages'),
					_('Delete all %d messages? This cannot be undone.').format(list.length), _('Delete all'),
					function() { return delAll(list, ctx.api.deleteMessage, _('messages'), _('Messages deleted'), msgRefreshUsage, 'messages'); });
			});
		}

		function optInt(el) {
			var v = String(el.value).trim();
			return v === '' ? null : parseInt(v, 10);
		}

		function save() {
			err.textContent = '';
			if (!loaded) { err.textContent = _('The settings could not be read yet.'); return; }

			var my = myNo.value.trim(), hn = hotNo.value.trim();
			var dp = parseInt(pause.value, 10), fmin = parseInt(flashMin.value, 10), fmax = parseInt(flashMax.value, 10);
			var rx = optInt(rxGain), ui = parseInt(usb.value, 10);

			if (my && !NUMBER_RE.test(my)) { err.textContent = _('Numbers may contain digits, *, # and a leading +'); return; }
			if (!(dp >= 3 && dp <= 15)) { err.textContent = _('Dial pause must be 3 to 15 seconds'); return; }
			if (!(fmin >= 20 && fmin <= 500)) { err.textContent = _('Hook-flash minimum must be 20 to 500 ms'); return; }
			if (!(fmax >= 300 && fmax <= 3000)) { err.textContent = _('Hook-flash maximum must be 300 to 3000 ms'); return; }
			if (fmax < fmin + 100) { err.textContent = _('Hook-flash maximum must be at least 100 ms above the minimum'); return; }
			if (rx != null && !(rx >= 0 && rx <= 65535)) { err.textContent = _('Modem receive gain must be 0 to 65535'); return; }
			if (!(ui >= 0 && ui <= 9)) { err.textContent = _('Modem USB interface must be 0 to 9'); return; }
			var rr = parseInt(ringRate.value, 10), orate = parseInt(offhookRate.value, 10);
			if (!(rr >= 100 && rr <= 3000)) { err.textContent = _('The indicator call-blink rate must be 100 to 3000 ms'); return; }
			if (!(orate >= 200 && orate <= 10000)) { err.textContent = _('The indicator off-hook blink rate must be 200 to 10000 ms'); return; }

			var rp = dirPath(recPath.value), rm = parseInt(recMax.value, 10);
			if (!rp) { err.textContent = _('The recording folder must be an absolute path that is not the root'); return; }
			if (!(rm >= 1 && rm <= 4095)) { err.textContent = _('The recording budget must be 1 to 4095 MB'); return; }

			var mg = machGreeting.value.trim(), mp = dirPath(machPath.value);
			var md = parseInt(machDelay.value, 10), mmb = parseInt(machMax.value, 10), ms = parseInt(machSecs.value, 10);
			if (!/^\//.test(mg) || mg.length >= 128) { err.textContent = _('The greeting file must be an absolute path shorter than 128 characters'); return; }
			if (!mp || mp.length >= 128) { err.textContent = _('The message folder must be an absolute path shorter than 128 characters and not the root'); return; }
			if (mp === rp) { err.textContent = _('The messages cannot be kept in the recording folder. Choose another folder.'); return; }
			if (!(md >= 0 && md <= 60)) { err.textContent = _('The answering machine delay must be 0 to 60 seconds'); return; }
			if (!(mmb >= 1 && mmb <= 4095)) { err.textContent = _('The message budget must be 1 to 4095 MB'); return; }
			if (!(ms >= 10 && ms <= 600)) { err.textContent = _('The longest message must be 10 to 600 seconds'); return; }

			ctx.api.savePhoneSettings({
				line: { cid: cid.value, cidWhen: when(), myNumber: my, dialPauseSec: dp },
				hookFlash: { minMs: fmin, maxMs: fmax },
				tones: { ringback: ringback.value },
				audio: { earpieceVolume: volume.value === '' ? null : parseInt(volume.value, 10), modemRxGain: rx,
					micGainDb: optInt(micGain), earpieceGainDb: optInt(earGain), hd: hd.get() },
				features: { clir: loaded.features.clir, conference: conf.get(), callWaiting: wait.get(), hotline: { number: hn, delaySec: parseInt(hotDelay.value, 10) || loaded.features.hotline.delaySec } },
				indicator: { led: led.value, ringRateMs: rr, offhookRateMs: orate },
				modem: { usbInterface: ui },
				page: { refreshSec: parseInt(refresh.value, 10) || 1 },
				machine: { enabled: machOn.get(), greeting: mg, delaySec: md, path: mp, maxMb: mmb, maxSec: ms }
			}).then(function() {
				return ctx.api.saveRecordingSettings({ enabled: recEnabled.get(), path: rp, maxMb: rm });
			}).then(function() {
				ctx.toast(_('Settings saved and applied'));
				return load();
			}).then(function() {
				ctx.emit('settings', loaded);
			}, function(e) { err.textContent = e.message; });
		}

		var sdList = E('div');
		function drawSpeed() {
			sdList.textContent = '';
			var items = ctx.contacts.filter(function(c) { return c.quick; });
			if (!items.length)
				sdList.appendChild(E('div', { 'class': 'av-empty' }, _('No speed dial numbers yet.')));
			items.forEach(function(c) {
				sdList.appendChild(E('div', { 'class': 'av-it' }, [
					u.tag(c.quick),
					E('div', { 'class': 't' }, [ c.name, E('small', { 'class': 'av-mono' }, c.numbers[0].value) ]),
					u.iconButton(_('Remove speed dial'), 'x', function() {
						ctx.api.saveContact({ id: c.id, name: c.name, numbers: c.numbers, quick: '' })
							.then(function() { return ctx.refreshContacts(); }, function(e) { ctx.toast(e.message); });
					})
				]));
			});
		}
		ctx.on('contacts', drawSpeed);

		var speedReady = !!ctx.api.available.saveContact;

		var root = E('div', [
			E('div', { 'class': 'cbi-section' }, [
				E('h3', _('Line')),
				u.field(_('Caller ID'), _('How the caller\'s number reaches the handset\'s display'), cid),
				u.field(_('Caller-id timing'), _('Where the FSK variants put the message: between the first and the second ring burst, or before the first ring (the European placement). DTMF has its own placement.'), cidWhen),
				u.field(_('My number'), _('Shown on the Overview. It is never dialled.'), myNo),
				u.field(_('Dial pause'), _('The number is dialled this long after the last digit (3 to 15 seconds)'),
					E('div', { 'class': 'av-inline' }, [ pause, E('span', { 'class': 'av-mut' }, 's') ])),
				u.field(_('Hook-flash timing'), _('How long a press of the flash key lasts, in milliseconds. The maximum must be at least 100 above the minimum.'),
					E('div', { 'class': 'av-inline' }, [ flashMin, E('span', { 'class': 'av-mut' }, _('to')), flashMax, E('span', { 'class': 'av-mut' }, 'ms') ]))
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', _('Tones')),
				u.field(_('Ringback tone'), _('Automatic plays our ringing tone until the network\'s own sound is heard. Choose Never play if the network already sends one.'), ringback)
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', _('Audio')),
				u.field(_('Earpiece volume'), _('The modem\'s own volume levels'), volume),
				offIf('micGainDb', [ u.field(_('Microphone gain'), _('In dB. Lower the microphone if the other side hears distortion.'),
					E('div', { 'class': 'av-inline' }, [ micGain, E('span', { 'class': 'av-mut' }, 'dB'), live.micGainDb ? '' : tag() ])) ]),
				offIf('earpieceGainDb', [ u.field(_('Earpiece gain'), _('In dB'),
					E('div', { 'class': 'av-inline' }, [ earGain, E('span', { 'class': 'av-mut' }, 'dB'), live.earpieceGainDb ? '' : tag() ])) ]),
				offIf('hd', [ u.field(_('HD voice'), _('Wideband 16 kHz audio, off by default. Saving the change applies it when the line is idle: the chip switches to the wideband mode and the modem\'s PCM rate changes, which restarts the modem (mobile data drops for about half a minute). Switching back does the same in reverse.'),
					E('div', { 'class': 'av-inline' }, [ hd.node ])) ])
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', _('Features')),
				offIf('callWaiting', [ u.field(_('Call waiting'), _('A short beep tells you that a second call is coming in. Flash takes it and holds the first call; flash and 1 ends the first call and takes the new one; flash and 0 refuses it.'),
					E('div', { 'class': 'av-inline' }, [ wait.node, live.callWaiting ? '' : tag() ])) ]),
				offIf('conference', [ u.field(_('3-way calling'), _('Flash holds the call. Dial the second number, then flash and 3 to join the two calls.'),
					E('div', { 'class': 'av-inline' }, [ conf.node, live.conference ? '' : tag() ])) ]),
				offIf('hotline', [ u.field(_('Hotline'), _('Dials this number if you lift the handset and dial nothing for the delay (3 to 99 seconds). Other calls can still be dialled in that time.'),
					E('div', { 'class': 'av-inline' }, [ hotNo, hotDelay, E('span', { 'class': 'av-mut' }, 's') ])) ])
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', _('Recording')),
				E('div', { 'class': 'av-note' }, _('Calls are recorded onto the router\'s own storage while they happen; the recordings are listed on the Calls tab. The oldest give way when the budget is full. A call the answering machine answered is kept as its message, not as a recording; the part of the conversation after you pick the handset up is not recorded.')),
				u.field(_('Record calls'), _('Off by default'), E('div', { 'class': 'av-inline' }, [ recEnabled.node ])),
				u.field(_('Recording folder'), _('Where the recordings are kept. The folder must start with a slash.'), recPath),
				u.field(_('Storage budget'), _('The recordings may fill this many megabytes before the oldest give way (1 to 4095)'),
					E('div', { 'class': 'av-inline' }, [ recMax, E('span', { 'class': 'av-mut' }, 'MB') ])),
				u.field(_('Stored recordings'), _('Everything recorded so far lives in the recording folder.'),
					E('div', { 'class': 'av-inline' }, [ recUsage,
						u.button(_('Delete all'), 'trash', 'cbi-button-remove important', function() { recDeleteAll(); }) ]))
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', _('Answering machine')),
				E('div', { 'class': 'av-note' }, _('An incoming call the handset does not take is answered by the machine: the caller hears the greeting and leaves a message, which appears on the Calls tab. Only the cellular line is answered. Picking up the handset hands the call over: the message ends at the lift and keeps what the caller said up to it — the classic answering-machine behaviour.')),
				u.field(_('Answer the phone'), _('Off by default. The greeting is played toward the caller.'),
					E('div', { 'class': 'av-inline' }, [ machOn.node ])),
				u.field(_('Greeting file'), _('An absolute path on the router to a 16-bit PCM WAV at 8 or 16 kHz, mono or stereo. A file that cannot be read switches the machine off.'),
					machGreeting),
				u.field(_('Answer after'), _('Seconds to ring before the machine answers (0 to 60); zero answers at once'),
					E('div', { 'class': 'av-inline' }, [ machDelay, E('span', { 'class': 'av-mut' }, 's') ])),
				u.field(_('Message folder'), _('Where the messages are kept. The folder must start with a slash.'), machPath),
				u.field(_('Storage budget'), _('The messages may fill this many megabytes before the oldest give way (1 to 4095)'),
					E('div', { 'class': 'av-inline' }, [ machMax, E('span', { 'class': 'av-mut' }, 'MB') ])),
				u.field(_('Longest message'), _('The machine ends the call after this many seconds (10 to 600)'),
					E('div', { 'class': 'av-inline' }, [ machSecs, E('span', { 'class': 'av-mut' }, 's') ])),
				u.field(_('Stored messages'), _('Everything the machine kept lives in the message folder. A message whose call leaves the log can still be played and deleted on the Calls tab.'),
					E('div', { 'class': 'av-inline' }, [ msgUsage,
						u.button(_('Delete all'), 'trash', 'cbi-button-remove important', function() { msgDeleteAll(); }) ])),
				machNote
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', [ _('Speed dial'),
					E('span', { 'class': 'av-r' }, [ speedReady ? '' : tag(),
						u.button(_('Add'), 'plus', '', function() { editors.speedDial(ctx, drawSpeed); }, speedReady ? {} : { 'disabled': true }) ]) ]),
				E('div', { 'class': 'av-note' }, _('A speed dial is two digits followed by #. Without # the digits are an ordinary number.')),
				sdList
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', [ _('Indicator'), live.led ? '' : E('span', { 'class': 'av-r' }, tag()) ]),
				E('div', { 'class': 'av-note' }, _('The LED is solid while the line is ready for calls, blinks fast while a call rings, blinks slowly while the handset is off hook, and stays dark otherwise.')),
				offIf('led', [ u.field(_('Phone LED'), _('One of the router\'s LEDs that shows the phone state'), led),
					u.field(_('Call-blink rate'), _('The fast blink while a call rings, in milliseconds (100 to 3000)'),
						E('div', { 'class': 'av-inline' }, [ ringRate, E('span', { 'class': 'av-mut' }, 'ms') ])),
					u.field(_('Off-hook blink rate'), _('The slow blink while the handset is off hook, in milliseconds (200 to 10000)'),
						E('div', { 'class': 'av-inline' }, [ offhookRate, E('span', { 'class': 'av-mut' }, 'ms') ])) ])
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('details', [
					E('summary', _('Advanced')),
					u.field(_('Modem receive gain'), _('0 to 65535, 8192 is unity. Leave empty to keep the modem\'s own value.'), rxGain),
					u.field(_('Modem USB interface'), _('Change only if the modem is not found. The port in use is shown on the Health tab.'), usb),
					u.field(_('Live view refresh'), _('How often this page asks the phone service for the current state. One second shows a lifted handset or a ring almost at once.'), refresh)
				])
			]),
			err,
			E('div', { 'class': 'av-acts' }, [
				u.button(_('Save and apply'), '', 'cbi-button-save important', save),
				u.button(_('Reset'), '', 'cbi-button-reset', function() { load(); loadRec(); })
			])
		]);

		load();
		loadRec();
		recRefreshUsage();
		msgRefreshUsage();
		drawSpeed();
		return root;
	}
});
