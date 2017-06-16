#include "ctypedef.h"
#include "rtp-packet.h"
#include "rtp-payload-internal.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

// RFC7798 RTP Payload Format for High Efficiency Video Coding (HEVC)

#define KHz         90 // 90000Hz
#define FU_START    0x80
#define FU_END      0x40

#define RTP_HEADER_SIZE 12 // don't include RTP CSRC and RTP Header Extension

struct rtp_encode_h265_t
{
	struct rtp_packet_t pkt;
	struct rtp_payload_t handler;
	void* cbparam;
	int size;
};

static void* rtp_h265_pack_create(int size, uint8_t pt, uint16_t seq, uint32_t ssrc, uint32_t frequency, struct rtp_payload_t *handler, void* param)
{
	struct rtp_encode_h265_t *packer;
	packer = (struct rtp_encode_h265_t *)malloc(sizeof(*packer));
	if (!packer) return NULL;

	memset(packer, 0, sizeof(*packer));
	memcpy(&packer->handler, handler, sizeof(packer->handler));
	packer->cbparam = param;
	packer->size = size;

	assert(KHz * 1000 == frequency);
	packer->pkt.rtp.v = RTP_VERSION;
	packer->pkt.rtp.pt = pt;
	packer->pkt.rtp.seq = seq;
	packer->pkt.rtp.ssrc = ssrc;
	return packer;
}

static void rtp_h265_pack_destroy(void* pack)
{
	struct rtp_encode_h265_t *packer;
	packer = (struct rtp_encode_h265_t *)pack;
#if defined(_DEBUG) || defined(DEBUG)
	memset(packer, 0xCC, sizeof(*packer));
#endif
	free(packer);
}

static const uint8_t* h265_nalu_find(const uint8_t* p, size_t bytes)
{
	size_t i;
	for (i = 2; i + 1 < bytes; i++)
	{
		if (0x01 == p[i] && 0x00 == p[i - 1] && 0x00 == p[i - 2])
			return p + i + 1;
	}
	return p + bytes;
}

static void rtp_h265_pack_get_info(void* pack, uint16_t* seq, uint32_t* timestamp)
{
	struct rtp_encode_h265_t *packer;
	packer = (struct rtp_encode_h265_t *)pack;
	*seq = (uint16_t)packer->pkt.rtp.seq;
	*timestamp = packer->pkt.rtp.timestamp;
}

static int rtp_h265_pack_nalu(struct rtp_encode_h265_t *packer, const uint8_t* nalu, int bytes, int64_t time)
{
	int n;
	uint8_t *rtp;

	packer->pkt.payload = nalu;
	packer->pkt.payloadlen = bytes;
	n = RTP_FIXED_HEADER + packer->pkt.payloadlen;
	rtp = (uint8_t*)packer->handler.alloc(packer->cbparam, n);
	if (!rtp) return ENOMEM;

	packer->pkt.rtp.m = 1; // set marker flag
	n = rtp_packet_serialize(&packer->pkt, rtp, n);
	if ((size_t)n != RTP_FIXED_HEADER + packer->pkt.payloadlen)
	{
		assert(0);
		return -1;
	}

	++packer->pkt.rtp.seq;
	packer->handler.packet(packer->cbparam, rtp, n, time, 0);
	packer->handler.free(packer->cbparam, rtp);
	return 0;
}

static int rtp_h265_pack_fu(struct rtp_encode_h265_t *packer, const uint8_t* ptr, int bytes, int64_t time, int MAX_PACKET)
{
	int n;
	unsigned char *rtp;

	uint8_t fu_header = (ptr[0] >> 1) & 0x3F;

	ptr += 2; // skip NAL Unit Type byte
	bytes -= 2;
	assert(bytes > 0);

	// FU-A start
	for (fu_header |= FU_START; bytes > 0; ++packer->pkt.rtp.seq)
	{
		if (bytes <= MAX_PACKET - 3)
		{
			assert(0 == (fu_header & FU_START));
			fu_header = FU_END | (fu_header & 0x3F); // FU end
			packer->pkt.payloadlen = bytes + 3 /*header + fu_header*/;
		}
		else
		{
			packer->pkt.payloadlen = MAX_PACKET;
		}

		packer->pkt.payload = ptr - 3 /*header + fu_header*/;
		n = RTP_FIXED_HEADER + packer->pkt.payloadlen;
		rtp = (uint8_t*)packer->handler.alloc(packer->cbparam, n);
		if (!rtp) return ENOMEM;

		packer->pkt.rtp.m = (FU_END & fu_header) ? 1 : 0; // set marker flag
		n = rtp_packet_serialize(&packer->pkt, rtp, n);
		if ((size_t)n != RTP_FIXED_HEADER + packer->pkt.payloadlen)
		{
			assert(0);
			return -1;
		}

		rtp[RTP_FIXED_HEADER + 0] = 49 << 1;
		rtp[RTP_FIXED_HEADER + 1] = 1;
		rtp[RTP_FIXED_HEADER + 2] = fu_header;
		packer->handler.packet(packer->cbparam, rtp, n, time, 0);
		packer->handler.free(packer->cbparam, rtp);

		bytes -= packer->pkt.payloadlen - 3;
		ptr += packer->pkt.payloadlen - 3;
		fu_header &= 0x3F; // clear flags
	}

	return 0;
}

static int rtp_h265_pack_input(void* pack, const void* h265, int bytes, int64_t time)
{
	int r = 0;
	const uint8_t *p1, *p2, *pend;
	struct rtp_encode_h265_t *packer;
	packer = (struct rtp_encode_h265_t *)pack;
	packer->pkt.rtp.timestamp = (uint32_t)time * KHz; // ms -> 90KHZ

	pend = (const uint8_t*)h265 + bytes;
	for (p1 = h265_nalu_find((const uint8_t*)h265, bytes); p1 < pend && 0 == r; p1 = p2)
	{
		size_t nalu_size;

		// filter H.265 start code(0x00000001)
		p2 = h265_nalu_find(p1 + 1, pend - p1 - 1);
		nalu_size = p2 - p1;

		// filter suffix '00' bytes
		while (0 == p1[nalu_size - 1]) --nalu_size;

		if (nalu_size < (size_t)packer->size)
		{
			// single NAl unit packet 
			r = rtp_h265_pack_nalu(packer, p1, nalu_size, time);
		}
		else
		{
			r = rtp_h265_pack_fu(packer, p1, nalu_size, time, packer->size);
		}
	}

	return 0;
}

struct rtp_payload_encode_t *rtp_h265_encode()
{
	static struct rtp_payload_encode_t packer = {
		rtp_h265_pack_create,
		rtp_h265_pack_destroy,
		rtp_h265_pack_get_info,
		rtp_h265_pack_input,
	};

	return &packer;
}
