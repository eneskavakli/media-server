#include "rtsp-demuxer.h"
#include "rtp-demuxer.h"
#include "rtp-profile.h"
#include "rtcp-header.h"
#include "sdp-a-fmtp.h"
#include "mpeg-ts-proto.h"
#include "mpeg-ps.h"
#include "mpeg-ts.h"
#include "avbsf.h" // https://github.com/ireader/avcodec
#include "mpeg4-aac.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#if defined(OS_WINDOWS)
    #if !defined(strcasecmp)
        #define strcasecmp	_stricmp
    #endif
#endif

struct rtsp_demuxer_t;
struct rtp_payload_info_t
{
    struct rtsp_demuxer_t* ctx;

    int frequency;
    int payload;
    char encoding[64];

    uint8_t* extra;
    int extra_bytes;

    uint16_t seq; // RTP-Info seq
    uint32_t base; // RTP-Info timestamp

    uint32_t last; // last rtp packet timestamp
    int64_t pts; // last mapped rtp packet timestamp

    struct rtp_demuxer_t* rtp;
    void* ts, * ps; // only one

    union
    {
        struct sdp_a_fmtp_h264_t h264;
        struct sdp_a_fmtp_h265_t h265;
        struct sdp_a_fmtp_mpeg4_t mpeg4;

        struct mpeg4_aac_t aac;
    } fmtp;

    struct avbsf_t* bsf;
    void* filter;
};

struct rtsp_demuxer_t
{
    void* param;
    int (*onpacket)(void* param, int stream, int payload, const char* encoding, int64_t pts, int64_t dts, const void* data, int bytes, int flags);

    struct rtp_payload_info_t pt[4];
    int count; // payload type count
    int idx; // last payload index

    uint8_t ptr[8 * 1024];
    int off;
    int error;
};

int sdp_aac_latm_load(uint8_t* data, int bytes, const char* config);
int sdp_aac_mpeg4_load(uint8_t* data, int bytes, const char* config);
int sdp_h264_load(uint8_t* data, int bytes, const char* config);
int sdp_h265_load(uint8_t* data, int bytes, const char* vps, const char* sps, const char* pps, const char* sei);

static inline int rtsp_demuxer_mpegts_onpacket(void* param, int program, int track, int codecid, int flags, int64_t pts, int64_t dts, const void* data, size_t bytes)
{
    static const int s_mpeg2[] = { PSI_STREAM_H264, PSI_STREAM_H265, PSI_STREAM_AAC, PSI_STREAM_MP3, };
    static const int s_payload[] = { RTP_PAYLOAD_H264, RTP_PAYLOAD_H265, RTP_PAYLOAD_MP4A, RTP_PAYLOAD_MP3, };
    static const char* s_encoding[] = { "H264", "H265", "MP4A-LATM", "", };

    int i, r, len;
    struct rtp_payload_info_t* pt;
    pt = (struct rtp_payload_info_t*)param;

    for (i = 0; i < sizeof(s_mpeg2) / sizeof(s_mpeg2[0]); i++)
    {
        if (codecid == s_mpeg2[i])
        {
            if (PSI_STREAM_AAC == codecid)
            {
                if (0 == pt->fmtp.aac.sampling_frequency)
                    mpeg4_aac_adts_load((const uint8_t*)data, bytes, &pt->fmtp.aac);

                len = mpeg4_aac_adts_frame_length((const uint8_t*)data, bytes);
                while (pt->fmtp.aac.sampling_frequency > 0 && len > 7 && (size_t)len <= bytes)
                {
                    r = pt->ctx->onpacket(pt->ctx->param, track, s_payload[i], s_encoding[i], pts / 90, dts / 90, data, len, flags);
                    if (0 != r)
                        return r;

                    pts += 90000 /* 90KHz */ * 1024 /*samples per frame*/ / pt->fmtp.aac.sampling_frequency;
                    dts += 90000 /* 90KHz */ * 1024 /*samples per frame*/ / pt->fmtp.aac.sampling_frequency;
                    bytes -= len;
                    data = (const uint8_t*)data + len;
                    len = mpeg4_aac_adts_frame_length((const uint8_t*)data, bytes);
                }

                return bytes > 0 ? pt->ctx->onpacket(pt->ctx->param, track, s_payload[i], s_encoding[i], pts / 90, dts / 90, data, len, flags) : 0;
            }
            else
            {
                return pt->ctx->onpacket(pt->ctx->param, track, s_payload[i], s_encoding[i], pts / 90, dts / 90, data, (int)bytes, flags);
            }
        }
    }

    (void)program; //ignore
    return -1;
}

static inline void rtsp_demuxer_mpegps_onpacket(void* param, int track, int codecid, int flags, int64_t pts, int64_t dts, const void* data, size_t bytes)
{
    struct rtp_payload_info_t* pt;
    pt = (struct rtp_payload_info_t*)param;
    pt->ctx->error = rtsp_demuxer_mpegts_onpacket(param, 0, track, codecid, flags, pts, dts, data, bytes);
    assert(0 == pt->ctx->error);
}

static int rtsp_demuxer_bsf_onpacket(void* param, int64_t pts, int64_t dts, const uint8_t* data, int bytes, int flags)
{
    struct rtp_payload_info_t* pt;
    pt = (struct rtp_payload_info_t*)param;
    return pt->ctx->onpacket(pt->ctx->param, 0, pt->payload, pt->encoding, pts, dts, data, bytes, flags);
}

static inline void rtsp_demuxer_ontspacket(void* param, const void* packet, int bytes, uint32_t timestamp, int flags)
{
    struct rtp_payload_info_t* pt;
    pt = (struct rtp_payload_info_t*)param;
    
    while (bytes >= 188)
    {
        pt->ctx->error = (int)ts_demuxer_input(pt->ts, packet, 188);
        assert(0 == pt->ctx->error);
        bytes -= 188;
        packet = (const uint8_t*)packet + 188;
    }
    (void)timestamp, (void)flags; //ignore
}

static inline void rtsp_demuxer_onpspacket(void* param, const void* packet, int bytes, uint32_t timestamp, int flags)
{
    struct rtp_payload_info_t* pt;
    pt = (struct rtp_payload_info_t*)param;
    pt->ctx->error = (int)ps_demuxer_input(pt->ps, packet, bytes);
    assert(pt->ctx->error == bytes);
    (void)timestamp, (void)flags; //ignore
}

static inline void rtsp_demuxer_onrtppacket(void* param, const void* data, int bytes, uint32_t timestamp, int flags)
{
    struct rtp_payload_info_t* pt;
    pt = (struct rtp_payload_info_t*)param;

    // RTP timestamp => PTS/DTS
    if (0 == pt->last && 0 == pt->pts)
    {
        pt->last = timestamp;
        pt->pts = 0;
    }
    else
    {
        pt->pts += ((int64_t)(int32_t)(timestamp - pt->last)) * 1000 / pt->frequency;
        pt->last = timestamp;
    }

    if (pt->bsf && pt->filter)
        pt->ctx->error = pt->bsf->input(pt->filter, pt->pts, pt->pts, (const uint8_t*)data, bytes);
    else
        pt->ctx->error = pt->ctx->onpacket(pt->ctx->param, 0, pt->payload, pt->encoding, pt->pts, pt->pts, data, bytes, flags);
    assert(0 == pt->ctx->error);
}

static int rtsp_demuxer_payload_close(struct rtp_payload_info_t* pt)
{
    if (pt->bsf && pt->filter)
    {
        pt->bsf->destroy(&pt->filter);
        pt->bsf = NULL;
    }

    if (pt->ts)
    {
        ts_demuxer_destroy(pt->ts);
        pt->ts = NULL;
    }

    if (pt->ps)
    {
        ps_demuxer_destroy(pt->ps);
        pt->ps = NULL;
    }

    if (pt->rtp)
    {
        rtp_demuxer_destroy(&pt->rtp);
        pt->rtp = NULL;
    }

    return 0;
}

int rtsp_demuxer_add_payload(struct rtsp_demuxer_t* demuxer, int frequency, int payload, const char* encoding, const char* fmtp)
{
    int len;
    struct rtp_payload_info_t* pt;
    if (demuxer->count >= sizeof(demuxer->pt) / sizeof(demuxer->pt[0]))
        return -1; // too many payload type

    pt = &demuxer->pt[demuxer->count];
    memset(pt, 0, sizeof(*pt));
    snprintf(pt->encoding, sizeof(pt->encoding) - 1, "%s", encoding);
    pt->frequency = frequency;
    pt->payload = payload;
    pt->extra = demuxer->ptr + demuxer->off;
    pt->extra_bytes = 0;
    pt->ctx = demuxer;

    if (RTP_PAYLOAD_MP2T == payload)
    {
        pt->ts = ts_demuxer_create(rtsp_demuxer_mpegts_onpacket, pt);
        pt->rtp = rtp_demuxer_create(frequency, payload, encoding, rtsp_demuxer_ontspacket, pt);
    }
    else if (0 == strcasecmp(encoding, "MP2P"))
    {
        pt->ps = ps_demuxer_create(rtsp_demuxer_mpegps_onpacket, pt);
        pt->rtp = rtp_demuxer_create(frequency, payload, encoding, rtsp_demuxer_onpspacket, pt);
    }
    else
    {
        assert(demuxer->off <= sizeof(demuxer->ptr));
        len = sizeof(demuxer->ptr) - demuxer->off;

        if (0 == strcasecmp(encoding, "H264"))
        {
            if (fmtp && 0 == sdp_a_fmtp_h264(fmtp, &payload, &pt->fmtp.h264))
                pt->extra_bytes = sdp_h264_load(pt->extra, len, pt->fmtp.h264.sprop_parameter_sets);
            pt->bsf = avbsf_h264();
        }
        else if (0 == strcasecmp(encoding, "H265") || 0 == strcasecmp(encoding, "HEVC"))
        {
            if (fmtp && 0 == sdp_a_fmtp_h265(fmtp, &payload, &pt->fmtp.h265))
                pt->extra_bytes = sdp_h265_load(pt->extra, len, pt->fmtp.h265.sprop_vps, pt->fmtp.h265.sprop_sps, pt->fmtp.h265.sprop_pps, pt->fmtp.h265.sprop_sei);
            pt->bsf = avbsf_h265();
        }
        else if (fmtp && (0 == strcasecmp(encoding, "MPEG4-GENERIC")))
        {
            if (fmtp && 0 == sdp_a_fmtp_mpeg4(fmtp, &payload, &pt->fmtp.mpeg4))
                pt->extra_bytes = sdp_aac_mpeg4_load(pt->extra, len, pt->fmtp.mpeg4.config);
            pt->bsf = avbsf_aac();
        }
        else if (fmtp && (0 == strcasecmp(encoding, "MP4A-LATM")))
        {
            if (fmtp && 0 == sdp_a_fmtp_mpeg4(fmtp, &payload, &pt->fmtp.mpeg4))
                pt->extra_bytes = sdp_aac_latm_load(pt->extra, len, pt->fmtp.mpeg4.config);
            pt->bsf = avbsf_aac();
        }

        if (pt->bsf)
            pt->filter = pt->bsf->create(pt->extra, pt->extra_bytes > 0 ? pt->extra_bytes : 0, rtsp_demuxer_bsf_onpacket, pt);
        pt->rtp = rtp_demuxer_create(frequency, payload, encoding, rtsp_demuxer_onrtppacket, pt);
    }

    if (!pt->rtp || (pt->bsf && !pt->filter))
    {
        rtsp_demuxer_payload_close(pt);
        return -1;
    }

    demuxer->count++;
    demuxer->off += pt->extra_bytes;
    assert(demuxer->off <= sizeof(demuxer->ptr));
    return 0;
}

struct rtsp_demuxer_t* rtsp_demuxer_create(int frequency, int payload, const char* encoding, const char* fmtp, rtsp_demuxer_onpacket onpkt, void* param)
{
    struct rtsp_demuxer_t* demuxer;
    demuxer = calloc(1, sizeof(*demuxer));
    if (!demuxer) return NULL;

    assert(frequency > 0);
    demuxer->param = param;
    demuxer->onpacket = onpkt;
    
    if (0 != rtsp_demuxer_add_payload(demuxer, frequency, payload, encoding, fmtp))
    {
        rtsp_demuxer_destroy(demuxer);
        return NULL;
    }

    return demuxer;
}

int rtsp_demuxer_destroy(struct rtsp_demuxer_t* demuxer)
{
    int i;
    if (demuxer)
    {
        for (i = 0; i < demuxer->count; i++)
            rtsp_demuxer_payload_close(&demuxer->pt[i]);
        free(demuxer);
    }
    return 0;
}

int rtsp_demuxer_rtpinfo(struct rtsp_demuxer_t* demuxer, uint16_t seq, uint32_t timestamp)
{
    struct rtp_payload_info_t* pt;
    if (demuxer->idx >= demuxer->count || demuxer->idx < 0)
    {
        assert(0);
        return -1;
    }

    pt = &demuxer->pt[demuxer->idx];
    pt->seq = seq;
    pt->base = timestamp;
    if (0 == pt->last)
        pt->last = timestamp;
    pt->pts = ((int64_t)(int32_t)(pt->last - timestamp)) * 1000 / pt->frequency;
    return 0;
}

int rtsp_demuxer_input(struct rtsp_demuxer_t* demuxer, const void* data, int bytes)
{
    int i;
    uint8_t id;
    struct rtp_payload_info_t* pt;

    id = bytes > 1 ? ((const uint8_t*)data)[1] : 255;
    for (i = demuxer->idx; i < demuxer->count + demuxer->idx; i++)
    {
        pt = &demuxer->pt[i % demuxer->count];

        // RFC7983 SRTP: https://tools.ietf.org/html/draft-ietf-avtcore-rfc5764-mux-fixes
        assert(((const uint8_t*)data)[0] > 127 && ((const uint8_t*)data)[0] < 192);

        // http://www.iana.org/assignments/rtp-parameters/rtp-parameters.xhtml#rtp-parameters-4
        // RFC 5761 (RTCP-mux) states this range for secure RTCP/RTP detection.
        // RTCP packet types in the ranges 1-191 and 224-254 SHOULD only be used when other values have been exhausted.
        if ( (id & 0x7F) == pt->payload || (192 <= id && id <= 223) )
        {
            demuxer->idx = i % demuxer->count;
            return rtp_demuxer_input(pt->rtp, data, bytes);
        }
    }

    assert(0);
    return -1;
}

int rtsp_demuxer_rtcp(struct rtsp_demuxer_t* demuxer, void* buf, int len)
{
    if (demuxer->idx >= demuxer->count || demuxer->idx < 0)
    {
        assert(0);
        return -1;
    }

    return rtp_demuxer_rtcp(demuxer->pt[demuxer->idx].rtp, buf, len);
}
