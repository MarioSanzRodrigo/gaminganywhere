/*
 * Copyright (c) 2013 Chun-Ying Huang
 *
 * This file is part of GamingAnywhere (GA).
 *
 * GA is free software; you can redistribute it and/or modify it
 * under the terms of the 3-clause BSD License as published by the
 * Free Software Foundation: http://directory.fsf.org/wiki/License:BSD_3Clause
 *
 * GA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the 3-clause BSD License along with GA;
 * if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <cstdint>

#include "vsource.h"
#include "rtspconf.h"
#include "encoder-common.h"

#include "ga-common.h"
#include "ga-avcodec.h"
#include "ga-conf.h"
#include "ga-module.h"

#include "dpipe.h"

// { //RAL
extern "C" {
#include <libcjson/cJSON.h>

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libmediaprocsutils/log.h>
#include <libmediaprocsutils/check_utils.h>
#include <libmediaprocsutils/stat_codes.h>
#include <libmediaprocsutils/fifo.h>
#include <libmediaprocsutils/schedule.h>
#include <libmediaprocs/proc_if.h>
#include <libmediaprocs/procs.h>
#include <libmediaprocs/procs_api_http.h>
#include <libmediaprocs/proc.h>
#include <libmediaprocscodecs/ffmpeg_x264.h>
}

procs_ctx_t *procs_ctx= NULL; //RAL: FIXME!! //Take elsewhere
// } //RAL
static struct RTSPConf *rtspconf = NULL;

static int vencoder_initialized = 0;
static int vencoder_started = 0;
static pthread_t vencoder_tid[VIDEO_SOURCE_CHANNEL_MAX];
//// encoders for encoding
//static AVCodecContext *vencoder[VIDEO_SOURCE_CHANNEL_MAX];
// Mutex for reconfiguration settings
//static pthread_mutex_t vencoder_reconf_mutex[VIDEO_SOURCE_CHANNEL_MAX];
//static ga_ioctl_reconfigure_t vencoder_reconf[VIDEO_SOURCE_CHANNEL_MAX];

// specific data for h.264/h.265 //FIXME!!: //NOT USED
//static char *_sps[VIDEO_SOURCE_CHANNEL_MAX];
//static int _spslen[VIDEO_SOURCE_CHANNEL_MAX];
//static char *_pps[VIDEO_SOURCE_CHANNEL_MAX];
//static int _ppslen[VIDEO_SOURCE_CHANNEL_MAX];
//static char *_vps[VIDEO_SOURCE_CHANNEL_MAX];
//static int _vpslen[VIDEO_SOURCE_CHANNEL_MAX];

/* CRC-32C (iSCSI) polynomial in reversed bit order. */
#define POLY 0x82f63b78

/* CRC-32 (Ethernet, ZIP, etc.) polynomial in reversed bit order. */
/* #define POLY 0xedb88320 */

uint32_t crc32c(uint32_t crc, const unsigned char *buf, size_t len)
{
    int k;

    crc = ~crc;
    while (len--) {
        crc ^= *buf++;
        for (k = 0; k < 8; k++)
            crc = crc & 1 ? (crc >> 1) ^ POLY : crc >> 1;
    }
    return ~crc;
}

static int
vencoder_deinit(void *arg) {
	int iid;
	for(iid = 0; iid < video_source_channels(); iid++) {
		//if(vencoder[iid] != NULL)
		//	ga_avcodec_close(vencoder[iid]);
		//pthread_mutex_destroy(&vencoder_reconf_mutex[iid]);
		//vencoder[iid] = NULL;
		// RAL: //FIXME
		// { //RAL
		int ret_code= procs_opt(procs_ctx, "PROCS_ID_DELETE",
				iid/*enc_proc_id*/); // before joining to unblock processor
		// } //RAL
	}
	vencoder_initialized = 0;
	ga_error("video encoder: deinitialized.\n");
	return 0;
}

static int vencoder_init(void *arg)
{
	int iid;
	char *pipefmt = (char*) arg;
	struct RTSPConf *rtspconf = rtspconf_global();
	//
	if(rtspconf == NULL) {
		ga_error("video encoder: no configuration found\n");
		return -1;
	}
	if(vencoder_initialized != 0)
		return 0;

	// { //RAL
	/* Register encoder, decoder, RTSP multiplexer and RTSP de-multiplexer
	 * processor types.
	 */
	procs_module_opt("PROCS_REGISTER_TYPE", &proc_if_ffmpeg_x264_enc);
	/*if(procs_module_opt("PROCS_REGISTER_TYPE", &proc_if_ffmpeg_x264_enc)!= STAT_SUCCESS) {
		fprintf(stderr, "Error at line: %d\n", __LINE__);
		exit(-1);
	}*/

	/* Get PROCS module's instance */ //RAL //FIXME!! //take elsewhere
	if((procs_ctx= procs_open(NULL))== NULL) {
		fprintf(stderr, "Error at line: %d\n", __LINE__);
		exit(-1);
	}
	// }
	//
	for(iid = 0; iid < video_source_channels(); iid++) {
		char pipename[64];
		int outputW, outputH;
		dpipe_t *pipe;
		//
//_sps[iid] = _pps[iid] = NULL;
//_spslen[iid] = _ppslen[iid] = 0;
//pthread_mutex_init(&vencoder_reconf_mutex[iid], NULL);
//vencoder_reconf[iid].id = -1;
		snprintf(pipename, sizeof(pipename), pipefmt, iid);
		outputW = video_source_out_width(iid);
		outputH = video_source_out_height(iid);
		if((pipe = dpipe_lookup(pipename)) == NULL) {
			ga_error("video encoder: pipe %s is not found\n", pipename);
			goto init_failed;
		}
		ga_error("video encoder: video source #%d from '%s' (%dx%d).\n",
			iid, pipe->name, outputW, outputH);

		//RAL //FIXME!!: Configuration here using textual 'rtspconf->vso'
		// {
		{
			int ret_code;
			char *rest_str= NULL;
			char proc_settings[128]= {0};
			//cJSON *cjson_rest= NULL, *cjson_aux= NULL;

			snprintf(proc_settings, sizeof(proc_settings),
					"width_output=%d&height_output=%d", outputW, outputH);
			ret_code= procs_opt(procs_ctx, "PROCS_POST", "ffmpeg_x264_enc"/*proc_name*/, proc_settings, &rest_str);
			if(ret_code!= STAT_SUCCESS || rest_str== NULL) {
				fprintf(stderr, "Error at line: %d\n", __LINE__);
				exit(-1);
			}
// We "know" id==iid //FIXME!!
			//if((cjson_rest= cJSON_Parse(rest_str))== NULL) {
			//	fprintf(stderr, "Error at line: %d\n", __LINE__);
			//	exit(-1);
			//}
			//if((cjson_aux= cJSON_GetObjectItem(cjson_rest, "proc_id"))== NULL) {
			//	fprintf(stderr, "Error at line: %d\n", __LINE__);
			//	exit(-1);
			//}
			//if((*ref_proc_id= cjson_aux->valuedouble)< 0) {
			//	fprintf(stderr, "Error at line: %d\n", __LINE__);
			//	exit(-1);
			//}
			free(rest_str); rest_str= NULL;
			//cJSON_Delete(cjson_rest); cjson_rest= NULL;
		}
		// }
		//vencoder[iid] = ga_avcodec_vencoder_init(NULL,
		//		rtspconf->video_encoder_codec,
		//		outputW, outputH,
		//		rtspconf->video_fps, rtspconf->vso);
		//if(vencoder[iid] == NULL)
		//	goto init_failed;
	}
	vencoder_initialized = 1;
	ga_error("video encoder: initialized.\n");
	return 0;
init_failed:
	vencoder_deinit(NULL);
	return -1;
}

static void* mux_thr(void *t) // RAL: FIXME!!
{
	//thr_ctx_t *thr_ctx= (thr_ctx_t*)t;
	proc_frame_ctx_t *proc_frame_ctx= NULL;

	/* Check argument */
	/*if(thr_ctx== NULL) {
		fprintf(stderr, "Bad argument 'consumer_thr()'\n");
		exit(1);
	}*/

	/* Get frame from encoder and send to multiplexer */
	while(vencoder_started != 0 && encoder_running() > 0/*thr_ctx->flag_exit== 0*/) {
		int ret_code;

		/* Receive encoded frame */
		if(proc_frame_ctx!= NULL)
			proc_frame_ctx_release(&proc_frame_ctx);
		ret_code= procs_recv_frame(procs_ctx, 0/*enc_proc_id*/,
				&proc_frame_ctx);
		if(ret_code!= STAT_SUCCESS) {
			if(ret_code== STAT_EAGAIN)
				schedule(); // Avoid closed loops
			else
				fprintf(stderr, "Error while encoding frame'\n");
			continue;
		}

		/* Send encoded frame to multiplexer.
		 * IMPORTANT: Set correctly the elementary stream Id. to be able to
		 * correctly multiplex each frame.
		 */
		if(proc_frame_ctx== NULL)
			continue;
		fprintf(stderr, "Got frame!!'\n");

		{
			register int data_size;
			AVPacket *avpacket= NULL;
			struct timeval tv;
			LOG_CTX_INIT(NULL);

			/* Allocate FFmpeg's packet structure */
			avpacket= av_packet_alloc(); // Calls 'av_init_packet()' internally
			ASSERT(avpacket!= NULL);

			/* Input encoded data only uses one data plane; note that:
			 * - 'proc_frame_ctx->width[0]': represents the size of the packet in
			 * bytes;
			 * - 'proc_frame_ctx->p_data[0]': is the pointer to the packet data.
			 */
			data_size= proc_frame_ctx->width[0];
			ret_code= av_new_packet(avpacket, data_size);
			ASSERT(ret_code== 0 && avpacket->data!= NULL &&
					avpacket->size== data_size);
			memcpy(avpacket->data, proc_frame_ctx->p_data[0], data_size);

			/* Copy presentation and decoding time-stamps */
			avpacket->pts= proc_frame_ctx->pts;
			avpacket->dts= proc_frame_ctx->dts;
			avpacket->stream_index= proc_frame_ctx->es_id;

			// send the packet
			if(encoder_send_packet("video-encoder",
				0/*iid*//*rtspconf->video_id*/, avpacket,
				avpacket->pts, &tv) < 0) {
				fprintf(stderr, "Error while multiplexing frame'\n"); //goto video_quit;
			}
			//if(video_written == 0) {
			//	video_written = 1;
			//	ga_error("first video frame written (pts=%lld)\n", pts);
			//}
			if(avpacket!= NULL) {
				//avpacket_release((void**)&avpacket);
				av_packet_free((AVPacket**)
						&avpacket); //<- Internally set pointer to NULL
				avpacket= NULL; // redundant
			}
		}

		/*proc_frame_ctx->es_id= thr_ctx->elem_strem_id_video_server;
		ret_code= procs_send_frame(thr_ctx->procs_ctx, thr_ctx->mux_proc_id,
				proc_frame_ctx);
		if(ret_code!= STAT_SUCCESS) {
			if(ret_code== STAT_EAGAIN)
				schedule(); // Avoid closed loops
			else
				fprintf(stderr, "Error while multiplexing frame'\n");
			continue;
		}*/
	}

	if(proc_frame_ctx!= NULL)
		proc_frame_ctx_release(&proc_frame_ctx);
	return NULL;
}

static void *
vencoder_threadproc(void *arg) {
	// arg is pointer to source pipename
	int iid, outputW, outputH;
	vsource_frame_t *frame = NULL;
	char *pipename = (char*) arg;
	dpipe_t *pipe = dpipe_lookup(pipename);
	dpipe_buffer_t *data = NULL;
//AVCodecContext *encoder = NULL;
	pthread_t mux_thread; //RAL
	int ret_code; //RAL
	//
	AVFrame *pic_in = NULL;
	unsigned char *pic_in_buf = NULL;
	int pic_in_size;
	unsigned char *nalbuf = NULL, *nalbuf_a = NULL;
	int nalbuf_size = 0, nalign = 0;
	long long basePts = -1LL, newpts = 0LL, pts = -1LL, ptsSync = 0LL;
	pthread_mutex_t condMutex = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
	//
	int video_written = 0;
	//
	if(pipe == NULL) {
		ga_error("video encoder: invalid pipeline specified (%s).\n", pipename);
		goto video_quit;
	}
	//
	rtspconf = rtspconf_global();
	// init variables
	iid = pipe->channel_id;
//encoder = vencoder[iid];
	//
	outputW = video_source_out_width(iid);
	outputH = video_source_out_height(iid);
	//
	encoder_pts_clear(iid);
	//
	nalbuf_size = 100000+12 * outputW * outputH;
	nalbuf_size+= 4; //RAL: To be able to add CRC32 at the end
	if(ga_malloc(nalbuf_size, (void**) &nalbuf, &nalign) < 0) {
		ga_error("video encoder: buffer allocation failed, terminated.\n");
		goto video_quit;
	}
	nalbuf_a = nalbuf + nalign;
	//
	if((pic_in = av_frame_alloc()) == NULL) {
		ga_error("video encoder: picture allocation failed, terminated.\n");
		goto video_quit;
	}
	pic_in->width = outputW;
	pic_in->height = outputH;
	pic_in->format = AV_PIX_FMT_YUV420P;
	pic_in_size = avpicture_get_size(AV_PIX_FMT_YUV420P, outputW, outputH);
	if((pic_in_buf = (unsigned char*) av_malloc(pic_in_size)) == NULL) {
		ga_error("video encoder: picture buffer allocation failed, terminated.\n");
		goto video_quit;
	}
	avpicture_fill((AVPicture*) pic_in, pic_in_buf,
			AV_PIX_FMT_YUV420P, outputW, outputH);
	//ga_error("video encoder: linesize = %d|%d|%d\n", pic_in->linesize[0], pic_in->linesize[1], pic_in->linesize[2]);
	// start encoding
	ga_error("video encoding started: tid=%ld %dx%d@%dfps, nalbuf_size=%d, pic_in_size=%d.\n",
		ga_gettid(),
		outputW, outputH, rtspconf->video_fps,
		nalbuf_size, pic_in_size);
	//

	ret_code= pthread_create(&mux_thread, NULL, mux_thr, NULL); //RAL
	if(ret_code!= 0) {
		fprintf(stderr, "Error at line: %d\n", __LINE__);
		exit(-1);
	}

	while(vencoder_started != 0 && encoder_running() > 0) {
// Reconfigure encoder (if required) //FIXME!!: NOT USED
//vencoder_reconfigure(iid); //FIXME!!: NOT USED
		proc_frame_ctx_t proc_frame_ctx= {0};
		AVPacket pkt;
		int got_packet = 0;
		// wait for notification
		struct timeval tv;
		struct timespec to;
		gettimeofday(&tv, NULL);
		to.tv_sec = tv.tv_sec+1;
		to.tv_nsec = tv.tv_usec * 1000;
		data = dpipe_load(pipe, &to);
		if(data == NULL) {
			ga_error("viedo encoder: image source timed out.\n");
			continue;
		}
		frame = (vsource_frame_t*) data->pointer;
		// handle pts
		if(basePts == -1LL) {
			basePts = frame->imgpts;
			ptsSync = encoder_pts_sync(rtspconf->video_fps);
			newpts = ptsSync;
		} else {
			newpts = ptsSync + frame->imgpts - basePts;
		}
		// XXX: assume always YUV420P
		if(pic_in->linesize[0] == frame->linesize[0]
		&& pic_in->linesize[1] == frame->linesize[1]
		&& pic_in->linesize[2] == frame->linesize[2]) {
			bcopy(frame->imgbuf, pic_in_buf, pic_in_size);
		} else {
			ga_error("video encoder: YUV mode failed - mismatched linesize(s) (src:%d,%d,%d; dst:%d,%d,%d)\n",
				frame->linesize[0], frame->linesize[1], frame->linesize[2],
				pic_in->linesize[0], pic_in->linesize[1], pic_in->linesize[2]);
			dpipe_put(pipe, data);
			goto video_quit;
		}
		tv = frame->timestamp;
		dpipe_put(pipe, data);
		// pts must be monotonically increasing
		if(newpts > pts) {
			pts = newpts;
		} else {
			pts++;
		}
		// encode
		encoder_pts_put(iid, pts, &tv);
		pic_in->pts = pts;
		av_init_packet(&pkt);
		pkt.data = nalbuf_a;
		pkt.size = nalbuf_size;

	    proc_frame_ctx.data= pic_in_buf;
	    proc_frame_ctx.p_data[0]= pic_in->data[0];
	    proc_frame_ctx.p_data[1]= pic_in->data[1];
	    proc_frame_ctx.p_data[2]= pic_in->data[2];
	    proc_frame_ctx.linesize[0]= pic_in->linesize[0];
	    proc_frame_ctx.linesize[1]= pic_in->linesize[1];
	    proc_frame_ctx.linesize[2]= pic_in->linesize[2];
	    proc_frame_ctx.width[0]= pic_in->width;
	    proc_frame_ctx.width[1]= pic_in->width>> 1;
	    proc_frame_ctx.width[2]= pic_in->width>> 1;
	    proc_frame_ctx.height[0]= pic_in->height;
	    proc_frame_ctx.height[1]= pic_in->height>> 1;
	    proc_frame_ctx.height[2]= pic_in->height>> 1;
	    proc_frame_ctx.proc_sample_fmt= PROC_IF_FMT_YUV420P;
	    proc_frame_ctx.es_id= 0;
	    proc_frame_ctx.pts= pts;

        /* Encode the image */
        procs_send_frame(procs_ctx, iid/*enc_proc_id*/, &proc_frame_ctx);

	    //if(avcodec_encode_video2(encoder, &pkt, pic_in, &got_packet) < 0) {
	    //	ga_error("video encoder: encode failed, terminated.\n");
	    //	goto video_quit;
	    //}
	    //if(got_packet) {
	    //	if(pkt.pts == (int64_t) AV_NOPTS_VALUE) {
	    //		pkt.pts = pts;
	    //	}
	    //	pkt.stream_index = 0;
			//
	    //	if(pkt.pts != AV_NOPTS_VALUE) {
	    //		if(encoder_ptv_get(iid, pkt.pts, &tv, 0) == NULL) {
	    //			gettimeofday(&tv, NULL);
	    //		}
	    //	} else {
	    //		gettimeofday(&tv, NULL);
	    //	}

/*			if(pkt.data!= NULL && (encoder->codec_id== AV_CODEC_ID_LHE ||
					encoder->codec_id== AV_CODEC_ID_MLHE)) {
				uint32_t crc_32= crc32c(0, pkt.data, pkt.size); // include all pay-load bytes (except CRC)
				pkt.data[pkt.size++]= crc_32>> 24;
				pkt.data[pkt.size++]= (crc_32>> 16)& 0xFF;
				pkt.data[pkt.size++]= (crc_32>>  8)& 0xFF;
				pkt.data[pkt.size++]= crc_32& 0xFF;
				printf("%s %d: crc_32: %u\n", __FILE__, __LINE__, crc_32); fflush(stdout); //comment-me
			}
*/
			// send the packet
			//if(encoder_send_packet("video-encoder",
			//	iid/*rtspconf->video_id*/, &pkt,
			//	pkt.pts, &tv) < 0) {
			//	goto video_quit;
			//}
			//if(video_written == 0) {
			//	video_written = 1;
			//	ga_error("first video frame written (pts=%lld)\n", pts);
			//}
		//} if(got_packet)
	}
	//
video_quit:
	if(pipe) {
		pipe = NULL;
	}
	//
	if(pic_in_buf)	av_free(pic_in_buf);
	if(pic_in)	av_free(pic_in);
	if(nalbuf)	free(nalbuf);
	//
	ga_error("video encoder: thread terminated (tid=%ld).\n", ga_gettid());
	//
	return NULL;
}

static int
vencoder_start(void *arg) {
	int iid;
	char *pipefmt = (char*) arg;
#define	MAXPARAMLEN	64
	static char pipename[VIDEO_SOURCE_CHANNEL_MAX][MAXPARAMLEN];
	if(vencoder_started != 0)
		return 0;
	vencoder_started = 1;
	for(iid = 0; iid < video_source_channels(); iid++) {
		snprintf(pipename[iid], MAXPARAMLEN, pipefmt, iid);
		if(pthread_create(&vencoder_tid[iid], NULL, vencoder_threadproc, pipename[iid]) != 0) {
			vencoder_started = 0;
			ga_error("video encoder: create thread failed.\n");
			return -1;
		}
	}
	ga_error("video encdoer: all started (%d)\n", iid);
	return 0;
}

static int
vencoder_stop(void *arg) {
	int iid;
	void *ignored;
	if(vencoder_started == 0)
		return 0;
	vencoder_started = 0;
	for(iid = 0; iid < video_source_channels(); iid++) {
		pthread_join(vencoder_tid[iid], &ignored);
	}
	ga_error("video encdoer: all stopped (%d)\n", iid);
	return 0;
}
#if 0 //RAL //FIXME!!
static void *
vencoder_raw(void *arg, int *size) {
#if defined __APPLE__
	int64_t in = (int64_t) arg;
	int iid = (int) (in & 0xffffffffLL);
#elif defined __x86_64__
	int iid = (long long) arg;
#else
	int iid = (int) arg;
#endif
	if(vencoder_initialized == 0)
		return NULL;
	if(size)
		*size = sizeof(vencoder[iid]);
	return vencoder[iid];
}
#endif
static int
vencoder_ioctl(int command, int argsize, void *arg) {
//RAL: FIXME!! // WONT BE USED
	return GA_IOCTL_ERR_NOTSUPPORTED;
}

ga_module_t *
module_load() {
	static ga_module_t m;
	//struct RTSPConf *rtspconf = rtspconf_global();
	char mime[64];
	//
	bzero(&m, sizeof(m));
	m.type = GA_MODULE_TYPE_VENCODER;
	m.name = strdup("ffmpeg-video-encoder");
	if(ga_conf_readv("video-mimetype", mime, sizeof(mime)) != NULL) {
		m.mimetype = strdup(mime);
	}
	m.init = vencoder_init;
	m.start = vencoder_start;
	//m.threadproc = vencoder_threadproc;
	m.stop = vencoder_stop;
	m.deinit = vencoder_deinit;
	//
	m.raw = NULL; //vencoder_raw; //RAL: FIXME!!
	m.ioctl = vencoder_ioctl;
	return &m;
}

