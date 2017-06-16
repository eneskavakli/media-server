#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include "ctypedef.h"
#include "rtp-packet.h"
#include "rtp-payload-internal.h"

struct rtp_decode_ts_t
{
	struct rtp_payload_t handler;
	void* cbparam;

	int flag; // lost packet
	uint16_t seq; // rtp seq
	uint32_t timestamp;

	uint8_t* ptr;
	size_t size, capacity;
};

static void* rtp_ts_unpack_create(struct rtp_payload_t *handler, void* cbparam)
{
	struct rtp_decode_ts_t *unpacker;
	unpacker = (struct rtp_decode_ts_t *)malloc(sizeof(*unpacker));
	if (!unpacker)
		return NULL;

	memset(unpacker, 0, sizeof(*unpacker));
	memcpy(&unpacker->handler, handler, sizeof(unpacker->handler));
	unpacker->cbparam = cbparam;
	return unpacker;
}

static void rtp_ts_unpack_destroy(void* p)
{
	struct rtp_decode_ts_t *unpacker;
	unpacker = (struct rtp_decode_ts_t *)p;

	if (unpacker->ptr)
		free(unpacker->ptr);
#if defined(_DEBUG) || defined(DEBUG)
	memset(unpacker, 0xCC, sizeof(*unpacker));
#endif
	free(unpacker);
}

static int rtp_ts_unpack_input(void* p, const void* packet, int bytes, int64_t time)
{
	struct rtp_packet_t pkt;
	struct rtp_decode_ts_t *unpacker;

	unpacker = (struct rtp_decode_ts_t *)p;
	if (!unpacker || 0 != rtp_packet_deserialize(&pkt, packet, bytes) || pkt.payloadlen < 1)
		return -1;

	if ((uint16_t)pkt.rtp.seq != unpacker->seq + 1 && 0 != unpacker->seq)
	{
		// packet lost
		unpacker->flag = 1;
		unpacker->size = 0;
		unpacker->seq = (uint16_t)pkt.rtp.seq;
		printf("%s: rtp packet lost.\n", __FUNCTION__);
		return EFAULT;
	}

	unpacker->seq = (uint16_t)pkt.rtp.seq;

	assert(pkt.payloadlen > 0);
	if (unpacker->size + pkt.payloadlen > unpacker->capacity)
	{
		void *ptr = realloc(unpacker->ptr, unpacker->size + pkt.payloadlen + 2048);
		if (!ptr)
		{
			unpacker->flag = 1;
			unpacker->size = 0;
			return ENOMEM;
		}

		unpacker->ptr = (uint8_t*)ptr;
		unpacker->capacity = unpacker->size + pkt.payloadlen + 2048;
	}

	// RTP marker bit
	if (pkt.rtp.m)
	{
		// Set to 1 whenever the timestamp is discontinuous
		assert(pkt.payloadlen > 0);
		assert(1 == unpacker->flag || 0 == unpacker->size || pkt.rtp.timestamp == unpacker->timestamp);
		if (pkt.payload && pkt.payloadlen > 0)
		{
			memcpy(unpacker->ptr + unpacker->size, pkt.payload, pkt.payloadlen);
			unpacker->size += pkt.payloadlen;
		}

		if (unpacker->size > 0 && 0 == unpacker->flag)
		{
			unpacker->handler.packet(unpacker->cbparam, unpacker->ptr, unpacker->size, time, 0);
		}

		// frame boundary
		unpacker->flag = 0;
		unpacker->size = 0;
	}
	else if (pkt.rtp.timestamp != unpacker->timestamp)
	{
		if (unpacker->size > 0 && 0 == unpacker->flag)
		{
			unpacker->handler.packet(unpacker->cbparam, unpacker->ptr, unpacker->size, time, 0);
		}

		// frame boundary
		unpacker->flag = 0;
		unpacker->size = 0;
		if (pkt.payload && pkt.payloadlen > 0)
		{
			assert(unpacker->capacity >= unpacker->size + pkt.payloadlen);
			memcpy(unpacker->ptr + unpacker->size, pkt.payload, pkt.payloadlen);
			unpacker->size = pkt.payloadlen;
		}
	}
	else
	{
		if (pkt.payload && pkt.payloadlen > 0)
		{
			memcpy(unpacker->ptr + unpacker->size, pkt.payload, pkt.payloadlen);
			unpacker->size += pkt.payloadlen;
		}
	}

	unpacker->timestamp = pkt.rtp.timestamp;
	return 0;
}

struct rtp_payload_decode_t *rtp_ts_decode()
{
	static struct rtp_payload_decode_t decode = {
		rtp_ts_unpack_create,
		rtp_ts_unpack_destroy,
		rtp_ts_unpack_input,
	};

	return &decode;
}
