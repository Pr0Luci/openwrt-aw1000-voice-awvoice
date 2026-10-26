// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
'use strict';
'require baseclass';
'require view.awvoice.phone.live as provider';

/*
 * api.js - the one door between the phone page and its data. The tabs call only the methods below; they never read
 * UCI, ubus or files themselves. phone/live.js is the provider; the shapes and rules of every method are the data
 * contract v2 in DESIGN.md section 2.
 *
 * Every method returns a Promise. Failures reject with an Error whose .message is safe to show to the user. A method
 * whose backend is not built yet rejects with "Not available yet", and api.available[name] is false so that the page
 * can show its controls disabled instead of calling it.
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

var api = {
	available: provider.available || {},
	settingsLive: provider.settingsLive || {},
	downStatus: function() { return provider.downStatus(); }
};

METHODS.forEach(function(name) {
	if (typeof provider[name] !== 'function')
		throw new Error('Phone provider is missing ' + name + '()');
	api[name] = function() { return provider[name].apply(provider, arguments); };
});

return baseclass.extend(api);
