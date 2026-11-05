// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
'use strict';
'require baseclass';
'require rpc';
'require uci';
'require fs';
'require view.awvoice.phone.live as provider';

/*
 * api.js - the one door between the phone page and its data. The tabs call only the methods below; they never read
 * UCI, ubus or files themselves. phone/live.js is the provider; the shape of a method is the contract stated beside it.
 *
 * Every method returns a Promise. Failures reject with an Error whose .message is safe to show to the user. A method
 * whose backend is not built yet rejects with "Not available yet", and api.available[name] is false so that the page
 * can show its controls disabled instead of calling it.
 *
 * The recording and message calls (list, read chunk, delete) are served from this file directly: the daemon side is
 * one self-contained ubus object plus the options of the awvoice package's main section, so they get a small extra
 * door here instead of growing the provider. The shapes are the recording contract: 16-bit PCM in canonical 44-byte
 * WAV files; a message is the answering machine's recording of a caller, in the same canonical shape.
 *
 * The answering machine's greeting is one file at the daemon's own constant path (machine.h), so its door carries no
 * name: one chunk read at an offset, and begin/put/commit/abort for what the page uploads or records. A refusal comes
 * back as { ok, refused } with the daemon's word, and phone/greeting.js is what turns that word into a sentence.
 *
 * The page's own phone reads the daemon's core object: the call's view (call_status) and the browser-audio grant.
 * The SIP mode is one option of the awvoice package, and the installed SIP service's object (qmodem.sip) and its
 * rpcd plugin serve the SIP tab alone: the mode is read from UCI and written through the daemon's reload, and the
 * service's status is passed through behind an availability flag.
 */

var METHODS = [
	'getStatus', 'getHealth', 'hangup', 'getClock',
	'getPhoneSettings', 'savePhoneSettings',
	'runTest', 'serviceAction',
	'listCalls', 'deleteCalls',
	'listContacts', 'saveContact', 'removeContact',
	'getBlocking', 'saveBlocking',
	'getForwarding', 'saveForwarding',
	'listAlarms', 'saveAlarm', 'removeAlarm',
	'listQuiet', 'saveQuiet', 'removeQuiet', 'saveQuietAllow'
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

/* the answering machine's greeting: one file at one constant place, so no name ever travels to these methods */
var greetBus = {
	get: remote('awvoice', 'greeting_get', [ 'offset' ]),
	begin: remote('awvoice', 'greeting_begin'),
	put: remote('awvoice', 'greeting_put', [ 'offset', 'b64' ]),
	commit: remote('awvoice', 'greeting_commit', [ 'polish' ]),
	abort: remote('awvoice', 'greeting_abort')
};

var SIP_WORDS = [ 'web', 'interop', 'baresip' ];

/*
 * The switch's two sides: the line's own phones (`web`), and a network phone beside them (`sip`). The words behind
 * those sides stay here, in one place: a stored word reads as the side it runs, and a side writes a word. Asking for
 * a network phone keeps the box's own word for what serves it, so moving the switch says only whether one is wanted
 * and never changes how it is served. A stored word this build has no side for is the side that asks nothing of
 * any other program, which is the one a line can always be on.
 */
function sipSide(word) {
	word = word == null ? 'web' : String(word);
	return word !== 'web' && SIP_WORDS.indexOf(word) >= 0 ? 'sip' : 'web';
}

function sipWord(side, stored) {
	if (side !== 'sip')
		return 'web';
	return stored === 'interop' || stored === 'baresip' ? stored : 'interop';
}

/* the page's own door: the daemon's core object, the call's view and the browser-audio grant */
var phoneBus = {
	call: remote('awvoice', 'call_status'),
	health: remote('awvoice', 'health'),
	dial: remote('awvoice', 'web_originate', [ 'number' ]),
	answer: remote('awvoice', 'web_answer'),
	reject: remote('awvoice', 'web_reject'),
	hangup: remote('awvoice', 'web_hangup'),
	dtmf: remote('awvoice', 'web_dtmf', [ 'digit' ]),
	flash: remote('awvoice', 'web_flash'),
	audio: remote('awvoice', 'web_audio_open', [ 'call_revision' ])
};

/* the installed SIP service's own object and config: read for the card, written only by the admin's hand */
var sipSvcBus = {
	service: remote('qmodem.sip', 'status'),
	genCred: remote('qmodem.sip', 'generate_credentials', [ 'username' ]),
	diag: remote('luci.awvoice', 'sipdiag'),
	provision: remote('luci.awvoice', 'provision')
};

/* an absent object is a ubus "not found", by its text or its status code; no other failure is that */
function objectGone(e) {
	var m = String((e && e.message) || e || '');
	return /not found|no such|unknown object/i.test(m) || (e && Number(e.code) === 6);
}

/* the same words the provider puts on a ubus failure, so both doors fail alike */
function fail(err) {
	var m = String((err && err.message) || err || '');
	if (/not found|no such|unknown object|-32000/i.test(m))
		return new Error(_('The phone service is not running.'));
	if (/denied|permission|-32002/i.test(m))
		return new Error(_('This account may not do that.'));
	return new Error(_('The phone service did not answer.'));
}

/* a failure of the installed SIP service or of the page's own rpcd plugin, named for what it is */
function sipSvcFail(err) {
	var m = String((err && err.message) || err || '');
	if (/not found|no such|unknown object|-32000/i.test(m))
		return new Error(_('The installed SIP service is not running.'));
	if (/denied|permission|-32002/i.test(m))
		return new Error(_('This account may not do that.'));
	return new Error(_('The installed SIP service did not answer.'));
}

/* one of the two sides; the section is made when it is absent and storeUci saves, commits and applies it. The word
 * written is the side's, keeping the router's own method when it already runs one: moving the switch says only
 * whether a network phone is wanted, and never changes how it is served. */
function writeSide(side) {
	if (side !== 'web' && side !== 'sip')
		return Promise.reject(new Error(_('Unknown SIP mode')));
	uci.unload(CONF);
	return uci.load(CONF).then(function() {
		var word = sipWord(side, uci.get(CONF, 'sip', 'mode') == null ? null : String(uci.get(CONF, 'sip', 'mode')));

		if (uci.get(CONF, 'sip') == null)
			uci.add(CONF, 'sip', 'sip');
		uci.set(CONF, 'sip', 'mode', word);
		return word;
	}).then(function(word) {
		return storeUci().then(function() {
			return { mode: word };
		});
	}, function(e) { throw e && e.message ? e : fail(e); });
}

/* ---- the installed service's own init script, in one place ------------------------------------------------------------
 *
 * The path lives here and nowhere else on the page: the switch's side of this package is a name of a program that
 * belongs to another one, so this one line is what a port to other firmware has to change. The account's reach is
 * the rpcd grant, which lists these same three commands and nothing else. A start or a stop is never retried on a
 * failure, and never reached for when the script is absent: a box without the package still has a line side that
 * follows the switch.
 */
var SIP_SVC = '/etc/init.d/qmodem_voip_sipd';

/* the state of the service after the switch asked for it: what the admin is told, and nothing more */
function sipSvcAction(action) {
	return fs.exec(SIP_SVC, [ action ]).then(function(r) {
		return (r && r.code === 0) ? 'ok' : 'failed';
	}, function(e) {
		var m = String((e && e.message) || e || '');
		if (/denied|permission|-32002/i.test(m))
			return 'denied';
		if (/not found|no such|ENOENT/i.test(m))
			return 'absent';
		return 'failed';
	});
}

/* the script is a file before it is a command: asking rpcd for a missing one is a failure the admin should not see */
function sipSvcPresent() {
	return sipDiag().then(function(d) { return !!(d && d.script); }, function() { return true; });
}

/* the running state only, deliberately: this service's credential file lives on tmpfs and nothing in the package
 * writes it back at start, so enabling the unit at boot makes it crash-loop from the next reboot until someone
 * regenerates that file. A boot link is added to the switch once that file is written at start - which is a fix to
 * the other package, not to this one - and until then a reboot is served by the same press the switch already has */
function sipSvcStop() {
	return sipSvcPresent().then(function(there) {
		if (!there)
			return 'absent';
		return sipSvcAction('stop').then(function(r) { return r === 'ok' ? 'stopped' : r; });
	});
}

function sipSvcStart() {
	return sipSvcPresent().then(function(there) {
		if (!there)
			return 'absent';
		return sipSvcAction('start').then(function(r) { return r === 'ok' ? 'started' : r; });
	});
}

/* the daemon holds the qmodem_voip name only once the line is quiet enough to take the change, and lets go of it at
 * that same moment, so both directions of the switch wait for the handover they need, with a bound: past the wait
 * the switch is stored, the service is left where it is, and the card finishes it when the line comes quiet */
var NAME_WAIT_TRIES = 8;
var NAME_WAIT_MS = 250;

function nameIsHeld(r) {
	r = r || {};
	return !!(r.sip && r.sip.objectOwner === 'awvoice');
}

function waitForName(wanted, tries) {
	tries = tries == null ? NAME_WAIT_TRIES : tries;
	if (tries <= 0)
		return Promise.resolve(null);
	return phoneBus.health().then(function(r) {
		if (nameIsHeld(r) === wanted)
			return r;		/* the same reply the wait looked at, so the caller also sees how the line stands */
		return new Promise(function(ready) { setTimeout(ready, NAME_WAIT_MS); })
			.then(function() { return waitForName(wanted, tries - 1); });
	}, function() { return null; });
}

/* the service answers its own pidfile, through this package's rpcd plugin: running means a live inbound or outbound
 * process, and a service that is stopped leaves no ubus object behind to ask */
function sipSvcRunning() {
	return sipDiag().then(function(d) {
		return !!(d && (d.inboundPid > 0 || d.outboundPid > 0));
	}, function() { return false; });
}

function storedSide() {
	uci.unload(CONF);
	return uci.load(CONF).then(function() {
		return sipSide(uci.get(CONF, 'sip', 'mode'));
	}, function() { return 'web'; });
}

/* the switch, taken through in one go: the side is written, the handover of the name is waited for, and the service
 * is moved after it - started when the name became this daemon's, stopped once it is no longer that. A handover that
 * does not come inside the wait is reported as waiting, never as done. */
function moveSwitch(side) {
	if (side !== 'web' && side !== 'sip')
		return Promise.reject(new Error(_('Unknown SIP mode')));

	return writeSide(side).then(function(r) {
		return waitForName(side === 'sip', NAME_WAIT_TRIES).then(function(h) {
			/* no handover inside the wait, or a call on the line as it came: the service is left where it is and
			   the wait is finished by the next read, not by a second press from the admin */
			if (!h || lineBusy(h))
				return { mode: r.mode, side: side, service: 'waiting' };
			return (side === 'sip' ? sipSvcStart() : sipSvcStop()).then(function(state) {
				return { mode: r.mode, side: side, service: state };
			});
		});
	});
}

/* a call is on the line while it is ringing, set up, active or ending: the service is then carrying legs of it, and
 * moving the service under a call is what this ordering exists to avoid. The daemon takes and releases the name only
 * when the line is quiet, so this is the same moment seen from the page */
function lineBusy(h) {
	var st = h && h.line ? String(h.line.state || '') : '';

	return st === 'ringing' || st === 'setup' || st === 'call' || st === 'release' || st === 'dialling';
}

/* what the switch asks for and what the router actually looks like, put in order after a wait: only the action that
 * is missing is taken, and a line that is still busy leaves everything where it is */
function reconcileSwitch() {
	return storedSide().then(function(side) {
		return phoneBus.health().then(function(h) {
			var held = nameIsHeld(h);

			if (lineBusy(h))
				return 'waiting';
			if (side === 'sip' && held)
				return sipSvcRunning().then(function(run) {
					return run ? 'in-order' : sipSvcStart();
				});
			if (side === 'web' && !held)
				return sipSvcRunning().then(function(run) {
					return run ? sipSvcStop() : 'in-order';
				});
			return 'waiting';
		}, function() { return 'waiting'; });
	});
}

/* the switch's result is a state word, never a sentence: the card that shows the switch is what says it to the
 * admin, and the words are `started`, `stopped`, `absent` (no such program on this router), `denied` (not this
 * account's to reach), `failed` (it answered with an error) or `waiting` (the line is in a call, so the name this
 * service needs is not yet this daemon's to hand over) */

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

/* ---- the page's own call: its view and its actions ---------------------------------------------------------------- */

var DIAL_RE = /^\+?[0-9*#]{1,32}$/;
var DIGIT_RE = /^[0-9*#A-D]$/;

/* a method's reply: refused carries one of the line's fixed words, null when the action was taken */
function callResult(r) {
	r = r || {};
	return { ok: r.ok === true, refused: str(r.refused) || null };
}

/*
 * A greeting method's reply: the same ok/refused shape, with what the daemon also reported about the file - the bytes it
 * has staged, and the greeting's length once it is in place. The u8 a blobmsg carries arrives as 0 or 1, so `ok` is read
 * both ways here rather than in each caller.
 */
function greetingResult(r) {
	var out = callResult(r);

	out.ok = (r || {}).ok === 1 || (r || {}).ok === true;
	out.size = Math.max(0, num((r || {}).size) || 0);
	out.ms = Math.max(0, num((r || {}).ms) || 0);
	return out;
}

/* the call's own view in the page's field names; every number clamped, an unknown state kept as it came */
function callView(r) {
	return {
		available: true,
		state: str(r.state) || 'idle',
		origin: str(r.origin) || null,
		endpoint: str(r.endpoint) || null,
		numberPresent: r.number_present === true,
		withheld: r.caller_id_withheld === true,
		number: str(r.remote_number),
		durationSec: Math.max(0, num(r.call_duration_seconds) || 0),
		enabled: r.enabled === true,
		revision: Math.max(0, num(r.revision) || 0),
		restartEpoch: Math.max(0, num(r.restart_epoch) || 0),
		sequence: Math.max(0, num(r.sequence) || 0),
		dropCount: Math.max(0, num(r.drop_count) || 0),
		answerOwner: str(r.answer_owner) || 'none',
		held: r.held === true,
		waiting: r.waiting === true,
		mediaEngine: str(r.media_engine) === 'ready' ? 'ready' : 'idle',
		captureFrames: Math.max(0, num(r.media_capture_frames) || 0),
		playbackFrames: Math.max(0, num(r.media_playback_frames) || 0)
	};
}

/* ---- the installed SIP service (qmodem_sip): its own settings, methods and init script, not this package's ----------------- */

var SIP_CONF = 'qmodem_sip';
var SIP_USER_RE = /^[A-Za-z0-9._-]{1,63}$/;
var SIP_IFACE_RE = /^[A-Za-z0-9_.:-]{1,32}$/;
var SIP_HOST_RE = /^[A-Za-z0-9_.:-]{1,128}$/;
var SIP_PASS_RE = /^[\x21-\x7e]{8,127}$/;
var SIP_REALM_RE = /^\S{0,64}$/;

function sipSection(name) {
	return uci.get(SIP_CONF, name) != null;
}

function sipStr(section, option) {
	var v = uci.get(SIP_CONF, section, option);
	return v == null ? '' : String(v);
}

function sipBool(section, option, def) {
	var v = uci.get(SIP_CONF, section, option);
	return v == null ? def : String(v) === '1';
}

function sipInt(section, option, def) {
	var v = uci.get(SIP_CONF, section, option);
	return v != null && /^[0-9]+$/.test(String(v)) ? parseInt(v, 10) : def;
}

function portOk(v) {
	return typeof v === 'number' && v >= 1 && v <= 65535 && v === Math.floor(v);
}

/* the interfaces the network package knows, for the direction pickers: a name absent from this list is still accepted
 * by the service, so the field stays free text */
function readInterfaces() {
	uci.unload('network');
	return uci.load('network').then(function() {
		return uci.sections('network', 'interface').map(function(s) { return s['.name']; })
			.filter(function(n) { return SIP_IFACE_RE.test(n); }).sort();
	}, function() { return []; });
}

function readSipDirection(name, defPort) {
	return {
		enabled: sipBool(name, 'enabled', false),
		iface: sipStr(name, 'interface') || (name === 'inbound' ? 'lan' : 'wan'),
		listenPort: sipInt(name, 'listen_port', defPort),
		transport: sipStr(name, 'transport') || (name === 'inbound' ? 'udp_tcp' : 'tls'),
		username: sipStr(name, 'username'),
		passwordSet: sipStr(name, 'password') !== ''
	};
}

function readSipConfig() {
	var inbound = readSipDirection('inbound', 5060), outbound = readSipDirection('outbound', 5061);

	outbound.server = sipStr('outbound', 'server');
	outbound.port = sipInt('outbound', 'port', 5061);
	outbound.realm = sipStr('outbound', 'realm');
	outbound.interval = sipInt('outbound', 'register_interval', 300);
	return {
		inbound: inbound, outbound: outbound
	};
}

function normSipDir(d) {
	d = d || {};
	return { enabled: d.enabled === true, running: d.running === true, registered: d.registered === true };
}

function sipServiceStatus() {
	return sipSvcBus.service().then(function(r) {
		r = r || {};
		return {
			configured: r.configured === true, username: str(r.username),
			inbound: normSipDir(r.inbound), outbound: normSipDir(r.outbound),
			voipdAvailable: r.voipd_available === true
		};
	}, function() { return null; });
}

/* presence facts only: never a credential's content */
function sipDiag() {
	return sipSvcBus.diag().then(function(r) {
		r = r || {};
		return {
			config: r.config === true, script: r.script === true, mediaSocket: r.mediaSocket === true,
			credentials: r.credentials === true,
			inboundPid: Math.max(0, num(r.inboundPid) || 0), outboundPid: Math.max(0, num(r.outboundPid) || 0)
		};
	}, function() { return null; });
}

/* the same rules the installed service's own validator applies, checked here first so the admin sees the reason.
 * An unused direction keeps its stored values but nothing that would be rejected at the service's next start. */
function validateSipDirection(d, dir) {
	if (!d || typeof d !== 'object')
		return _('Invalid %s settings').format(dir);
	if (d.iface && !SIP_IFACE_RE.test(d.iface))
		return _('The %s interface name is not valid').format(dir);
	if (!portOk(d.listenPort))
		return _('The %s local port must be 1 to 65535').format(dir);
	if (d.username && !SIP_USER_RE.test(d.username))
		return _('The account name may contain letters, digits, dot, dash and underscore');
	if (typeof d.password === 'string' && d.password !== '' && !SIP_PASS_RE.test(d.password))
		return _('The password must be 8 to 127 printable characters without spaces');
	if (!d.enabled)
		return null;
	if (!d.iface)
		return _('Choose the %s interface').format(dir);
	if (!d.username)
		return _('Enter the account name');
	if (!d.passwordSet && !(typeof d.password === 'string' && SIP_PASS_RE.test(d.password)))
		return _('The password must be 8 to 127 printable characters without spaces');
	if (dir === 'inbound') {
		if (d.transport !== 'udp_tcp')
			return _('The inbound transport must be UDP and TCP');
	} else {
		if (d.transport !== 'tls')
			return _('The outbound transport must be TLS');
		if (!SIP_HOST_RE.test(d.server || ''))
			return _('Enter the SIP server address');
		if (!portOk(d.port))
			return _('The server port must be 1 to 65535');
		if (!SIP_REALM_RE.test(d.realm || ''))
			return _('The realm may not contain spaces');
		if (!(d.interval >= 60 && d.interval <= 3600))
			return _('The registration interval must be 60 to 3600 seconds');
	}
	return null;
}

function validateSipService(s) {
	if (!s || !s.inbound || !s.outbound)
		return _('Invalid SIP service settings');
	return validateSipDirection(s.inbound, 'inbound') || validateSipDirection(s.outbound, 'outbound');
}

function ensureSipSection(name, type) {
	if (uci.get(SIP_CONF, name) == null)
		uci.add(SIP_CONF, type, name);
}

function putSip(section, option, value) {
	if (value == null || value === '')
		uci.unset(SIP_CONF, section, option);
	else
		uci.set(SIP_CONF, section, option, String(value));
}

/* The options this page owns are the ones an admin can set from it: the direction's on/off word, its network and
 * port, its transport, and its sign-in name. A password is written only when a new one is typed - the stored value
 * is never read back into the page. What the service does with the voice and the text messages is its own default
 * (the firmware ships it), so voip_enabled, sms_enabled and sms_modem are not written from here. */
function writeSipDirection(name, d) {
	putSip(name, 'enabled', d.enabled ? '1' : '0');
	putSip(name, 'interface', d.iface);
	putSip(name, 'listen_port', String(d.listenPort));
	putSip(name, 'transport', d.transport);
	putSip(name, 'username', d.username);
	if (typeof d.password === 'string' && d.password !== '')
		putSip(name, 'password', d.password);
	if (name === 'outbound') {
		putSip(name, 'server', d.server);
		putSip(name, 'port', String(d.port));
		putSip(name, 'realm', d.realm);
		putSip(name, 'register_interval', String(d.interval));
	}
}

/* the words this package's own door answers with. A phone reads only a URL from a scanned code, so the
 * account is a short-lived file the web server hands out - every reason why that may not be possible is said
 * here, in the page's own words, not as a code. */
var SIP_PROVISION_WORDS = {
	NO_ENDPOINT: _('This router serves no plain address a phone can fetch, so no code can be made. The image\'s own web server configuration owns that listener.'),
	DIRECTION_OFF: _('Phones on this network are switched off. Switch them on first.'),
	NO_ACCOUNT: _('There is no account name for phones on this network yet.'),
	NO_PASSWORD: _('The stored password cannot be read back here. Type it once, or press "Create a new password", and then make the code.'),
	NO_ADDRESS: _('The network those phones use has no address right now.'),
	NOT_AVAILABLE: _('This router cannot make a code.'),
	HELPER_FAILED: _('The code could not be written on this router.')
};

var api = {
	available: Object.assign({ listRecordings: true, getRecording: true, deleteRecording: true,
		getRecordingSettings: true, saveRecordingSettings: true,
		listMessages: true, getMessage: true, deleteMessage: true,
		getGreeting: true, greetingBegin: true, greetingPut: true, greetingCommit: true, greetingAbort: true },
		provider.available || {}),
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

	/* ---- the answering machine's greeting: one file at one constant place, so no name travels ---- */

	/* one chunk of the greeting's own bytes, standard base64, from `offset`; "more" says the next chunk follows at
	 * offset + the chunk's raw length (the tab walks them, the way it walks a recording's) */
	getGreeting: function(offset) {
		if (!(offset >= 0))
			return Promise.reject(new Error(_('Not a greeting offset')));
		return greetBus.get(Math.floor(offset)).then(function(r) {
			r = r || {};
			return {
				total: Math.max(0, num(r.total) || 0), offset: Math.max(0, num(r.offset) || 0),
				b64: str(r.b64), more: r.more === 1 || r.more === true
			};
		}, function(e) { throw fail(e); });
	},

	/* open the staging of one upload; BUSY when another is still open, SYMLINK when the greeting is a link */
	greetingBegin: function() {
		return greetBus.begin().then(function(r) { return greetingResult(r); }, function(e) { throw fail(e); });
	},

	/* the next piece at exactly `offset`, base64 of at most the daemon's piece size; a gap or a repeat is OFFSET */
	greetingPut: function(offset, text) {
		if (!(offset >= 0) || typeof text !== 'string')
			return Promise.reject(new Error(_('The upload piece is not valid')));
		return greetBus.put(Math.floor(offset), text).then(function(r) { return greetingResult(r); }, function(e) { throw fail(e); });
	},

	/* prove the staged bytes and put them in the greeting's place. `polish` is 1 for a fresh recording: the daemon
	 * takes the silence off both ends and adds the tone; an uploaded file keeps the audio its author made. */
	greetingCommit: function(polish) {
		return greetBus.commit(polish ? 1 : 0).then(function(r) { return greetingResult(r); }, function(e) { throw fail(e); });
	},

	/* drop the staging; the page calls it after any failure, and it succeeds when nothing was open anyway */
	greetingAbort: function() {
		return greetBus.abort().then(function(r) { return greetingResult(r); }, function(e) { throw fail(e); });
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
	},

	/* the side a word names, for the cards that describe the line in the customer's own words: the words themselves
	 * are known here only, so a card never repeats the list and never shows a word it was handed */
	sipSide: sipSide,

	/* the other program's own start script, by name only: the diagnostics say which file the presence facts were
	 * read beside, and the page has one place that holds that name so a port says it once */
	getSipServiceScript: function() {
		return SIP_SVC;
	},

	getSipSettings: function() {
		uci.unload(CONF);
		return uci.load(CONF).then(function() {
			return sipSide(uci.get(CONF, 'sip', 'mode'));
		}, function(e) { throw fail(e); });
	},

	/* one of the two sides, written and applied; the switch's own door beside it is `saveSipSwitch` */
	saveSipSettings: writeSide,

	/* the line's own view of its call, read from the daemon itself, whatever the SIP mode; a daemon that is not
	 * running answers available:false instead of failing the poll */
	getCallStatus: function() {
		return phoneBus.call().then(function(r) {
			return callView(r || {});
		}, function(e) {
			if (objectGone(e))
				return { available: false };
			throw fail(e);
		});
	},

	/* the line's block of the health reply, the installed service's block and the voice port's owner beside it; a
	 * key this daemon does not publish stays null */
	getPhoneHealth: function() {
		return phoneBus.health().then(function(r) {
			var w = (r && r.web) || {}, s = (r && r.sip) || {};
			var at = (r && r.atPort) || {};
			return {
				mode: str(w.mode) || null, objectOwner: str(s.objectOwner) || null,
				portOwner: str(at.portOwner) || null, lastRefusal: str(s.lastRefusal) || null,
				audio: str(w.audio) || null, port: Math.max(0, num(w.port) || 0),
				address: str(w.address) || null
			};
		}, function(e) { throw fail(e); });
	},

	/* ---- the page's call control: the daemon's own methods, so a refusal keeps its word even on a failed call ---- */

	webOriginate: function(number) {
		number = typeof number === 'string' ? number.trim() : '';
		if (!DIAL_RE.test(number) || number === '*' || number === '#')
			return Promise.reject(new Error(_('Enter a number: digits, * or #, and a leading +')));
		return phoneBus.dial(number).then(callResult, function(e) { throw fail(e); });
	},

	webAnswer: function() {
		return phoneBus.answer().then(callResult, function(e) { throw fail(e); });
	},

	webReject: function() {
		return phoneBus.reject().then(callResult, function(e) { throw fail(e); });
	},

	webHangup: function() {
		return phoneBus.hangup().then(callResult, function(e) { throw fail(e); });
	},

	webDtmf: function(digit) {
		digit = typeof digit === 'string' ? digit : '';
		if (!DIGIT_RE.test(digit))
			return Promise.reject(new Error(_('Not a keypad digit')));
		return phoneBus.dtmf(digit).then(callResult, function(e) { throw fail(e); });
	},

	/* the hook flash the handset's flash key makes in a call: hold, swap, take the waiting call, take a held call
	 * back; a key within its window is a code */
	webFlash: function() {
		return phoneBus.flash().then(callResult, function(e) { throw fail(e); });
	},

	/* the page's own audio: a token for the call it saw, presented once over the encrypted stream. The refusal words
	 * are the line's own, so the card shows the same sentence a report from the field reads */
	webAudioOpen: function(revision) {
		revision = num(revision);
		if (!(revision > 0))
			return Promise.reject(new Error(_('There is no call to attach to')));
		return phoneBus.audio(revision).then(function(r) {
			r = r || {};
			var res = callResult(r);

			if (!res.ok)
				return res;
			res.token = str(r.token);
			res.port = Math.max(0, num(r.port) || 0);
			res.rate = Math.max(0, num(r.rate) || 0);
			res.frameMs = Math.max(0, num(r.frame_ms) || 0);
			res.expiresMs = Math.max(0, num(r.expires_ms) || 0);
			if (!res.token || !res.port || res.rate !== 8000 || res.frameMs !== 20)
				throw new Error(_('The audio grant is not a stream this page can open'));
			return res;
		}, function(e) { throw fail(e); });
	},

	/* ---- the installed SIP service (qmodem_sip): its own settings, its own methods, its own init script ------------ */

	/* the config, the service's status and the presence facts in one read; a missing config is a read-only card, not
	 * an error, and a stopped service leaves the status null rather than failing the card */
	getSipService: function() {
		return readInterfaces().then(function(ifaces) {
			uci.unload(SIP_CONF);
			return uci.load(SIP_CONF).then(function() { return true; }, function() { return false; })
				.then(function(present) {
					return Promise.all([
						sipServiceStatus(),
						sipDiag().catch(function() { return null; })
					]).then(function(r) {
						return {
							available: present && sipSection('main') != null,
							interfaces: ifaces,
							config: present ? readSipConfig() : null,
							service: r[0], diag: r[1]
						};
					});
				});
		});
	},

	/* writes the config the installed service reads; passwords travel only when the admin typed a new one and are
	 * never echoed back. The call answers {saved:true}; the caller decides whether to restart the service */
	saveSipService: function(s) {
		var msg = validateSipService(s);
		if (msg)
			return Promise.reject(new Error(msg));
		uci.unload(SIP_CONF);
		return uci.load(SIP_CONF).then(function() {
			ensureSipSection('inbound', 'direction');
			ensureSipSection('outbound', 'direction');
			writeSipDirection('inbound', s.inbound);
			writeSipDirection('outbound', s.outbound);
			return uci.save();
		}).then(function() {
			return recBus.commit(SIP_CONF);
		}).then(function() {
			return { saved: true };
		}, function(e) { throw e && e.message ? e : fail(e); });
	},

	/* the installed service's own init script, by its own name: the restart that makes a saved config live. Its own
	 * failure words stay here: the service card asks for a restart and wants the code back, while the switch below
	 * asks for a start or a stop and wants a state */
	applySipService: function() {
		return fs.exec(SIP_SVC, [ 'restart' ]).then(function(r) {
			if (r && r.code === 0)
				return { ok: true, code: 0 };
			throw new Error(_('The installed SIP service did not restart (%s)').format(String(r && r.code != null ? r.code : '?')));
		}, function(e) { throw sipSvcFail(e); });
	},

	/* ---- the switch's own side of the installed service ---------------------------------------------------------------
	 *
	 * The switch asks for a network phone, so the service that answers one runs while it is asked for and does not
	 * while it is not: a line switched off yet still listening on 5060 is neither what the admin asked nor what the router costs
	 * costs. The order is the whole of it. The side is written first, because the side is this package's own setting
	 * and the line has to follow it; the handover of the qmodem_voip name is then waited for, since the daemon takes
	 * and lets go of that name only at a moment when the line is quiet; and the service moves last, after the name it
	 * binds itself to is in the state the switch asks for. Going on, a service started before the name is this daemon's finds
	 * no one to talk to and every call lands with no audio - the service looks the name up once, at its own start,
	 * and never asks again. Going off, a service stopped while its leg is still in a call cuts that call under itself.
	 *
	 * The service is a program of its own: it may be absent, the account may not reach it, it may refuse. None of
	 * that changes what the switch says - the side is stored and the line follows it - so a result carries the side
	 * and what became of the service beside it, and the card tells the two apart.
	 */
	saveSipSwitch: moveSwitch,

	/* the switch asked for something the line could not hand over yet: the card calls this on its next read, and it
	 * takes only the action that is still missing - never a second one, and never one that cuts a live call */
	reconcileSipSwitch: reconcileSwitch,

	/* the installed service's own method: it generates, stores and returns the password once; the page never keeps it */
	generateSipCredentials: function(username) {
		username = typeof username === 'string' ? username.trim() : '';
		if (!SIP_USER_RE.test(username))
			return Promise.reject(new Error(_('The account name may contain letters, digits, dot, dash and underscore')));
		return sipSvcBus.genCred(username).then(function(r) {
			r = r || {};
			if (str(r.status) !== 'success' || !str(r.password))
				throw new Error(_('The installed service did not return a password'));
			return { username: str(r.username) || username, password: str(r.password), configured: r.configured === 1 || r.configured === true };
		}, function(e) { throw sipSvcFail(e); });
	}
,

	/* the one-time code a softphone scans: the URL of the account file this router writes, with the minute it
	 * stops being served. Nothing of the account travels through the page - only its address and its name. */
	sipProvision: function() {
		return sipSvcBus.provision().then(function(r) {
			r = r || {};
			if (r.ok !== true) {
				/* the writer's own word travels as a detail: the admin is told the code could not be written,
				   and the one word that says why travels beside it for whoever reads this back */
				var why = SIP_PROVISION_WORDS[str(r.reason)] || _('This router cannot make a code.');
				var detail = /^[A-Z_]{3,20}$/.test(str(r.detail)) ? ' (' + str(r.detail) + ')' : '';
				throw new Error(String(why) + detail);
			}
			if (!/^http:\/\/[^\s"]+$/.test(str(r.url)) || !isFinite(+r.expires) || +r.expires <= 0)
				throw new Error(_('The router answered with a code that cannot be read.'));
			return { url: str(r.url), expires: Math.floor(+r.expires), ttl: Math.floor(+r.ttl) || 0,
				username: str(r.username), realm: str(r.realm) };
		}, function(e) {
			if (objectGone(e))
				throw new Error(_('This router cannot make a code.'));
			if (/denied|permission|-32002/i.test(String((e && e.message) || e || '')))
				throw new Error(_('This account may not make a code.'));
			throw e && e.message ? e : fail(e);
		});
	}
};

METHODS.forEach(function(name) {
	if (typeof provider[name] !== 'function')
		throw new Error('Phone provider is missing ' + name + '()');
	api[name] = function() { return provider[name].apply(provider, arguments); };
});

return baseclass.extend(api);
