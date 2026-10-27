// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
'use strict';
'require baseclass';
'require view.awvoice.phone.editors as editors';

/*
 * tab_settings.js - Phone settings tab: line, tones, audio, features, recording, speed dial, indicator and advanced
 * options. Edits are held in the form and written with "Save and apply"; "Reset" discards them. Fields whose daemon
 * support is a later milestone are shown disabled with a "Not available yet" tag (api.settingsLive says which are live).
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
				setRefresh(p.page.refreshSec);
				err.textContent = '';
				return ctx.api.getHealth();
			}).then(function(health) {
				h = health;
				fill();
			}, function(e) { err.textContent = e.message; });
		}

		function loadRec() {
			return ctx.api.getRecordingSettings().then(function(s) {
				recEnabled.set(s.enabled);
				recPath.value = s.path;
				recMax.value = String(s.maxMb);
			}, function(e) { err.textContent = err.textContent || e.message; });
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

			var rp = recPath.value.trim(), rm = parseInt(recMax.value, 10);
			if (!/^\//.test(rp)) { err.textContent = _('The recording folder must start with a slash'); return; }
			if (!(rm >= 1 && rm <= 4095)) { err.textContent = _('The recording budget must be 1 to 4095 MB'); return; }

			ctx.api.savePhoneSettings({
				line: { cid: cid.value, cidWhen: when(), myNumber: my, dialPauseSec: dp },
				hookFlash: { minMs: fmin, maxMs: fmax },
				tones: { ringback: ringback.value },
				audio: { earpieceVolume: volume.value === '' ? null : parseInt(volume.value, 10), modemRxGain: rx,
					micGainDb: optInt(micGain), earpieceGainDb: optInt(earGain), hd: hd.get() },
				features: { clir: loaded.features.clir, conference: conf.get(), callWaiting: wait.get(), hotline: { number: hn, delaySec: parseInt(hotDelay.value, 10) || loaded.features.hotline.delaySec } },
				indicator: { led: led.value, ringRateMs: rr, offhookRateMs: orate },
				modem: { usbInterface: ui },
				page: { refreshSec: parseInt(refresh.value, 10) || 1 }
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
				E('div', { 'class': 'av-note' }, _('Calls are recorded onto the router\'s own storage while they happen; the recordings are listed on the Calls tab. The oldest give way when the budget is full.')),
				u.field(_('Record calls'), _('Off by default'), E('div', { 'class': 'av-inline' }, [ recEnabled.node ])),
				u.field(_('Recording folder'), _('Where the recordings are kept. The folder must start with a slash.'), recPath),
				u.field(_('Storage budget'), _('The recordings may fill this many megabytes before the oldest give way (1 to 4095)'),
					E('div', { 'class': 'av-inline' }, [ recMax, E('span', { 'class': 'av-mut' }, 'MB') ]))
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
		drawSpeed();
		return root;
	}
});
