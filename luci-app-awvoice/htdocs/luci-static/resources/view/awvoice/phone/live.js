// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
'use strict';
'require baseclass';
'require rpc';
'require uci';
'require fs';

/*
 * live.js - the provider of the phone page: the data contract v2 (DESIGN.md section 2) served from the phone daemon.
 *
 *   status, health, hangup, ring_test, reload   ubus object awvoice (the daemon's own typed replies)
 *   calls, calls_delete                          ubus object awvoice (the call log)
 *   settings                                     UCI package awvoice, section main, then ubus awvoice reload
 *   log tail                                     /sbin/logread -e awvoice-phoned, redacted again here
 *   start / stop / restart                       ubus luci.awvoice service (the app's rpcd plugin, which guards)
 *   router clock                                 ubus system info
 *
 * A method whose backend is not built yet (the loopback test) rejects with "Not available yet"; available[name] says which
 * ones those are. No sample data lives here.
 */

/* one remote call of an object on the router's message bus, as a function that returns a Promise */
function remote(object, method, params) {
	return rpc.declare({ object: object, method: method, params: params || [] });
}

var bus = {
	status: remote('awvoice', 'status'),
	health: remote('awvoice', 'health'),
	hangup: remote('awvoice', 'hangup'),
	ring: remote('awvoice', 'ring_test', [ 'seconds' ]),
	reload: remote('awvoice', 'reload'),
	lineTest: remote('awvoice', 'line_test'),
	forwardingGet: remote('awvoice', 'forwarding_get'),
	calls: remote('awvoice', 'calls'),
	callsDelete: remote('awvoice', 'calls_delete', [ 'type' ]),
	service: remote('luci.awvoice', 'service', [ 'action' ]),
	commit: remote('uci', 'commit', [ 'config' ]),
	system: remote('system', 'info')
};

var CONF = 'awvoice', SECT = 'main';
var LOG_LINES = 40;
var RING_TEST_SEC = 5;

var STATES = { idle: 1, offhook: 1, ring: 1, call: 1, noservice: 1, down: 1 };
var PHASES = { ringing: 1, dialing: 1, alerting: 1, active: 1 };

/* the backends that exist today; everything else rejects with "Not available yet" */
var AVAILABLE = {
	getStatus: true, getHealth: true, hangup: true, getClock: true,
	getPhoneSettings: true, savePhoneSettings: true, runTest: true, serviceAction: true,
	ringTest: true, lineTest: true, loopbackTest: false,
	listCalls: true, deleteCalls: true,
	listContacts: true, saveContact: true, removeContact: true,
	getBlocking: true, saveBlocking: true,
	getForwarding: true, saveForwarding: true,
	listAlarms: true, saveAlarm: true, removeAlarm: true
};

/* the settings fields whose daemon support exists (M4); the rest are shown but cannot be saved yet */
var SETTINGS_LIVE = {
	cid: true, cidWhen: true, myNumber: true, dialPauseSec: true, hookFlash: true, ringback: true,
	earpieceVolume: true, modemRxGain: true, usbInterface: true,
	clir: true, hotline: true, conference: true, callWaiting: true, led: true, hd: true
};

function notYet() {
	return Promise.reject(new Error(_('Not available yet')));
}

/* a ubus failure in words that are safe to show; the technical text stays out of the page */
function fail(err) {
	var m = String((err && err.message) || err || '');
	if (/not found|no such|unknown object|-32000/i.test(m))
		return new Error(_('The phone service is not running.'));
	if (/denied|permission|-32002/i.test(m))
		return new Error(_('This account may not do that.'));
	return new Error(_('The phone service did not answer.'));
}

function tri(v) { return v === true || v === false ? v : null; }
function num(v) { return typeof v === 'number' && isFinite(v) ? v : null; }
function str(v) { return typeof v === 'string' ? v : ''; }

function downStatus() {
	return {
		state: 'down', line: 'down', hook: null, call: null,
		voiceNetwork: { registered: null, ims: null },
		signal: { rsrpDbm: null, network: null },
		service: { running: false }, myNumber: ''
	};
}

function normStatus(r) {
	r = r || {};
	var c = r.call && typeof r.call === 'object' ? r.call : null;
	var vn = r.voiceNetwork || {}, sg = r.signal || {}, sv = r.service || {};
	return {
		state: STATES[r.state] ? r.state : 'down',
		line: str(r.line),
		hook: r.hook === 'on' || r.hook === 'off' ? r.hook : null,
		call: c ? {
			id: num(c.id), direction: c.direction === 'OUT' ? 'OUT' : 'IN', number: str(c.number),
			phase: PHASES[c.phase] ? c.phase : 'dialing', elapsedSec: Math.max(0, num(c.elapsedSec) || 0)
		} : null,
		voiceNetwork: { registered: tri(vn.registered), ims: tri(vn.ims) },
		signal: { rsrpDbm: num(sg.rsrpDbm), network: str(sg.network) || null },
		service: { running: sv.running !== false },
		myNumber: str(r.myNumber)
	};
}

function normHealth(r) {
	r = r || {};
	var a = r.atPort || {}, l = r.line || {}, m = r.modem || {}, s = r.system || {}, b = s.bridge || {}, k = r.counters || {}, i = r.indicator || {};
	return {
		atPort: { port: str(a.port), usbInterface: num(a.usbInterface), responding: tri(a.responding), lastReplyAgoSec: num(a.lastReplyAgoSec) },
		line: { channel: num(l.channel), hook: l.hook === 'on' || l.hook === 'off' ? l.hook : null, state: str(l.state),
			calibrated: tri(l.calibrated), batteryMv: num(l.batteryMv), loopUa: num(l.loopUa) },
		modem: { simReady: tri(m.simReady), registered: tri(m.registered), imsRegistered: tri(m.imsRegistered),
			rsrpDbm: num(m.rsrpDbm), network: str(m.network) || null, audioSlave: tri(m.audioSlave), clir: m.clir === 'network' || m.clir === 'hide' || m.clir === 'show' ? m.clir : null, clirStatus: str(m.clirStatus) || null },
		system: { moduleLoaded: tri(s.moduleLoaded), bridge: { running: tri(b.running), frames: num(b.frames), resyncs: num(b.resyncs),
			txSkipped: num(b.txSkipped), maxGapUs: num(b.maxGapUs) } },
		indicator: { led: str(i.led) || null, state: str(i.state) || null,
			leds: Array.isArray(i.leds) ? i.leds.map(str).filter(function(x) { return x; }) : [] },
		counters: { modemTimeouts: num(k.modemTimeouts), hookGapsOver20ms: num(k.hookGapsOver20ms),
			hookGapMaxMs: num(k.hookGapMaxMs), faults: num(k.faults) },
		log: { tail: [] }
	};
}

/* numbers must not reach the page through the log: long digit runs and grouped digits are masked */
function redact(line) {
	return String(line)
		.replace(/\+?\d{2,4}[ -]\d{3,4}[ -]\d{3,4}/g, '[number]')
		.replace(/\+?\d{6,}/g, '[number]');
}

function readTail() {
	return fs.exec_direct('/sbin/logread', [ '-e', 'awvoice-phoned' ], 'text').then(function(out) {
		return String(out || '').split('\n').filter(function(s) { return s.length; }).slice(-LOG_LINES).map(redact);
	}, function() { return []; });
}

/* ---- settings ---- */

function optText(name, def) {
	var v = uci.get(CONF, SECT, name);
	return v == null ? def : String(v);
}

function optInt(name, lo, hi, def) {
	var v = uci.get(CONF, SECT, name);
	if (v == null || !/^-?\d+$/.test(String(v)))
		return def;
	v = parseInt(v, 10);
	return v >= lo && v <= hi ? v : def;
}

function pick(name, allowed, def) {
	var v = optText(name, def);
	return allowed.indexOf(v) >= 0 ? v : def;
}

function readSettings() {
	var rb = optText('ringback', 'auto');
	if (rb === '1') rb = 'on';
	else if (rb === '0') rb = 'off';
	if ([ 'auto', 'on', 'off' ].indexOf(rb) < 0) rb = 'auto';

	return {
		line: { cid: pick('cid', [ 'dtmf', 'off', 'bellcore', 'etsi' ], 'dtmf'), cidWhen: pick('cid_when', [ 'ring1', 'prering' ], 'ring1'), myNumber: optText('my_number', ''), dialPauseSec: optInt('dial_pause_s', 3, 15, 6) },
		hookFlash: { minMs: optInt('flash_min_ms', 20, 500, 80), maxMs: optInt('flash_max_ms', 300, 3000, 300) },
		tones: { ringback: rb },
		audio: {
			earpieceVolume: optInt('earpiece_volume', 0, 99, null),
			modemRxGain: optInt('modem_rx_gain', 0, 65535, null),
			micGainDb: optInt('mic_gain_db', -99, 99, null),
			earpieceGainDb: optInt('earpiece_gain_db', -99, 99, null),
			hd: optText('hd', '0') === '1'
		},
		features: {
			clir: pick('clir', [ 'network', 'hide', 'show' ], 'network'),
			conference: optText('conference', '0') === '1',
			callWaiting: optText('call_waiting', '1') !== '0',
			hotline: { number: optText('hotline_number', ''), delaySec: optInt('hotline_delay', 3, 99, 10) }
		},
		indicator: { led: optText('led', ''), ringRateMs: optInt('ring_rate_ms', 100, 3000, 300), offhookRateMs: optInt('offhook_rate_ms', 200, 10000, 1000) },
		modem: { usbInterface: optInt('at_interface', 0, 9, 2) },
		page: { refreshSec: optInt('page_refresh_s', 1, 30, 1) }
	};
}

function bad(msg) {
	return Promise.reject(new Error(msg));
}

var CONTACT_MAX = 100, CONTACT_NUMBERS_MAX = 3, CONTACT_TYPES = { mobile: 1, home: 1, work: 1 };

/* the contacts: UCI sections of type contact; the section name is the id */
function readContacts() {
	return uci.sections(CONF, 'contact').map(function(s) {
		var raw = s.number == null ? [] : (Array.isArray(s.number) ? s.number : [ s.number ]);
		return {
			id: s['.name'], name: str(s.name),
			numbers: raw.map(function(t) {
				var i = String(t).lastIndexOf(':');
				var type = i >= 0 ? String(t).slice(i + 1) : 'mobile';
				return { value: i >= 0 ? String(t).slice(0, i) : String(t), type: CONTACT_TYPES[type] ? type : 'mobile' };
			}),
			quick: str(s.quick)
		};
	});
}

/* the daemon's rules for a contact (phonebook_add), checked here first so that the message is clear */
function validateContact(c, others) {
	var numRe = /^\+?[0-9*#]{1,32}$/, k;
	if (!c || typeof c.name !== 'string' || !c.name.trim() || c.name.length > 32)
		return _('The name must be 1 to 32 characters');
	if (!Array.isArray(c.numbers) || c.numbers.length < 1 || c.numbers.length > CONTACT_NUMBERS_MAX)
		return _('A contact has one to three numbers');
	for (k = 0; k < c.numbers.length; k++)
		if (!numRe.test(c.numbers[k].value) || !CONTACT_TYPES[c.numbers[k].type])
			return _('Numbers may contain digits, *, # and a leading +');
	if (c.quick && !/^[0-9]{2}$/.test(c.quick))
		return _('Speed dial code must be exactly 2 digits');
	if (c.quick && others.some(function(o) { return o.quick === c.quick; }))
		return _('That speed dial code is already used');
	return null;
}

/* write what is staged in uci, commit it and tell the daemon */
function storeConfig() {
	return uci.save().then(function() {
		return bus.commit(CONF);
	}).then(function() {
		uci.unload(CONF);
		return bus.reload().then(function() { return true; }, function() {
			throw new Error(_('Saved, but the phone service did not take the change. Restart it on the Health tab.'));
		});
	});
}

/* call blocking: the first UCI section of type blocking (its name cannot be main: that is the settings section) */
var BLOCK_LISTS = { incoming: 'incoming', incomingPrefix: 'incoming_prefix', outgoing: 'outgoing', outgoingPrefix: 'outgoing_prefix' };
var BLOCK_MAX = 50;

function listOf(v) {
	return v == null ? [] : (Array.isArray(v) ? v.map(String) : [ String(v) ]);
}

function readBlocking() {
	var s = uci.sections(CONF, 'blocking')[0] || {};
	return {
		anonymous: s.anonymous === '1',
		incoming: listOf(s.incoming), incomingPrefix: listOf(s.incoming_prefix),
		outgoing: listOf(s.outgoing), outgoingPrefix: listOf(s.outgoing_prefix)
	};
}

function validateBlocking(b) {
	var exact = /^\+?[0-9*#]{1,32}$/, prefix = /^\+?[0-9]{1,32}$/, bad = null;
	if (!b || typeof b.anonymous !== 'boolean')
		return _('Invalid blocking settings');
	Object.keys(BLOCK_LISTS).forEach(function(k) {
		var re = /Prefix$/.test(k) ? prefix : exact;
		if (!Array.isArray(b[k]) || b[k].length > BLOCK_MAX)
			bad = bad || _('A list holds at most 50 entries');
		else if (!b[k].every(function(v) { return re.test(v) && v !== '+'; }))
			bad = bad || (/Prefix$/.test(k) ? _('Prefixes may contain digits and a leading +') : _('Numbers may contain digits, *, # and a leading +'));
	});
	return bad;
}

/* alarm calls: UCI sections of type alarm; the section name is the id */
var ALARM_MAX = 20;

function readAlarms() {
	return uci.sections(CONF, 'alarm').map(function(s) {
		return {
			id: s['.name'], label: str(s.label), time: str(s.time),
			days: listOf(s.day).map(Number).filter(function(d) { return d >= 1 && d <= 7; }),
			enabled: s.enabled !== '0'
		};
	});
}

function validateAlarm(a) {
	if (!a || typeof a.label !== 'string' || a.label.length > 32)
		return _('The label may have at most 32 characters');
	if (!/^([01][0-9]|2[0-3]):[0-5][0-9]$/.test(a.time))
		return _('Enter the time as HH:MM, for example 06:30');
	if (!Array.isArray(a.days) || !a.days.length || !a.days.every(function(d) { return d >= 1 && d <= 7 && d === Math.floor(d); }))
		return _('Pick at least one day');
	return null;
}

/* call forwarding: the first UCI section of type forwarding is the setting; the network's own report comes from the daemon (forwarding_get) */
var FWD_WHEN = [ 'always', 'busy', 'noanswer', 'unreachable', 'all_conditional' ];

function netWord(v) { return v === 'on' || v === 'off' ? v : 'unknown'; }

function readForwarding() {
	var s = uci.sections(CONF, 'forwarding')[0] || {};
	var d = parseInt(s.delay, 10);
	return {
		enabled: s.enabled === '1', to: str(s.to), when: FWD_WHEN.indexOf(s.when) >= 0 ? s.when : 'always',
		delaySec: d >= 5 && d <= 30 && d % 5 === 0 ? d : 20
	};
}

function validateForwarding(f) {
	if (!f || typeof f.enabled !== 'boolean')
		return _('Invalid forwarding settings');
	if (f.to && !/^\+?[0-9]{1,20}$/.test(f.to))
		return _('The forwarding number may contain digits and a leading +, at most 20 characters');
	if (f.enabled && !f.to)
		return _('Enter the number to forward to');
	if (FWD_WHEN.indexOf(f.when) < 0)
		return _('Choose when calls are forwarded');
	if (!(f.delaySec >= 5 && f.delaySec <= 30 && f.delaySec % 5 === 0))
		return _('The no-answer delay must be 5 to 30 seconds in steps of 5');
	return null;
}

/* what the network reports, read until the daemon has finished setting it (it starts when the line is idle) */
function forwardingReadback(tries) {
	return new Promise(function(resolve) { window.setTimeout(resolve, 1500); }).then(function() {
		return bus.forwardingGet();
	}).then(function(r) {
		r = r || {};
		if (r.applying === true && tries > 0)
			return forwardingReadback(tries - 1);
		return r;
	});
}

function validate(p) {
	var numRe = /^\+?[0-9*#]{1,32}$/;
	if (p.line.cid !== 'dtmf' && p.line.cid !== 'off' && p.line.cid !== 'bellcore' && p.line.cid !== 'etsi')
		return _('Caller ID must be DTMF, FSK (Bellcore), FSK (ETSI) or Off');
	if (p.line.cidWhen != null && [ 'ring1', 'prering' ].indexOf(p.line.cidWhen) < 0)
		return _('The caller-id timing must be after the first ring or before the first ring');
	if (p.line.myNumber && !numRe.test(p.line.myNumber))
		return _('Numbers may contain digits, *, # and a leading +');
	if (!(p.line.dialPauseSec >= 3 && p.line.dialPauseSec <= 15))
		return _('Dial pause must be 3 to 15 seconds');
	if (!(p.hookFlash.minMs >= 20 && p.hookFlash.minMs <= 500))
		return _('Hook-flash minimum must be 20 to 500 ms');
	if (!(p.hookFlash.maxMs >= 300 && p.hookFlash.maxMs <= 3000))
		return _('Hook-flash maximum must be 300 to 3000 ms');
	if (p.hookFlash.maxMs < p.hookFlash.minMs + 100)
		return _('Hook-flash maximum must be at least 100 ms above the minimum');
	if ([ 'auto', 'on', 'off' ].indexOf(p.tones.ringback) < 0)
		return _('Ringback tone must be Automatic, On or Off');
	if (p.audio.earpieceVolume != null && !(p.audio.earpieceVolume >= 0 && p.audio.earpieceVolume <= 99))
		return _('Earpiece volume is out of range');
	if (p.audio.modemRxGain != null && !(p.audio.modemRxGain >= 0 && p.audio.modemRxGain <= 65535))
		return _('Modem receive gain must be 0 to 65535');
	if (!(p.modem.usbInterface >= 0 && p.modem.usbInterface <= 9))
		return _('Modem USB interface must be 0 to 9');
	if ([ 'network', 'hide', 'show' ].indexOf(p.features.clir) < 0)
		return _('Hide my number must be Network default, Hide or Show');
	if (p.features.hotline.number && !/^\+?[0-9*#]{1,20}$/.test(p.features.hotline.number))
		return _('The hotline number may contain digits, *, # and a leading +, at most 20 characters');
	if (!(p.features.hotline.delaySec >= 3 && p.features.hotline.delaySec <= 99))
		return _('The hotline delay must be 3 to 99 seconds');
	if (p.page != null && p.page.refreshSec != null && !(p.page.refreshSec >= 1 && p.page.refreshSec <= 30))
		return _('The live view refresh must be 1 to 30 seconds');
	if (p.indicator != null) {
		if (p.indicator.led && !/^[0-9A-Za-z:_-]{1,63}$/.test(p.indicator.led))
			return _('The indicator LED name is not valid');
		if (!(p.indicator.ringRateMs >= 100 && p.indicator.ringRateMs <= 3000))
			return _('The indicator call-blink rate must be 100 to 3000 ms');
		if (!(p.indicator.offhookRateMs >= 200 && p.indicator.offhookRateMs <= 10000))
			return _('The indicator off-hook blink rate must be 200 to 10000 ms');
	}
	return null;
}

return baseclass.extend({
	available: AVAILABLE,
	downStatus: downStatus,

	/* ---- live (M4) ---- */

	getStatus: function() {
		return bus.status().then(normStatus, function(e) { throw fail(e); });
	},

	getHealth: function() {
		return Promise.all([ bus.health(), readTail() ]).then(function(r) {
			var h = normHealth(r[0]);
			h.log.tail = r[1];
			return h;
		}, function(e) { throw fail(e); });
	},

	hangup: function() {
		return bus.hangup().then(function(r) {
			r = r || {};
			return { ok: r.ok === true, refused: r.refused || null };
		}, function(e) { throw fail(e); });
	},

	/* the router's wall clock against this computer's: the alarm editor warns when they differ */
	getClock: function() {
		return bus.system().then(function(r) {
			var t = r && typeof r.localtime === 'number' ? r.localtime : null;
			if (t == null)
				return { routerTime: '', diffMin: null };
			var d = new Date(t * 1000), pad = function(n) { return (n < 10 ? '0' : '') + n; };
			var mine = Date.now() / 1000 - new Date().getTimezoneOffset() * 60;
			return {
				routerTime: d.getUTCFullYear() + '-' + pad(d.getUTCMonth() + 1) + '-' + pad(d.getUTCDate()) + ' ' + pad(d.getUTCHours()) + ':' + pad(d.getUTCMinutes()),
				diffMin: Math.round((t - mine) / 60)
			};
		}, function(e) { throw fail(e); });
	},

	getPhoneSettings: function() {
		uci.unload(CONF);
		return uci.load(CONF).then(readSettings, function(e) { throw fail(e); });
	},

	/* writes the options of the milestone that exists and applies them; the other fields are left as they are */
	savePhoneSettings: function(p) {
		var msg = validate(p);
		if (msg)
			return bad(msg);

		uci.unload(CONF);
		var stored = uci.load(CONF).then(function() {
			function put(name, v) {
				if (v == null || v === '')
					uci.unset(CONF, SECT, name);
				else
					uci.set(CONF, SECT, name, String(v));
			}

			if (uci.get(CONF, SECT) == null)
				uci.add(CONF, 'phone', SECT);

			put('cid', p.line.cid);
			put('my_number', p.line.myNumber);
			put('dial_pause_s', p.line.dialPauseSec);
			put('flash_min_ms', p.hookFlash.minMs);
			put('flash_max_ms', p.hookFlash.maxMs);
			put('ringback', p.tones.ringback);
			put('earpiece_volume', p.audio.earpieceVolume);
			put('modem_rx_gain', p.audio.modemRxGain);
			put('hd', p.audio.hd ? '1' : '0');
			put('at_interface', p.modem.usbInterface);
			put('page_refresh_s', p.page != null && p.page.refreshSec != null ? p.page.refreshSec : null);
			put('clir', p.features.clir);
			put('hotline_number', p.features.hotline.number);
			put('hotline_delay', p.features.hotline.delaySec);
			put('conference', p.features.conference ? '1' : '0');
			put('call_waiting', p.features.callWaiting ? '1' : '0');
			if (p.indicator != null) {
				put('led', p.indicator.led);
				put('ring_rate_ms', p.indicator.ringRateMs);
				put('offhook_rate_ms', p.indicator.offhookRateMs);
			}
			if (p.line.cidWhen != null)
				put('cid_when', p.line.cidWhen);

			return uci.save();
		}).then(function() {
			return bus.commit(CONF);
		}).then(null, function(e) { throw fail(e); });

		return stored.then(function() {
			uci.unload(CONF);
			return bus.reload().then(function() { return { ok: true, refused: null }; }, function() {
				throw new Error(_('Saved, but the phone service did not take the change. Restart it on the Health tab.'));
			});
		});
	},

	runTest: function(kind) {
		if (kind === 'ring')
			return bus.ring(RING_TEST_SEC).then(function(r) {
				r = r || {};
				return { ok: r.ok === true, refused: r.refused || null };
			}, function(e) { throw fail(e); });
		if (kind === 'line')
			return bus.lineTest().then(function(r) {
				r = r || {};
				if (r.refused)
					return { ok: false, refused: r.refused };
				/* the page shows "finished" for ok and a message for a fault: the verdict is turned into words here */
				var words = {
					off_hook: _('The handset was lifted during the test. Put it on hook and run it again.'),
					no_battery: _('The line has no battery voltage. The phone line may be faulty.'),
					leak: _('The line leaks current. Check the phone and the cable.'),
					interrupted: _('A call came in during the test. Run it again.'),
					error: _('The line could not be measured.')
				};
				if (r.ok === true)
					return { ok: true, refused: null };
				throw new Error(words[r.verdict] || words.error);
			}, function(e) { throw fail(e); });
		return notYet();
	},

	serviceAction: function(action) {
		if ([ 'start', 'stop', 'restart' ].indexOf(action) < 0)
			return bad(_('Unknown action'));
		return bus.service(action).then(function(r) {
			r = r || {};
			return { ok: r.ok === true, refused: r.refused || null };
		}, function(e) { throw fail(e); });
	},

	/* the call log, newest first */
	listCalls: function() {
		return bus.calls().then(function(r) {
			var list = r && Array.isArray(r.calls) ? r.calls : [];
			return list.map(function(c) {
				return {
					id: num(c.id), type: c.type === 'OUT' || c.type === 'MISS' ? c.type : 'IN', number: str(c.number), name: str(c.name),
					ts: num(c.ts) || 0, durationSec: Math.max(0, num(c.durationSec) || 0), result: str(c.result), cause: num(c.cause) || 0
				};
			});
		}, function(e) { throw fail(e); });
	},

	/* all calls, or only one type ('IN', 'OUT' or 'MISS') */
	deleteCalls: function(p) {
		var type = p && p.type;
		if (type != null && [ 'IN', 'OUT', 'MISS' ].indexOf(type) < 0)
			return bad(_('Unknown call type'));
		return bus.callsDelete(type || undefined).then(function(r) {
			return { ok: !!(r && r.ok === true), refused: null };
		}, function(e) { throw fail(e); });
	},

	listContacts: function() {
		uci.unload(CONF);
		return uci.load(CONF).then(readContacts, function(e) { throw fail(e); });
	},

	/* id 0 (or none) adds a contact; returns the saved contact */
	saveContact: function(c) {
		uci.unload(CONF);
		return uci.load(CONF).then(function() {
			var all = readContacts(), id = c && c.id ? String(c.id) : '';
			var msg = validateContact(c, all.filter(function(o) { return o.id !== id; }));
			if (msg)
				throw new Error(msg);
			if (!id) {
				if (all.length >= CONTACT_MAX)
					throw new Error(_('The phone book is full'));
				id = uci.add(CONF, 'contact');
			} else if (!all.some(function(o) { return o.id === id; })) {
				throw new Error(_('This contact no longer exists'));
			}
			uci.set(CONF, id, 'name', c.name.trim());
			uci.set(CONF, id, 'number', c.numbers.map(function(n) { return n.value + ':' + n.type; }));
			if (c.quick)
				uci.set(CONF, id, 'quick', c.quick);
			else
				uci.unset(CONF, id, 'quick');
			return storeConfig().then(function() {
				return { id: id, name: c.name.trim(), numbers: c.numbers, quick: c.quick || '' };
			});
		}).then(null, function(e) { throw e && e.message ? e : fail(e); });
	},

	removeContact: function(id) {
		uci.unload(CONF);
		return uci.load(CONF).then(function() {
			if (!readContacts().some(function(o) { return o.id === String(id); }))
				throw new Error(_('This contact no longer exists'));
			uci.remove(CONF, String(id));
			return storeConfig().then(function() { return { ok: true, refused: null }; });
		}).then(null, function(e) { throw e && e.message ? e : fail(e); });
	},

	getBlocking: function() {
		uci.unload(CONF);
		return uci.load(CONF).then(readBlocking, function(e) { throw fail(e); });
	},

	/* returns the saved blocking settings */
	saveBlocking: function(b) {
		var msg = validateBlocking(b);
		if (msg)
			return bad(msg);
		uci.unload(CONF);
		return uci.load(CONF).then(function() {
			var sec = uci.sections(CONF, 'blocking')[0], id = sec ? sec['.name'] : uci.add(CONF, 'blocking', 'blocking');
			uci.set(CONF, id, 'anonymous', b.anonymous ? '1' : '0');
			Object.keys(BLOCK_LISTS).forEach(function(k) {
				if (b[k].length)
					uci.set(CONF, id, BLOCK_LISTS[k], b[k]);
				else
					uci.unset(CONF, id, BLOCK_LISTS[k]);
			});
			return storeConfig();
		}).then(function() {
			return {
				anonymous: b.anonymous, incoming: b.incoming.slice(), incomingPrefix: b.incomingPrefix.slice(),
				outgoing: b.outgoing.slice(), outgoingPrefix: b.outgoingPrefix.slice()
			};
		}, function(e) { throw e && e.message ? e : fail(e); });
	},

	listAlarms: function() {
		uci.unload(CONF);
		return uci.load(CONF).then(readAlarms, function(e) { throw fail(e); });
	},

	/* id 0 (or none) adds an alarm; returns the saved alarm */
	saveAlarm: function(a) {
		var msg = validateAlarm(a);
		if (msg)
			return bad(msg);
		uci.unload(CONF);
		return uci.load(CONF).then(function() {
			var all = readAlarms(), id = a.id ? String(a.id) : '';
			if (!id) {
				if (all.length >= ALARM_MAX)
					throw new Error(_('At most 20 alarms can be set'));
				id = uci.add(CONF, 'alarm');
			} else if (!all.some(function(o) { return o.id === id; })) {
				throw new Error(_('This alarm no longer exists'));
			}
			var days = a.days.slice().sort().map(String);
			uci.set(CONF, id, 'label', a.label);
			uci.set(CONF, id, 'time', a.time);
			uci.set(CONF, id, 'day', days);
			uci.set(CONF, id, 'enabled', a.enabled ? '1' : '0');
			return storeConfig().then(function() {
				return { id: id, label: a.label, time: a.time, days: days.map(Number), enabled: !!a.enabled };
			});
		}).then(null, function(e) { throw e && e.message ? e : fail(e); });
	},

	removeAlarm: function(id) {
		uci.unload(CONF);
		return uci.load(CONF).then(function() {
			if (!readAlarms().some(function(o) { return o.id === String(id); }))
				throw new Error(_('This alarm no longer exists'));
			uci.remove(CONF, String(id));
			return storeConfig().then(function() { return { ok: true, refused: null }; });
		}).then(null, function(e) { throw e && e.message ? e : fail(e); });
	},

	getForwarding: function() {
		uci.unload(CONF);
		return Promise.all([ uci.load(CONF).then(readForwarding), bus.forwardingGet().then(null, function() { return {}; }) ]).then(function(r) {
			r[0].network = netWord(r[1] && r[1].network);
			return r[0];
		}, function(e) { throw fail(e); });
	},

	/* saves the setting, lets the daemon set it at the network and returns what the network reports; a refusal is the error text */
	saveForwarding: function(f) {
		var msg = validateForwarding(f);
		if (msg)
			return bad(msg);
		uci.unload(CONF);
		return uci.load(CONF).then(function() {
			var sec = uci.sections(CONF, 'forwarding')[0], id = sec ? sec['.name'] : uci.add(CONF, 'forwarding', 'forwarding');
			uci.set(CONF, id, 'enabled', f.enabled ? '1' : '0');
			uci.set(CONF, id, 'to', f.to);
			uci.set(CONF, id, 'when', f.when);
			uci.set(CONF, id, 'delay', String(f.delaySec));
			return storeConfig();
		}).then(function() {
			return forwardingReadback(10);
		}).then(function(r) {
			if (r.error)
				throw new Error(_('The network did not accept the setting (%s)').format(r.error));
			return { enabled: f.enabled, to: f.to, when: f.when, delaySec: f.delaySec, network: netWord(r.network) };
		}, function(e) { throw e && e.message ? e : fail(e); });
	},

	/* ---- later milestones: nothing to call yet ---- */


	settingsLive: SETTINGS_LIVE
});
