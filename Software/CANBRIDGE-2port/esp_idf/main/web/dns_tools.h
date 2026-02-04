/**
 * @file dns_tools.h
 * @brief Various DNS tools to parse and generate DNS queries/answers
 * 	(Hardware-Agnostic)
 *
 * This file contains the software implementation of the DNS parsing logic.
 * The design is hardware-agnostic, requiring an external adaptation layer
 * for hardware interaction.
 *
 * **Conventions:**
 * C89, Linux kernel style, MISRA, rule of 10, No hardware specific code,
 * only generic C and some binding layer. Be extra specific about types.
 *
 * Scientific units where posible at end of the names, for example:
 * - timer_10s (timer_10s has a resolution of 10s per bit)
 * - power_150w (power 150W per bit or 0.15kw per bit)
 *
 * Keep variables without units if they're unknown or not specified or hard
 * to define with short notation.
 *
 * ```LICENSE
 * Copyright (c) 2025 furdog <https://github.com/furdog>
 *
 * SPDX-License-Identifier: 0BSD
 * ```
 *
 * Be free, be wise and take care of yourself!
 * With best wishes and respect, furdog
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/*****************************************************************************
 * DNS TOOLS
 *****************************************************************************/
/** DNS message state machine. Responsible for query parsing/answering */
struct dns_msg {
	uint8_t *_packet_buf; /**< Pointer to the UDP payload data buffer */
	size_t   _packet_cap; /** Raw (UDP) packet capacity */
	size_t   _packet_len; /** Raw (UDP) packet length */

	/** Offset inside packet (when actively parsing) */
	size_t _ofs;

	char     name[64]; /**< Domain name string in aaa.bbb.ccc form */
	uint8_t _name_len; /**< Length of domain name */

	uint16_t query_type;  /**< DNS query type */
	uint16_t query_class; /**< DNS query class */

	/** Set to __LINE__ if something is not right */
	uint32_t malformed;
};

/** Initializes DNS message state machine. Takes pointer to a buffer and
 *  it's capacity. The buffer is where DNS query and answer is stored.
 *  Basically an UDP payload we will work with */
static void dns_msg_init(struct dns_msg *self, uint8_t *buf, size_t cap)
{
	self->_packet_buf = buf;
	self->_packet_cap = cap;
	self->_packet_len = 0u;

	self->_ofs = 0u;

	self->_name_len = 0u;

	self->query_type  = 0u;
	self->query_class = 0u;

	self->malformed = 0u;
}

/** Parses DNS header */
static void _dns_msg_parse_hdr(struct dns_msg *self)
{
	if ((self->_packet_len < 12u) || (self->_ofs != 0u)) {
		/* Can't exceed msg len */
		self->malformed = __LINE__;
	} else {
		self->_ofs = 12;
	}
}

/** Parses DNS name list. Appends single entry into aaa.bbb.ccc form string.
 *  Returns true if it's last entry, or if it's malformed.
 *  DNS name list has the following binary format: [len]text[len]text...[0]
 *  	where [len] is a single byte and text is ASCII encoded text */
static bool _dns_msg_parse_name_entry(struct dns_msg *self)
{
	/* Single list entry length */
	uint8_t  len = self->_packet_buf[self->_ofs];
	uint16_t len_full = 1u + (uint16_t)len;

	/* Last entry always has zero length */
	bool last_entry = (len == 0u);

	if (((self->_ofs + len_full) > self->_packet_len) ||
	    ((self->_name_len + len + 1u) > 64u)) {
		self->malformed = __LINE__;
	} else {		
		uint8_t i;

		self->_ofs += 1u; /* Skip len byte */

		/* Append dot into name */
		if (!last_entry && (self->_name_len > 0u)) {
			self->name[self->_name_len] = '.';
			self->_name_len++;
		}

		/* Appends entry into readable form string */
		for (i = 0; i < len; i++) {
			self->name[self->_name_len] =
				self->_packet_buf[self->_ofs];

			/* Advance to the next byte */
			self->_ofs++;
			self->_name_len++;
		}
	}

	/* Insert null terminator at the end of last entry */
	if (last_entry) {
		self->name[self->_name_len] = '\0';
	}

	return last_entry || self->malformed;
}

/** Parse DNS message. Stores query_type, query_class and DNS name.
 * `malformed` will be nonzero in case of critical fault. Takes `len` param,
 *  which is basically incoming UDP packet payload length */
static void dns_msg_parse_query(struct dns_msg *self, size_t len)
{
	/* Set maximum packet length */
	self->_packet_len = len;

	/* Parse header */
	_dns_msg_parse_hdr(self);

	if (self->malformed == 0u) {
		/* Parse all entries */
		while (_dns_msg_parse_name_entry(self) != true) {};
	}

	if (self->malformed == 0u) {
		/* Parse query type and class */
		if ((self->_ofs + 4u) > self->_packet_len) {
			self->malformed = __LINE__;
		} else {
			self->query_type =
				(self->_packet_buf[self->_ofs + 0u] << 8) |
				(self->_packet_buf[self->_ofs + 1u] << 0);

			self->query_class =
				(self->_packet_buf[self->_ofs + 2u] << 8) |
				(self->_packet_buf[self->_ofs + 3u] << 0);

			self->_ofs += 4u;
		}
	}
}

/** Adds answer to buffer that was derived from query parser.
 *  Returns total number of answer bytes (basically raw UDP payload length) */
size_t dns_msg_add_answer(struct dns_msg *self, uint8_t *answer, size_t len)
{
	size_t total_len = (self->_ofs + len);

	if ((total_len > self->_packet_cap) || (self->_packet_buf == NULL)) {
		self->malformed = __LINE__;
		total_len = 0u;
	}

	if (self->malformed == 0u) {
		/* Standard Response Logic */
		self->_packet_buf[2] = 0x81;
		self->_packet_buf[3] = 0x80; /* Standard Response */

		self->_packet_buf[6] = 0;
		self->_packet_buf[7] = 1; /* 1 Answer */
            
		(void)memcpy(&self->_packet_buf[self->_ofs], answer, len);
        }

	return total_len;
}

/** Gets DNS query type */
static const char *dns_msg_get_type_str(struct dns_msg *self)
{
	const char *result = "OTHER";

	if (self->query_type == 28u) {
		result = "AAAA (IPv6)";
	} else if (self->query_type == 1u) {
		result = "A (IPv4)";
	} else {}

	return result;
}
