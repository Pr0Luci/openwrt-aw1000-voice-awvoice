// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
'use strict';

/*
 * live.test.js - checks the provider (phone/live.js) against stand-ins for the LuCI runtime: status and health
 * normalisation, the settings read with defaults, validation, the order of a save (write, commit, reload), the log
 * redaction and the "Not available yet" rejections. Run: node tests/live.test.js
 */

const fs = require('fs');
const path = require('path');
const assert = require('assert');

const src = fs.readFileSync(path.join(__dirname, '../htdocs/luci-static/resources/view/awvoice/phone/live.js'), 'utf8');

function makeProvider(env) {
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
	const factory = new Function('_', 'baseclass', 'rpc', 'uci', 'fs', src);
	const p = factory((s) => s, baseclass, rpc, uci, fsStub);
	return { p, calls, store };
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

	// the call log: the daemon's name comes through ('' when it gave none), the number as it came
	t = makeProvider({ replies: { 'awvoice.calls': { calls: [
		{ id: 3, type: 'IN', number: '+12345', name: 'Ann', ts: 1700000000, durationSec: 12, result: 'answered', cause: 0 },
		{ id: 2, type: 'MISS', number: '1777', ts: 1700000100, durationSec: 0, result: 'missed', cause: 0 } ] } } });
	let calls = await t.p.listCalls();
	assert.strictEqual(calls.length, 2);
	assert.strictEqual(calls[0].name, 'Ann');
	assert.strictEqual(calls[0].number, '+12345');
	assert.strictEqual(calls[1].name, '');
	assert.strictEqual(calls[1].type, 'MISS');

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

	// save: validated first (nothing written), then write -> commit -> reload
	const good = () => ({ line: { cid: 'dtmf', myNumber: '0112345678', dialPauseSec: 6 }, hookFlash: { minMs: 80, maxMs: 300 }, tones: { ringback: 'auto' },
		audio: { earpieceVolume: null, modemRxGain: 8192, micGainDb: null, earpieceGainDb: null }, modem: { usbInterface: 2 },
		features: { clir: 'network', conference: false, callWaiting: true, hotline: { number: '', delaySec: 10 } }, indicator: { led: '', ringRateMs: 300, offhookRateMs: 1000 }, page: { refreshSec: 1 } });
	t = makeProvider({ replies: { 'uci.commit': {}, 'awvoice.reload': { ok: true } } });
	let bad = good(); bad.hookFlash = { minMs: 250, maxMs: 300 };
	await assert.rejects(t.p.savePhoneSettings(bad), /100 ms/);
	bad = good(); bad.hookFlash.minMs = 10;
	await assert.rejects(t.p.savePhoneSettings(bad), /20 to 500/);
	bad = good(); bad.line.myNumber = 'abc';
	await assert.rejects(t.p.savePhoneSettings(bad), /digits/);
	assert.deepStrictEqual(t.calls, []);
	const r = await t.p.savePhoneSettings(good());
	assert.strictEqual(r.ok, true);
	const order = t.calls.filter((c) => /^(uci\.save|uci\.commit|awvoice\.reload)/.test(c));
	assert.deepStrictEqual(order, [ 'uci.save', 'uci.commit(awvoice)', 'awvoice.reload()' ]);
	assert.ok(t.calls.indexOf('uci.unset earpiece_volume') >= 0);
	assert.ok(t.calls.indexOf('uci.set modem_rx_gain=8192') >= 0);
	assert.ok(t.calls.indexOf('uci.set clir=network') >= 0);
	assert.ok(t.calls.indexOf('uci.set page_refresh_s=1') >= 0);
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
	console.log('live.js: all checks passed');
}

main().catch((e) => { console.error(e); process.exit(1); });
