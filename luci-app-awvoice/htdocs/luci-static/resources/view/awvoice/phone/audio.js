// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
'use strict';
'require baseclass';

/*
 * audio.js - the page's browser audio: the microphone's voice to the daemon and the call's voice to the speaker, over
 * one encrypted WebSocket (RFC 6455) carrying the 12-byte-frame stream of web_proto.h.
 *
 * start() opens the stream, presents the token the daemon minted for the call, and - once the daemon grants the ring -
 * asks for the microphone and runs two AudioWorklet processors: one captures 20 ms / 8 kHz frames (a low-pass and a
 * decimation when the audio context is not already at 8 kHz), the other plays the daemon's frames through a small
 * jitter queue. Mute stops the uplink only; stop() says BYE, closes the stream and gives the microphone back.
 *
 * A failure is a state, not an exception the page has to catch: start() resolves { ok, refused, reason, engine }, where
 * refused is the daemon's word (refused-busy, refused-nocall, refused-revision, refused-mode) and reason is a page-level
 * sentence (no microphone, no secure context, the stream dropped).
 */

var HDR = 12, MAGIC = 0x41, VERSION = 1;
var T_HELLO = 1, T_GRANT = 2, T_PCM = 3, T_MUTE = 4, T_BYE = 5;
var PCM_BYTES = 320;

function frame(type, seq, ts, payload) {
	var out = new Uint8Array(HDR + (payload ? payload.length : 0));
	var dv = new DataView(out.buffer);

	out[0] = MAGIC;
	out[1] = VERSION;
	out[2] = type;
	out[3] = 0;
	dv.setUint32(4, seq >>> 0, false);
	dv.setUint32(8, ts >>> 0, false);
	if (payload)
		out.set(payload, HDR);
	return out;
}

function ascii(text) {
	var out = new Uint8Array(text.length), k;

	for (k = 0; k < text.length; k++)
		out[k] = text.charCodeAt(k) & 0x7f;
	return out;
}

function word(bytes) {
	var s = '', k;

	for (k = 0; k < bytes.length; k++)
		s += String.fromCharCode(bytes[k]);
	return s;
}

return baseclass.extend({
	/* the browser can offer this page's audio at all: a secure page, a microphone and the worklet API */
	supported: function() {
		return !!(window.isSecureContext && (window.AudioContext || window.webkitAudioContext) &&
			navigator.mediaDevices && navigator.mediaDevices.getUserMedia &&
			window.AudioWorkletNode && window.WebSocket);
	},

	/*
	 * opts: { token, port, onEvent(ev) }, ev = { type: 'granted'|'refused'|'live'|'level'|'closed'|'stopped',
	 * word?, level?, reason? }. Resolves once the grant and the microphone are settled; the stream runs until stop()
	 * or the daemon closes it.
	 */
	start: function(opts) {
		var ws = null, ctx = null, stream = null, source = null, capture = null, player = null, sink = null;
		var seq = 0, done = false, timer = null;
		var engine = { live: false, muted: false, framesIn: 0, framesOut: 0, stop: stop, mute: mute };

		function emit(ev) {
			if (opts.onEvent)
				opts.onEvent(ev);
		}

		/* every resource goes: the stream, the microphone, the context and the processors */
		function cleanup() {
			if (ws) {
				try {
					if (ws.readyState === 1)
						ws.send(frame(T_BYE, seq, seq * 20, ascii('bye')));
					ws.close();
				} catch (e) { /* already gone */ }
				ws = null;
			}
			engine.live = false;
			if (stream) {
				stream.getTracks().forEach(function(t) { t.stop(); });
				stream = null;
			}
			if (ctx) {
				try { ctx.close(); } catch (e) { /* already closed */ }
				ctx = null;
			}
			capture = player = source = sink = null;
		}

		function finish(res) {
			if (done)
				return;
			done = true;
			if (timer) {
				window.clearTimeout(timer);
				timer = null;
			}
			if (!res.ok)
				cleanup();
			res.engine = engine;
			resolve(res);
		}

		var resolve = null;

		function beginAudio() {
			/* the browser's own gain control is asked off and echo/noise handling kept: on a live call the gain
			   control lifted a minus 20 dBFS test tone to minus 0.2 dBFS, and the send level belongs to
			   audio-level.js, which holds the wire under minus 3 dBFS instead */
			return navigator.mediaDevices.getUserMedia({ audio: {
				echoCancellation: true, noiseSuppression: true, autoGainControl: false, channelCount: 1
			} }).then(function(s) {
				stream = s;
				/* the engine's own rate is kept: the worklet resamples when it is not 8000, so the browser's echoing
				 * and noise handling of the chosen microphone stay in charge */
				ctx = new window.AudioContext();
				return ctx.audioWorklet.addModule(L.resource('view/awvoice/phone/audio-level.js'));
			}).then(function() {
				return ctx.audioWorklet.addModule(L.resource('view/awvoice/phone/audio-worklet.js'));
			}).then(function() {
				source = ctx.createMediaStreamSource(stream);
				capture = new window.AudioWorkletNode(ctx, 'awvoice-capture');
				sink = ctx.createGain();
				sink.gain.value = 0;
				capture.connect(sink);
				sink.connect(ctx.destination);
				source.connect(capture);
				capture.port.onmessage = function(e) {
					var d = e.data || {};

					if (typeof d.level === 'number')
						emit({ type: 'level', level: d.level });
					if (engine.muted || !engine.live || !ws || ws.readyState !== 1)
						return;
					if (d.pcm && d.pcm.byteLength === PCM_BYTES) {
						engine.framesOut++;
						ws.send(frame(T_PCM, seq, seq * 20, new Uint8Array(d.pcm)));
						seq++;
					}
				};
				player = new window.AudioWorkletNode(ctx, 'awvoice-player');
				player.connect(ctx.destination);
				engine.live = true;
				emit({ type: 'live' });
				return ctx.resume().catch(function() { /* the click already allowed it */ });
			});
		}

		function stop() {
			var wasDone = done;

			if (!done) {
				done = true;
				if (timer) {
					window.clearTimeout(timer);
					timer = null;
				}
			}
			cleanup();
			if (!wasDone && resolve)
				resolve({ ok: false, refused: null, reason: null, engine: engine });
			emit({ type: 'stopped' });
		}

		function mute(on) {
			engine.muted = !!on;
			if (ws && ws.readyState === 1 && !done) {
				var b = new Uint8Array(1);

				b[0] = on ? 1 : 0;
				try { ws.send(frame(T_MUTE, seq, seq * 20, b)); } catch (e) { /* the close path handles it */ }
			}
			engine.muted = !!on;
			return engine.muted;
		}

		function onMessage(msg) {
			var buf = msg.data, dv, type, payload, verdict;

			if (!(buf instanceof ArrayBuffer) || buf.byteLength < HDR)
				return;
			dv = new DataView(buf);
			if (dv.getUint8(0) !== MAGIC || dv.getUint8(1) !== VERSION)
				return;
			type = dv.getUint8(2);
			payload = new Uint8Array(buf, HDR);
			if (type === T_GRANT) {
				verdict = word(payload);
				if (verdict !== 'granted') {
					emit({ type: 'refused', word: verdict });
					finish({ ok: false, refused: verdict, reason: null });
					return;
				}
				emit({ type: 'granted' });
				if (timer) {
					window.clearTimeout(timer);
					timer = null;
				}
				beginAudio().then(function() {
					engine.live = true;
					finish({ ok: true, refused: null, reason: null });
				}, function(e) {
					var reason = e && e.name === 'NotAllowedError'
						? _('The microphone was not allowed for this page')
						: e && e.message ? e.message : _('The microphone could not be opened');

					done = true;
					cleanup();
					resolve({ ok: false, refused: 'no-microphone', reason: reason, engine: engine });
				});
				return;
			}
			if (type === T_PCM && engine.live && player && payload.length === PCM_BYTES) {
				var copy = payload.slice();

				engine.framesIn++;
				player.port.postMessage(copy.buffer, [ copy.buffer ]);
			}
		}

		return new Promise(function(ok) {
			/* the daemon publishes the address it bound: it is the one the listener answers on */
			var host = opts.host || window.location.hostname;
			var url = 'wss://' + host + ':' + opts.port + '/audio';

			resolve = ok;
			try {
				ws = new window.WebSocket(url);
			} catch (e) {
				finish({ ok: false, refused: null, reason: e && e.message ? e.message : _('The audio stream could not be opened') });
				return;
			}
			ws.binaryType = 'arraybuffer';
			ws.onopen = function() {
				ws.send(frame(T_HELLO, 0, 0, ascii(opts.token)));
			};
			ws.onmessage = onMessage;
			ws.onerror = function() {
				finish({ ok: false, refused: null, reason: _('The secure audio stream could not be opened. If the browser warns about the router\'s certificate on this port, open https://%s:%d/ once and accept it.').format(host, opts.port) });
			};
			ws.onclose = function() {
				if (!done) {
					finish({ ok: false, refused: null, reason: _('The audio stream closed before it was granted') });
					return;
				}
				engine.live = false;
				emit({ type: 'closed', reason: _('The audio stream ended') });
			};
			timer = window.setTimeout(function() {
				finish({ ok: false, refused: null, reason: _('The audio stream did not answer') });
			}, 10000);
		});
	}
});
