// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
'use strict';
'require baseclass';
'require rpc';
'require uci';
'require view.awvoice.phone.live as provider';

/*
 * api.js - the one door between the phone page and its data. The tabs call only the methods below; they never read
 * UCI, ubus or files themselves. phone/live.js is the provider; the shapes and rules of every method are the data
 * contract v2 in DESIGN.md section 2.
 *
 * Every method returns a Promise. Failures reject with an Error whose .message is safe to show to the user. A method
 * whose backend is not built yet rejects with "Not available yet", and api.available[name] is false so that the page
 * can show its controls disabled instead of calling it.
 *
 * The recording and message calls (list, read chunk, delete) are served from this file directly: the daemon side is
 * one self-contained ubus object plus the options of the awvoice package's main section, so they get a small extra
 * door here instead of growing the provider. The shapes are the recording contract: 16-bit PCM in canonical 44-byte
 * WAV files; a message is the answering machine's recording of a caller, in the same canonical shape.
 */

var METHODS = [
	'getStatus', 'getHealth', 'hangup', 'getClock',
	'getPhoneSettings', 'savePhoneSettings',
	'runTest', 'serviceAction',
	'listCalls', 'deleteCalls',
	'listContacts', 'saveContact', 'removeContact',
	'getBlocking', 'saveBlocking',
	'getForwarding', 'saveForwarding',
	'listAlarms', 'saveAlarm', 'removeAlarm'
];

/* ---- recordings and the answering machine's messages ---- */

function remote(object, method, params) {
	return rpc.declare({ object: object, method: method, params: params || [] });
}

var CONF = 'awvoice', SECT = 'main';
var RECORD_PATH_DEF = '/etc/awvoice/recordings';
var RECORD_MAX_LO = 1, RECORD_MAX_HI = 4095, RECORD_MAX_DEF = 50;

var recBus = {
	list: remote('awvoice', 'recordings'),
	get: remote('awvoice', 'recording_get', [ 'name', 'offset' ]),
	del: remote('awvoice', 'recording_delete', [ 'name' ]),
	commit: remote('uci', 'commit', [ 'config' ]),
	reload: remote('awvoice', 'reload')
};

var msgBus = {
	list: remote('awvoice', 'messages'),
	get: remote('awvoice', 'message_get', [ 'name', 'offset' ]),
	del: remote('awvoice', 'message_delete', [ 'name' ])
};

/* the same words the provider puts on a ubus failure, so both doors fail alike */
function fail(err) {
	var m = String((err && err.message) || err || '');
	if (/not found|no such|unknown object|-32000/i.test(m))
		return new Error(_('The phone service is not running.'));
	if (/denied|permission|-32002/i.test(m))
		return new Error(_('This account may not do that.'));
	return new Error(_('The phone service did not answer.'));
}

function num(v) { return typeof v === 'number' && isFinite(v) ? v : null; }
function str(v) { return typeof v === 'string' ? v : ''; }

/* the daemon refuses any other name, so the page never sends one */
function recName(name) {
	return typeof name === 'string' && /^[0-9-]+\.wav$/.test(name) ? name : null;
}

function recOpt(name, def) {
	var v = uci.get(CONF, SECT, name);
	return v == null ? def : String(v);
}

/* write what is staged in uci, commit it and tell the daemon: the provider's own save path, for the daemon-only options */
function storeUci() {
	return uci.save().then(function() {
		return recBus.commit(CONF);
	}).then(function() {
		uci.unload(CONF);
		return recBus.reload().then(function() { return true; }, function() {
			throw new Error(_('Saved, but the phone service did not take the change. Restart it on the Health tab.'));
		});
	});
}

/* a File of the daemon's reply: the page's shape, whatever the daemon left out */
function entry(x) {
	return {
		name: str(x.name), bytes: Math.max(0, num(x.bytes) || 0),
		rate: Math.max(0, num(x.rate) || 0), seconds: Math.max(0, num(x.seconds) || 0),
		channels: Math.max(1, num(x.channels) || 1)
	};
}

var api = {
	available: Object.assign({ listRecordings: true, getRecording: true, deleteRecording: true,
		getRecordingSettings: true, saveRecordingSettings: true,
		listMessages: true, getMessage: true, deleteMessage: true }, provider.available || {}),
	settingsLive: provider.settingsLive || {},
	downStatus: function() { return provider.downStatus(); },

	/* what the daemon kept on its recording storage */
	listRecordings: function() {
		return recBus.list().then(function(r) {
			var list = r && Array.isArray(r.recordings) ? r.recordings : [];
			return list.map(entry);
		}, function(e) { throw fail(e); });
	},

	/* one chunk of a recording's raw PCM bytes, standard base64; more says whether the next chunk follows at
	 * offset + the chunk's raw length */
	getRecording: function(name, offset) {
		name = recName(name);
		if (!name || !(offset >= 0))
			return Promise.reject(new Error(_('Not a recording name')));
		return recBus.get(name, Math.floor(offset)).then(function(r) {
			r = r || {};
			return {
				name: str(r.name), rate: Math.max(0, num(r.rate) || 0), total: Math.max(0, num(r.total) || 0),
				offset: Math.max(0, num(r.offset) || 0), b64: str(r.b64), more: r.more === 1 || r.more === true
			};
		}, function(e) { throw fail(e); });
	},

	deleteRecording: function(name) {
		name = recName(name);
		if (!name)
			return Promise.reject(new Error(_('Not a recording name')));
		return recBus.del(name).then(function(r) {
			r = r || {};
			return { ok: r.ok === 1 || r.ok === true, removed: r.removed === 1 || r.removed === true };
		}, function(e) { throw fail(e); });
	},

	/* the answering machine's messages, newest first: a message's name leads with the epoch its call was answered */
	listMessages: function() {
		return msgBus.list().then(function(r) {
			var list = r && Array.isArray(r.messages) ? r.messages : [];
			return list.map(function(x) {
				var m = entry(x);

				m.ts = Math.max(0, num(x.ts) || 0);
				return m;
			}).sort(function(a, b) {
				if (a.ts !== b.ts)
					return b.ts - a.ts;
				return a.name < b.name ? 1 : a.name > b.name ? -1 : 0;
			});
		}, function(e) { throw fail(e); });
	},

	/* one chunk of a message, as getRecording; the name is the only handle and the daemon refuses any other */
	getMessage: function(name, offset) {
		name = recName(name);
		if (!name || !(offset >= 0))
			return Promise.reject(new Error(_('Not a message name')));
		return msgBus.get(name, Math.floor(offset)).then(function(r) {
			r = r || {};
			return {
				name: str(r.name), rate: Math.max(0, num(r.rate) || 0), total: Math.max(0, num(r.total) || 0),
				offset: Math.max(0, num(r.offset) || 0), b64: str(r.b64), more: r.more === 1 || r.more === true
			};
		}, function(e) { throw fail(e); });
	},

	deleteMessage: function(name) {
		name = recName(name);
		if (!name)
			return Promise.reject(new Error(_('Not a message name')));
		return msgBus.del(name).then(function(r) {
			r = r || {};
			return { ok: r.ok === 1 || r.ok === true, removed: r.removed === 1 || r.removed === true };
		}, function(e) { throw fail(e); });
	},

	getRecordingSettings: function() {
		uci.unload(CONF);
		return uci.load(CONF).then(function() {
			var max = parseInt(recOpt('record_max_mb', String(RECORD_MAX_DEF)), 10);
			return {
				enabled: recOpt('record_enabled', '0') === '1',
				path: recOpt('record_path', RECORD_PATH_DEF),
				maxMb: max >= RECORD_MAX_LO && max <= RECORD_MAX_HI ? max : RECORD_MAX_DEF
			};
		}, function(e) { throw fail(e); });
	},

	/* returns the saved settings; the daemon applies them through its own reload. The folder is written as the
	 * daemon reads it: no trailing slashes, and the root is refused (the daemon would fall back to its default) */
	saveRecordingSettings: function(s) {
		var max = s && s.maxMb;
		var path = s && typeof s.path === 'string' ? s.path.trim().replace(/\/+$/, '') : '';

		if (!s || typeof s.enabled !== 'boolean')
			return Promise.reject(new Error(_('Invalid recording settings')));
		if (path.charAt(0) !== '/' || path === '/')
			return Promise.reject(new Error(_('The recording folder must be an absolute path that is not the root')));
		if (typeof max !== 'number' || !(max >= RECORD_MAX_LO && max <= RECORD_MAX_HI) || max !== Math.floor(max))
			return Promise.reject(new Error(_('The recording budget must be 1 to 4095 MB')));
		uci.unload(CONF);
		return uci.load(CONF).then(function() {
			if (uci.get(CONF, SECT) == null)
				uci.add(CONF, 'phone', SECT);
			uci.set(CONF, SECT, 'record_enabled', s.enabled ? '1' : '0');
			uci.set(CONF, SECT, 'record_path', path);
			uci.set(CONF, SECT, 'record_max_mb', String(Math.floor(max)));
			return uci.save();
		}).then(storeUci).then(function() {
			return { enabled: s.enabled, path: path, maxMb: Math.floor(max) };
		}, function(e) { throw e && e.message ? e : fail(e); });
	}
};

METHODS.forEach(function(name) {
	if (typeof provider[name] !== 'function')
		throw new Error('Phone provider is missing ' + name + '()');
	api[name] = function() { return provider[name].apply(provider, arguments); };
});

return baseclass.extend(api);
