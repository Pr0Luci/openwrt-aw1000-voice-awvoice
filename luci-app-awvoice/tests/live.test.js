// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
'use strict';

/*
 * live.test.js - checks the provider (phone/live.js), the page's data door (phone/api.js) and the greeting's decisions
 * (phone/greeting.js) against stand-ins for the LuCI runtime: status and health normalisation, the settings read with
 * defaults, validation, the order of a save (write, commit, reload), the log redaction, the "Not available yet"
 * rejections, the answering machine's message and greeting calls, the greeting's WAV and piece plan, the call's own
 * view, and the SIP switch: its two sides, the words behind them, and what each side writes. Run: node tests/live.test.js
 */

const fs = require('fs');
const path = require('path');
const assert = require('assert');

const dir = path.join(__dirname, '../htdocs/luci-static/resources/view/awvoice/phone');
const src = fs.readFileSync(path.join(dir, 'live.js'), 'utf8');
const apiSrc = fs.readFileSync(path.join(dir, 'api.js'), 'utf8');
const greetSrc = fs.readFileSync(path.join(dir, 'greeting.js'), 'utf8');
const levelSrc = fs.readFileSync(path.join(dir, 'audio-level.js'), 'utf8');
const audioSrc = fs.readFileSync(path.join(dir, 'audio.js'), 'utf8');
const workletSrc = fs.readFileSync(path.join(dir, 'audio-worklet.js'), 'utf8');
const webSrc = fs.readFileSync(path.join(dir, 'webphone.js'), 'utf8');
const sipSrc = fs.readFileSync(path.join(dir, 'tab_sip.js'), 'utf8');
const styleSrc = fs.readFileSync(path.join(dir, 'style.js'), 'utf8');
const overviewSrc = fs.readFileSync(path.join(dir, 'tab_overview.js'), 'utf8');
const healthSrc = fs.readFileSync(path.join(dir, 'tab_health.js'), 'utf8');
const toolsSrc = fs.readFileSync(path.join(dir, 'tab_tools.js'), 'utf8');
const callsSrc = fs.readFileSync(path.join(dir, 'tab_calls.js'), 'utf8');
const rulesSrc = fs.readFileSync(path.join(dir, 'tab_rules.js'), 'utf8');
const editorsSrc = fs.readFileSync(path.join(dir, 'editors.js'), 'utf8');
const shellSrc = fs.readFileSync(path.join(dir, '..', 'phone.js'), 'utf8');
const helpersSrc = fs.readFileSync(path.join(dir, 'ui.js'), 'utf8');

/* the stand-ins both files are loaded against; `calls` records every rpc and uci write in order. Two shapes of UCI are
 * supported: a flat option map for one section (the page's own awvoice config), and `configs`, section maps per config
 * (the installed SIP service's config and the network's interfaces) */
function runtime(env) {
	const calls = [];
	const rpc = { declare: (d) => (...args) => {
		calls.push(d.object + '.' + d.method + '(' + args.join(',') + ')');
		const r = env.replies[d.object + '.' + d.method];
		return typeof r === 'function' ? r(args) : r instanceof Error ? Promise.reject(r) : Promise.resolve(r);
	} };
	const store = Object.assign({}, env.uci || {});
	const configs = env.configs || null;
	/* the committed file: a save puts what was staged here, an unload takes the config away and a load reads it
	   back - the way the router's own uci module behaves, so a read that forgets to load again fails here too */
	let seed = configs ? JSON.parse(JSON.stringify(configs)) : null;
	const sectionOf = (c, s) => (configs && configs[c] ? configs[c][s] : undefined);
	const uci = {
		unload: (c) => { if (configs) delete configs[c]; },
		/* a config the router does not have, and never had, fails to load, as the real one does */
		load: (c) => {
			if (!configs) return Promise.resolve();
			if (!configs[c] && seed && seed[c]) configs[c] = JSON.parse(JSON.stringify(seed[c]));
			return configs[c] ? Promise.resolve() : Promise.reject(new Error('Entry not found'));
		},
		get: (c, s, o) => (configs
			? (o === undefined ? (sectionOf(c, s) ? {} : undefined) : (sectionOf(c, s) ? sectionOf(c, s)[o] : undefined))
			: (o === undefined ? (store.__sect ? {} : undefined) : store[o])),
		sections: (c, type) => (configs && configs[c]
			? Object.keys(configs[c]).filter((n) => configs[c][n] && configs[c][n].__type === type)
				.map((n) => Object.assign({ '.name': n }, configs[c][n]))
			: []),
		set: (c, s, o, v) => {
			if (configs) { if (!configs[c]) configs[c] = {}; if (!configs[c][s]) configs[c][s] = {}; configs[c][s][o] = v; calls.push('uci.set ' + c + '.' + s + '.' + o + '=' + v); }
			else { store[o] = v; calls.push('uci.set ' + o + '=' + v); }
		},
		unset: (c, s, o) => {
			if (configs) { if (sectionOf(c, s)) delete sectionOf(c, s)[o]; calls.push('uci.unset ' + c + '.' + s + '.' + o); }
			else { delete store[o]; calls.push('uci.unset ' + o); }
		},
		add: (c, type, name) => {
			if (configs) {
				configs[c] = configs[c] || {};
				const given = !!name;
				/* a section the page just made is readable straight back, type and all, the way the router's own
				 * delta serves it: what a save returns is what the file holds */
				name = name || 'cfg1';
				configs[c][name] = configs[c][name] || { __type: type };
				calls.push('uci.add ' + c + ' ' + type + (given ? ' ' + name : ''));
			} else { store.__sect = true; calls.push('uci.add'); }
			return name;
		},
		/* a section that goes is gone from the staged config, as it is on the router: what the page reads back after a
		 * save is the file the write made, so a list that has to move can be seen moving */
		remove: (c, s) => {
			if (configs && configs[c]) { delete configs[c][s]; calls.push('uci.remove ' + c + '.' + s); }
		},
		save: () => { if (configs) seed = JSON.parse(JSON.stringify(configs)); calls.push('uci.save'); return Promise.resolve(); }
	};
	const fsStub = {
		exec_direct: () => env.log ? Promise.resolve(env.log) : Promise.reject(new Error('denied')),
		exec: (cmd, args) => {
			calls.push('fs.exec ' + cmd + ' ' + (args || []).join(' '));
			const r = env.exec && env.exec[cmd];
			return r instanceof Error ? Promise.reject(r) : Promise.resolve(r || { code: 1 });
		}
	};
	const baseclass = { extend: (o) => o };
	return { calls, store, configs, rpc, uci, fsStub, baseclass };
}

/* the translation stand-in: the text itself, with the one formatter the page uses */
function tr(s) {
	const o = new String(s);
	o.format = function() {
		let i = 0;
		return String(s).replace(/%[sd]/g, () => String(arguments[i++]));
	};
	return o;
}

function loadProvider(rt) {
	const factory = new Function('_', 'baseclass', 'rpc', 'uci', 'fs', src);
	return factory(tr, rt.baseclass, rt.rpc, rt.uci, rt.fsStub);
}

function makeProvider(env) {
	const rt = runtime(env);
	return { p: loadProvider(rt), calls: rt.calls, store: rt.store };
}

function makeApi(env) {
	const rt = runtime(env);
	const provider = loadProvider(rt);
	const factory = new Function('_', 'baseclass', 'rpc', 'uci', 'fs', 'provider', apiSrc);
	return { p: factory(tr, rt.baseclass, rt.rpc, rt.uci, rt.fsStub, provider), calls: rt.calls, store: rt.store, configs: rt.configs };
}

/* the greeting's decisions on their own: the module holds no DOM and no ubus, so it runs here as it runs in the page */
function makeGreeting() {
	const factory = new Function('_', 'baseclass', greetSrc);
	return factory(tr, { extend: (o) => o });
}

async function main() {
	// status: ubus nulls and missing fields become the contract's nulls; unknown state becomes down
	let t = makeProvider({ replies: { 'awvoice.status': { state: 'call', line: 'call', hook: 'off',
		call: { id: 3, direction: 'OUT', number: '0112223344', phase: 'active', elapsedSec: 7 }, signal: { rsrpDbm: -95 }, service: { running: true } } } });
	let s = await t.p.getStatus();
	assert.strictEqual(s.state, 'call');
	assert.strictEqual(s.call.elapsedSec, 7);
	assert.strictEqual(s.voiceNetwork.ims, null);
	assert.strictEqual(s.signal.network, null);
	t = makeProvider({ replies: { 'awvoice.status': { state: 'weird' } } });
	assert.strictEqual((await t.p.getStatus()).state, 'down');

	// the answering machine's part in the call: its three states travel, anything else is no state at all
	t = makeProvider({ replies: { 'awvoice.status': { state: 'call', machine: 'answering' } } });
	assert.strictEqual((await t.p.getStatus()).machine, 'answering');
	t = makeProvider({ replies: { 'awvoice.status': { state: 'call', machine: 'answered' } } });
	assert.strictEqual((await t.p.getStatus()).machine, 'answered');
	t = makeProvider({ replies: { 'awvoice.status': { state: 'call', machine: 'recording' } } });
	assert.strictEqual((await t.p.getStatus()).machine, null);
	assert.strictEqual(t.p.downStatus().machine, null);

	// quiet hours over the offered call: true and false travel, a daemon that says nothing leaves the page unknown
	t = makeProvider({ replies: { 'awvoice.status': { state: 'ring', quiet: true } } });
	assert.strictEqual((await t.p.getStatus()).quiet, true);
	t = makeProvider({ replies: { 'awvoice.status': { state: 'ring', quiet: false } } });
	assert.strictEqual((await t.p.getStatus()).quiet, false);
	t = makeProvider({ replies: { 'awvoice.status': { state: 'ring' } } });
	assert.strictEqual((await t.p.getStatus()).quiet, null);
	assert.strictEqual(t.p.downStatus().quiet, null);

	// a dead service is a friendly rejection, not the raw ubus text
	t = makeProvider({ replies: { 'awvoice.status': new Error('RPC call to awvoice/status failed with error -32000: Object not found') } });
	await assert.rejects(t.p.getStatus(), /not running/);

	// health: the log tail is redacted and long numbers never get through; a denied logread gives an empty tail
	t = makeProvider({ replies: { 'awvoice.health': { atPort: { responding: true }, counters: { faults: 0 } } },
		log: 'a\nawvoice-phoned: dialled 94112000004 ok\nawvoice-phoned: from 011 200 0003\n' });
	let h = await t.p.getHealth();
	assert.strictEqual(h.log.tail.length, 3);
	assert.ok(h.log.tail.every((l) => !/\d{6}/.test(l) && l.indexOf('4567') < 0));
	assert.strictEqual(h.atPort.lastReplyAgoSec, null);
	t = makeProvider({ replies: { 'awvoice.health': {} } });
	assert.deepStrictEqual((await t.p.getHealth()).log.tail, []);

	// the machine's health block: a loaded greeting and its length travel, and so does the word for a file it could not
	// use; a missing block is "unknown", not "off"
	t = makeProvider({ replies: { 'awvoice.health': { machine: { greetingLoaded: true, greetingMs: 4250, greetingError: '', delaySec: 10 } } } });
	h = await t.p.getHealth();
	assert.deepStrictEqual(h.machine, { greetingLoaded: true, greetingMs: 4250, greetingError: '', delaySec: 10 });
	t = makeProvider({ replies: { 'awvoice.health': { machine: { greetingLoaded: false, greetingMs: 0, greetingError: 'TRUNCATED', delaySec: 10 } } } });
	assert.strictEqual((await t.p.getHealth()).machine.greetingError, 'TRUNCATED');
	t = makeProvider({ replies: { 'awvoice.health': {} } });
	assert.deepStrictEqual((await t.p.getHealth()).machine, { greetingLoaded: null, greetingMs: 0, greetingError: '', delaySec: 0 });

	// the line's block, the installed service's block, the voice port's owner and the HD word travel; a daemon that
	// publishes none stays null
	t = makeProvider({ replies: { 'awvoice.health': { atPort: { portOwner: 'busy' }, modem: { hdVoice: 'set' },
		web: { mode: 'interop', audio: 'listening', port: 7443 },
		sip: { objectOwner: 'awvoice', lastRefusal: 'port', consumerPid: 1234 } } } });
	h = await t.p.getHealth();
	assert.strictEqual(h.atPort.portOwner, 'busy');
	assert.strictEqual(h.modem.hdVoice, 'set');
	assert.deepStrictEqual(h.web, { mode: 'interop', audio: 'listening', port: 7443 });
	assert.deepStrictEqual(h.sip, { objectOwner: 'awvoice', lastRefusal: 'port', consumerPid: 1234 });
	t = makeProvider({ replies: { 'awvoice.health': {} } });
	h = await t.p.getHealth();
	assert.strictEqual(h.atPort.portOwner, null);
	assert.strictEqual(h.modem.hdVoice, null);
	assert.deepStrictEqual(h.web, { mode: null, audio: null, port: 0 });
	assert.deepStrictEqual(h.sip, { objectOwner: null, lastRefusal: null, consumerPid: null });

	// the audio path's one word: standing by is the normal state of a quiet line, a path that is wanted and not
	// running is the fault, and a reply without the block leaves the page on the bridge's own flag rather than a guess
	t = makeProvider({ replies: { 'awvoice.health': { system: { bridge: { running: true },
		audioPath: { running: true, wanted: true, why: 'call', upMs: 4200, starts: 3, stops: 2, lastError: '' } } } } });
	h = await t.p.getHealth();
	assert.deepStrictEqual(h.system.audio, { state: 'crossing', why: 'call', upMs: 4200, starts: 3, stops: 2, lastError: '' });
	t = makeProvider({ replies: { 'awvoice.health': { system: { bridge: { running: false },
		audioPath: { running: false, wanted: false, why: 'idle', upMs: 0, starts: 3, stops: 3, lastError: '' } } } } });
	assert.strictEqual((await t.p.getHealth()).system.audio.state, 'standing-by');
	t = makeProvider({ replies: { 'awvoice.health': { system: { audioPath: { running: false, wanted: true,
		why: 'recording', lastError: 'No such device' } } } } });
	h = await t.p.getHealth();
	assert.strictEqual(h.system.audio.state, 'refused');
	assert.strictEqual(h.system.audio.lastError, 'No such device');
	t = makeProvider({ replies: { 'awvoice.health': { system: { bridge: { running: true } } } } });
	h = await t.p.getHealth();
	assert.strictEqual(h.system.audio.state, null);
	// and the page asks for that word rather than painting a standing-by path as stopped
	assert.ok(/ap\.state \? audioWord\(ap\.state\) : yn\(br\.running/.test(healthSrc),
		'the health tab no longer falls back on the bridge flag when the daemon says nothing');
	assert.ok(/'Wanted, not running'/.test(healthSrc), 'the refused state has no word to show');
	assert.ok(/Standing by/.test(healthSrc), 'the standing-by state has no word to show');

	// the numbering plan in force and the network it came from travel; a daemon that publishes neither, or publishes
	// the two as null before it has asked, leaves both null rather than a country without a name
	t = makeProvider({ replies: { 'awvoice.health': { line: { numbering: 'LK' }, modem: { operator: '413', operatorName: 'Dialog Dialog' } } } });
	h = await t.p.getHealth();
	assert.strictEqual(h.line.numbering, 'LK');
	assert.strictEqual(h.modem.operator, '413');
	assert.strictEqual(h.modem.operatorName, 'Dialog Dialog');
	t = makeProvider({ replies: { 'awvoice.health': { line: { numbering: null }, modem: { operator: null, operatorName: null } } } });
	h = await t.p.getHealth();
	assert.strictEqual(h.line.numbering, null);
	assert.strictEqual(h.modem.operator, null);
	assert.strictEqual(h.modem.operatorName, null);

	// the storage block: both folders travel with their state; a refused folder keeps the reason, a missing block
	// reads as "not checked" rather than as a usable folder
	t = makeProvider({ replies: { 'awvoice.health': { storage: {
		recordings: { path: '/mnt/sda1/awvoice', ready: true, freeMb: 1234 },
		messages: { path: '/etc/awvoice/messages', ready: false, freeMb: null, error: 'No space left on device' } } } } });
	h = await t.p.getHealth();
	assert.deepStrictEqual(h.storage.recordings, { path: '/mnt/sda1/awvoice', ready: true, freeMb: 1234, error: null });
	assert.deepStrictEqual(h.storage.messages, { path: '/etc/awvoice/messages', ready: false, freeMb: null, error: 'No space left on device' });
	t = makeProvider({ replies: { 'awvoice.health': {} } });
	h = await t.p.getHealth();
	assert.deepStrictEqual(h.storage.recordings, { path: '', ready: null, freeMb: null, error: null });
	assert.deepStrictEqual(h.storage.messages, { path: '', ready: null, freeMb: null, error: null });

	// the call log: the daemon's name comes through ('' when it gave none), the number as it came, and the
	// recording's file name travels ('' when the daemon gave none: an old daemon's reply has no field at all);
	// answeredBy names who took the call and stays '' when the reply has no such field
	t = makeProvider({ replies: { 'awvoice.calls': { calls: [
		{ id: 3, type: 'IN', number: '+12345', name: 'Ann', ts: 1700000000, durationSec: 12, result: 'answered', answeredBy: 'web', cause: 0, recording: '1700000000-3.wav', message: '1700000005-3.wav' },
		{ id: 2, type: 'MISS', number: '1777', ts: 1700000100, durationSec: 0, result: 'missed', cause: 0 } ] } } });
	let calls = await t.p.listCalls();
	assert.strictEqual(calls.length, 2);
	assert.strictEqual(calls[0].name, 'Ann');
	assert.strictEqual(calls[0].number, '+12345');
	assert.strictEqual(calls[0].recording, '1700000000-3.wav');
	assert.strictEqual(calls[0].message, '1700000005-3.wav');
	assert.strictEqual(calls[0].answeredBy, 'web');
	assert.strictEqual(calls[1].name, '');
	assert.strictEqual(calls[1].type, 'MISS');
	assert.strictEqual(calls[1].recording, '');
	assert.strictEqual(calls[1].message, '');
	assert.strictEqual(calls[1].answeredBy, '');

	// settings: defaults for what is unset, ringback 1/0 as on/off, out-of-range values fall back
	t = makeProvider({ replies: {}, uci: { ringback: '0', flash_min_ms: '5', at_interface: '3', earpiece_volume: '4' } });
	let ps = await t.p.getPhoneSettings();
	assert.strictEqual(ps.tones.ringback, 'off');
	assert.strictEqual(ps.hookFlash.minMs, 80);
	assert.strictEqual(ps.hookFlash.maxMs, 300);
	assert.strictEqual(ps.modem.usbInterface, 3);
	assert.strictEqual(ps.audio.earpieceVolume, 4);
	assert.strictEqual(ps.audio.modemRxGain, null);
	assert.strictEqual(ps.line.cid, 'dtmf');
	assert.strictEqual(ps.page.refreshSec, 1);
	t = makeProvider({ replies: {}, uci: { page_refresh_s: '7' } });
	assert.strictEqual((await t.p.getPhoneSettings()).page.refreshSec, 7);
	t = makeProvider({ replies: {}, uci: { page_refresh_s: '99' } });
	assert.strictEqual((await t.p.getPhoneSettings()).page.refreshSec, 1);

	// the answering machine's settings: defaults, stored values, and out-of-range values fall back. The greeting has no
	// place in them: it is one constant file the daemon manages, so a stored path is not read and not written back
	t = makeProvider({ replies: {} });
	assert.deepStrictEqual((await t.p.getPhoneSettings()).machine, { enabled: false,
		delaySec: 10, path: '/etc/awvoice/messages', maxMb: 50, maxSec: 120 });
	t = makeProvider({ replies: {}, uci: { machine_enabled: '1', machine_greeting: '/tmp/g.wav', machine_delay: '0',
		machine_path: '/tmp/messages', machine_max_mb: '200', machine_max_s: '30' } });
	assert.deepStrictEqual((await t.p.getPhoneSettings()).machine, { enabled: true,
		delaySec: 0, path: '/tmp/messages', maxMb: 200, maxSec: 30 });
	t = makeProvider({ replies: {}, uci: { machine_delay: '61', machine_max_mb: '0', machine_max_s: '9' } });
	let mset = (await t.p.getPhoneSettings()).machine;
	assert.strictEqual(mset.delaySec, 10);
	assert.strictEqual(mset.maxMb, 50);
	assert.strictEqual(mset.maxSec, 120);

	// save: validated first (nothing written), then write -> commit -> reload
	const good = () => ({ line: { cid: 'dtmf', myNumber: '0112345678', dialPauseSec: 6 }, hookFlash: { minMs: 80, maxMs: 300 }, tones: { ringback: 'auto' },
		audio: { earpieceVolume: null, modemRxGain: 8192, micGainDb: null, earpieceGainDb: null }, modem: { usbInterface: 2 },
		features: { clir: 'network', conference: false, callWaiting: true, hotline: { number: '', delaySec: 10 } }, indicator: { led: '', ringRateMs: 300, offhookRateMs: 1000 }, page: { refreshSec: 1 },
		machine: { enabled: true, delaySec: 10, path: '/etc/awvoice/messages', maxMb: 50, maxSec: 120 } });
	t = makeProvider({ replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true } } });
	let bad = good(); bad.hookFlash = { minMs: 250, maxMs: 300 };
	await assert.rejects(t.p.savePhoneSettings(bad), /100 ms/);
	bad = good(); bad.hookFlash.minMs = 10;
	await assert.rejects(t.p.savePhoneSettings(bad), /20 to 500/);
	bad = good(); bad.line.myNumber = 'abc';
	await assert.rejects(t.p.savePhoneSettings(bad), /digits/);
	bad = good(); bad.machine.path = 'messages';
	await assert.rejects(t.p.savePhoneSettings(bad), /message folder/);
	bad = good(); bad.machine.path = '///';
	await assert.rejects(t.p.savePhoneSettings(bad), /not the root/);
	bad = good(); bad.machine.delaySec = 61;
	await assert.rejects(t.p.savePhoneSettings(bad), /0 to 60 seconds/);
	bad = good(); bad.machine.maxMb = 0;
	await assert.rejects(t.p.savePhoneSettings(bad), /1 to 4095 MB/);
	bad = good(); bad.machine.maxSec = 9;
	await assert.rejects(t.p.savePhoneSettings(bad), /10 to 600 seconds/);
	bad = good(); bad.machine.enabled = 'yes';
	await assert.rejects(t.p.savePhoneSettings(bad), /not valid/);
	assert.deepStrictEqual(t.calls, []);
	const r = await t.p.savePhoneSettings(good());
	assert.strictEqual(r.ok, true);
	const order = t.calls.filter((c) => /^(uci\.save|uci\.commit|awvoice\.reload)/.test(c));
	assert.deepStrictEqual(order, [ 'uci.save', 'uci.commit(awvoice)', 'awvoice.reload()' ]);
	assert.ok(t.calls.indexOf('uci.unset earpiece_volume') >= 0);
	assert.ok(t.calls.indexOf('uci.set modem_rx_gain=8192') >= 0);
	assert.ok(t.calls.indexOf('uci.set clir=network') >= 0);
	assert.ok(t.calls.indexOf('uci.set page_refresh_s=1') >= 0);
	assert.ok(t.calls.indexOf('uci.set machine_enabled=1') >= 0);
	/* the retired greeting option is removed rather than left in the config to look meaningful, and it is never written */
	assert.ok(t.calls.indexOf('uci.unset machine_greeting') >= 0);
	assert.ok(t.calls.every((c) => c.indexOf('uci.set machine_greeting') < 0));
	assert.ok(t.calls.indexOf('uci.set machine_delay=10') >= 0);
	assert.ok(t.calls.indexOf('uci.set machine_path=/etc/awvoice/messages') >= 0);
	assert.ok(t.calls.indexOf('uci.set machine_max_mb=50') >= 0);
	assert.ok(t.calls.indexOf('uci.set machine_max_s=120') >= 0);
	// a settings object without the machine (a page from before it) leaves the machine's options untouched
	t = makeProvider({ replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true } } });
	bad = good(); delete bad.machine;
	await t.p.savePhoneSettings(bad);
	assert.deepStrictEqual(t.calls.filter((c) => /^uci\./.test(c) && /machine_/.test(c)), []);
	t = makeProvider({ replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true } } });
	bad = good(); bad.page = { refreshSec: 31 };
	await assert.rejects(t.p.savePhoneSettings(bad), /1 to 30 seconds/);
	assert.deepStrictEqual(t.calls.filter((c) => /^uci\./.test(c)), []);

	// service and tests pass the refusal through; later tests reject
	t = makeProvider({ replies: { 'luci.awvoice.service': { ok: false, refused: 'ACTIVE_CALL' }, 'awvoice.ring_test': { ok: true, refused: null } } });
	assert.deepStrictEqual(await t.p.serviceAction('stop'), { ok: false, refused: 'ACTIVE_CALL' });
	await assert.rejects(t.p.serviceAction('format'), /Unknown/);
	assert.strictEqual((await t.p.runTest('ring')).ok, true);
	assert.ok(t.calls.indexOf('awvoice.ring_test(5)') >= 0);
	await assert.rejects(t.p.runTest('loopback'), /Not available yet/);
	assert.strictEqual(t.p.available.loopbackTest, false);
	assert.strictEqual(t.p.settingsLive.conference, true);
	assert.strictEqual(t.p.settingsLive.callWaiting, true);

	/* ---- quiet hours: the windows the page writes, the list it holds, and what the line answered a moment ago ---- */

	// the health reply's quiet block travels; a daemon that publishes nothing is "not known", never "may ring"
	t = makeProvider({ replies: { 'awvoice.health': { quiet: { on: true, cause: 'window', ask: 'schedule' } } } });
	assert.deepStrictEqual((await t.p.getHealth()).quiet, { on: true, cause: 'window', ask: 'schedule' });
	t = makeProvider({ replies: { 'awvoice.health': { quiet: { on: false, cause: '', ask: 'ringing' } } } });
	assert.deepStrictEqual((await t.p.getHealth()).quiet, { on: false, cause: null, ask: 'ringing' });
	t = makeProvider({ replies: { 'awvoice.health': {} } });
	assert.deepStrictEqual((await t.p.getHealth()).quiet, { on: null, cause: null, ask: null });

	// every `dnd` section, in the order the file writes them, with the daemon's own defaults for what it leaves out
	const qcfg = () => ({ awvoice: {
		w1: { __type: 'dnd', from: '22:00', to: '07:00', days: [ 'mon', 'tue', 'wed', 'thu', 'fri' ], mode: 'machine', repeat_min: '15', allow: [ '0771234567' ] },
		w2: { __type: 'dnd', all_day: '1', mode: 'reject' },
		w3: { __type: 'dnd', enabled: '0', from: '12:00', to: '13:00', allow: [ '0779999999', '0771234567' ] },
		m: { __type: 'phone' }
	} });
	t = makeProvider({ configs: qcfg() });
	let q = await t.p.listQuiet();
	assert.deepStrictEqual(q.windows.map((w) => w.id), [ 'w1', 'w2', 'w3' ]);
	assert.strictEqual(q.windows[0].enabled, true);
	assert.strictEqual(q.windows[0].mode, 'machine');
	assert.strictEqual(q.windows[0].repeatMin, 15);
	assert.deepStrictEqual(q.windows[0].days, [ 1, 2, 3, 4, 5 ]);
	/* no day list is every day, no mode word is the plainest quiet, no enabled word is on */
	assert.strictEqual(q.windows[1].allDay, true);
	assert.strictEqual(q.windows[1].enabled, true);
	assert.deepStrictEqual(q.windows[1].days, [ 1, 2, 3, 4, 5, 6, 7 ]);
	assert.strictEqual(q.windows[2].enabled, false);
	// the always-ring numbers are the feature's: every section's list joins one, without a number twice
	assert.deepStrictEqual(q.allow, [ '0771234567', '0779999999' ]);

	// one window written as the daemon reads it: all day has no span, the day list is always spelled out
	t = makeProvider({ configs: { awvoice: { w1: { __type: 'dnd', from: '00:00', to: '01:00' } } },
		replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true } } });
	let saved = await t.p.saveQuiet({ id: 'w1', enabled: false, allDay: true, from: '09:00', to: '10:00', days: [ 7 ], mode: 'silent', repeatMin: 30 });
	assert.ok(t.calls.indexOf('uci.set awvoice.w1.enabled=0') >= 0);
	assert.ok(t.calls.indexOf('uci.set awvoice.w1.all_day=1') >= 0);
	assert.ok(t.calls.indexOf('uci.unset awvoice.w1.from') >= 0, 'an all-day window kept a span it does not have');
	assert.ok(t.calls.indexOf('uci.unset awvoice.w1.to') >= 0);
	assert.ok(t.calls.indexOf('uci.set awvoice.w1.days=sun') >= 0);
	assert.ok(t.calls.indexOf('uci.set awvoice.w1.mode=silent') >= 0);
	assert.ok(t.calls.indexOf('uci.set awvoice.w1.repeat_min=30') >= 0);
	assert.strictEqual(saved.allDay, true);
	assert.strictEqual(saved.from, '');
	assert.strictEqual(saved.id, 'w1', 'a saved window did not come back from the file it was written to');
	assert.deepStrictEqual(t.calls.filter((c) => /^(uci\.save|uci\.commit|awvoice\.reload)/.test(c)),
		[ 'uci.save', 'uci.commit(awvoice)', 'awvoice.reload()' ]);

	// a span, and the new window takes the place the file gives it
	t = makeProvider({ configs: { awvoice: { w1: { __type: 'dnd', from: '00:00', to: '01:00' } } },
		replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true } } });
	saved = await t.p.saveQuiet({ id: 0, enabled: true, allDay: false, from: '22:00', to: '07:00', days: [ 1, 2, 3, 4, 5 ], mode: 'machine', repeatMin: 15 });
	assert.ok(t.calls.indexOf('uci.add awvoice dnd') >= 0);
	assert.strictEqual(saved.id, 'cfg1');
	assert.strictEqual(saved.repeatMin, 15, 'the window read back after the save is not the one the file holds');
	assert.ok(t.calls.indexOf('uci.set awvoice.cfg1.from=22:00') >= 0);
	assert.ok(t.calls.indexOf('uci.unset awvoice.cfg1.all_day') >= 0, 'a window with a span was also marked all day');

	// what the daemon would refuse, said before the write instead of in its log afterwards
	const qgood = { id: 'w1', enabled: true, allDay: false, from: '22:00', to: '07:00', days: [ 1, 2, 3, 4, 5 ], mode: 'machine', repeatMin: 15 };
	t = makeProvider({ configs: qcfg(), replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true } } });
	await assert.rejects(t.p.saveQuiet(Object.assign({}, qgood, { from: '25:00' })), /HH:MM/);
	await assert.rejects(t.p.saveQuiet(Object.assign({}, qgood, { to: '7:0' })), /HH:MM/);
	await assert.rejects(t.p.saveQuiet(Object.assign({}, qgood, { mode: 'quiet' })), /what a quiet line does/);
	await assert.rejects(t.p.saveQuiet(Object.assign({}, qgood, { repeatMin: 1441 })), /0 to 1440 minutes/);
	await assert.rejects(t.p.saveQuiet(Object.assign({}, qgood, { repeatMin: -1 })), /0 to 1440 minutes/);
	await assert.rejects(t.p.saveQuiet(Object.assign({}, qgood, { days: [] })), /at least one day/);
	await assert.rejects(t.p.saveQuiet(Object.assign({}, qgood, { enabled: 'yes' })), /not valid/);
	await assert.rejects(t.p.saveQuiet(Object.assign({}, qgood, { id: 'nope' })), /no longer exists/);
	assert.deepStrictEqual(t.calls.filter((c) => /^uci\./.test(c)), [], 'a refused window reached the config file');

	// the eighth window is the last the daemon keeps
	const eight = {};
	for (let i = 1; i <= 8; i++)
		eight['w' + i] = { __type: 'dnd', from: '22:00', to: '07:00' };
	t = makeProvider({ configs: { awvoice: eight }, replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true } } });
	await assert.rejects(t.p.saveQuiet(Object.assign({}, qgood, { id: 0 })), /At most 8/);
	assert.deepStrictEqual(t.calls.filter((c) => /^uci\./.test(c)), []);

	// deleting a window takes the window and leaves the numbers: they move to the one that comes first now
	t = makeProvider({ configs: qcfg(), replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true } } });
	await t.p.removeQuiet('w1');
	assert.ok(t.calls.indexOf('uci.remove awvoice.w1') >= 0);
	assert.ok(t.calls.indexOf('uci.set awvoice.w2.allow=0771234567,0779999999') >= 0,
		'the always-ring list went to the file with the window it was written beside');
	assert.ok(t.calls.indexOf('uci.unset awvoice.w3.allow') >= 0);
	t = makeProvider({ configs: qcfg(), replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true } } });
	await assert.rejects(t.p.removeQuiet('nope'), /no longer exists/);
	// the last window going takes the list with it: nothing is held beside no window at all
	t = makeProvider({ configs: { awvoice: { w1: { __type: 'dnd', from: '22:00', to: '07:00', allow: [ '0771234567' ] } } },
		replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true } } });
	await t.p.removeQuiet('w1');
	assert.deepStrictEqual(t.calls.filter((c) => /\.allow=/.test(c)), []);

	// the list is written whole onto the first window and off every other, and is refused when it cannot be held
	t = makeProvider({ configs: qcfg(), replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true } } });
	assert.deepStrictEqual(await t.p.saveQuietAllow([ '0771234567', '+94771234567', '*71' ]), [ '0771234567', '+94771234567', '*71' ]);
	assert.ok(t.calls.indexOf('uci.unset awvoice.w2.allow') >= 0);
	assert.ok(t.calls.indexOf('uci.set awvoice.w1.allow=0771234567,+94771234567,*71') >= 0);
	t = makeProvider({ configs: qcfg(), replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true } } });
	await t.p.saveQuietAllow([]);
	assert.ok(t.calls.indexOf('uci.unset awvoice.w1.allow') >= 0, 'an empty list left its numbers in the file');
	assert.deepStrictEqual(t.calls.filter((c) => /\.allow=/.test(c)), []);
	t = makeProvider({ configs: qcfg(), replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true } } });
	await assert.rejects(t.p.saveQuietAllow([ 'abc' ]), /digits/);
	await assert.rejects(t.p.saveQuietAllow([ '+' ]), /digits/);
	await assert.rejects(t.p.saveQuietAllow(Array.from({ length: 21 }, (v, i) => '077000000' + i)), /At most 20 numbers/);
	t = makeProvider({ configs: { awvoice: { m: { __type: 'phone' } } }, replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true } } });
	await assert.rejects(t.p.saveQuietAllow([ '0771234567' ]), /window first/);
	assert.deepStrictEqual(t.calls.filter((c) => /^uci\./.test(c)), []);

	// a contact's own quiet-hours ask: read only the word the daemon reads, written and read back whole
	t = makeProvider({ configs: { awvoice: {
		c1: { __type: 'contact', name: 'Ann', number: [ '0771234567:mobile' ], ring_through: '1' },
		c2: { __type: 'contact', name: 'Bob', number: '0770000000:home' },
		c3: { __type: 'contact', name: 'Cy', number: '0771111111', ring_through: 'yes' }
	} } });
	let qs = await t.p.listContacts();
	assert.strictEqual(qs.find((c) => c.id === 'c1').ringThrough, true);
	assert.strictEqual(qs.find((c) => c.id === 'c2').ringThrough, false);
	assert.strictEqual(qs.find((c) => c.id === 'c3').ringThrough, false, 'a word that is not the daemon\'s yes was taken as one');
	t = makeProvider({ configs: { awvoice: { c1: { __type: 'contact', name: 'Ann', number: [ '0771234567:mobile' ], ring_through: '1' } } },
		replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true } } });
	saved = await t.p.saveContact({ id: 'c1', name: 'Ann', numbers: [ { value: '0771234567', type: 'mobile' } ], quick: '', ringThrough: false });
	assert.ok(t.calls.indexOf('uci.unset awvoice.c1.ring_through') >= 0);
	assert.strictEqual(saved.ringThrough, false);
	t = makeProvider({ configs: { awvoice: { c1: { __type: 'contact', name: 'Ann', number: [ '0771234567:mobile' ] } } },
		replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true } } });
	saved = await t.p.saveContact({ id: 'c1', name: 'Ann', numbers: [ { value: '0771234567', type: 'mobile' } ], quick: '', ringThrough: true });
	assert.ok(t.calls.indexOf('uci.set awvoice.c1.ring_through=1') >= 0);
	assert.strictEqual(saved.ringThrough, true);
	/* a contact with no ask written has none, and a save that says nothing about it leaves the file without the option */
	t = makeProvider({ configs: { awvoice: { c1: { __type: 'contact', name: 'Ann', number: [ '0771234567:mobile' ] } } },
		replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true } } });
	saved = await t.p.saveContact({ id: 'c1', name: 'Ann', numbers: [ { value: '0771234567', type: 'mobile' } ], quick: '' });
	assert.strictEqual(saved.ringThrough, false);
	assert.strictEqual(t.calls.filter((c) => /ring_through=/.test(c)).length, 0);

	// api.js: the messages come newest first (a name leads with its epoch; equal stamps fall back to the name),
	// and every field of a File survives with a safe default
	t = makeApi({ replies: { 'awvoice.messages': { messages: [
		{ name: '1700000000-3.wav', ts: 1700000000, bytes: 16000, rate: 8000, seconds: 2, channels: 1 },
		{ name: '1700000200-4.wav', ts: 1700000200, bytes: 32000, rate: 16000, seconds: 1, channels: 2 },
		{ name: '1700000200-2.wav', ts: 1700000200 } ] } } });
	let msgs = await t.p.listMessages();
	assert.deepStrictEqual(msgs.map((m) => m.name), [ '1700000200-4.wav', '1700000200-2.wav', '1700000000-3.wav' ]);
	assert.deepStrictEqual(msgs[0], { name: '1700000200-4.wav', bytes: 32000, rate: 16000, seconds: 1, channels: 2, ts: 1700000200 });
	assert.strictEqual(msgs[1].bytes, 0);
	assert.strictEqual(msgs[1].channels, 1);
	assert.strictEqual(t.p.available.listMessages, true);
	assert.strictEqual(t.p.available.getMessage, true);
	assert.strictEqual(t.p.available.deleteMessage, true);
	t = makeApi({ replies: { 'awvoice.messages': {} } });
	assert.deepStrictEqual(await t.p.listMessages(), []);
	t = makeApi({ replies: { 'awvoice.messages': new Error('RPC call to awvoice/messages failed with error -32000: Object not found') } });
	await assert.rejects(t.p.listMessages(), /not running/);

	// one chunk of a message; the page refuses any name but the daemon's own shape before it calls out
	t = makeApi({ replies: { 'awvoice.message_get': { name: '1700000000-3.wav', rate: 8000, total: 16000, offset: 0, b64: 'AAAA', more: 1 } } });
	assert.deepStrictEqual(await t.p.getMessage('1700000000-3.wav', 0),
		{ name: '1700000000-3.wav', rate: 8000, total: 16000, offset: 0, b64: 'AAAA', more: true });
	assert.ok(t.calls.indexOf('awvoice.message_get(1700000000-3.wav,0)') >= 0);
	await t.p.getMessage('1700000000-3.wav', 2.9);
	assert.ok(t.calls.indexOf('awvoice.message_get(1700000000-3.wav,2)') >= 0);
	await assert.rejects(t.p.getMessage('../etc/passwd', 0), /Not a message name/);
	await assert.rejects(t.p.getMessage(' 1700000000-3.wav', 0), /Not a message name/);
	await assert.rejects(t.p.getMessage('1700000000-3.wav', -1), /Not a message name/);

	// delete: the daemon's flags become booleans, and the name is checked the same way
	t = makeApi({ replies: { 'awvoice.message_delete': { ok: 1, removed: true } } });
	assert.deepStrictEqual(await t.p.deleteMessage('1700000000-3.wav'), { ok: true, removed: true });
	assert.ok(t.calls.indexOf('awvoice.message_delete(1700000000-3.wav)') >= 0);
	t = makeApi({ replies: { 'awvoice.message_delete': { ok: 0, removed: 0 } } });
	assert.deepStrictEqual(await t.p.deleteMessage('1700000000-3.wav'), { ok: false, removed: false });
	t = makeApi({ replies: { 'awvoice.message_delete': { ok: 1 } } });
	await assert.rejects(t.p.deleteMessage('greeting.wav'), /Not a message name/);
	assert.deepStrictEqual(t.calls, []);

	// sip: the switch has two sides. A stored word reads as the side it runs, a word this build does not know reads as
	// the side that touches nothing, and a save writes the word of the side picked - keeping the box's own method when
	// the box already runs one, so moving the switch changes whether SIP runs and never how it runs
	t = makeApi({ replies: {} });
	assert.strictEqual(await t.p.getSipSettings(), 'web');
	t = makeApi({ replies: {}, uci: { mode: 'interop' } });
	assert.strictEqual(await t.p.getSipSettings(), 'sip');
	t = makeApi({ replies: {}, uci: { mode: 'baresip' } });
	assert.strictEqual(await t.p.getSipSettings(), 'sip');
	t = makeApi({ replies: {}, uci: { mode: 'off' } });		/* a word this build has not kept */
	assert.strictEqual(await t.p.getSipSettings(), 'web');
	t = makeApi({ replies: {}, uci: { mode: 'bridge' } });
	assert.strictEqual(await t.p.getSipSettings(), 'web');
	t = makeApi({ replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true } } });
	assert.deepStrictEqual(await t.p.saveSipSettings('web'), { mode: 'web' });
	assert.deepStrictEqual(t.calls, [ 'uci.add', 'uci.set mode=web', 'uci.save', 'uci.commit(awvoice)', 'awvoice.reload()' ]);
	t = makeApi({ replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true } }, uci: { mode: 'web' } });
	await t.p.saveSipSettings('sip');
	assert.ok(t.calls.indexOf('uci.set mode=interop') >= 0);		/* the side asked for, on a box with no method yet */
	t = makeApi({ replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true } }, uci: { mode: 'interop' } });
	await t.p.saveSipSettings('sip');
	assert.ok(t.calls.indexOf('uci.set mode=interop') >= 0);
	t = makeApi({ replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true } }, uci: { mode: 'baresip' } });
	await t.p.saveSipSettings('sip');
	assert.ok(t.calls.indexOf('uci.set mode=baresip') >= 0);		/* the box keeps the method it was set to */
	t = makeApi({ replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true } }, uci: { mode: 'off' } });
	await t.p.saveSipSettings('sip');
	assert.ok(t.calls.indexOf('uci.set mode=interop') >= 0);		/* a box whose stored word has no side writes the served one */
	t = makeApi({ replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true } }, uci: { mode: 'off' } });
	await t.p.saveSipSettings('web');
	assert.ok(t.calls.indexOf('uci.set mode=web') >= 0);
	t = makeApi({ replies: {} });
	await assert.rejects(t.p.saveSipSettings('bridge'), /Unknown SIP mode/);
	assert.deepStrictEqual(t.calls, []);

	/* the switch is the master of the installed service as well as of the line's own side: the side is written, the
	   handover of the qmodem_voip name is waited for, and only then does the service move. Started before the name is
	   the daemon's, the service binds to nothing and every call lands with no audio; stopped while a leg of its own is still
	   up, it cuts that call. A wait that runs out is said as waiting, and the page finishes it on a later read */
	{
		const held = { sip: { objectOwner: 'awvoice' } };
		const theirs = { sip: { objectOwner: 'voipd' } };
		const there = { script: true, inboundPid: 0, outboundPid: 0 };
		const svc = (pid) => ({ script: true, inboundPid: pid, outboundPid: 0 });

		// on, with the name already held: the start comes after the settings have been applied, not before them
		t = makeApi({ replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true }, 'awvoice.health': held,
			'luci.awvoice.sipdiag': there }, exec: { '/etc/init.d/qmodem_voip_sipd': { code: 0 } } });
		assert.deepStrictEqual(await t.p.saveSipSwitch('sip'), { mode: 'interop', side: 'sip', service: 'started' });
		assert.ok(t.calls.indexOf('uci.set mode=interop') >= 0);
		assert.deepStrictEqual(t.calls.filter((c) => c.indexOf('fs.exec') === 0),
			[ 'fs.exec /etc/init.d/qmodem_voip_sipd start' ]);
		assert.ok(t.calls.indexOf('awvoice.reload()') < t.calls.indexOf('fs.exec /etc/init.d/qmodem_voip_sipd start'),
			'the service was started before the phone service had taken the change');
		assert.ok(t.calls.indexOf('awvoice.health(\')') < t.calls.indexOf('fs.exec /etc/init.d/qmodem_voip_sipd start'),
			'the service was started without reading whose the qmodem_voip name is');

		// off: the service stops after the settings are applied, and the stored word is the line's own side
		t = makeApi({ replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true }, 'awvoice.health': theirs,
			'luci.awvoice.sipdiag': svc(4321) }, exec: { '/etc/init.d/qmodem_voip_sipd': { code: 0 } } });
		assert.deepStrictEqual(await t.p.saveSipSwitch('web'), { mode: 'web', side: 'web', service: 'stopped' });
		assert.ok(t.calls.indexOf('uci.set mode=web') >= 0);
		assert.deepStrictEqual(t.calls.filter((c) => c.indexOf('fs.exec') === 0),
			[ 'fs.exec /etc/init.d/qmodem_voip_sipd stop' ]);

		// the name is still the line's while a call is up: nothing is started into that, and the switch says waiting
		t = makeApi({ replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true },
			'awvoice.health': theirs, 'luci.awvoice.sipdiag': there } });
		assert.deepStrictEqual(await t.p.saveSipSwitch('sip'), { mode: 'interop', side: 'sip', service: 'waiting' });
		assert.strictEqual(t.calls.filter((c) => c.indexOf('fs.exec') === 0).length, 0,
			'a service was moved while the qmodem_voip name was not held by this daemon');

		/* the handover came while a call was still on the line: the name is held, the call is not over, and starting
		   the service in that moment is what puts a leg through a call the admin never agreed to share */
		t = makeApi({ replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true },
			'awvoice.health': { line: { state: 'call' }, sip: { objectOwner: 'awvoice' } },
			'luci.awvoice.sipdiag': there }, exec: { '/etc/init.d/qmodem_voip_sipd': { code: 0 } } });
		assert.deepStrictEqual(await t.p.saveSipSwitch('sip'), { mode: 'interop', side: 'sip', service: 'waiting' });
		assert.strictEqual(t.calls.filter((c) => c.indexOf('fs.exec') === 0).length, 0,
			'the service was started while the line was in a call');

		// no service on the box: the switch still reaches the line, and the page is told there is nothing to start
		t = makeApi({ replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true }, 'awvoice.health': held,
			'luci.awvoice.sipdiag': { script: false } } });
		assert.deepStrictEqual(await t.p.saveSipSwitch('sip'), { mode: 'interop', side: 'sip', service: 'absent' });
		assert.strictEqual(t.calls.filter((c) => c.indexOf('fs.exec') === 0).length, 0,
			'a start was asked for a script the box does not have');

		// the account may not reach it, and the script refuses: both are said, neither is a silent success
		t = makeApi({ replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true }, 'awvoice.health': held,
			'luci.awvoice.sipdiag': there },
			exec: { '/etc/init.d/qmodem_voip_sipd': new Error('RPC call denied') } });
		assert.deepStrictEqual(await t.p.saveSipSwitch('sip'), { mode: 'interop', side: 'sip', service: 'denied' });
		t = makeApi({ replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true }, 'awvoice.health': held,
			'luci.awvoice.sipdiag': there }, exec: { '/etc/init.d/qmodem_voip_sipd': { code: 1 } } });
		assert.deepStrictEqual(await t.p.saveSipSwitch('sip'), { mode: 'interop', side: 'sip', service: 'failed' });

		// a switch that ran out its wait is finished by the next read, and only with the one action that is missing
		t = makeApi({ replies: { 'awvoice.health': held, 'luci.awvoice.sipdiag': svc(0) },
			uci: { mode: 'interop' }, exec: { '/etc/init.d/qmodem_voip_sipd': { code: 0 } } });
		assert.strictEqual(await t.p.reconcileSipSwitch(), 'started');
		assert.deepStrictEqual(t.calls.filter((c) => c.indexOf('fs.exec') === 0),
			[ 'fs.exec /etc/init.d/qmodem_voip_sipd start' ]);
		t = makeApi({ replies: { 'awvoice.health': held, 'luci.awvoice.sipdiag': svc(77) }, uci: { mode: 'interop' } });
		assert.strictEqual(await t.p.reconcileSipSwitch(), 'in-order');
		assert.strictEqual(t.calls.filter((c) => c.indexOf('fs.exec') === 0).length, 0,
			'a running service was started again');
		t = makeApi({ replies: { 'awvoice.health': theirs, 'luci.awvoice.sipdiag': svc(77) }, uci: {},
			exec: { '/etc/init.d/qmodem_voip_sipd': { code: 0 } } });
		assert.strictEqual(await t.p.reconcileSipSwitch(), 'stopped');
		t = makeApi({ replies: { 'awvoice.health': theirs, 'luci.awvoice.sipdiag': svc(0) }, uci: {} });
		assert.strictEqual(await t.p.reconcileSipSwitch(), 'in-order');
		/* off is only settled once the name is no longer held: a call still holding the line keeps its service up */
		t = makeApi({ replies: { 'awvoice.health': held, 'luci.awvoice.sipdiag': svc(77) }, uci: {} });
		assert.strictEqual(await t.p.reconcileSipSwitch(), 'waiting');
		assert.strictEqual(t.calls.filter((c) => c.indexOf('fs.exec') === 0).length, 0,
			'a service was stopped under a call that still holds the line');
		/* a call of any kind, on either side, is enough: the service is not moved out from under a leg it is carrying */
		for (const state of [ 'ringing', 'setup', 'call', 'release', 'dialling' ]) {
			t = makeApi({ replies: { 'awvoice.health': { line: { state: state }, sip: { objectOwner: 'awvoice' } },
				'luci.awvoice.sipdiag': svc(0) }, uci: { mode: 'interop' },
				exec: { '/etc/init.d/qmodem_voip_sipd': { code: 0 } } });
			assert.strictEqual(await t.p.reconcileSipSwitch(), 'waiting', state + ': a call was interrupted');
			assert.strictEqual(t.calls.filter((c) => c.indexOf('fs.exec') === 0).length, 0,
				state + ': the service was moved while the line was busy');
		}
		t = makeApi({ replies: { 'awvoice.health': { line: { state: 'idle' }, sip: { objectOwner: 'awvoice' } },
			'luci.awvoice.sipdiag': svc(0) }, uci: { mode: 'interop' },
			exec: { '/etc/init.d/qmodem_voip_sipd': { code: 0 } } });
		assert.strictEqual(await t.p.reconcileSipSwitch(), 'started');		/* the quiet line is the one that is served */

		await assert.rejects(t.p.saveSipSwitch('bridge'), /Unknown SIP mode/);
	}

	/* the path of the other program is named in exactly one file of this package, and the account's reach is the
	   three commands the grant lists: a fourth command, or a second place that names the script, reddens here */
	{
		const acl = JSON.parse(fs.readFileSync(path.join(__dirname, '..', 'root', 'usr', 'share', 'rpcd', 'acl.d',
			'luci-app-awvoice.json'), 'utf8'))['luci-app-awvoice'];
		const cmds = Object.keys(acl.write.file).filter((c) => c.indexOf('/etc/init.d/') === 0).sort();
		const named = ['api.js', 'tab_sip.js', 'live.js', 'phone.js'].filter((f) => {
			return fs.readFileSync(f === 'phone.js' ? path.join(dir, '..', 'phone.js') : path.join(dir, f), 'utf8')
				.indexOf('/etc/init.d/qmodem_voip_sipd') >= 0;
		});

		assert.deepStrictEqual(cmds, [ '/etc/init.d/qmodem_voip_sipd restart', '/etc/init.d/qmodem_voip_sipd start',
			'/etc/init.d/qmodem_voip_sipd stop' ]);
		assert.deepStrictEqual(named, [ 'api.js' ], 'more than one page file names another program\'s script');
		/* the card moves the service through the page's door, never by reading the settings and execing on its own,
		   and a switch the line could not hand over yet comes back on the next read instead of being left to the admin */
		assert.ok(/api\.saveSipSwitch/.test(sipSrc), 'the mode card does not move the service through the page door');
		assert.ok(/api\.reconcileSipSwitch/.test(sipSrc), 'a switch left waiting is never finished by a later read');
		/* the reads redraw the switch's own state: a save repaints the chip before the service reply is in, and a
		   card that never repaints it after that reply keeps saying what the box was a moment ago */
		assert.ok(/buildServiceCard\(\);\s*drawMode\(\);/.test(sipSrc),
			'the service read leaves the switch state undrawn');
		/* a move that just happened cannot report itself in the same instant: the card reads the box again a few
		   times on its own, and stops the moment the two agree */
		assert.ok(/settleLeft = SETTLE_MAX;\s*settleSwitch\(\);/.test(sipSrc),
			'the card never re-reads the box after moving the service');
		assert.ok(!/fs\.exec|init\.d/.test(sipSrc), 'the mode card reaches for a command itself');
		/* one disagreement decision, shared by the chip, the note and the button: three places that each work out
		   whether the service and the switch agree are three places that can disagree with each other on screen */
		assert.ok(/function switchOut\(/.test(sipSrc), 'the mode card has no one disagreement to read');
		/* each control is checked on the exact expression it uses: a shape that only looks for the word `out`
		   anywhere near a control passes while the control itself still reads the stored word alone */
		[ /modeChip\.className = 'av-statuschip static ' \+ \(\(pending \|\| out\) \? 'unk' : 'ok'\)/,
		  /modeChip\.lastChild\.textContent = \(pending \|\| out\) \? _\('not applied yet'\)/,
		  /modeSvcNote\.hidden = !switchOut\(\)/,
		  /saveBtn\.disabled = busy \? true : \(!\(pending \|\| out\)\)/ ].forEach(function(shape, i) {
			assert.ok(shape.test(sipSrc), 'the mode card\'s disagreement is not shared with control ' + (i + 1));
		});
		/* the side decides what the service is asked for: on starts it, off stops it, and neither is the other by
		   accident - a card that asks for a stop while turning a network phone on is silent about its own bug */
		{
			const api = fs.readFileSync(path.join(dir, 'api.js'), 'utf8');
			const move = /function moveSwitch[\s\S]*?\n}/.exec(api);

			assert.ok(move, 'the switch no longer has one path of its own');
			assert.ok(/waitForName\(side === 'sip'/.test(move[0]),
				'the switch does not wait for the qmodem_voip handover in both directions');
			assert.ok(/side === 'sip' \? sipSvcStart\(\) : sipSvcStop\(\)/.test(move[0]),
				'the switch asks the service for the wrong side');
		}
		/* the service card offers only what the page can change. The voice and text-message options are the
		   firmware's own defaults now, so nothing here names them, nothing writes them, and a stored value is
		   left exactly as the image set it - the card that stopped showing them must not keep editing them. */
		{
			const api = fs.readFileSync(path.join(dir, 'api.js'), 'utf8');
			const write = /function writeSipDirection[\s\S]*?\n}/.exec(api);

			assert.ok(write, 'the service card no longer has one write path');
			[ 'voip_enabled', 'sms_enabled', 'sms_modem' ].forEach(function(option) {
				assert.ok(write[0].indexOf(option) < 0, 'the page still writes ' + option);
				assert.ok(sipSrc.indexOf(option) < 0, 'the card still names ' + option);
			});
			assert.ok(!/Voice and messages/.test(sipSrc), 'the card still offers the voice and messages block');
			assert.ok(!/Modem name/.test(sipSrc), 'the card still asks for a modem name');
			assert.ok(!/SIP_MODEM_RE|smsModem/.test(api),
				'the page still carries a validation for a value it no longer reads or writes');
			/* the typed password stays, and the generator takes the place the removed block held */
			assert.ok(/u\.gridField\(_\('Password'\), fields\.inPass,/.test(sipSrc),
				'the inbound password field is no longer the card\'s own');
			assert.ok(/u\.gridField\(_\('A password made for you'\), genBtn,/.test(sipSrc),
				'the generator did not move into the row the removed block left');
			/* the audio path and the RTP range are not offered at all: the service binds one port fixed at its
			   own build and the socket is this package's daemon's file, so an edit can only take audio away */
			[ 'media_socket_path', 'rtp_start', 'rtp_end', 'SIP_SOCKET_DEF' ].forEach(function(option) {
				assert.ok(api.indexOf(option) < 0, 'the page still reads or writes ' + option);
			});
			[ 'Advanced', 'Audio connection file', 'Audio ports', 'socketPath' ].forEach(function(word) {
				assert.ok(sipSrc.indexOf(word) < 0, 'the card still shows ' + word);
			});
		}
		/* the two direction cards stand open like every other card on the page. A folded card hides the very
		   fields a status chip points at, and the title carries the state line and the switch in the row the
		   service card uses for its refresh button. */
		{
			const card = /function dirCard\(which\)[\s\S]*?\n\t{3}}/.exec(sipSrc);

			assert.ok(card, 'the direction cards no longer have one builder');
			[ "E('details'", 'summary', '.open =' ].forEach(function(word) {
				assert.ok(sipSrc.indexOf(word) < 0, 'a direction card folds again: ' + word);
			});
			assert.ok(/E\('h3', \[\s*\n\s*E\('span', \{ 'class': 'av-dir-t' \}/.test(card[0]),
				'the direction card title is not the heading every other card uses');
			assert.ok(/E\('span', \{ 'class': 'av-r' \}, \[ state, box\.node \]\)/.test(card[0]),
				'the state line and the switch are not in the title row');
			assert.ok(/box\.input\.addEventListener\('change', refreshState\)/.test(card[0]),
				'the title state line does not follow the switch');
		}
		/* the card's actions are two verbs, not two spellings of the same word: one that writes and restarts the
		   service, and the framework's own Reset, which is on the page whether or not there is something to undo.
		   A second button that only wrote the file left the running service on the old values and gave the admin
		   no label to choose by. */
		{
			assert.ok(/E\('div', \{ 'class': 'cbi-page-actions' \}, \[ saveApply, resetBtn \]\)/.test(sipSrc),
				'the card\'s action bar is not the two verbs');
			assert.ok(/_\('Save and restart'\)/.test(sipSrc), 'the write verb does not say it restarts');
			assert.ok(/_\('Reset'\)/.test(sipSrc), 'the undo verb is not the framework\'s word');
			[ "_('Save')", 'cbi-button-save', 'saveOnly', 'Discard changes' ].forEach(function(word) {
				assert.ok(sipSrc.indexOf(word) < 0, 'the card offers ' + word + ' again');
			});
			/* the reset stands on the page at all times: hiding it until something is edited is the second door
			   that confused the admin, and the framework never hides its own Reset. The chip above it may come and
			   go - it is a state, not a control */
			assert.ok(!/resetBtn\.hidden/.test(sipSrc), 'the reset is hidden until something changes');
			assert.ok(/function resetFields\(\)[\s\S]*?if \(!dirty\) \{\s*\n\s*loadService\(\);/.test(sipSrc),
				'the reset with nothing changed does not read the stored values back');
			assert.ok(!/thenRestart/.test(sipSrc), 'the card still has a write that leaves the service on old values');
			/* the words in the bar and the words in the dialog it opens are the same verb: a dialog that renames the
			   action is a second place the admin has to work out */
			assert.ok(/'click': function\(\) \{ ui\.hideModal\(\); saveService\(\); \} \}, _\('Save and restart'\)/.test(sipSrc),
				'the restart is not confirmed before it writes');
		}
	}

	/* the words and the sides are named in one place each, and the switch's own list holds the two sides only: a
	   rename on either side reddens this before the card silently offers something the daemon will not take */
	{
		const asWordList = (text) => text.split(',').map((s) => s.trim().replace(/'/g, '')).filter(Boolean);
		const words = /var SIP_WORDS = \[([^\]]*)\];/.exec(apiSrc);
		const sides = /var SWITCH = \[([^\]]*)\];/.exec(sipSrc);

		assert.ok(words, 'api.js does not name the words the settings can hold');
		assert.deepStrictEqual(asWordList(words[1]), [ 'web', 'interop', 'baresip' ]);
		assert.ok(sides, 'the mode card does not name its two sides');
		assert.deepStrictEqual(asWordList(sides[1]), [ 'web', 'sip' ]);
		/* the words behind the sides belong to the door alone: a card that repeats them shows the customer a name he
		   never chose, and a card that decides the side on its own can disagree with the switch above it */
		assert.ok(!/[^a-zA-Z_](interop|baresip)[^a-zA-Z_]/.test(sipSrc), 'the mode card names a method');
		assert.ok(/api\.sipSide/.test(sipSrc), 'the mode card does not read the side through the page door');
		/* the value that comes back from the settings has to select one of the two entries the card itself offers:
		   pointing the control at an entry it does not hold leaves nothing chosen and the save button waiting
		   for a change that can never be measured */
		assert.ok(/sel\.value = \(m === 'sip' \? 'sip' : 'web'\)/.test(sipSrc),
			'the mode card does not select one of its own two sides after a load');
		/* the words the page may write are the words the daemon answers to: the daemon names each state in one place
		   (sip_mode_name), so a state added, renamed or dropped on one side and not the other reddens here */
		{
			const guard = fs.readFileSync(path.join(__dirname, '..', '..', 'awvoice-phoned', 'src', 'daemon', 'sip_guard.c'), 'utf8');
			const named = [];
			const one = /case SIP_MODE_\w+:\s*return "([a-z]+)";/g;
			let hit;

			while ((hit = one.exec(guard)) !== null)
				named.push(hit[1]);
			assert.ok(named.length >= 3, 'the daemon names fewer than three states: ' + named.join(','));
			assert.deepStrictEqual(named.slice().sort(), asWordList(words[1]).slice().sort(),
				'the page and the daemon no longer name the same settings words');
		}
	}

	// the call's own view travels behind available, every field typed, the number kept in this page's own names; a
	// missing daemon is unavailable, not an error, and a refusal keeps its own words
	t = makeApi({ replies: { 'awvoice.call_status': { state: 'active', answer_owner: 'web', endpoint: 'browser',
		number_present: true, caller_id_withheld: false, remote_number: '0112345678', call_duration_seconds: 12,
		enabled: true, revision: 4, restart_epoch: 2, drop_count: 3, held: true, waiting: false,
		media_engine: 'ready', media_capture_frames: 250, media_playback_frames: 251 } } });
	const ss = await t.p.getCallStatus();
	assert.strictEqual(ss.available, true);
	assert.deepStrictEqual(ss, { available: true, state: 'active', origin: null, endpoint: 'browser',
		numberPresent: true, withheld: false, number: '0112345678', durationSec: 12, enabled: true,
		revision: 4, restartEpoch: 2, sequence: 0, dropCount: 3, answerOwner: 'web', held: true, waiting: false,
		mediaEngine: 'ready', captureFrames: 250, playbackFrames: 251 });
	t = makeApi({ replies: { 'awvoice.call_status': {} } });
	assert.strictEqual((await t.p.getCallStatus()).state, 'idle');
	t = makeApi({ replies: { 'awvoice.call_status': new Error('RPC call to awvoice/call_status failed with error 6: Not found') } });
	assert.deepStrictEqual(await t.p.getCallStatus(), { available: false });
	t = makeApi({ replies: { 'awvoice.call_status': Object.assign(new Error('RPC call failed'), { code: 6 }) } });
	assert.deepStrictEqual(await t.p.getCallStatus(), { available: false });
	t = makeApi({ replies: { 'awvoice.call_status': new Error('RPC call failed with error -32002: Permission denied') } });
	await assert.rejects(t.p.getCallStatus(), /may not do that/);

	// the line's block, the installed service's block and the voice port's owner: the keys travel, an unpublished
	// one is null and not an error
	t = makeApi({ replies: { 'awvoice.health': { web: { mode: 'interop', audio: 'listening', address: 'router.lan' },
		sip: { objectOwner: 'awvoice', lastRefusal: '' }, atPort: { portOwner: 'free' } } } });
	assert.deepStrictEqual(await t.p.getPhoneHealth(), { mode: 'interop', objectOwner: 'awvoice', portOwner: 'free', lastRefusal: null, audio: 'listening', port: 0, address: 'router.lan' });
	t = makeApi({ replies: { 'awvoice.health': {} } });
	assert.deepStrictEqual(await t.p.getPhoneHealth(), { mode: null, objectOwner: null, portOwner: null, lastRefusal: null, audio: null, port: 0, address: null });

	// the page's audio grant: a token for the call it saw, the stream's shape, and the daemon's refusal word
	t = makeApi({ replies: { 'awvoice.web_audio_open': { ok: true, refused: null, token: 'a'.repeat(64), port: 7443,
		rate: 8000, frame_ms: 20, expires_ms: 30000 } } });
	assert.deepStrictEqual(await t.p.webAudioOpen(4), { ok: true, refused: null, token: 'a'.repeat(64), port: 7443,
		rate: 8000, frameMs: 20, expiresMs: 30000 });
	assert.ok(t.calls.indexOf('awvoice.web_audio_open(4)') >= 0);
	await assert.rejects(t.p.webAudioOpen(0), /no call/);
	await assert.rejects(t.p.webAudioOpen('x'), /no call/);
	t = makeApi({ replies: { 'awvoice.web_audio_open': { ok: false, refused: 'refused-busy' } } });
	assert.deepStrictEqual(await t.p.webAudioOpen(4), { ok: false, refused: 'refused-busy' });
	t = makeApi({ replies: { 'awvoice.web_audio_open': { ok: true, token: 'b'.repeat(64), port: 7443, rate: 16000, frame_ms: 20 } } });
	await assert.rejects(t.p.webAudioOpen(4), /not a stream/);
	t = makeApi({ replies: { 'awvoice.web_audio_open': new Error('RPC call failed with error -32000: Object not found') } });
	await assert.rejects(t.p.webAudioOpen(4), /not running/);

	// the page's own call control: every action goes to the daemon's named method, a refusal keeps its word, and a
	// number the line could never dial is refused before the call goes out
	t = makeApi({ replies: { 'awvoice.web_originate': { ok: true, refused: null } } });
	assert.deepStrictEqual(await t.p.webOriginate(' +94112345678 '), { ok: true, refused: null });
	assert.ok(t.calls.indexOf('awvoice.web_originate(+94112345678)') >= 0);
	await assert.rejects(t.p.webOriginate('abc'), /digits/);
	await assert.rejects(t.p.webOriginate('*'), /digits/);
	await assert.rejects(t.p.webOriginate('+'), /digits/);
	await assert.rejects(t.p.webOriginate('1'.repeat(33)), /digits/);
	assert.strictEqual(t.calls.length, 1);
	t = makeApi({ replies: { 'awvoice.web_originate': { ok: false, refused: 'refused-busy' } } });
	assert.deepStrictEqual(await t.p.webOriginate('#123'), { ok: false, refused: 'refused-busy' });
	t = makeApi({ replies: { 'awvoice.web_answer': { ok: true } } });
	assert.deepStrictEqual(await t.p.webAnswer(), { ok: true, refused: null });
	t = makeApi({ replies: { 'awvoice.web_reject': { ok: false, refused: 'refused-nocall' } } });
	assert.deepStrictEqual(await t.p.webReject(), { ok: false, refused: 'refused-nocall' });
	t = makeApi({ replies: { 'awvoice.web_hangup': { ok: true, refused: null } } });
	assert.deepStrictEqual(await t.p.webHangup(), { ok: true, refused: null });
	t = makeApi({ replies: { 'awvoice.web_dtmf': { ok: true } } });
	assert.deepStrictEqual(await t.p.webDtmf('#'), { ok: true, refused: null });
	assert.ok(t.calls.indexOf('awvoice.web_dtmf(#)') >= 0);
	await assert.rejects(t.p.webDtmf('x'), /keypad/);
	await assert.rejects(t.p.webDtmf(''), /keypad/);
	/* the flash: the daemon's word travels, and a refusal keeps its own */
	t = makeApi({ replies: { 'awvoice.web_flash': { ok: true, refused: null } } });
	assert.deepStrictEqual(await t.p.webFlash(), { ok: true, refused: null });
	assert.ok(t.calls.indexOf('awvoice.web_flash()') >= 0);
	t = makeApi({ replies: { 'awvoice.web_flash': { ok: false, refused: 'refused-nocall' } } });
	assert.deepStrictEqual(await t.p.webFlash(), { ok: false, refused: 'refused-nocall' });
	t = makeApi({ replies: { 'awvoice.web_hangup': new Error('RPC call failed with error -32000: Object not found') } });
	await assert.rejects(t.p.webHangup(), /not running/);

	// the installed SIP service: its config read with the shipped defaults, never a stored password in the reply;
	// its status and the presence facts typed the page's way
	const sipEnv = () => ({
		configs: {
			qmodem_sip: {
				main: { __type: 'sipd', media_socket_path: '/var/run/qmodem_voip/media.sock', rtp_start: '40000', rtp_end: '40031' },
				inbound: { __type: 'direction', enabled: '1', interface: 'lan', listen_port: '5060', transport: 'udp_tcp',
					voip_enabled: '1', sms_enabled: '0', username: 'awvoice', password: 'secret123' },
				outbound: { __type: 'direction', enabled: '0', interface: 'wan', listen_port: '5061', transport: 'tls',
					server: 'sip.example.net', port: '5061', username: 'user1', password: '', realm: 'example.net',
					register_interval: '300', voip_enabled: '1', sms_enabled: '0' }
			},
			network: { wan: { __type: 'interface' }, lan: { __type: 'interface' }, loopback: { __type: 'interface' } }
		},
		replies: {
			'qmodem.sip.status': { status: 'ok', configured: true, username: 'awvoice',
				inbound: { enabled: true, running: true, registered: true },
				outbound: { enabled: false, running: false }, voipd_available: true },
			'luci.awvoice.sipdiag': { status: 'ok', config: true, script: true, mediaSocket: true, credentials: true,
				socketPath: '/var/run/qmodem_voip/media.sock', inboundPid: 4321, outboundPid: 0 }
		}
	});
	t = makeApi(sipEnv());
	let sv = await t.p.getSipService();
	assert.strictEqual(sv.available, true);
	assert.deepStrictEqual(sv.interfaces, [ 'lan', 'loopback', 'wan' ]);
	assert.strictEqual(sv.config.main, undefined, 'the card still reads the audio path and ports');
	assert.strictEqual(sv.config.inbound.username, 'awvoice');
	assert.strictEqual(sv.config.inbound.passwordSet, true);
	assert.strictEqual(sv.config.inbound.password, undefined);
	assert.strictEqual(sv.config.outbound.passwordSet, false);
	assert.strictEqual(sv.config.outbound.port, 5061);
	assert.strictEqual(sv.config.outbound.interval, 300);
	assert.deepStrictEqual(sv.service, { configured: true, username: 'awvoice',
		inbound: { enabled: true, running: true, registered: true },
		outbound: { enabled: false, running: false, registered: false }, voipdAvailable: true });
	assert.deepStrictEqual(sv.diag, { config: true, script: true, mediaSocket: true, credentials: true,
		inboundPid: 4321, outboundPid: 0 });

	// a config that is absent is a read-only card: available false, no config, and the two other reads may fail
	t = makeApi({ configs: { network: { lan: { __type: 'interface' } } },
		replies: { 'qmodem.sip.status': new Error('RPC call failed with error -32000: Object not found'),
			'luci.awvoice.sipdiag': new Error('RPC call failed with error -32000: Object not found') } });
	sv = await t.p.getSipService();
	assert.strictEqual(sv.available, false);
	assert.strictEqual(sv.config, null);
	assert.deepStrictEqual(sv.interfaces, [ 'lan' ]);
	assert.strictEqual(sv.service, null);
	assert.strictEqual(sv.diag, null);

	// saving: the same rules the service's own validator applies, checked before anything is written
	const svcGood = () => ({
		inbound: { enabled: true, iface: 'lan', listenPort: 5060, transport: 'udp_tcp',
			username: 'awvoice', password: '', passwordSet: true },
		outbound: { enabled: false, iface: 'wan', listenPort: 5061, transport: 'tls', server: '', port: 5061,
			username: '', password: '', passwordSet: false, realm: '', interval: 300 }
	});
	const svcEnv = () => Object.assign(sipEnv(), { replies: Object.assign({}, sipEnv().replies, { 'uci.commit': {} }) });
	t = makeApi(svcEnv());
	// the audio path and the RTP range are neither read nor written: what the image set stays as it is
	let g = svcGood();
	await t.p.saveSipService(g);
	assert.strictEqual(t.configs.qmodem_sip.main.media_socket_path, '/var/run/qmodem_voip/media.sock',
		'the page overwrote the media socket it no longer shows');
	assert.strictEqual(t.configs.qmodem_sip.main.rtp_start, '40000',
		'the page overwrote the RTP range it no longer shows');
	t = makeApi(svcEnv());
	g = svcGood(); g.inbound.iface = 'bad name';
	await assert.rejects(t.p.saveSipService(g), /interface name/);
	g = svcGood(); g.inbound.listenPort = 0;
	await assert.rejects(t.p.saveSipService(g), /local port/);
	g = svcGood(); g.inbound.username = 'bad name';
	await assert.rejects(t.p.saveSipService(g), /account name/);
	g = svcGood(); g.inbound.password = 'short';
	await assert.rejects(t.p.saveSipService(g), /8 to 127/);
	g = svcGood(); g.outbound = Object.assign(g.outbound, { enabled: true, username: 'user1', passwordSet: true });
	await assert.rejects(t.p.saveSipService(g), /server address/);
	g = svcGood(); g.outbound = Object.assign(g.outbound, { enabled: true, username: 'user1', passwordSet: true,
		server: 'sip.example.net', interval: 30 });
	await assert.rejects(t.p.saveSipService(g), /60 to 3600/);
	g = svcGood(); g.outbound = Object.assign(g.outbound, { enabled: true, username: 'user1', passwordSet: true,
		server: 'sip.example.net', realm: 'two words' });
	await assert.rejects(t.p.saveSipService(g), /realm/);
	assert.deepStrictEqual(t.calls.filter((c) => /^(uci\.|fs\.)/.test(c)), []);

	// a good save writes the direction, commits the service's config, and a typed password is the only password written
	g = svcGood(); g.inbound.listenPort = 5070; g.inbound.password = 'newsecret1';
	assert.deepStrictEqual(await t.p.saveSipService(g), { saved: true });
	assert.ok(t.calls.indexOf('uci.set qmodem_sip.inbound.listen_port=5070') >= 0);
	assert.ok(t.calls.indexOf('uci.set qmodem_sip.inbound.password=newsecret1') >= 0);
	assert.ok(t.calls.indexOf('uci.commit(qmodem_sip)') >= 0);
	assert.strictEqual(t.configs.qmodem_sip.inbound.listen_port, '5070');
	t = makeApi(svcEnv());
	g = svcGood(); g.inbound.username = 'renamed';
	await t.p.saveSipService(g);
	assert.strictEqual(t.configs.qmodem_sip.inbound.password, 'secret123');
	assert.ok(t.calls.indexOf('uci.set qmodem_sip.inbound.password=secret123') < 0);

	// an option the card no longer shows is neither written nor cleared: the image's own default is what runs
	t = makeApi(svcEnv());
	g = svcGood();
	await t.p.saveSipService(g);
	assert.strictEqual(t.calls.filter((c) => /\.(voip_enabled|sms_enabled|sms_modem)=/.test(c)).length, 0,
		'the page wrote an option it no longer shows');
	assert.strictEqual(t.configs.qmodem_sip.inbound.voip_enabled, '1',
		'the stored voice-path option was overwritten by a save that no longer offers it');

	// the installed service's restart goes through its own init script, and a refused restart is an error the page shows
	t = makeApi({ replies: {}, exec: { '/etc/init.d/qmodem_voip_sipd': { code: 0 } } });
	assert.deepStrictEqual(await t.p.applySipService(), { ok: true, code: 0 });
	assert.ok(t.calls.indexOf('fs.exec /etc/init.d/qmodem_voip_sipd restart') >= 0);
	t = makeApi({ replies: {}, exec: { '/etc/init.d/qmodem_voip_sipd': { code: 1 } } });
	await assert.rejects(t.p.applySipService(), /did not restart/);

	// generate credentials: the service's own method, the password returned once and never kept here
	t = makeApi({ replies: { 'qmodem.sip.generate_credentials': { status: 'success', configured: 1, username: 'awvoice', password: 'AbCdEf123456' } } });
	assert.deepStrictEqual(await t.p.generateSipCredentials('awvoice'), { username: 'awvoice', password: 'AbCdEf123456', configured: true });
	assert.ok(t.calls.indexOf('qmodem.sip.generate_credentials(awvoice)') >= 0);
	await assert.rejects(t.p.generateSipCredentials('bad name'), /account name/);
	t = makeApi({ replies: { 'qmodem.sip.generate_credentials': { status: 'success' } } });
	await assert.rejects(t.p.generateSipCredentials('awvoice'), /did not return a password/);
	t = makeApi({ replies: { 'qmodem.sip.generate_credentials': new Error('RPC call failed with error -32000: Object not found') } });
	await assert.rejects(t.p.generateSipCredentials('awvoice'), /installed SIP service is not running/);

	/* ---- the code a softphone scans: one door, and a word for every way it can say no ---- */
	{
		const CODE_URL = 'http://192.168.1.1:8080/awvoice-' + '0'.repeat(32) + '.xml';
		t = makeApi({ replies: { 'luci.awvoice.provision': {
			ok: true, url: CODE_URL, expires: 1700000600, ttl: 600, username: 'awvoice', realm: 'qmodem-voip' } } });
		assert.deepStrictEqual(await t.p.sipProvision(),
			{ url: CODE_URL, expires: 1700000600, ttl: 600, username: 'awvoice', realm: 'qmodem-voip' });
		assert.ok(t.calls.indexOf('luci.awvoice.provision()') >= 0, 'the code did not go through the page door');

		for (const c of [
			[ 'NO_ENDPOINT', /plain address/ ],
			[ 'DIRECTION_OFF', /switched off/ ],
			[ 'NO_ACCOUNT', /account name/ ],
			[ 'NO_PASSWORD', /cannot be read back/ ],
			[ 'NO_ADDRESS', /no address/ ],
			[ 'HELPER_FAILED', /could not be written/ ],
			[ 'SOMETHING_NEW', /cannot make a code/ ]
		]) {
			const x = makeApi({ replies: { 'luci.awvoice.provision': { ok: false, reason: c[0] } } });
			await assert.rejects(x.p.sipProvision(), c[1], c[0] + ' was not said in words');
		}

		/* an answer that is not an address is refused outright: a code that scans to nothing is worse than no code */
		t = makeApi({ replies: { 'luci.awvoice.provision': { ok: true, url: 'not an address', expires: 0 } } });
		await assert.rejects(t.p.sipProvision(), /cannot be read/);
		t = makeApi({ replies: { 'luci.awvoice.provision': new Error('RPC call failed with error -32002: Access denied') } });
		await assert.rejects(t.p.sipProvision(), /may not make a code/);
		t = makeApi({ replies: { 'luci.awvoice.provision': new Error('RPC call failed with error -32000: Object not found') } });
		await assert.rejects(t.p.sipProvision(), /cannot make a code/);
	}

	/* ---- the greeting: the door, and the decisions the card and the uploader act on ---- */

	/*
	 * Every file the page ships must parse. The tabs are DOM code this harness never renders, so a broken string or a
	 * stray quote in one of them would otherwise first show up as a dead tab in the browser; parsing them all here is
	 * the cheapest place that catches it.
	 */
	fs.readdirSync(dir).filter((f) => f.endsWith('.js')).concat(['../phone.js']).forEach((f) => {
		const text = fs.readFileSync(path.join(dir, f), 'utf8');
		assert.ok(text.length > 0, f + ' is empty');
		try {
			new Function('_', 'baseclass', 'rpc', 'uci', 'fs', 'L', 'E', 'poll', text);
		} catch (e) {
			assert.fail(f + ' does not parse: ' + e.message);
		}
	});

	/*
	 * The health tab reads the daemon's reply by name. A key the daemon does not write shows nothing at all and the
	 * tab still looks healthy, so every line.* and modem.* name the tab asks for is checked twice: against the writer
	 * in awvoice-phoned/src/daemon/reply.c, and against the page's own normaliser in live.js - that function rebuilds
	 * each block from a list written out by hand, so a name it leaves out reaches no card even though both the daemon
	 * and the tab name it correctly. One side renaming or dropping a field reddens this before a person notices an
	 * empty row.
	 */
	{
		const health = fs.readFileSync(path.join(dir, 'tab_health.js'), 'utf8');
		const reply = fs.readFileSync(path.join(__dirname, '..', '..', 'awvoice-phoned', 'src', 'daemon', 'reply.c'), 'utf8');
		const norm = src.slice(src.indexOf('function normHealth'), src.indexOf('/* numbers must not reach'));
		const vars = {};

		assert.ok(norm.length > 400, 'live.js has no health normaliser to check: ' + norm.length);
		assert.ok(src.indexOf('/* numbers must not reach') > src.indexOf('function normHealth'),
			'the health normaliser does not end where this check expects it');
		norm.replace(/\b([a-z]+) = r\.(\w+)/g, (m, v, blk) => { vars[blk] = v; return m; });
		assert.ok(vars.line && vars.modem, 'the normaliser does not name its line and modem blocks');
		const used = new Set();
		health.replace(/h\.(line|modem)\.([A-Za-z0-9_]+)/g, (m, blk, key) => {
			used.add(blk + '.' + key);
			return m;
		});
		assert.ok(used.size >= 10, 'the health tab reads too few fields: ' + used.size);
		used.forEach((name) => {
			const blk = name.slice(0, name.indexOf('.')), key = name.slice(name.indexOf('.') + 1);

			assert.ok(new RegExp('(o->\\w+\\(o->ctx, "|tri\\(o, ")' + key + '"').test(reply), 'the daemon writes no ' + name);
			assert.ok(new RegExp('\\b' + vars[blk] + '\\.' + key + '\\b').test(norm), 'the page drops ' + name + ' between the reply and the card');
		});
		assert.ok(used.has('line.numbering') && used.has('modem.operator'));	/* the plan in force, and the network it came from */

		/* the quiet hours answer, the same three ways: the block the daemon writes, the block the page rebuilds from
		   it, and the card that shows it. A name missing at either step shows the admin an empty row and nothing else,
		   which is how a health field goes unpublished without ever looking broken. */
		{
			const block = /o->open\(o->ctx, "quiet", 0\);[\s\S]*?o->close\(o->ctx\);/.exec(reply);
			const norm2 = /quiet: \{[^}]*\}/.exec(norm);

			assert.ok(block, 'the daemon publishes no quiet-hours block in the health reply');
			assert.ok(norm2, 'live.js publishes no quiet block of its own');
			assert.ok(/q = r\.quiet/.test(norm), 'live.js takes the quiet block from somewhere other than the reply');
			[ 'on', 'cause', 'ask' ].forEach(function(key) {
				assert.ok(new RegExp('o->\\w+\\(o->ctx, "' + key + '"').test(block[0]), 'the daemon writes no quiet ' + key);
				assert.ok(new RegExp('\\b' + key + ':').test(norm2[0]), 'live.js drops the quiet ' + key + ' between the reply and the card');
				assert.ok(new RegExp('q\\.' + key + '\\b').test(health), 'the health card never reads the quiet ' + key);
			});
			assert.ok(/quietNow\(h\.quiet\)/.test(health), 'the health card does not ask the line about quiet hours');
			/* a daemon that publishes nothing is a line the page has no answer for, not a line it may ring */
			assert.ok(/if \(!q \|\| q\.on == null\)/.test(health), 'the card answers an unknown quiet state as if the line rang');
		}
	}

	// one chunk of the greeting's own file: the shape the tab walks, with the reply's numbers clamped
	t = makeApi({ replies: { 'awvoice.greeting_get': { total: 84468, offset: 24576, b64: 'Zm9vYmFy', more: 1 } } });
	assert.deepStrictEqual(await t.p.getGreeting(24576), { total: 84468, offset: 24576, b64: 'Zm9vYmFy', more: true });
	assert.ok(t.calls.indexOf('awvoice.greeting_get(24576)') >= 0);
	t = makeApi({ replies: { 'awvoice.greeting_get': {} } });
	assert.deepStrictEqual(await t.p.getGreeting(0), { total: 0, offset: 0, b64: '', more: false });
	await assert.rejects(t.p.getGreeting('x'), /greeting offset/);
	t = makeApi({ replies: { 'awvoice.greeting_get': new Error('RPC call failed with error -32000: Object not found') } });
	await assert.rejects(t.p.getGreeting(0), /phone service is not running/);

	// a refusal travels as the daemon's word in the body, so rpcd hands it on instead of losing it as an error
	t = makeApi({ replies: { 'awvoice.greeting_begin': { ok: 0, refused: 'BUSY' } } });
	assert.deepStrictEqual(await t.p.greetingBegin(), { ok: false, refused: 'BUSY', size: 0, ms: 0 });
	t = makeApi({ replies: { 'awvoice.greeting_begin': { ok: 1 } } });
	assert.strictEqual((await t.p.greetingBegin()).ok, true);
	t = makeApi({ replies: { 'awvoice.greeting_put': { ok: 1, size: 44 } } });
	assert.deepStrictEqual(await t.p.greetingPut(0, 'Zm9v'), { ok: true, refused: null, size: 44, ms: 0 });
	assert.ok(t.calls.indexOf('awvoice.greeting_put(0,Zm9v)') >= 0);
	await assert.rejects(t.p.greetingPut(-1, 'Zm9v'), /upload piece/);
	await assert.rejects(t.p.greetingPut(0, null), /upload piece/);
	t = makeApi({ replies: { 'awvoice.greeting_commit': { ok: 0, refused: 'FORMAT' } } });
	assert.strictEqual((await t.p.greetingCommit(0)).refused, 'FORMAT');
	t = makeApi({ replies: { 'awvoice.greeting_commit': { ok: 1, ms: 5280 } } });
	assert.strictEqual((await t.p.greetingCommit(1)).ms, 5280);
	t = makeApi({ replies: { 'awvoice.greeting_abort': { ok: 1 } } });
	assert.strictEqual((await t.p.greetingAbort()).ok, true);

	const gr = makeGreeting();

	// the file's one place and the daemon's own numbers: a change on one side fails here rather than surprising a user
	assert.strictEqual(gr.PATH, '/etc/awvoice/greeting.wav');		/* machine.h MACHINE_GREETING_PATH */
	assert.strictEqual(gr.MAX_BYTES, 2 * 1024 * 1024);		/* machine.h MACHINE_MAX_BYTES */
	assert.strictEqual(gr.PUT_BYTES, 16384);			/* machine.h MACHINE_GREETING_PUT_MAX */
	assert.strictEqual(gr.GET_BYTES, 24576);			/* api.c RECORDING_CHUNK */
	assert.strictEqual(gr.HEADER, 44);
	assert.strictEqual(gr.RATE, 8000);

	// the card, from the health reply alone: what is known, what is loaded, and whether there is anything to play
	assert.deepStrictEqual(gr.card(null), { known: false, loaded: false, ms: 0, word: '', playable: false });
	assert.deepStrictEqual(gr.card({}), { known: false, loaded: false, ms: 0, word: '', playable: false });
	assert.deepStrictEqual(gr.card({ machine: {} }), { known: false, loaded: false, ms: 0, word: '', playable: false });
	assert.deepStrictEqual(gr.card({ machine: { greetingLoaded: true, greetingMs: 5280, greetingError: '' } }),
		{ known: true, loaded: true, ms: 5280, word: '', playable: true });
	assert.strictEqual(gr.card({ machine: { greetingLoaded: false, greetingError: 'MISSING' } }).playable, false);
	assert.strictEqual(gr.card({ machine: { greetingLoaded: false, greetingError: 'UNREADABLE' } }).playable, false);
	assert.strictEqual(gr.card({ machine: { greetingLoaded: false, greetingError: 'FORMAT' } }).playable, true);

	// every word the daemon can say about the file has its own sentence, and an unknown one is named rather than hidden
	const WORDS = [ 'MISSING', 'FORMAT', 'EMPTY', 'TRUNCATED', 'TOO_BIG', 'MEMORY', 'SYMLINK', 'BUSY', 'OFFSET', 'IO', 'UNREADABLE' ];
	const said = new Set();
	WORDS.forEach((w) => {
		const s = gr.sentence(w);
		assert.ok(s.length > 8, 'no sentence for ' + w);
		/* the generic line names the word; a word the page knows must not fall through to it, or the daemon grew a
		 * refusal the user only sees as a code */
		assert.ok(s.indexOf('(' + w + ')') < 0, 'no sentence of its own for ' + w);
		assert.ok(!said.has(s), 'two words share one sentence: ' + w);
		said.add(s);
	});
	assert.strictEqual(gr.sentence(''), '');
	assert.ok(/SOMETHING_NEW/.test(gr.sentence('SOMETHING_NEW')));	/* an unforeseen word is told with itself named */

	// the pre-check on a chosen file: not a WAV is stopped before a byte travels; the shape inside is the page's job to fix
	const riff = (n) => {
		const b = new Uint8Array(n || 44);
		b.set([ 0x52, 0x49, 0x46, 0x46, 4, 0, 0, 0, 0x57, 0x41, 0x56, 0x45 ], 0);
		b.set([ 0x66, 0x6d, 0x74, 0x20 ], 12);
		b.set([ 0x64, 0x61, 0x74, 0x61 ], 36);
		return b;
	};
	assert.strictEqual(gr.rejectWav(null, 0), 'FORMAT');
	assert.strictEqual(gr.rejectWav(new Uint8Array([ 1, 2, 3 ]), 3), 'FORMAT');
	assert.strictEqual(gr.rejectWav(new Uint8Array([ 0x52, 0x49, 0x46, 0x46, 0, 0, 0, 0, 0x57, 0x41, 0x56, 0x45 ]), 8), 'TRUNCATED');
	assert.strictEqual(gr.rejectWav(riff(44), 44), null);
	assert.strictEqual(gr.rejectWav(riff(44), gr.MAX_BYTES + 1), 'TOO_BIG');
	{
		const other = riff(44);
		other[0] = 0x4d;						/* a file that does not start "RIFF" is not a WAV */
		assert.strictEqual(gr.rejectWav(other, 44), 'FORMAT');
	}

	// the piece plan: whole pieces to the end, never a byte over the daemon's piece, and the offsets it must acknowledge
	assert.deepStrictEqual(gr.plan(0), []);
	assert.deepStrictEqual(gr.plan(-5), []);
	assert.deepStrictEqual(gr.plan(10), [ { offset: 0, size: 10 } ]);
	assert.strictEqual(gr.plan(44 + 128).length, 1);		/* a short greeting is one piece: the plan never splits what fits */
	assert.strictEqual(gr.plan(172, 100).length, 2);
	{
		const pieces = gr.plan(gr.PUT_BYTES * 3 + 10);
		let at = 0;
		assert.strictEqual(pieces.length, 4);
		pieces.forEach((p) => {
			assert.strictEqual(p.offset, at);
			assert.ok(p.size > 0 && p.size <= gr.PUT_BYTES);
			at += p.size;
		});
		assert.strictEqual(at, gr.PUT_BYTES * 3 + 10);
		assert.strictEqual(gr.plan(100, 40).length, 3);
		assert.strictEqual(gr.plan(100, 0).length, 1);	/* a piece size of none is the default size, not a plan of nothing */
	}

	// the WAV the page writes is the shape the daemon reads back: the same 44 bytes, byte for byte
	{
		const s = new Int16Array([ 1, -1, 258 ]);
		const w = gr.wav(s, 8000);
		const dv = new DataView(w.buffer);
		assert.strictEqual(w.length, 44 + 6);
		assert.strictEqual(String.fromCharCode(w[0], w[1], w[2], w[3]), 'RIFF');
		assert.strictEqual(String.fromCharCode(w[8], w[9], w[10], w[11]), 'WAVE');
		assert.strictEqual(dv.getUint32(4, true), 36 + 6);
		assert.strictEqual(dv.getUint32(16, true), 16);		/* the fmt chunk's size */
		assert.strictEqual(dv.getUint16(20, true), 1);		/* PCM */
		assert.strictEqual(dv.getUint16(22, true), 1);		/* mono */
		assert.strictEqual(dv.getUint32(24, true), 8000);
		assert.strictEqual(dv.getUint32(28, true), 16000);	/* byte rate */
		assert.strictEqual(dv.getUint16(32, true), 2);		/* block align */
		assert.strictEqual(dv.getUint16(34, true), 16);		/* bits a sample */
		assert.strictEqual(String.fromCharCode(w[36], w[37], w[38], w[39]), 'data');
		assert.strictEqual(dv.getUint32(40, true), 6);
		assert.strictEqual(dv.getInt16(44, true), 1);
		assert.strictEqual(dv.getInt16(46, true), -1);
		assert.strictEqual(dv.getInt16(48, true), 258);
		assert.strictEqual(gr.wav(s, 44100), null);		/* a rate the bus does not run at is not written at all */
		assert.strictEqual(gr.wav(new Int16Array(0), 8000).length, 44);	/* a header of no audio: the daemon calls it empty */
	}

	// Float32 to Int16, clamped at both ends: a microphone that ran over full scale must not wrap into the other phase
	{
		const c = gr.to16(new Float32Array([ 0, 1, -1, 2, -2, 0.5, -0.5 ]));
		assert.deepStrictEqual(Array.from(c), [ 0, 32767, -32768, 32767, -32768, 16384, -16384 ]);
	}

	// the capture processor's frames are big-endian on the wire; a WAV is little-endian
	{
		const be = new Uint8Array([ 0x01, 0x02, 0xff, 0xfe ]);
		assert.deepStrictEqual(Array.from(gr.swap16(be)), [ 0x02, 0x01, 0xfe, 0xff ]);
		assert.deepStrictEqual(Array.from(gr.swap16(new Uint8Array([ 7 ]))), []);	/* an odd tail is not half swapped */
	}

	// base64 both ways, with the padding the standard spells: the daemon's reader is strict and this is what it gets
	assert.strictEqual(gr.b64(new Uint8Array([])), '');
	assert.strictEqual(gr.b64(new Uint8Array([ 0x66 ])), 'Zg==');
	assert.strictEqual(gr.b64(new Uint8Array([ 0x66, 0x6f ])), 'Zm8=');
	assert.strictEqual(gr.b64(new Uint8Array([ 0x66, 0x6f, 0x6f ])), 'Zm9v');
	assert.strictEqual(gr.b64(new Uint8Array([ 0xfb, 0xff ])), '+/8=');
	{
		const all = new Uint8Array(256);
		for (let k = 0; k < 256; k++)
			all[k] = k;
		const enc = gr.b64(all);
		assert.strictEqual(enc.length, 344);
		assert.deepStrictEqual(Array.from(gr.unb64(enc)), Array.from(all));	/* every byte value, both ways */
		assert.strictEqual(gr.rawLen(enc), 256);
		assert.strictEqual(gr.rawLen('Zg=='), 1);
		assert.strictEqual(gr.rawLen('Zm8='), 2);
		assert.strictEqual(gr.rawLen('Zm9v'), 3);
		assert.strictEqual(gr.unb64('Zm9*'), null);		/* a character outside the alphabet is no bytes at all */
	}

	/* the send level the page owns: bounded, held under the ceiling, and the opposite of what the browser's gain
	   control did on a live call (it lifted a minus 20 dBFS test tone to minus 0.2 dBFS, into the hard clamp) */
	new Function(levelSrc)();
	const LV = globalThis.AwvoiceLevel ? globalThis.AWVOICE_LEVEL : null;
	assert.ok(LV, 'audio-level.js puts its class on the worklet global scope');
	assert.strictEqual(LV.gainMax, 8);
	assert.strictEqual(LV.gainMin, 0.25);
	assert.ok(Math.abs(LV.floor - 0.002) < 1e-9);

	const drive = (amp, freq, seconds, rate) => {
		const lvl = new globalThis.AwvoiceLevel(rate);
		let peak = 0, n = Math.round(rate * seconds), k, y;
		for (k = 0; k < n; k++) {
			y = lvl.step(amp * Math.sin(2 * Math.PI * freq * k / rate));
			if (k > rate * 0.05 && Math.abs(y) > peak)	/* the first 50 ms is the attack settling */
				peak = Math.abs(y);
		}
		return { peak, gain: lvl.gain };
	};

	let l = drive(0.95, 1000, 2, 48000);
	assert.ok(l.peak <= LV.ceiling * 1.2, 'a full-scale tone is held under the ceiling, saw ' + l.peak);
	assert.ok(l.peak >= LV.ceiling * 0.5, 'and not crushed into silence, saw ' + l.peak);
	l = drive(4, 1000, 2, 48000);
	assert.ok(l.peak <= LV.ceiling * 1.2, 'a stream past full scale never passes the ceiling, saw ' + l.peak);
	l = drive(0.004, 300, 6, 44100);
	assert.ok(l.gain >= 4 && l.gain <= LV.gainMax + 1e-6, 'a whisper is lifted hard, within the bound, gain ' + l.gain);
	assert.ok(l.peak <= LV.ceiling * 1.2, 'and the lifted whisper still cannot pass the ceiling, saw ' + l.peak);
	/* the level a laptop microphone actually hands over with the browser's gain control off: about minus 37 dBFS.
	   The loop must bring that to the telephone nominal, which is the whole point of the bound being generous. */
	l = drive(0.0141, 1000, 10, 48000);
	assert.ok(l.peak >= 0.05 && l.peak <= 0.2, 'a quiet talker lands near the nominal level, peak ' + l.peak);
	/* and below the noise floor nothing is pushed up: an empty room stays an empty room */
	l = drive(0.0005, 250, 8, 48000);
	assert.ok(l.gain <= 1.05, 'the hiss of an empty room is not lifted, gain ' + l.gain);
	assert.ok(l.peak <= 0.0006, 'and does not appear on the wire, peak ' + l.peak);
	l = drive(0, 1000, 3, 48000);
	assert.strictEqual(l.peak, 0);
	assert.ok(l.gain >= 0 && l.gain <= LV.gainMax, 'silence invents nothing, gain ' + l.gain);

	assert.strictEqual(LV.levelHold(0.5, 0.9, LV.holdStep), 0.9,	/* a louder peak shows at once */);
	assert.ok(Math.abs(LV.levelHold(0.5, 0.2, LV.holdStep) - 0.4925) < 1e-9);
	assert.strictEqual(LV.levelHold(0.5, 1.4, LV.holdStep), 1);
	assert.strictEqual(LV.levelHold(0.00005, 0, LV.holdStep), 0);
	assert.ok(levelSrc.includes('holdStep: 0.985'));
	assert.ok(webSrc.includes('* 0.985'), 'the bar decays with the step audio-level.js names');
	assert.ok(workletSrc.includes('new AwvoiceLevel(sampleRate)'), 'the capture levels before it decimates');
	assert.ok(audioSrc.includes('autoGainControl: false'), 'the browser is not handed the send level');
	assert.ok(audioSrc.includes("addModule(L.resource('view/awvoice/phone/audio-level.js'))"),
		'the level file is loaded into the worklet before the processors');

	/* A banner or a tile is a box on the page, and the theme paints its box: they carry the framework's own
	   message box and ifacebox, and the page's stylesheet gives them no background, border or radius of their own.
	   A rule that paints them again is a box that only matches the one theme it was measured against. */
	{
		const boxes = [
			[ 'the overview banner', overviewSrc, /E\('div', \{ 'class': 'av-ban alert-message', 'role': 'status' \}/ ],
			[ 'the health banner', healthSrc, /E\('div', \{ 'class': 'av-ban alert-message', 'role': 'status' \}/ ],
			[ 'the health banner of a service that does not answer', healthSrc, /'av-ban alert-message error'/ ],
			[ 'the test tools note', toolsSrc, /'av-ban alert-message warning'/ ],
			[ 'the not-built-yet banner', helpersSrc, /'av-ban alert-message warning'/ ]
		];

		boxes.forEach(function(b) {
			assert.ok(b[2].test(b[1]), b[0] + ' is not drawn by the framework message box');
		});
		assert.ok(/'av-ban alert-message' \+ \(b\[0\] \? ' ' \+ b\[0\] : ''\)/.test(overviewSrc),
			'the overview banner does not carry its state to the message box');
		assert.ok(/'av-ban alert-message' \+ \(okAt \? '' : ' warning'\)/.test(healthSrc),
			'the health banner does not carry its state to the message box');
		[ overviewSrc, healthSrc ].forEach(function(s) {
			assert.ok(/av-tile cbi-section/.test(s) && !/ifacebox-head/.test(s),
				'a status tile is not the framework card the page is built out of');
			assert.ok(/E\('small', /.test(s), 'the tile lost the design it has always had: a quiet label over the state');
		});
		[ '.avp .av-ban{', '.avp .av-tile{' ].forEach(function(rule) {
			const m = new RegExp(rule.replace(/[.*+?^${}()|[\]\\]/g, '\\$&') + '[^\']*');
			const line = m.exec(styleSrc);
			assert.ok(line, rule + ' is gone from the stylesheet');
			[ 'background', 'border', 'border-radius' ].forEach(function(p) {
				assert.ok(!line[0].includes(p), rule + ' paints its own ' + p + ' again');
			});
		});
		/* a row of pills is the page's own tab row, and the log block is a text surface: both take their panel from
		   the theme, exactly like the components beside them, and the page's stylesheet paints no surface on them */
		[ [ 'the call filter', callsSrc ], [ 'the rule types', rulesSrc ], [ 'the SIP sections', sipSrc ] ].forEach(function(b) {
			assert.ok(/'av-chips cbi-tabmenu'/.test(b[1]), b[0] + ' row is not a framework tab row');
		});
		assert.ok(/'av-log cbi-input-text/.test(healthSrc), 'the log block is not a framework text surface');
		/* quiet hours on the page: the Rules tab holds the windows and the always-ring numbers, the contact editor
		   holds one caller's ask, the health card is told by the line, and none of it reaches the router past the door */
		assert.ok(/\[ 'qh', _\('Quiet hours'\) \]/.test(rulesSrc), 'the Rules tab has no quiet-hours section');
		assert.ok(/editors\.quiet\(ctx, w, refreshQuiet\)/.test(rulesSrc) && /editors\.quiet\(ctx, null, refreshQuiet\)/.test(rulesSrc),
			'the quiet-hours pane does not open the editor for a window it shows and for a new one');
		assert.ok(/qhAllowBtn\.disabled = !quiet\.windows\.length/.test(rulesSrc),
			'the always-ring list is offered where no window can hold it');
		assert.ok(!/\buci\.|\brpc\.|\bfs\.exec/.test(rulesSrc), 'the Rules tab reaches the router past the page door');
		assert.ok(/return \{ windows: readQuiet\(\), allow: readQuietAllow\(\) \};/.test(src),
			'the page door reads only half of what the line reads for quiet hours');
		assert.ok(/ringThrough: through\.get\(\)/.test(editorsSrc), 'the contact editor never asks about quiet hours');
		assert.ok(/ringThrough: c\.ringThrough/.test(editorsSrc), 'adding a speed-dial code forgets the contact\'s quiet-hours ask');
		/* the call card is another side of the same phone: while the line says quiet hours hold this caller, its own
		   ring stays off and its word says what the call is. A reply that carries no quiet word at all leaves the ring
		   running, because that is what every service before quiet hours sent. The line the daemon holds the phone
		   off for is its own log line, pinned in the daemon's tests (test_quiet). */
		assert.ok(/function bellHeld\(\) \{[\s\S]{0,120}ctx\.status\.quiet === true/.test(webSrc),
			'the call card does not ask the line whether quiet hours hold this caller');
		assert.ok(/drawRing\(call\.state === 'incoming_ringing' && !bellHeld\(\)\)/.test(webSrc),
			'the page keeps ringing itself while the line says the handset is held quiet');
		assert.ok(/st === 'incoming_ringing' && bellHeld\(\) \? _\('Call held quiet'\)/.test(webSrc),
			'the call card still calls a held call a ringing one');
		/* the option names are the contract between this page and the daemon's reader: read beside it, so a name the
		   two sides do not share fails here instead of becoming a setting the line never sees */
		{
			const reader = fs.readFileSync(path.join(__dirname, '../../awvoice-phoned/src/daemon/config_uci.c'), 'utf8');
			[ 'enabled', 'from', 'to', 'all_day', 'days', 'mode', 'repeat_min', 'allow', 'name', 'quick', 'number', 'ring_through' ]
				.forEach(function(o) {
					assert.ok(new RegExp('uci_lookup_option(_string)?\\(ctx, sec, "' + o + '"\\)').test(reader),
						'the page writes ' + o + ', the daemon never reads it');
					assert.ok(new RegExp("'" + o + "'").test(src), 'the daemon reads ' + o + ', the page never writes it');
				});
			/* the bounds are the daemon's too, and the page must refuse exactly what it refuses: a window the page
			   accepts and the daemon drops is a quiet hour the admin believes is set and the line never honours */
			{
				const dndh = fs.readFileSync(path.join(__dirname, '../../awvoice-phoned/src/core/dnd.h'), 'utf8');
				[ [ 'DND_WINDOWS', /\bQUIET_MAX = ([0-9]+)/ ],
					[ 'DND_ALLOW_MAX', /\bQUIET_ALLOW_MAX = ([0-9]+)/ ],
					[ 'DND_MINUTES_DAY', /\bQUIET_REPEAT_MAX = ([0-9]+)/ ] ].forEach(function(pair) {
					const mine = new RegExp('#define ' + pair[0] + '\\s+([0-9]+)').exec(dndh);
					const theirs = pair[1].exec(src);

					assert.ok(mine, 'dnd.h states no ' + pair[0] + ' for this check to read');
					assert.ok(theirs, 'live.js states no bound beside ' + pair[0]);
					assert.strictEqual(Number(theirs[1]), Number(mine[1]),
						'the page holds ' + theirs[1] + ' where the daemon holds ' + mine[1] + ' (' + pair[0] + ')');
				});
			}
		}
		/* the row that finds a call is a panel of the page too: the framework card paints it, the page only lays
		   the row out, and the narrow width still folds it to two columns */
		assert.ok(/'av-fl cbi-section'/.test(callsSrc), 'the call filter row is not a framework card');
		{
			const fl = /\.avp \.av-fl\{[^']*/.exec(styleSrc);
			assert.ok(fl && /display:grid/.test(fl[0]), 'the filter row lost its own layout');
			[ 'background', 'border', 'padding' ].forEach(function(p) {
				assert.ok(!fl[0].includes(p), 'the filter row paints its own ' + p + ' again');
			});
			assert.ok(/@container avp \(max-width:700px\)[\s\S]*?\.avp \.av-fl\{grid-template-columns:minmax\(0,1fr\) minmax\(0,1fr\)/.test(styleSrc),
				'the filter row does not fold at the narrow width');
		}
		assert.ok(/'av-toast alert-message'/.test(shellSrc), 'the message of a moment is not a framework message box');
		{
			const toast = /\.av-toast\{[^']*/.exec(styleSrc);
			assert.ok(toast, 'the message of a moment has no rule at all');
			[ 'background', 'border', 'color:' ].forEach(function(p) {
				assert.ok(!toast[0].includes(p), 'the page paints the message box ' + p + ' again');
			});
		}
		/* the line's number is at the end of the state card, not a line under it: one column of two rows, held to
		   the card's right edge and read from the left, and gone entirely when there is no number to show */
		assert.ok(/E\('div', \{ 'class': 'av-ban alert-message', 'role': 'status' \}, \[ banIcon, E\('div', \[ banTitle, banSub \]\), numCol \]\)/.test(overviewSrc),
			'the number is not at the end of the state card');
		assert.ok(/E\('div', \{ 'class': 'av-ban-num', 'hidden': true \}, \[ numTitle, numValue \]\)/.test(overviewSrc),
			'the number is not two rows in one column');
		assert.ok(/numCol\.hidden = !st\.myNumber/.test(overviewSrc), 'a number the line does not have is shown anyway');
		assert.ok(!/av-note/.test(overviewSrc) && !/My number: %s/.test(overviewSrc),
			'the number is a loose line under the card again');
		{
			const num = /\.avp \.av-ban-num\{[^']*/.exec(styleSrc);
			assert.ok(num, 'the number column has no rule of its own');
			assert.ok(num[0].includes('margin-left:auto'), 'the number column does not stick to the card edge');
			assert.ok(num[0].includes('text-align:start'), 'the number is not read from the left');
			assert.ok(!/background|border-radius/.test(num[0]), 'the number column paints a box of its own');
		}
		[ '.avp .av-chips{', '.avp .av-log{' ].forEach(function(rule) {
			const m = new RegExp(rule.replace(/[.*+?^${}()|[\]\\]/g, '\\$&') + '[^\']*');
			const line = m.exec(styleSrc);
			assert.ok(line, rule + ' is gone from the stylesheet');
			[ 'background', 'border-radius' ].forEach(function(p) {
				assert.ok(!line[0].includes(p), rule + ' paints its own ' + p + ' again');
			});
		});
	}

	/* The code a softphone scans. One door writes the account file, one word answers every way it can say no,
	   and the password never travels further than that door: the page draws a code from an address alone. */
	{
		const plugin = fs.readFileSync(path.join(__dirname, '..', 'root', 'usr', 'libexec', 'rpcd', 'luci.awvoice'), 'utf8');
		const helper = fs.readFileSync(path.join(__dirname, '..', 'root', 'usr', 'libexec', 'awvoice-provision'), 'utf8');
		const acl = JSON.parse(fs.readFileSync(path.join(__dirname, '..', 'root', 'usr', 'share', 'rpcd', 'acl.d',
			'luci-app-awvoice.json'), 'utf8'))['luci-app-awvoice'];

		assert.ok(/'require uqr'/.test(sipSrc), 'the page does not use the framework\'s own code renderer');
		assert.ok(/uqr\.renderSVG\(c\.url, \{/.test(sipSrc), 'the code is not drawn from the address the door gave');
		assert.ok(/ctx\.api\.sipProvision\(\)/.test(sipSrc), 'the code button does not go through the page door');
		/* the code is drawn at a size a phone can focus on: the renderer's own module is small, so the page says
		   how big it is on screen and the camera of the phone does the rest */
		assert.ok(/'\.avp \.av-qr svg\{width:212px;height:212px\}',/.test(styleSrc),
			'the code is drawn at whatever size the renderer module happens to give');
		assert.ok(/saveBtn,\n\t{4}qrBtn\n\t{3}\]\),\n\t{3}modeDesc/.test(sipSrc),
			'the code button is not in the mode card\'s own row');
		assert.ok(/without a session for %d minutes/.test(sipSrc), 'the page does not say the file is served unsigned');
		assert.ok(/password is in the file it reaches/.test(sipSrc), 'the page does not say the password is in the file');
		/* the app that reads this code is named: no other softphone takes an address off a code this way */
		assert.ok(/_\('Show a Linphone QR code'\)/.test(sipSrc), 'the code button does not name the app it is for');
		assert.ok(/_\('Scan this with Linphone'\)/.test(sipSrc), 'the code screen does not name the app it is for');
		assert.ok(/the app offers its scanner only while it holds no account/.test(sipSrc),
			'the screen does not say the app must hold no account before it offers a scan');
		assert.ok(!/c\.password/.test(sipSrc), 'the page reads a password out of the code door');

		/* the door answers in words only, and the file it writes is the helper's, not the page's */
		assert.ok(/provision: remote\('luci\.awvoice', 'provision'\)/.test(apiSrc), 'the door is not this package\'s own');
		assert.deepStrictEqual(acl.write.ubus['luci.awvoice'], [ 'provision', 'service' ]);
		assert.ok(!((acl.read.ubus['luci.awvoice'] || []).indexOf('provision') >= 0), 'a write door was opened for reading');
		assert.ok(/printf '\{"service":\{"action":"str"\},"sipdiag":\{\},"provision":\{\}\}/.test(plugin),
			'the door does not advertise itself');

		/* every reason the door can answer with is said in the page's own words, or the admin sees a code */
		/* the door's own words are the contract; the writer's refusals travel inside one of them */
		const reasons = new Set();
		plugin.replace(/"reason":"([A-Z_]+)"/g, (m, r) => { reasons.add(r); return m; });
		assert.ok(!/'\{'\*'"'"'\''"reason":"BAD_/.test(plugin), 'the door forwards a writer refusal as its own word');
		assert.ok(/"reason":"HELPER_FAILED","detail"/.test(plugin), 'the door drops the writer\'s reason on the floor');
		[...reasons].forEach(function(r) {
			assert.ok(new RegExp('\t' + r + ': _[(]').test(apiSrc), 'the door answers ' + r + ' with no words');
		});

		/* the password is handed to the file writer and never printed by the door */
		assert.ok(!/printf[^\n]*\$pass/.test(plugin), 'the door prints the account password');
		plugin.split('\n').filter((l) => l.includes('$pass')).forEach(function(l) {
			assert.ok(/^\t*\[ -n "\$pass"/.test(l) || /--passwd "\$pass"/.test(l),
				'the door uses the account password somewhere the writer does not need it');
		});
		assert.ok(/ESC_PASSWD=\$\(printf '%s' "\$PASSWD" \| xml_esc\)/.test(helper),
			'the file writer does not escape the password it copies');
		/* the file lands where the listener reaches it and nowhere the page's own root can be read */
		assert.ok(/^OUT=\/www\/awvoice-provision$/m.test(helper),
			'the file writer drops its work into the root that serves the page');
		assert.ok(/^PATH_PFX=$/m.test(helper), 'the code address carries a path the listener does not answer on');
		assert.ok(/PROV_DIR=\/www\/awvoice-provision/.test(plugin), 'the door asks for the file somewhere else');
		assert.ok(/--out "\$PROV_DIR" --path ''/.test(plugin), 'the door does not pin where the file goes');
		assert.ok((fs.statSync(path.join(__dirname, '..', 'root', 'usr', 'libexec', 'awvoice-provision')).mode & 0o111) !== 0,
			'the file writer is not executable');
	}

	console.log('audio-level.js: all checks passed');
	console.log('greeting.js: all checks passed');
	console.log('live.js: all checks passed');
	console.log('api.js: all checks passed');
}

main().catch((e) => { console.error(e); process.exit(1); });
