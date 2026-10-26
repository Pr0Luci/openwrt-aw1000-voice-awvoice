// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/* The parsers of the modem's idle answers, with reply lines captured on the router: make -f Makefile.host test */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "modemq.h"

int main(void)
{
	char net[MQ_NETWORK_MAX], band[MQ_NETWORK_MAX];

	/* SIM */
	assert(modemq_sim_ready("AT+CPIN?\r\r\n+CPIN: READY\r\n\r\nOK\r\n") == 1);
	assert(modemq_sim_ready("\r\n+CPIN: SIM PIN\r\n\r\nOK\r\n") == 0);
	assert(modemq_sim_ready("\r\n+CME ERROR: 10\r\n") == MQ_UNKNOWN);
	assert(modemq_sim_ready("") == MQ_UNKNOWN);

	/* registration: the query form <n>,<stat>, the report form <stat>; 1 home and 5 roaming count, 0 2 3 4 do not */
	assert(modemq_registered("AT+CEREG?\r\r\n+CEREG: 0,1\r\n\r\nOK\r\n") == 1);
	assert(modemq_registered("+CEREG: 2,5,\"D7A9\",\"0123ABCD\",7\r\nOK\r\n") == 1);
	assert(modemq_registered("+CEREG: 0,0\r\nOK\r\n") == 0);
	assert(modemq_registered("+CEREG: 0,2\r\nOK\r\n") == 0);
	assert(modemq_registered("+CEREG: 0,3\r\nOK\r\n") == 0);
	assert(modemq_registered("+CEREG: 1\r\n") == 1);
	assert(modemq_registered("+CEREG: 0\r\n") == 0);
	assert(modemq_registered("+CEREG: x,y\r\n") == MQ_UNKNOWN);
	assert(modemq_registered("ERROR\r\n") == MQ_UNKNOWN);
	assert(modemq_registered("+CEREG: 0,12\r\n") == MQ_UNKNOWN);

	/* IMS: the second field of the modem's own form */
	assert(modemq_ims("AT+QCFG=\"ims\"\r\r\n+QCFG: \"ims\",0,1\r\n\r\nOK\r\n") == 1);
	assert(modemq_ims("+QCFG: \"ims\",1,0\r\nOK\r\n") == 0);
	assert(modemq_ims("+QCFG: \"ims\",0,7\r\nOK\r\n") == MQ_UNKNOWN);
	assert(modemq_ims("+QCFG: \"ims\",0\r\nOK\r\n") == MQ_UNKNOWN);
	assert(modemq_ims("ERROR\r\n") == MQ_UNKNOWN);
	assert(modemq_ims("+QCFG: \"volte_disable\",0\r\n") == MQ_UNKNOWN);

	/* signal: the reference signal power is the second value */
	assert(modemq_rsrp_dbm("AT+QCSQ\r\r\n+QCSQ: \"LTE\",-62,-93,8,-11\r\n\r\nOK\r\n") == -93);
	assert(modemq_rsrp_dbm("+QCSQ: \"NR5G\",-70,-101,12,-12\r\nOK\r\n") == -101);
	assert(modemq_rsrp_dbm("+QCSQ: \"NOSERVICE\"\r\nOK\r\n") == MQ_UNKNOWN);
	assert(modemq_rsrp_dbm("+QCSQ: \"GSM\",-70\r\nOK\r\n") == MQ_UNKNOWN);
	assert(modemq_rsrp_dbm("+QCSQ: \"LTE\",-62\r\n") == MQ_UNKNOWN);
	assert(modemq_rsrp_dbm("+QCSQ: \"LTE\",-62,200,8,-11\r\n") == MQ_UNKNOWN);
	assert(modemq_rsrp_dbm("ERROR\r\n") == MQ_UNKNOWN);

	/* access technology for display and the band text */
	modemq_network("AT+QNWINFO\r\r\n+QNWINFO: \"FDD LTE\",\"41302\",\"LTE BAND 3\",1725\r\n\r\nOK\r\n", net, sizeof(net), band, sizeof(band));
	assert(!strcmp(net, "4G") && !strcmp(band, "LTE BAND 3"));
	modemq_network("+QNWINFO: \"NR5G-SA\",\"41302\",\"NR5G BAND 78\",632640\r\n", net, sizeof(net), band, sizeof(band));
	assert(!strcmp(net, "5G") && !strcmp(band, "NR5G BAND 78"));
	modemq_network("+QNWINFO: \"WCDMA\",\"41302\",\"WCDMA 2100\",10700\r\n", net, sizeof(net), band, sizeof(band));
	assert(!strcmp(net, "3G") && !strcmp(band, "WCDMA 2100"));
	modemq_network("+QNWINFO: \"EDGE\",\"41302\",\"GSM 900\",50\r\n", net, sizeof(net), band, sizeof(band));
	assert(!strcmp(net, "2G"));
	modemq_network("+QNWINFO: \"No Service\"\r\nOK\r\n", net, sizeof(net), band, sizeof(band));
	assert(!net[0] && !band[0]);
	modemq_network("ERROR\r\n", net, sizeof(net), band, sizeof(band));
	assert(!net[0] && !band[0]);
	modemq_network("+QNWINFO: \"FDD LTE\",\"41302\",\"A VERY LONG BAND TEXT THAT DOES NOT FIT\",1\r\n", net, sizeof(net), band, 8);
	assert(!strcmp(net, "4G") && !band[0]);		/* too long for the caller's buffer: dropped, not cut */

	/* PCM mode: the second field, whatever the first prints */
	assert(modemq_audio_slave("AT+QDAI?\r\r\n+QDAI: x,1,0,4,0,0,1,1\r\n\r\nOK\r\n") == 1);
	assert(modemq_audio_slave("+QDAI: 1,0,0,4,0,0,1,1\r\nOK\r\n") == 0);
	assert(modemq_audio_slave("+QDAI: 1,3,0,4\r\n") == MQ_UNKNOWN);
	assert(modemq_audio_slave("+QDAI: 1\r\n") == MQ_UNKNOWN);
	assert(modemq_audio_slave("ERROR\r\n") == MQ_UNKNOWN);

	/* the range of a setting from its test command */
	{
		int lo = -1, hi = -1;

		assert(modemq_range("AT+CLVL=?\r\r\n+CLVL: (0-5)\r\n\r\nOK\r\n", "+CLVL:", &lo, &hi) && lo == 0 && hi == 5);
		assert(modemq_range("+CLVL: (1-100)\r\n", "+CLVL:", &lo, &hi) && lo == 1 && hi == 100);
		assert(!modemq_range("+CLVL: (5-0)\r\n", "+CLVL:", &lo, &hi));
		assert(!modemq_range("+CLVL: (0,5)\r\n", "+CLVL:", &lo, &hi));
		assert(!modemq_range("+CLVL: 3\r\n", "+CLVL:", &lo, &hi));
		assert(!modemq_range("ERROR\r\n", "+CLVL:", &lo, &hi));
	}

	/* call waiting (voice class) */
	assert(modemq_ccwa("AT+CCWA=1,2,1\r\r\n+CCWA: 1,1\r\n\r\nOK\r\n") == 1);
	assert(modemq_ccwa("+CCWA: 0,1\r\n") == 0);
	assert(modemq_ccwa("+CCWA: 0,2\r\n+CCWA: 1,1\r\n") == 1);		/* the data class line is skipped */
	assert(modemq_ccwa("+CCWA: 1,255\r\n") == 1 && modemq_ccwa("+CCWA: 1,2\r\n") == MQ_UNKNOWN);
	assert(modemq_ccwa("+CCWA: 7,1\r\n") == MQ_UNKNOWN && modemq_ccwa("OK\r\n") == MQ_UNKNOWN && modemq_ccwa("+CME ERROR: 30\r\n") == MQ_UNKNOWN);

	/* CLIR: the setting and the network's state */
	{
		int m = 0;

		assert(modemq_clir("AT+CLIR?\r\r\n+CLIR: 1,4\r\n\r\nOK\r\n", &m) == 1 && m == 4);
		assert(modemq_clir("+CLIR: 0,1\r\n", &m) == 0 && m == 1);
		assert(modemq_clir("+CLIR: 2,0\r\n", &m) == 2 && m == 0);
		assert(modemq_clir("+CLIR: 2\r\n", &m) == 2 && m == MQ_UNKNOWN);
		assert(modemq_clir("+CLIR: 1,9\r\n", &m) == 1 && m == MQ_UNKNOWN);
		assert(modemq_clir("+CLIR: 3,1\r\n", &m) == MQ_UNKNOWN && m == MQ_UNKNOWN);
		assert(modemq_clir("ERROR\r\n", &m) == MQ_UNKNOWN && m == MQ_UNKNOWN);
		assert(modemq_clir("+CLIR: 1,1\r\n", NULL) == 1);
	}

	assert(modemq_count == 8 && !strcmp(modemq_command[0], "AT+QDAI?") && !strcmp(modemq_command[1], "AT+CLVL=?") && !strcmp(modemq_command[6], "AT+QNWINFO") && !strcmp(modemq_command[7], "AT+CLIR?"));
	return puts("modemq: all tests passed") < 0;
}
