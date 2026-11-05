// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
'use strict';
'require baseclass';

/*
 * fixture.js - test data for the render check (tests/harness/index.html). It stands in for phone/live.js only in that
 * harness and is not part of the installed package. ?state=idle|offhook|ring|ring_anon|dial|call|noservice|down picks
 * the line state; &later=1 also switches on the features the plain fixture leaves unanswered, with sample records.
 */

var q = new URLSearchParams(location.search);
var STATE = q.get('state') || 'idle';
var LATER = q.get('later') === '1';
var now = Math.floor(Date.now() / 1000);

function status() {
	var s = {
		state: 'idle', line: 'idle', hook: 'on', call: null,
		voiceNetwork: { registered: true, ims: true }, signal: { rsrpDbm: -97, network: 'LTE' },
		service: { running: true }, myNumber: '0112345678'
	};
	if (STATE === 'down')
		return Promise.reject(new Error('The phone service is not running.'));
	if (STATE === 'offhook') { s.state = 'offhook'; s.hook = 'off'; }
	if (STATE === 'ring') { s.state = 'ring'; s.call = { id: 3, direction: 'IN', number: '0112000001', phase: 'ringing', elapsedSec: 0 }; }
	if (STATE === 'ring_anon') { s.state = 'ring'; s.call = { id: 3, direction: 'IN', number: '', phase: 'ringing', elapsedSec: 0 }; }
	if (STATE === 'dial') { s.state = 'call'; s.hook = 'off'; s.call = { id: 4, direction: 'OUT', number: '0112223344', phase: 'alerting', elapsedSec: 0 }; }
	if (STATE === 'call') { s.state = 'call'; s.hook = 'off'; s.call = { id: 5, direction: 'IN', number: '0112000001', phase: 'active', elapsedSec: 125 }; }
	if (STATE === 'noservice') { s.state = 'noservice'; s.voiceNetwork = { registered: false, ims: false }; s.signal = { rsrpDbm: null, network: null }; }
	return Promise.resolve(s);
}

var settings = {
	line: { cid: 'dtmf', myNumber: '0112345678', dialPauseSec: 6 }, hookFlash: { minMs: 80, maxMs: 300 }, tones: { ringback: 'auto' },
	audio: { earpieceVolume: 3, modemRxGain: null, micGainDb: null, earpieceGainDb: null },
	features: { clir: 'network', conference: false, hotline: { number: '', delaySec: 10 } },
	indicator: { led: '' }, modem: { usbInterface: 2 }
};

var calls = [
	{ id: 1, type: 'MISS', number: '0112000001', ts: now - 600, durationSec: 0, result: 'missed', cause: 19 },
	{ id: 2, type: 'IN', number: '', ts: now - 4000, durationSec: 75, result: 'answered', cause: 16 },
	{ id: 3, type: 'OUT', number: '0112223344', ts: now - 90000, durationSec: 0, result: 'busy', cause: 17 }
];
var contacts = [ { id: 1, name: 'Home', numbers: [ { value: '0112223344', type: 'home' } ], quick: '11', ringThrough: true },
	{ id: 2, name: 'Doctor', numbers: [ { value: '0770000001', type: 'mobile' } ], quick: '', ringThrough: false } ];
var blocking = { anonymous: false, incoming: [ '0112000002' ], incomingPrefix: [ '+9477' ], outgoing: [], outgoingPrefix: [] };
var forwarding = { enabled: false, to: '', when: 'always', delaySec: 20, network: 'unknown' };
var alarms = [ { id: 1, label: 'Morning', time: '06:30', days: [ 1, 2, 3, 4, 5 ], enabled: true } ];
var quiet = {
	windows: [
		{ id: 'w1', enabled: true, from: '22:00', to: '07:00', allDay: false, days: [ 1, 2, 3, 4, 5 ], mode: 'machine', repeatMin: 15, allow: [ '0771234567' ] },
		{ id: 'w2', enabled: false, from: '', to: '', allDay: true, days: [ 6, 7 ], mode: 'silent', repeatMin: 0, allow: [] }
	],
	allow: [ '0771234567' ]
};

function later(v) { return Promise.resolve(JSON.parse(JSON.stringify(v))); }
function notYet() { return Promise.reject(new Error('Not available yet')); }
function served(v) { return LATER ? later(v) : notYet(); }

return baseclass.extend({
	available: {
		getStatus: true, getHealth: true, hangup: true, getClock: true, getPhoneSettings: true, savePhoneSettings: true,
		runTest: true, serviceAction: true, ringTest: true, lineTest: LATER, loopbackTest: false,
		listCalls: LATER, deleteCalls: LATER, listContacts: LATER, saveContact: LATER, removeContact: LATER,
		getBlocking: LATER, saveBlocking: LATER, getForwarding: LATER, saveForwarding: LATER, listAlarms: LATER, saveAlarm: LATER, removeAlarm: LATER,
		listQuiet: LATER, saveQuiet: LATER, removeQuiet: LATER, saveQuietAllow: LATER
	},
	settingsLive: { cid: true, myNumber: true, dialPauseSec: true, hookFlash: true, ringback: true, earpieceVolume: true,
		modemRxGain: true, usbInterface: true, micGainDb: LATER, earpieceGainDb: LATER, clir: LATER, led: LATER },
	downStatus: function() {
		return { state: 'down', line: 'down', hook: null, call: null, voiceNetwork: { registered: null, ims: null },
			signal: { rsrpDbm: null, network: null }, service: { running: false }, myNumber: '' };
	},
	getStatus: status,
	getHealth: function() {
		return later({
			atPort: { port: '/dev/ttyUSB2', usbInterface: 2, responding: true, lastReplyAgoSec: 1 },
			line: { channel: 1, hook: 'on', state: 'idle', calibrated: true, batteryMv: 59100, loopUa: 0 },
			modem: { simReady: true, registered: true, imsRegistered: true, rsrpDbm: -97, network: 'LTE', audioSlave: true },
			system: { moduleLoaded: true, bridge: { running: true, frames: 123456, resyncs: 0, txSkipped: 0, maxGapUs: 410 } },
			counters: { modemTimeouts: 0, hookGapsOver20ms: 1, hookGapMaxMs: 24, faults: 0 },
			quiet: { on: true, cause: 'window', ask: 'schedule' },
			log: { tail: [ 'daemon.info awvoice-phoned[312]: line in service', 'daemon.info awvoice-phoned[312]: call 5 active' ] }
		});
	},
	hangup: function() { return later({ ok: STATE === 'call', refused: STATE === 'call' ? null : 'NO_CALL' }); },
	getClock: function() { return later({ routerTime: '2026-10-23 09:30', diffMin: 12 }); },
	getPhoneSettings: function() { return later(settings); },
	savePhoneSettings: function(p) { settings = JSON.parse(JSON.stringify(p)); return later({ ok: true, refused: null }); },
	runTest: function(kind) { return kind === 'ring' || LATER && kind === 'line' ? later({ ok: true, refused: null }) : notYet(); },
	serviceAction: function() { return later({ ok: true, refused: null }); },
	listCalls: function() { return served(calls); },
	deleteCalls: function() { calls = []; return served({ ok: true }); },
	listContacts: function() { return served(contacts); },
	saveContact: function(c) { return served(c); },
	removeContact: function() { return served({ ok: true }); },
	getBlocking: function() { return served(blocking); },
	saveBlocking: function(b) { blocking = b; return served(b); },
	getForwarding: function() { return served(forwarding); },
	saveForwarding: function(f) { forwarding = Object.assign({}, f, { network: f.enabled ? 'on' : 'off' }); return served(forwarding); },
	listAlarms: function() { return served(alarms); },
	saveAlarm: function(a) { return served(a); },
	removeAlarm: function() { return served({ ok: true }); },
	/* the quiet hours, held the way the settings file holds them: windows in order, and one always-ring list */
	listQuiet: function() { return served(quiet); },
	saveQuiet: function(w) {
		var one = Object.assign({}, w, { id: w.id || String(Date.now()) });
		var at = quiet.windows.map(function(o) { return o.id; }).indexOf(one.id);
		if (at >= 0)
			quiet.windows[at] = one;
		else
			quiet.windows.push(one);
		return served(one);
	},
	removeQuiet: function(id) {
		quiet.windows = quiet.windows.filter(function(o) { return o.id !== String(id); });
		quiet.allow = [];
		return served({ ok: true });
	},
	saveQuietAllow: function(list) {
		quiet.allow = list.slice();
		return served(quiet.allow);
	}
});
