/*
 * OpenConnect (SSL + DTLS) VPN client
 *
 * Copyright © 2014 Intel Corporation.
 *
 * Author: David Woodhouse <dwmw2@infradead.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include <config.h>

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <pcsclite.h>
#include <winscard.h>

#include "openconnect-internal.h"

#define NAME_TAG	0x71
#define NAME_LIST_TAG	0x72
#define KEY_TAG		0x73
#define CHALLENGE_TAG	0x74
#define RESPONSE_TAG	0x75
#define T_RESPONSE_TAG	0x76
#define NO_RESPONSE_TAG	0x77
#define PROPERTY_TAG	0x78
#define VERSION_TAG	0x79
#define IMF_TAG		0x7a

#define PUT_INS		0x01
#define DELETE_INS	0x02
#define SET_CODE_INS	0x03
#define RESET_INS	0x04
#define LIST_INS	0xa1
#define CALCULATE_INS	0xa2
#define VALIDATE_INS	0xa3
#define CALCULATE_ALL_INS 0xa4
#define SEND_REMAINING_INS 0xa5

static const unsigned char appselect[] = { 0x00, 0xa4, 0x04, 0x00, 0x07,
					   0xa0, 0x00, 0x00, 0x05, 0x27, 0x21, 0x01 };
static const unsigned char list_keys[] = { 0x00, LIST_INS, 0x00, 0x00 };
static const unsigned char send_remaining[] = { 0x00, SEND_REMAINING_INS, 0x00, 0x00 };

static int yubikey_cmd(struct openconnect_info *vpninfo, SCARDHANDLE card, int errlvl,
		       const char *desc,
		       const unsigned char *out, size_t outlen, struct oc_text_buf *buf)
{
	DWORD status;

	buf_truncate(buf);

	do {
		size_t respsize = 258;
		
		if (buf_ensure_space(buf, 258))
			return -ENOMEM;
	
		status = SCardTransmit (card, SCARD_PCI_T1, out, outlen,
					NULL, (unsigned char *)&buf->data[buf->pos], &respsize);
		if (status != SCARD_S_SUCCESS) {
			vpn_progress(vpninfo, errlvl,
				     _("Failed to send \"%s\" to ykneo-oath applet: %s\n"),
				     desc, pcsc_stringify_error(status));
			return -EIO;
		}
		if (respsize < 2) {
			vpn_progress(vpninfo, errlvl,
				     _("Invalid short response to \"%s\" from ykneo-oath applet\n"),
				     desc);
			return -EIO;
		}

		buf->pos += respsize - 2;

		/* Continuation */
		out = send_remaining;
		outlen = sizeof(send_remaining);
	} while (buf->data[buf->pos] == 0x61);

	if ((unsigned char)buf->data[buf->pos] == 0x90 && buf->data[buf->pos+1] == 0x00)
		return 0;

	vpn_progress(vpninfo, errlvl,
		     _("Failure from ykneo-oath applet after \"%s\": %02x %02x\n"),
		     desc, buf->data[buf->pos], buf->data[buf->pos+1]);
	return -EIO;
}

static int buf_tlv(struct oc_text_buf *buf, int *loc, unsigned char *type)
{
	int len;
	int left = buf->pos - *loc;

	if (left < 2)
		return -EINVAL;

	*type = (unsigned char)buf->data[(*loc)++];
	len = (unsigned char)buf->data[(*loc)++];
	left -= 2;

	if (len > 0x82)
		return -EINVAL;
	else if (len == 0x81) {
		if (left < 1)
			return -EINVAL;
		len = (unsigned char)buf->data[(*loc)++];
		left--;
	} else if (len == 0x82) {
		if (left < 2)
			return -EINVAL;
		len = (unsigned char)buf->data[(*loc)++];
		len <<= 8;
		len = (unsigned char)buf->data[(*loc)++];
		left -= 2;
	}

	if (left < len)
		return -EINVAL;

	return len;
}

int set_yubikey_mode(struct openconnect_info *vpninfo, const char *token_str)
{
	SCARDHANDLE pcsc_ctx, pcsc_card;
	LONG status;
	char *readers = NULL, *reader;
	DWORD readers_size, proto;
	int ret, tlvlen, tlvpos;
	struct oc_text_buf *buf = NULL;

	status = SCardEstablishContext(SCARD_SCOPE_USER, NULL, NULL, &pcsc_ctx);
	if (status != SCARD_S_SUCCESS) {
		vpn_progress(vpninfo, PRG_ERR, _("Failed to establish PC/SC context: %s\n"),
			     pcsc_stringify_error(status));
		return -EIO;
	}
	vpn_progress(vpninfo, PRG_TRACE, _("Established PC/SC context\n"));

	ret = -ENOENT;
	status = SCardListReaders(pcsc_ctx, NULL, NULL, &readers_size);
	if (status != SCARD_S_SUCCESS) {
		vpn_progress(vpninfo, PRG_ERR, _("Failed to query reader list: %s\n"),
			     pcsc_stringify_error(status));
		goto out_ctx;
	}
	readers = malloc(readers_size);
	if (!readers)
		goto out_ctx;

	status = SCardListReaders(pcsc_ctx, NULL, readers, &readers_size);
	if (status != SCARD_S_SUCCESS) {
		vpn_progress(vpninfo, PRG_ERR, _("Failed to query reader list: %s\n"),
			     pcsc_stringify_error(status));
		goto out_ctx;
	}

	buf = buf_alloc();
	
	for (reader = readers; reader[0]; reader += strlen(reader) + 1) {
		unsigned char type;
		unsigned char *applet_ver, *applet_id;

		status = SCardConnect(pcsc_ctx, reader, SCARD_SHARE_SHARED,
				      SCARD_PROTOCOL_T1,
				      &pcsc_card, &proto);
		if (status != SCARD_S_SUCCESS) {
			vpn_progress(vpninfo, PRG_ERR, _("Failed to connect to PC/SC reader '%s': %s\n"),
				     reader, pcsc_stringify_error(status));
			continue;
		}
		vpn_progress(vpninfo, PRG_TRACE, _("Connected PC/SC reader '%s'\n"), reader);

		status = SCardBeginTransaction(pcsc_card);
		if (status != SCARD_S_SUCCESS) {
			vpn_progress(vpninfo, PRG_ERR, _("Failed to obtain exclusive access to reader '%s'\n"),
				     reader);
			continue;
		}
		
		ret = yubikey_cmd(vpninfo, pcsc_card, PRG_DEBUG, _("select applet command"),
				  appselect, sizeof(appselect), buf);
		if (ret)
			goto next_reader;

		tlvpos = 0;
		tlvlen = buf_tlv(buf, &tlvpos, &type);

		if (tlvlen < 0 || type != VERSION_TAG || tlvlen != 3) {
		bad_applet:
			vpn_progress(vpninfo, PRG_ERR,
				     _("Unrecognised response from ykneo-oath applet\n"));
			goto next_reader;
		}
		applet_ver = (unsigned char *)&buf->data[tlvpos];
		tlvpos += tlvlen;

		tlvlen = buf_tlv(buf, &tlvpos, &type);

		if (tlvlen < 0 || type != NAME_TAG || tlvlen != 8)
			goto bad_applet;
		applet_id = (unsigned char *)&buf->data[tlvpos];
		tlvpos += tlvlen;

		vpn_progress(vpninfo, PRG_INFO,
			     _("Found ykneo-oath applet v%d.%d.%d, ID %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n"),
			     applet_ver[0], applet_ver[1], applet_ver[2],
			     applet_id[0], applet_id[1], applet_id[2], applet_id[3],
			     applet_id[4], applet_id[5], applet_id[6], applet_id[7]);

		if (tlvpos != buf->pos) {
			if ((unsigned char)buf->data[tlvpos] == CHALLENGE_TAG) {
				vpn_progress(vpninfo, PRG_ERR,
					     _("Applet is locked. Unlocking is not yet implemented\n"));
				goto next_reader;
			}
			goto bad_applet;
		}

		ret = yubikey_cmd(vpninfo, pcsc_card, PRG_ERR, _("list keys command"), list_keys, sizeof(list_keys), buf);
		if (ret)
			continue;

		tlvpos = 0;
		while (tlvpos < buf->pos) {
			unsigned char mode, hash;

			tlvlen = buf_tlv(buf, &tlvpos, &type);
			if (type != NAME_LIST_TAG)
				goto bad_applet;

			mode = buf->data[tlvpos] & 0xf0;
			hash = buf->data[tlvpos] & 0x0f;
			if (mode != 0x10 && mode != 0x20)
				goto bad_applet;
			if (hash != 0x01 && hash != 0x02)
				goto bad_applet;

			if (!token_str ||
			    ((tlvlen - 1 == strlen(token_str)) && !memcmp(token_str, &buf->data[tlvpos+1], tlvlen-1))) {
				vpninfo->yubikey_objname = strndup(&buf->data[tlvpos+1], tlvlen-1);
				if (!vpninfo->yubikey_objname) {
					ret = -ENOMEM;
					SCardEndTransaction(pcsc_card, SCARD_LEAVE_CARD);
					SCardReleaseContext(pcsc_card);
					goto out_ctx;
				}
				/* Translators: This is filled in with mode and hash type, and the key identifier.
				   e.g. "Found HOTP/SHA1 key: 'Work VPN key'\n" */
				vpn_progress(vpninfo, PRG_INFO, _("Found %s/%s key '%s' on '%s'\n"),
					     (mode == 0x20) ? "TOTP" : "HOTP",
					     (hash == 0x2) ? "SHA256" : "SHA1",
					     vpninfo->yubikey_objname, reader);

				vpninfo->yubikey_mode = mode;
				vpninfo->pcsc_ctx = pcsc_ctx;
				vpninfo->pcsc_card = pcsc_card;
				vpninfo->token_mode = OC_TOKEN_MODE_YUBIKEY;
				SCardEndTransaction(pcsc_card, SCARD_LEAVE_CARD);

				goto success;
			}
			tlvpos += tlvlen;
		}
		if (token_str) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("Token '%s' not found on Yubikey '%s'. Searching for another Yubikey...\n"),
				       token_str, reader);
		}

	next_reader:
		SCardEndTransaction(pcsc_card, SCARD_LEAVE_CARD);
		SCardReleaseContext(pcsc_card);
	}
	ret = -ENOENT;
 out_ctx:
	SCardReleaseContext(pcsc_ctx);
 success:
	free(readers);
	buf_free(buf);
	
	return ret;
}

/* Return value:
 *  < 0, if unable to generate a tokencode
 *  = 0, on success
 */
int can_gen_yubikey_code(struct openconnect_info *vpninfo,
			 struct oc_auth_form *form,
			 struct oc_form_opt *opt)
{
	if ((strcmp(opt->name, "secondary_password") != 0) ||
	    vpninfo->token_bypassed)
		return -EINVAL;
	if (vpninfo->token_tries == 0) {
		vpn_progress(vpninfo, PRG_DEBUG,
			     _("OK to generate INITIAL tokencode\n"));
		vpninfo->token_time = 0;
	} else if (vpninfo->token_tries == 1) {
		vpn_progress(vpninfo, PRG_DEBUG,
			     _("OK to generate NEXT tokencode\n"));
		vpninfo->token_time += 30;
	} else {
		/* limit the number of retries, to avoid account lockouts */
		vpn_progress(vpninfo, PRG_INFO,
			     _("Server is rejecting the Yubikey token; switching to manual entry\n"));
		return -ENOENT;
	}
	return 0;
}

static int tlvlen_len(int tlvlen)
{
	if (tlvlen < 0x80)
		return 1;
	else if (tlvlen < 0x100)
		return 2;
	else
		return 3;
}

static int append_tlvlen(unsigned char *p, int tlvlen)
{
	if (tlvlen < 0x80) {
		if (p)
			p[0] = tlvlen;
		return 1;
	} else if (tlvlen < 0x100) {
		if (p) {
			p[0] = 0x81;
			p[1] = tlvlen;
		}
		return 2;
	} else {
		if (p) {
			p[0] = 0x82;
			p[1] = tlvlen >> 8;
			p[2] = tlvlen & 0xff;
		}
		return 3;
	}
}

int do_gen_yubikey_code(struct openconnect_info *vpninfo,
			struct oc_auth_form *form,
			struct oc_form_opt *opt)
{
	struct oc_text_buf *respbuf = NULL;
	DWORD status;
	int name_len = strlen(vpninfo->yubikey_objname);
	int name_tlvlen;
	int calc_tlvlen;
	unsigned char *reqbuf = NULL;
	int tokval;
	int i = 0;
	int ret;
	
	if (!vpninfo->token_time)
		vpninfo->token_time = time(NULL);

	vpn_progress(vpninfo, PRG_INFO, _("Generating Yubikey token code\n"));

	status = SCardBeginTransaction(vpninfo->pcsc_card);
	if (status != SCARD_S_SUCCESS) {
		vpn_progress(vpninfo, PRG_ERR, _("Failed to obtain exclusive access to Yubicard\n"));
		return -EIO;
	}

	respbuf = buf_alloc();
	ret = yubikey_cmd(vpninfo, vpninfo->pcsc_card, PRG_DEBUG, _("select applet command"),
			  appselect, sizeof(appselect), respbuf);
	if (ret)
		goto out;

	if (respbuf->pos != 15 || (unsigned char)respbuf->data[0] != VERSION_TAG ||
	    (unsigned char)respbuf->data[5] != NAME_TAG) {
	bad_resp:
		vpn_progress(vpninfo, PRG_ERR,
			     _("Unrecognised response from Yubikey when generating tokencode\n"));
		ret = -EIO;
		goto out;
	}

	name_tlvlen = tlvlen_len(strlen(vpninfo->yubikey_objname));
	calc_tlvlen = 1 /* NAME_TAG */ + name_tlvlen + name_len +
		1 /* CHALLENGE_TAG */ + 1 /* Challenge TLV len */;
	if (vpninfo->yubikey_mode == 0x20)
		calc_tlvlen += 8; /* TOTP needs the time as challenge */

	reqbuf = malloc(4 + tlvlen_len(calc_tlvlen) + calc_tlvlen);
	if (!reqbuf)
		goto out;
	
	reqbuf[i++] = 0;
	reqbuf[i++] = CALCULATE_INS;
	reqbuf[i++] = 0;
	reqbuf[i++] = 1;
	i += append_tlvlen(reqbuf + i, calc_tlvlen);
	reqbuf[i++] = NAME_TAG;
	i += append_tlvlen(reqbuf + i, name_len);
	memcpy(reqbuf + i, vpninfo->yubikey_objname, name_len);
	i += name_len;
	reqbuf[i++] = CHALLENGE_TAG;
	if (vpninfo->yubikey_mode == 0x20) {
		long token_steps = vpninfo->token_time / 30;
		reqbuf[i++] = 8;
		reqbuf[i++] = 0;
		reqbuf[i++] = 0;
		reqbuf[i++] = 0;
		reqbuf[i++] = 0;
		reqbuf[i++] = token_steps >> 24;
		reqbuf[i++] = token_steps >> 16;
		reqbuf[i++] = token_steps >> 8;
		reqbuf[i++] = token_steps;
	} else {
		reqbuf[i++] = 0; /* HOTP mode, zero-length challenge */
	}

	ret = yubikey_cmd(vpninfo, vpninfo->pcsc_card, PRG_ERR, _("calculate command"),
			  reqbuf, i, respbuf);
	if (ret)
		goto out;

	if (respbuf->pos != 7 || (unsigned char)respbuf->data[0] != T_RESPONSE_TAG ||
	    respbuf->data[1] != 5 || respbuf->data[2] > 8 || respbuf->data[2] < 6)
		goto bad_resp;

	tokval = ((unsigned char)respbuf->data[3] << 24) +
		((unsigned char)respbuf->data[4] << 16) +
		((unsigned char)respbuf->data[5] << 8) +
		(unsigned char)respbuf->data[6];
	opt->_value = malloc(respbuf->data[2] + 1);
	if (!opt->_value) {
		ret = -ENOMEM;
		goto out;
	}

	i = respbuf->data[2];
	opt->_value[i] = 0;
	while (i--) {
		opt->_value[i] = '0' + tokval % 10;
		tokval /= 10;
	}
	vpninfo->token_tries++;

 out:
	SCardEndTransaction(vpninfo->pcsc_card, SCARD_LEAVE_CARD);
	buf_free(respbuf);
	free(reqbuf);

	return ret;
}