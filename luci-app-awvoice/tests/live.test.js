// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
'use strict';

/*
 * live.test.js - checks the provider (phone/live.js) and the page's data door (phone/api.js) against stand-ins for
 * the LuCI runtime: status and health normalisation, the settings read with defaults, validation, the order of a
 * save (write, commit, reload), the log redaction, the "Not available yet" rejections, and the answering machine's
 * message calls. Run: node tests/live.test.js
 */

const fs = require('fs');
const path = require('path');
const assert = require('assert');

const dir = path.join(__dirname, '../htdocs/luci-static/resources/view/awvoice/phone');
const src = fs.readFileSync(path.join(dir, 'live.js'), 'utf8');
const apiSrc = fs.readFileSync(path.join(dir, 'api.js'), 'utf8');

/* the stand-ins both files are loaded against; `calls` records every rpc and uci write in order */
function runtime(env) {
	const calls = [];
	const rpc = { declare: (d) => (...args) => {
		calls.push(d.object + '.' + d.method + '(' + args.join(',') + ')');
		const r = env.replies[d.object + '.' + d.method];
		return typeof r === 'function' ? r(args) : r instanceof Error ? Promise.reject(r) : Promise.resolve(r);
	} };
	const store = Object.assign({}, env.uci || {});
	const uci = {
		unload() {}, load: () => Promise.resolve(),
		get: (c, s, o) => (o === undefined ? (store.__sect ? {} : undefined) : store[o]),
		set: (c, s, o, v) => { store[o] = v; calls.push('uci.set ' + o + '=' + v); },
		unset: (c, s, o) => { delete store[o]; calls.push('uci.unset ' + o); },
		add: () => { store.__sect = true; calls.push('uci.add'); },
		save: () => { calls.push('uci.save'); return Promise.resolve(); }
	};
	const fsStub = { exec_direct: () => env.log ? Promise.resolve(env.log) : Promise.reject(new Error('denied')) };
	const baseclass = { extend: (o) => o };
	return { calls, store, rpc, uci, fsStub, baseclass };
}

function loadProvider(rt) {
	const factory = new Function('_', 'baseclass', 'rpc', 'uci', 'fs', src);
	return factory((s) => s, rt.baseclass, rt.rpc, rt.uci, rt.fsStub);
}

function makeProvider(env) {
	const rt = runtime(env);
	return { p: loadProvider(rt), calls: rt.calls, store: rt.store };
}

function makeApi(env) {
	const rt = runtime(env);
	const provider = loadProvider(rt);
	const factory = new Function('_', 'baseclass', 'rpc', 'uci', 'fs', 'provider', apiSrc);
	return { p: factory((s) => s, rt.baseclass, rt.rpc, rt.uci, rt.fsStub, provider), calls: rt.calls, store: rt.store };
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

	// the machine's health block: a loaded greeting and its length travel; a missing block is "unknown", not "off"
	t = makeProvider({ replies: { 'awvoice.health': { machine: { greetingLoaded: true, greetingMs: 4250, delaySec: 10 } } } });
	h = await t.p.getHealth();
	assert.deepStrictEqual(h.machine, { greetingLoaded: true, greetingMs: 4250, delaySec: 10 });
	t = makeProvider({ replies: { 'awvoice.health': {} } });
	assert.deepStrictEqual((await t.p.getHealth()).machine, { greetingLoaded: null, greetingMs: 0, delaySec: 0 });

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
	// recording's file name travels ('' when the daemon gave none: an old daemon's reply has no field at all)
	t = makeProvider({ replies: { 'awvoice.calls': { calls: [
		{ id: 3, type: 'IN', number: '+12345', name: 'Ann', ts: 1700000000, durationSec: 12, result: 'answered', cause: 0, recording: '1700000000-3.wav', message: '1700000005-3.wav' },
		{ id: 2, type: 'MISS', number: '1777', ts: 1700000100, durationSec: 0, result: 'missed', cause: 0 } ] } } });
	let calls = await t.p.listCalls();
	assert.strictEqual(calls.length, 2);
	assert.strictEqual(calls[0].name, 'Ann');
	assert.strictEqual(calls[0].number, '+12345');
	assert.strictEqual(calls[0].recording, '1700000000-3.wav');
	assert.strictEqual(calls[0].message, '1700000005-3.wav');
	assert.strictEqual(calls[1].name, '');
	assert.strictEqual(calls[1].type, 'MISS');
	assert.strictEqual(calls[1].recording, '');
	assert.strictEqual(calls[1].message, '');

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

	// the answering machine's settings: defaults, stored values, and out-of-range values fall back
	t = makeProvider({ replies: {} });
	assert.deepStrictEqual((await t.p.getPhoneSettings()).machine, { enabled: false, greeting: '/etc/awvoice/greeting.wav',
		delaySec: 10, path: '/etc/awvoice/messages', maxMb: 50, maxSec: 120 });
	t = makeProvider({ replies: {}, uci: { machine_enabled: '1', machine_greeting: '/tmp/g.wav', machine_delay: '0',
		machine_path: '/tmp/messages', machine_max_mb: '200', machine_max_s: '30' } });
	assert.deepStrictEqual((await t.p.getPhoneSettings()).machine, { enabled: true, greeting: '/tmp/g.wav',
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
		machine: { enabled: true, greeting: '/etc/awvoice/greeting.wav', delaySec: 10, path: '/etc/awvoice/messages', maxMb: 50, maxSec: 120 } });
	t = makeProvider({ replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true } } });
	let bad = good(); bad.hookFlash = { minMs: 250, maxMs: 300 };
	await assert.rejects(t.p.savePhoneSettings(bad), /100 ms/);
	bad = good(); bad.hookFlash.minMs = 10;
	await assert.rejects(t.p.savePhoneSettings(bad), /20 to 500/);
	bad = good(); bad.line.myNumber = 'abc';
	await assert.rejects(t.p.savePhoneSettings(bad), /digits/);
	bad = good(); bad.machine.greeting = 'greeting.wav';
	await assert.rejects(t.p.savePhoneSettings(bad), /greeting file/);
	bad = good(); bad.machine.greeting = '/' + 'a'.repeat(127);
	await assert.rejects(t.p.savePhoneSettings(bad), /greeting file/);
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
	assert.ok(t.calls.indexOf('uci.set machine_greeting=/etc/awvoice/greeting.wav') >= 0);
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
	console.log('live.js: all checks passed');
	console.log('api.js: all checks passed');
}

main().catch((e) => { console.error(e); process.exit(1); });
