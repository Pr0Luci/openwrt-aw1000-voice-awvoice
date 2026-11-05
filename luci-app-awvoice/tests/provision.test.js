#!/usr/bin/env node
/* the one-time provisioning file: its bytes, its escaping, its refusals and its pruning.
   The script under test is the one the router runs, invoked here with a temporary web root. */
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const cp = require('child_process');

function assert(cond, msg) {
	if (!cond) {
		process.stderr.write('FAIL: ' + msg + '\n');
		process.exit(1);
	}
}

const SCRIPT = path.join(__dirname, '..', 'root', 'usr', 'libexec', 'awvoice-provision');
const OUT = fs.mkdtempSync(path.join(os.tmpdir(), 'awvoice-provision-'));
const TOKEN = '0123456789abcdef0123456789abcdef';

function run(args) {
	const r = cp.spawnSync('/bin/sh', [SCRIPT].concat(args), { encoding: 'utf8' });
	if (r.error)
		throw r.error;
	return { out: (r.stdout || '').trim(), code: r.status, err: (r.stderr || '').trim() };
}

function build(extra) {
	return run(['build', '--out', OUT].concat(extra || [
		'--user', 'awvoice', '--passwd', 'test1234', '--host', '192.168.1.1', '--port', '5060',
		'--realm', 'qmodem-voip', '--token', TOKEN, '--now', '1700000000', '--ttl', '600'
	]));
}

function reason(r) {
	return JSON.parse(r.out).reason;
}

fs.readdirSync(OUT).forEach((f) => fs.unlinkSync(path.join(OUT, f)));

/* the file, byte for byte, for one known account */
let r = build();
assert(r.code === 0, 'a plain account refused: ' + r.out + ' ' + r.err);
let j = JSON.parse(r.out);
const FILE = path.join(OUT, 'awvoice-' + TOKEN + '.xml');
const WANT = [
	'<?xml version="1.0" encoding="UTF-8"?>',
	'<config xmlns="http://www.linphone.org/xsds/lpconfig.xsd" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:schemaLocation="http://www.linphone.org/xsds/lpconfig.xsd lpconfig.xsd">',
	'  <section name="auth_info_0">',
	'    <entry name="username" overwrite="true">awvoice</entry>',
	'    <entry name="userid" overwrite="true">awvoice</entry>',
	'    <entry name="passwd" overwrite="true">test1234</entry>',
	'    <entry name="realm" overwrite="true">qmodem-voip</entry>',
	'  </section>',
	'  <section name="proxy_0">',
	'    <entry name="reg_proxy" overwrite="true">sip:192.168.1.1:5060;transport=udp</entry>',
	'    <entry name="reg_identity" overwrite="true">sip:awvoice@192.168.1.1:5060</entry>',
	'    <entry name="reg_expires" overwrite="true">300</entry>',
	'    <entry name="reg_sendregister" overwrite="true">1</entry>',
	'    <entry name="publish" overwrite="true">0</entry>',
	'    <entry name="dial_escape_plus" overwrite="true">0</entry>',
	'  </section>',
	'  <section name="sip">',
	'    <entry name="default_proxy" overwrite="true">0</entry>',
	'  </section>',
	'</config>',
	''
].join('\n');
assert(fs.readFileSync(FILE, 'utf8') === WANT, 'the account file is not the lpconfig the phone expects');
const PROVISION_PORT = 8080;
const PROVISION_PATH = '';
assert(j.url === 'http://192.168.1.1:' + PROVISION_PORT + PROVISION_PATH + '/awvoice-' + TOKEN + '.xml',
	'the URL is not the file the phone must fetch: ' + j.url);
assert(j.expires === 1700000600 && j.ttl === 600, 'the expiry is not now plus the lifetime');
assert(j.url.indexOf('test1234') < 0 && r.out.indexOf('test1234') < 0, 'the password travelled into the answer');
assert((fs.statSync(FILE).mode & 0o777) === 0o644, 'the file is not world-readable and owner-writable');
assert(!fs.existsSync(FILE + '.tmp'), 'a half-written file was left behind');

/* a password of XML markup survives as text, not as structure */
r = build(['--out', OUT, '--user', 'a.b', '--passwd', 'p&ss<w>"w\'', '--host', '10.0.0.1', '--port', '5060',
	'--realm', 'qmodem-voip', '--token', TOKEN, '--now', '1700000000', '--ttl', '600']);
assert(r.code === 0, 'a password of markup refused: ' + r.out);
let body = fs.readFileSync(FILE, 'utf8');
assert(body.indexOf('<entry name="passwd" overwrite="true">p&amp;ss&lt;w&gt;&quot;w&apos;</entry>') >= 0,
	'the password was not escaped: ' + body.split('\n').find((l) => l.includes('passwd')));
assert(body.indexOf('p&ss<w>') < 0, 'raw markup reached the document');

/* and a token nobody supplied: the name is random, 32 hex digits, and the file lands under it */
fs.readdirSync(OUT).forEach((f) => fs.unlinkSync(path.join(OUT, f)));
r = run(['build', '--out', OUT, '--user', 'awvoice', '--passwd', 'test1234', '--host', '192.168.1.1',
	'--port', '5060', '--realm', 'qmodem-voip', '--ttl', '600']);
j = JSON.parse(r.out);
assert(j.ok === true, 'a code without a given name refused: ' + r.out);
const names = fs.readdirSync(OUT).filter((f) => f.startsWith('awvoice-') && f.endsWith('.xml'));
assert(names.length === 1, 'the writer left more than one file: ' + names.join(','));
assert(/^awvoice-[0-9a-f]{32}\.xml$/.test(names[0]), 'the name is not a random hex token: ' + names[0]);
assert(j.url.endsWith('/' + names[0]), 'the URL does not name the file it wrote: ' + j.url);
assert(j.file.indexOf('test1234') < 0 && r.out.indexOf('test1234') < 0, 'the password travelled into the answer');

/* the refusals, each by its own name */
[
	['--user', 'a b', 'BAD_USER'],
	['--user', '', 'BAD_USER'],
	['--user', 'a/b', 'BAD_USER'],
	['--realm', 'two words', 'BAD_REALM'],
	['--host', '1.1; rm', 'BAD_HOST'],
	['--port', '0', 'BAD_PORT'],
	['--port', '70000', 'BAD_PORT'],
	['--port', 'abc', 'BAD_PORT'],
	['--passwd', 'short7', 'PASSWD_SHORT'],
	['--passwd', new Array(129).join('x'), 'PASSWD_LONG'],
	['--passwd', ' lead', 'BAD_PASSWD'],
	['--ttl', '30', 'TTL_SHORT'],
	['--ttl', 'abc', 'BAD_TTL'],
	['--token', 'XYZ', 'BAD_TOKEN']
].forEach(function(c) {
	const args = ['build', '--out', OUT, '--user', 'awvoice', '--passwd', 'test1234', '--host', '192.168.1.1',
		'--port', '5060', '--realm', 'qmodem-voip', '--token', TOKEN, '--now', '1700000000', '--ttl', '600'];
	for (let i = 0; i < c.length - 1; i += 2) {
		const at = args.indexOf(c[i]);
		if (at < 0)
			throw new Error('option missing from the harness: ' + c[i]);
		args[at + 1] = c[i + 1];
	}
	const got = run(args);
	assert(reason(got) === c[c.length - 1], c[c.length - 1] + ' not refused as itself: ' + got.out);
});

/* a host so long the code would exceed what a phone's decoder reads */
r = run(['build', '--out', OUT, '--user', 'awvoice', '--passwd', 'test1234', '--host', new Array(250).join('h'),
	'--port', '5060', '--realm', 'qmodem-voip', '--token', TOKEN, '--ttl', '600']);
assert(reason(r) === 'URL_LONG', 'a code longer than the decoder reads was accepted: ' + r.out);

/* no command, and an unknown one */
assert(reason(run([])) === 'NO_COMMAND', 'a bare run did something');
assert(reason(run(['frobnicate'])) === 'NO_COMMAND', 'an unknown command did something');
assert(reason(run(['build', '--sideways'])) === 'BAD_ARG', 'an unknown option was tolerated');

/* pruning: an expired file goes, a live one stays, and only the names this writer owns are touched */
fs.readdirSync(OUT).forEach((f) => fs.unlinkSync(path.join(OUT, f)));
const old = path.join(OUT, 'awvoice-' + 'f'.repeat(32) + '.xml');
const fresh = path.join(OUT, 'awvoice-' + TOKEN + '.xml');
const other = path.join(OUT, 'foreign.xml');
[old, fresh, other].forEach((f) => fs.writeFileSync(f, '<config/>\n'));
const twoHours = new Date(Date.now() - 2 * 3600 * 1000);
fs.utimesSync(old, twoHours, twoHours);
r = run(['prune', '--out', OUT, '--now', String(Math.floor(Date.now() / 1000)), '--ttl', '600']);
assert(r.code === 0 && JSON.parse(r.out).ok === true, 'prune refused: ' + r.out);
assert(!fs.existsSync(old), 'an expired credential stayed on the server');
assert(fs.existsSync(fresh), 'a file written now was pruned');
assert(fs.existsSync(other), 'pruning touched a foreign file');

fs.readdirSync(OUT).forEach((f) => fs.unlinkSync(path.join(OUT, f)));
fs.rmdirSync(OUT);
process.stdout.write('awvoice-provision: all tests passed\n');

function assert(cond, msg) {
	if (!cond) {
		process.stderr.write('FAIL: ' + msg + '\n');
		process.exit(1);
	}
}
