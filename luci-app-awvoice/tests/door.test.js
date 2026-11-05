#!/usr/bin/env node
/* the door itself: the rpcd plugin's provision method, run with a fabricated uhttpd, qmodem_sip and network
   underneath it. Every way it can answer is answered here, and the account file writer is a stand-in that
   records the arguments it was handed. */
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const cp = require('child_process');

const PKG = path.join(__dirname, '..');
const PLUGIN = path.join(PKG, 'root', 'usr', 'libexec', 'rpcd', 'luci.awvoice');
const BOX = fs.mkdtempSync(path.join(os.tmpdir(), 'awvoice-door-'));
const BIN = path.join(BOX, 'bin');
const STATE = path.join(BOX, 'state');
fs.mkdirSync(BIN);

function assert(cond, msg) {
	if (!cond) {
		process.stderr.write('FAIL: ' + msg + '\n');
		process.exit(1);
	}
}

function write(p, mode, text) {
	fs.writeFileSync(p, text, { mode });
	fs.chmodSync(p, mode);
}

/* ---- the fakes: one state file in uci's own shape, three commands ---- */
write(path.join(BIN, 'uci'), 0o755, [
	'#!/bin/sh',
	'[ "$1" = -q ] && shift',
	'cmd=$1; shift',
	'case "$cmd" in',
	'get)	sed -n "s/^$1=//p" "$AWVOICE_STATE" | head -1 ;;',
	'show)	grep "^$1\\." "$AWVOICE_STATE" ;;',
	'esac',
	'exit 0',
	''
].join('\n'));

write(path.join(BIN, 'ubus'), 0o755, [
	'#!/bin/sh',
	'echo ok',
	''
].join('\n'));

/* jsonfilter is asked for one thing here - the device a logical interface is carried on - so the fake
   answers that from the state and nothing else */
write(path.join(BIN, 'jsonfilter'), 0o755, [
	'#!/bin/sh',
	'cat > /dev/null',
	'case "$*" in',
	'*l3_device*)	sed -n "s/^device=//p" "$AWVOICE_STATE" | head -1 ;;',
	'esac',
	''
].join('\n'));

/* the kernel's answer, from the state: an interface with no address answers with no line */
write(path.join(BIN, 'ip'), 0o755, [
	'#!/bin/sh',
	'addr=$(sed -n "s/^address=//p" "$AWVOICE_STATE" | head -1)',
	'dev=$6',
	'[ -n "$addr" ] || exit 0',
	'printf \'10: %s    inet %s/24 brd 192.168.1.255 scope global %s\\\n\' "$dev" "$addr" "$dev"',
	''
].join('\n'));

/* the account file writer, as a recorder: it answers with fixed JSON and leaves its arguments behind */
write(path.join(BIN, 'awvoice-provision'), 0o755, [
	'#!/bin/sh',
	'printf "%s\\n" "$*" > "$AWVOICE_STATE.args"',
	'printf "%s\\n" "$@" > "$AWVOICE_STATE.argv"',
	'printf \'{"ok":true,"file":"/tmp/x.xml","url":"http://192.168.1.1:8080/awvoice-t.xml","expires":1700000600,"ttl":600,"username":"awvoice","realm":"qmodem-voip"}\\n\'',
	''
].join('\n'));

function state(lines) {
	fs.writeFileSync(STATE, lines.join('\n') + '\n');
}

function ask() {
	const env = Object.assign({}, process.env, {
		AWVOICE_STATE: STATE,
		PATH: BIN + ':/bin:/usr/bin',
		PROV_HELPER: path.join(BIN, 'awvoice-provision')
	});
	/* the plugin reads the helper's path from its own file; the test points it at the recorder by editing the copy */
	const run = path.join(BOX, 'luci.awvoice');
	let text = fs.readFileSync(PLUGIN, 'utf8').replace('PROV_HELPER=/usr/libexec/awvoice-provision',
		'PROV_HELPER=' + path.join(BIN, 'awvoice-provision'));
	fs.writeFileSync(run, text, { mode: 0o755 });
	const r = cp.spawnSync('/bin/sh', [run, 'call', 'provision'], { encoding: 'utf8', env, input: '' });
	if (r.error)
		throw r.error;
	return JSON.parse((r.stdout || '').trim());
}

const GOOD = [
	'uhttpd.main=uhttpd',
	'uhttpd.main.listen_http=0.0.0.0:80',
	'uhttpd.main.redirect_https=1',
	'uhttpd.provision=uhttpd',
	'uhttpd.provision.listen_http=0.0.0.0:8080',
	'uhttpd.provision.redirect_https=0',
	'qmodem_sip.inbound.enabled=1',
	'qmodem_sip.inbound.username=awvoice',
	'qmodem_sip.inbound.password=test1234',
	'qmodem_sip.inbound.listen_port=5060',
	'qmodem_sip.inbound.interface=lan',
	'device=br-lan',
	'address=192.168.1.1'
];

/* the happy path: the writer is called with the account the installed service holds, not with anything the page sent */
let j = ask(state(GOOD));
assert(j.ok === true && j.url.indexOf(':8080/awvoice-') > 0, 'the good path did not answer with a code: ' + JSON.stringify(j));
let args = fs.readFileSync(STATE + '.args', 'utf8');
assert(/--user awvoice/.test(args) && /--passwd test1234/.test(args), 'the writer did not get the account: ' + args);
assert(/--host 192\.168\.1\.1/.test(args) && /--port 5060/.test(args), 'the writer got the wrong address or port: ' + args);
assert(/--webport 8080/.test(args) && /--realm qmodem-voip/.test(args), 'the writer got the wrong listener or realm: ' + args);
const argv = fs.readFileSync(STATE + '.argv', 'utf8').split('\n');
const at = argv.indexOf('--path');
assert(at >= 0 && argv[at + 1] === '', 'the writer was told to serve under a path, not at the root of its own directory: ' + argv.join('|'));
assert(/--out \/www\/awvoice-provision/.test(args), 'the writer was pointed at the general web root: ' + args);
assert(args.indexOf('test1234') === args.lastIndexOf('test1234'), 'the password was handed over twice');

/* every refusal, by its own name */
[
	['NO_ENDPOINT', (l) => !/^uhttpd\.provision\./.test(l)],
	['DIRECTION_OFF', (l) => l.replace('inbound.enabled=1', 'inbound.enabled=0')],
	['NO_ACCOUNT', (l) => l.replace('inbound.username=awvoice', 'inbound.username=')],
	['NO_PASSWORD', (l) => l.replace('inbound.password=test1234', 'inbound.password=')],
	['NO_ADDRESS', (l) => l.replace('address=192.168.1.1', 'address=')]
].forEach(function(c) {
	j = ask(state(GOOD.filter(Boolean).map(c[1]).filter(Boolean)));
	assert(j.ok === false && j.reason === c[0], c[0] + ' answered as ' + JSON.stringify(j));
});

/* an interface name that is not a name at all is refused before the network is asked */
j = ask(state(GOOD.map((l) => l.replace('interface=lan', 'interface=lan;rm'))));
assert(j.reason === 'NO_ADDRESS', 'a hostile interface name was trusted: ' + JSON.stringify(j));

/* no writer on the box, and a writer that answers with nothing */
const moved = path.join(BIN, 'awvoice-provision');
const saved = fs.readFileSync(moved, 'utf8');

/* a writer that refuses keeps its own word as a detail, and the page is handed one of the door's words */
write(moved, 0o755, '#!/bin/sh\nprintf \'{"ok":false,"reason":"BAD_HOST"}\\n\'\n');
j = ask(state(GOOD));
assert(j.reason === 'HELPER_FAILED' && j.detail === 'BAD_HOST', 'a writer refusal was not wrapped: ' + JSON.stringify(j));
write(moved, 0o755, saved);

fs.unlinkSync(moved);
j = ask(state(GOOD));
assert(j.reason === 'NOT_AVAILABLE', 'a missing writer was not named: ' + JSON.stringify(j));
write(moved, 0o755, '#!/bin/sh\necho "not json at all"\n');
j = ask(state(GOOD));
assert(j.reason === 'HELPER_FAILED', 'a writer that answers nonsense was believed: ' + JSON.stringify(j));
write(moved, 0o755, saved);

/* the door is advertised, and only as a write */
const acl = JSON.parse(fs.readFileSync(path.join(PKG, 'root', 'usr', 'share', 'rpcd', 'acl.d',
	'luci-app-awvoice.json'), 'utf8'))['luci-app-awvoice'];
assert(acl.write.ubus['luci.awvoice'].indexOf('provision') >= 0, 'the door is not granted for writing');
assert((acl.read.ubus['luci.awvoice'] || []).indexOf('provision') < 0, 'the write door is granted for reading');

fs.rmSync(BOX, { recursive: true, force: true });
process.stdout.write('luci.awvoice door: all tests passed\n');
