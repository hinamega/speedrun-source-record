#include "speedrun-concat.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <stdlib.h>
#include <string.h>

#define MAX_STREAMS 32

bool speedrun_concat_files(const char *in_pre, const char *in_main, const char *out_final)
{
	if (!in_pre || !in_main || !out_final)
		return false;

	AVFormatContext *ifmt_ctx1 = NULL;
	AVFormatContext *ifmt_ctx2 = NULL;
	AVFormatContext *ofmt_ctx = NULL;
	AVPacket *pkt = NULL;
	bool success = false;
	int ret = 0;

	int stream_map1[MAX_STREAMS];
	int stream_map2[MAX_STREAMS];
	int64_t last_pts[MAX_STREAMS];
	int64_t last_dts[MAX_STREAMS];
	int64_t offset_ts[MAX_STREAMS];

	for (int i = 0; i < MAX_STREAMS; i++) {
		stream_map1[i] = -1;
		stream_map2[i] = -1;
		last_pts[i] = AV_NOPTS_VALUE;
		last_dts[i] = AV_NOPTS_VALUE;
		offset_ts[i] = 0;
	}

	/* 1. Open input 1 (pre-buffer) */
	if ((ret = avformat_open_input(&ifmt_ctx1, in_pre, NULL, NULL)) < 0) {
		blog(LOG_WARNING, "[SpeedrunConcat] Could not open pre-buffer file '%s' (err: %d)", in_pre, ret);
		goto cleanup;
	}
	if ((ret = avformat_find_stream_info(ifmt_ctx1, NULL)) < 0) {
		blog(LOG_WARNING, "[SpeedrunConcat] Failed to retrieve input stream info for '%s' (err: %d)", in_pre, ret);
		goto cleanup;
	}

	/* 2. Open input 2 (main run) */
	if ((ret = avformat_open_input(&ifmt_ctx2, in_main, NULL, NULL)) < 0) {
		blog(LOG_WARNING, "[SpeedrunConcat] Could not open main run file '%s' (err: %d)", in_main, ret);
		goto cleanup;
	}
	if ((ret = avformat_find_stream_info(ifmt_ctx2, NULL)) < 0) {
		blog(LOG_WARNING, "[SpeedrunConcat] Failed to retrieve input stream info for '%s' (err: %d)", in_main, ret);
		goto cleanup;
	}

	/* 3. Allocate output format context */
	if ((ret = avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, out_final)) < 0 || !ofmt_ctx) {
		blog(LOG_WARNING, "[SpeedrunConcat] Could not create output context for '%s' (err: %d)", out_final, ret);
		goto cleanup;
	}

	/* 4. Map streams from input 1 to output */
	int out_stream_idx = 0;
	for (unsigned int i = 0; i < ifmt_ctx1->nb_streams && out_stream_idx < MAX_STREAMS; i++) {
		AVStream *in_stream = ifmt_ctx1->streams[i];
		enum AVMediaType type = in_stream->codecpar->codec_type;

		if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO)
			continue;

		AVStream *out_stream = avformat_new_stream(ofmt_ctx, NULL);
		if (!out_stream) {
			blog(LOG_WARNING, "[SpeedrunConcat] Failed allocating output stream");
			goto cleanup;
		}

		ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
		if (ret < 0) {
			blog(LOG_WARNING, "[SpeedrunConcat] Failed to copy codec parameters (err: %d)", ret);
			goto cleanup;
		}
		out_stream->codecpar->codec_tag = 0;
		out_stream->time_base = in_stream->time_base;

		stream_map1[i] = out_stream_idx++;
	}

	/* 5. Map streams from input 2 to output by matching media types */
	for (unsigned int i = 0; i < ifmt_ctx2->nb_streams; i++) {
		AVStream *in2_stream = ifmt_ctx2->streams[i];
		enum AVMediaType type = in2_stream->codecpar->codec_type;

		for (unsigned int j = 0; j < ifmt_ctx1->nb_streams; j++) {
			if (stream_map1[j] < 0)
				continue;
			if (ifmt_ctx1->streams[j]->codecpar->codec_type == type) {
				/* Found matching stream index */
				stream_map2[i] = stream_map1[j];
				break;
			}
		}
	}

	/* 6. Open output file if required */
	if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
		ret = avio_open(&ofmt_ctx->pb, out_final, AVIO_FLAG_WRITE);
		if (ret < 0) {
			blog(LOG_WARNING, "[SpeedrunConcat] Could not open output file '%s' (err: %d)", out_final, ret);
			goto cleanup;
		}
	}

	/* 7. Write output header */
	ret = avformat_write_header(ofmt_ctx, NULL);
	if (ret < 0) {
		blog(LOG_WARNING, "[SpeedrunConcat] Error occurred when writing header to '%s' (err: %d)", out_final, ret);
		goto cleanup;
	}

	pkt = av_packet_alloc();
	if (!pkt) {
		blog(LOG_WARNING, "[SpeedrunConcat] Failed to allocate AVPacket");
		goto cleanup;
	}

	/* 8. Write packets from input 1 (pre-buffer) */
	while (av_read_frame(ifmt_ctx1, pkt) >= 0) {
		const int stream_idx = pkt->stream_index;
		if (stream_idx < 0 || (unsigned int)stream_idx >= ifmt_ctx1->nb_streams) {
			av_packet_unref(pkt);
			continue;
		}

		const int out_idx = stream_map1[stream_idx];
		if (out_idx < 0 || out_idx >= out_stream_idx) {
			av_packet_unref(pkt);
			continue;
		}

		AVStream *in_stream = ifmt_ctx1->streams[stream_idx];
		AVStream *out_stream = ofmt_ctx->streams[out_idx];

		pkt->stream_index = out_idx;
		av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);

		if (pkt->pts != AV_NOPTS_VALUE && (last_pts[out_idx] == AV_NOPTS_VALUE || pkt->pts > last_pts[out_idx]))
			last_pts[out_idx] = pkt->pts;
		if (pkt->dts != AV_NOPTS_VALUE && (last_dts[out_idx] == AV_NOPTS_VALUE || pkt->dts > last_dts[out_idx]))
			last_dts[out_idx] = pkt->dts;

		pkt->pos = -1;
		ret = av_interleaved_write_frame(ofmt_ctx, pkt);
		if (ret < 0) {
			blog(LOG_WARNING, "[SpeedrunConcat] Error muxing packet from pre-buffer (err: %d)", ret);
		}
		av_packet_unref(pkt);
	}

	/* Calculate timestamp offsets for input 2 (main run) */
	for (int i = 0; i < out_stream_idx; i++) {
		int64_t base = (last_dts[i] != AV_NOPTS_VALUE) ? last_dts[i] : last_pts[i];
		if (base != AV_NOPTS_VALUE) {
			/* Add a small delta (1 unit of stream time_base) to ensure strictly monotonic DTS */
			offset_ts[i] = base + 1;
		} else {
			offset_ts[i] = 0;
		}
	}

	/* 9. Write packets from input 2 (main run) */
	while (av_read_frame(ifmt_ctx2, pkt) >= 0) {
		const int stream_idx = pkt->stream_index;
		if (stream_idx < 0 || (unsigned int)stream_idx >= ifmt_ctx2->nb_streams) {
			av_packet_unref(pkt);
			continue;
		}

		const int out_idx = stream_map2[stream_idx];
		if (out_idx < 0 || out_idx >= out_stream_idx) {
			av_packet_unref(pkt);
			continue;
		}

		AVStream *in_stream = ifmt_ctx2->streams[stream_idx];
		AVStream *out_stream = ofmt_ctx->streams[out_idx];

		pkt->stream_index = out_idx;
		av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);

		const int64_t offset = offset_ts[out_idx];
		if (pkt->pts != AV_NOPTS_VALUE)
			pkt->pts += offset;
		if (pkt->dts != AV_NOPTS_VALUE) {
			pkt->dts += offset;
			if (last_dts[out_idx] != AV_NOPTS_VALUE && pkt->dts <= last_dts[out_idx])
				pkt->dts = last_dts[out_idx] + 1;
			last_dts[out_idx] = pkt->dts;
		}

		pkt->pos = -1;
		ret = av_interleaved_write_frame(ofmt_ctx, pkt);
		if (ret < 0) {
			blog(LOG_WARNING, "[SpeedrunConcat] Error muxing packet from main run (err: %d)", ret);
		}
		av_packet_unref(pkt);
	}

	/* 10. Write trailer */
	av_write_trailer(ofmt_ctx);
	success = true;
	blog(LOG_INFO, "[SpeedrunConcat] Successfully concatenated '%s' and '%s' -> '%s'", in_pre, in_main, out_final);

cleanup:
	if (pkt)
		av_packet_free(&pkt);
	if (ifmt_ctx1)
		avformat_close_input(&ifmt_ctx1);
	if (ifmt_ctx2)
		avformat_close_input(&ifmt_ctx2);
	if (ofmt_ctx) {
		if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE) && ofmt_ctx->pb)
			avio_closep(&ofmt_ctx->pb);
		avformat_free_context(ofmt_ctx);
	}
	return success;
}

struct concat_worker_params {
	char in_pre[512];
	char in_main[512];
	char out_final[512];
	bool delete_sources;
	speedrun_concat_callback callback;
	void *param;
};

static void *concat_worker_thread(void *arg)
{
	struct concat_worker_params *p = (struct concat_worker_params *)arg;
	if (!p)
		return NULL;

	bool ok = speedrun_concat_files(p->in_pre, p->in_main, p->out_final);
	if (ok && p->delete_sources) {
		os_unlink(p->in_pre);
		os_unlink(p->in_main);
	}

	if (p->callback) {
		p->callback(ok, p->out_final, p->param);
	}

	bfree(p);
	return NULL;
}

void speedrun_concat_files_async(const char *in_pre, const char *in_main, const char *out_final,
				 bool delete_sources_on_success, speedrun_concat_callback callback, void *param)
{
	struct concat_worker_params *p = bzalloc(sizeof(struct concat_worker_params));
	if (!p)
		return;

	snprintf(p->in_pre, sizeof(p->in_pre), "%s", in_pre);
	snprintf(p->in_main, sizeof(p->in_main), "%s", in_main);
	snprintf(p->out_final, sizeof(p->out_final), "%s", out_final);
	p->delete_sources = delete_sources_on_success;
	p->callback = callback;
	p->param = param;

	pthread_t tid;
	if (pthread_create(&tid, NULL, concat_worker_thread, p) == 0) {
		pthread_detach(tid);
	} else {
		blog(LOG_ERROR, "[SpeedrunConcat] Failed to create async worker thread");
		bfree(p);
	}
}
