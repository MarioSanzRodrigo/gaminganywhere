/*
 * Copyright (c) 2013-2014 Chun-Ying Huang
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

#include "rtspclient.h"

/* MediaProcessors's library related */
extern "C" {
#include <libcjson/cJSON.h>
#include <libmediaprocsutils/log.h>
#include <libmediaprocsutils/stat_codes.h>
#include <libmediaprocsutils/schedule.h>
#include <libmediaprocsutils/fifo.h>
#include <libmediaprocs/proc_if.h>
#include <libmediaprocs/procs.h>
#include <libmediaprocsmuxers/live555_rtsp.h>
#include <libmediaprocsmuxers/payloader_upm_rtsp.h>
#include <libmediaprocscodecs/ffmpeg_x264.h>
#include <libmediaprocscodecs/ffmpeg_m2v.h>
#include <libmediaprocscodecs/ffmpeg_mp3.h>
#include <libmediaprocscodecs/ffmpeg_lhe.h>
}

/* RTSP latency profiler */
#define PROFILE_MUX_LATENCY_E2E
#define PROFILE_LATENCY_E2E_GET_TIMESTAMP(FRAME) \
	profile_latency_e2e_get_timestamp(FRAME)
#ifdef PROFILE_MUX_LATENCY_E2E
#else
#define PROFILE_LATENCY_E2E_GET_TIMESTAMP(FRAME)
#endif

/* Prototypes */
static void* rtsp_thread(void *t);
static void* consumer_thr_video(void *t);
static void* consumer_thr_audio(void *t);
static int launch_decoding_thread(struct RTSPThreadParam *rtspThreadParam,
		const char *sdp_mimetype, int muxer_es_id);
static void join_decoding_thread(struct RTSPThreadParam *rtspThreadParam);
static void procs_post(procs_ctx_t *procs_ctx, const char *proc_name,
		const char *proc_settings, int *ref_proc_id);

/* Implementations */

struct RTSPConf *rtspconf; // Terrible global usage :(

void rtsperror(const char *fmt, ...)
{
	static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
	va_list ap;
	pthread_mutex_lock(&mutex);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	pthread_mutex_unlock(&mutex);
	return;
}

static inline void profile_latency_e2e_get_timestamp(
		proc_frame_ctx_t *proc_frame_ctx)
{
	int lsize, height, width;
	struct timespec t_lat;
	uint32_t send_usecs, recv_usecs, latency= 0;
	const int sizeof_uint32= sizeof(uint32_t);
	const int avg_cnt_max= 60; // 60 frames average
	static int64_t latency_avg= 0;
	static int avg_cnt= 0;

	/* Check arguments */
	if(proc_frame_ctx== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		return;
	}

	lsize= proc_frame_ctx->linesize[0];
	height= proc_frame_ctx->height[0];
	width= proc_frame_ctx->width[0];

	/* We should have compressed 1-dimensional data in our buffer (data
	 * is pointed by the first frame) and a 32-bit STC value attached at
	 * the end of the buffer (pointed by the second frame).
	 *  */
	if(proc_frame_ctx->p_data[0]== NULL || !(lsize> 0) || height!= 1 ||
			proc_frame_ctx->p_data[1]!= NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		return;
	}

	/* Correct the frame dimensions (exclude STC allocation) */
	proc_frame_ctx->linesize[0]= lsize- sizeof_uint32;
	proc_frame_ctx->width[0]= width- sizeof_uint32;

	/* Store STC time-stamp */
	clock_gettime(CLOCK_MONOTONIC, &t_lat);
	recv_usecs= (uint32_t)
			((uint64_t)t_lat.tv_sec*1000000+ (uint64_t)t_lat.tv_nsec/1000);
	send_usecs=  proc_frame_ctx->p_data[0][width- 4]<< 24;
	send_usecs|= proc_frame_ctx->p_data[0][width- 3]<< 16;
	send_usecs|= proc_frame_ctx->p_data[0][width- 2]<< 8;
	send_usecs|= proc_frame_ctx->p_data[0][width- 1];
	//printf("recv: 0x%0x, send: 0x%0x\n", recv_usecs, send_usecs); //comment-me
	//fflush(stdout); //comment-me
	if(recv_usecs> send_usecs)
		latency= recv_usecs- send_usecs;
	else rtsperror("'%s' failed. Line %d\n",
			__FUNCTION__, __LINE__); //comment-me

	/* Trace latency if applicable */
	if(latency> 0) {
		latency_avg+= latency;
		avg_cnt++;
	}
	if(avg_cnt> 0 && avg_cnt>= avg_cnt_max) {
		printf("\nRTSP Latency average over %d frames [usecs]: %u\n", avg_cnt,
				latency_avg/avg_cnt);
		latency_avg= 0;
		avg_cnt= 0;
	}
}

int rtsp_client_init(struct RTSPThreadParam *rtspThreadParam)
{
	int i, ret_code;
	procs_ctx_t *procs_ctx= NULL;
	fifo_elem_alloc_fxn_t fifo_elem_alloc_fxn= {
			.elem_ctx_dup= (fifo_elem_ctx_dup_fxn_t*)
			proc_frame_ctx_dup,
			.elem_ctx_release= (fifo_elem_ctx_release_fxn_t*)
			proc_frame_ctx_release
	};
	char proc_settings[1024]= {0};

	/* Check argument */
	if(rtspThreadParam== NULL) {
		rtsperror("Bad argument 'rtsp_thread()'\n");
		return -1;
	}

	/* Create PROCS module insance to be used for audio and video codecs */
	if((procs_ctx= procs_open(NULL))== NULL) {
		ga_error("Could not instantiate processors module.\n");
		return -1;
	}
	rtspThreadParam->procs_ctx= procs_ctx;

	/* Create FIFO's for decoder -> renderer interfacing */
	for(i= 0; i< VIDEO_SOURCE_CHANNEL_MAX; i++) {
		rtspThreadParam->fifo_ctx_video_array[i]= fifo_open(
				RENDERER_FIFO_SIZE, 0, &fifo_elem_alloc_fxn);
		if(rtspThreadParam->fifo_ctx_video_array[i]== NULL) {
			rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
			return -1;
		}
	}

    /* Register RTSP de-multiplexer instance and get corresponding Id. */
	snprintf(proc_settings, sizeof(proc_settings), "rtsp_url=%s",
			rtspThreadParam->url);
	procs_post(procs_ctx, proc_if_live555_rtsp_dmux.proc_name, proc_settings,
			&rtspThreadParam->dmux_proc_id);

	/* Launch demultiplexing thread */
	rtspThreadParam->running= true;
	ret_code= pthread_create(&rtspThreadParam->rtspthread, NULL, rtsp_thread,
			(void*)rtspThreadParam);
	if(ret_code!= 0) {
		rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		return -1;
	}

	return 0;
}

void rtsp_client_deinit(struct RTSPThreadParam *rtspThreadParam)
{
	int i, ret_code;
	void *thread_end_code= NULL;

	if(rtspThreadParam== NULL)
		return;

	rtspThreadParam->running= false;

	/* Unblock FIFO's for decoders -> renderer interfacing */
	for(int i= 0; i< VIDEO_SOURCE_CHANNEL_MAX; i++)
		fifo_set_blocking_mode(rtspThreadParam->fifo_ctx_video_array[i], 0);

	/* Delete (thus unblock) demultiplexer processor */
	ret_code= procs_opt(rtspThreadParam->procs_ctx, "PROCS_ID_DELETE",
			rtspThreadParam->dmux_proc_id);
	if(ret_code!= STAT_SUCCESS)
		fprintf(stderr, "Error at line: %d\n", __LINE__);

	/* Join audio decoding thread */
	rtsperror("Waiting thread to join... "); //comment-me
	pthread_join(rtspThreadParam->rtspthread, &thread_end_code);
	if(thread_end_code!= NULL) {
		if(*((int*)thread_end_code)!= 0) {
			rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		}
		free(thread_end_code);
		thread_end_code= NULL;
	}
	rtsperror("joined O.K.\n"); //comment-me

	/* Release FIFO's for decoders -> renderer interfacing */
	for(i= 0; i< VIDEO_SOURCE_CHANNEL_MAX; i++)
		fifo_close(&rtspThreadParam->fifo_ctx_video_array[i]);

	/* Close MediaProcessors's processors (PROCS) module instance */
	if(rtspThreadParam->procs_ctx!= NULL)
		procs_close(&rtspThreadParam->procs_ctx);
}

/**
 * De-multiplexer thread.
 */
static void* rtsp_thread(void *t)
{
	int i, ret_code, dec_proc_id, dmux_proc_id= -1, *ref_end_code= NULL;
	int elementary_streams_cnt= 0;
	struct RTSPThreadParam *rtspThreadParam= (struct RTSPThreadParam*)t;
	proc_frame_ctx_t *proc_frame_ctx= NULL;
	procs_ctx_t *procs_ctx= NULL;
	char *rest_str= NULL;
	cJSON *cjson_rest= NULL, *cjson_es_array= NULL, *cjson_aux= NULL;

	/* Allocate return context; initialize to a default 'ERROR' value */
	if((ref_end_code= (int*)malloc(sizeof(int)))== NULL) {
		rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		return NULL;
	}
	*ref_end_code= -1; // "error status" by default

	/* Check argument */
	if(rtspThreadParam== NULL) {
		rtsperror("Bad argument 'rtsp_thread()'\n");
		goto end;
	}

	/* Get processors module instance */
	if((procs_ctx= rtspThreadParam->procs_ctx)== NULL) {
		rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}

	/* Get demultiplexer processor Id. */
	if((dmux_proc_id= rtspThreadParam->dmux_proc_id)< 0) {
		rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}

	/* Initialize video codecs source array */
	rtspThreadParam->iid_max= 0;
	for(i= 0; i< VIDEO_SOURCE_CHANNEL_MAX; i++) {
		vdecoder_thread_t *vdecoder_thread=
				&rtspThreadParam->vdecoder_thread_array[i];
		vdecoder_thread->iid= i;
		vdecoder_thread->video_dec_proc_id= -1;
		vdecoder_thread->video_muxer_es_id= -1;
		vdecoder_thread->rtspThreadParam= rtspThreadParam;
	}

	/* Initialize audio codecs source array */
	rtspThreadParam->audio_dec_proc_id= -1;
	rtspThreadParam->audio_muxer_es_id= -1;

	/* Receive first frame from de-multiplexer -EPILOGUE-.
	 * The first time we receive data we have to check the elementary stream
	 * Id's. The idea is to use the elementary stream Id's to send each
	 * de-multiplexed frame to the correct decoding sink.
	 * We do this once, the first time we are receiving any frame,
	 * by consulting the de-multiplexer API.
	 */
	ret_code= STAT_EAGAIN;
	while(ret_code!= STAT_SUCCESS && rtspThreadParam->running== true) {
		schedule(); // Avoid closed loops
		ret_code= procs_recv_frame(procs_ctx, dmux_proc_id, &proc_frame_ctx);
	}
	if(ret_code!= STAT_SUCCESS || proc_frame_ctx== NULL) {
		if(rtspThreadParam->running== true)
			rtsperror("Error at line: %d\n", __LINE__);
		else
			*ref_end_code= 0; // "success status"
		goto end;
	}

	/* Parse elementary streams */
	ret_code= procs_opt(procs_ctx, "PROCS_ID_GET", dmux_proc_id, &rest_str);
	if(ret_code!= STAT_SUCCESS || rest_str== NULL) {
		rtsperror("Error at line: %d\n", __LINE__);
		goto end;
	}
	if((cjson_rest= cJSON_Parse(rest_str))== NULL) {
		rtsperror("Error at line: %d\n", __LINE__);
		goto end;
	}
	// Elementary streams objects array
	if((cjson_es_array= cJSON_GetObjectItem(cjson_rest,
			"elementary_streams"))== NULL) {
		rtsperror("Error at line: %d\n", __LINE__);
		goto end;
	}
	// Iterate elementary stream objects and launch decoding thread if
	// applicable
	elementary_streams_cnt= cJSON_GetArraySize(cjson_es_array);
	for(i= 0; i< elementary_streams_cnt; i++) {
		cJSON *cjson_es= cJSON_GetArrayItem(cjson_es_array, i);
		if(cjson_es!= NULL) {
			int elementary_stream_id;
			char *sdp_mimetype;

			/* Get stream Id. */
			cjson_aux= cJSON_GetObjectItem(cjson_es, "elementary_stream_id");
			if(cjson_aux== NULL) {
				rtsperror("Error at line: %d\n", __LINE__);
				continue;
			}
			elementary_stream_id= cjson_aux->valueint;

			/* Get MIME type */
			cjson_aux= cJSON_GetObjectItem(cjson_es, "sdp_mimetype");
			if(cjson_aux== NULL) {
				rtsperror("Error at line: %d\n", __LINE__);
				continue;
			}
			sdp_mimetype= cjson_aux->valuestring;

			ret_code= launch_decoding_thread(rtspThreadParam, sdp_mimetype,
					elementary_stream_id);
			if(ret_code!= STAT_SUCCESS) {
				rtsperror("Error at line: %d\n", __LINE__);
				continue;
			}
		}
	}
	free(rest_str); rest_str= NULL;
	cJSON_Delete(cjson_rest); cjson_rest= NULL;

	/* Get decoder processor id. to know which one to send the frame */
	dec_proc_id= -1;
	if(proc_frame_ctx->es_id== rtspThreadParam->audio_muxer_es_id) {
		dec_proc_id= rtspThreadParam->audio_dec_proc_id;
	} else {
		for(int i= 0; i< rtspThreadParam->iid_max; i++) {
			vdecoder_thread_t *vdecoder_thread=
					&rtspThreadParam->vdecoder_thread_array[i];
			if(proc_frame_ctx->es_id== vdecoder_thread->video_muxer_es_id) {
				dec_proc_id= vdecoder_thread->video_dec_proc_id;
				break;
			}
		}
	}

	/* Send first received frame to decoder */
	ret_code= procs_send_frame(procs_ctx, dec_proc_id, proc_frame_ctx);
	if(ret_code!= STAT_SUCCESS)
		rtsperror("Error while decoding frame'\n");

	/* De-multiplexer loop */
	while(rtspThreadParam->running== true) {

		/* Receive frame from de-multiplexer */
		if(proc_frame_ctx!= NULL)
			proc_frame_ctx_release(&proc_frame_ctx);
		ret_code= procs_recv_frame(procs_ctx, dmux_proc_id, &proc_frame_ctx);
		if(ret_code!= STAT_SUCCESS) {
			if(ret_code== STAT_EAGAIN)
				schedule(); // Avoid closed loops
			else
				rtsperror("Error while de-multiplexing frame'\n");
			continue;
		}

		/* Send received encoded frame to decoder */
		if(proc_frame_ctx== NULL)
			continue;

		/* Get decoder processor id. to know which one to send the frame */
		dec_proc_id= -1;
		if(proc_frame_ctx->es_id== rtspThreadParam->audio_muxer_es_id) {
			dec_proc_id= rtspThreadParam->audio_dec_proc_id;
		} else {

			/* Video only */
			PROFILE_LATENCY_E2E_GET_TIMESTAMP(proc_frame_ctx);

			for(int i= 0; i< rtspThreadParam->iid_max; i++) {
				vdecoder_thread_t *vdecoder_thread=
						&rtspThreadParam->vdecoder_thread_array[i];
				if(proc_frame_ctx->es_id== vdecoder_thread->video_muxer_es_id) {
					dec_proc_id= vdecoder_thread->video_dec_proc_id;
					break;
				}
			}
		}

		ret_code= procs_send_frame(procs_ctx, dec_proc_id, proc_frame_ctx);
		if(ret_code!= STAT_SUCCESS) {
			if(ret_code== STAT_EAGAIN)
				schedule(); // Avoid closed loops
			else
				rtsperror("Error while decoding frame'\n");
			continue;
		}
	}

	*ref_end_code= 0; // "success status"
end:
	if(proc_frame_ctx!= NULL)
		proc_frame_ctx_release(&proc_frame_ctx);
	join_decoding_thread(rtspThreadParam);
	return (void*)ref_end_code;
}

static void* consumer_thr_video(void *t)
{
	int iid, video_dec_proc_id, ret_code, *ref_end_code= NULL;
	vdecoder_thread_t *vdecoder_thread= (vdecoder_thread_t*)t;
	struct RTSPThreadParam *rtspThreadParam= NULL;
	procs_ctx_t *procs_ctx= NULL;
	proc_frame_ctx_t *proc_frame_ctx= NULL;

	/* Allocate return context; initialize to a default 'ERROR' value */
	if((ref_end_code= (int*)malloc(sizeof(int)))== NULL) {
		rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		return NULL;
	}
	*ref_end_code= -1; // "error status" by default

	/* Check arguments */
	if(vdecoder_thread== NULL) {
		rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}

	/* Get variables */
	if((rtspThreadParam= vdecoder_thread->rtspThreadParam)== NULL) {
		rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if((procs_ctx= rtspThreadParam->procs_ctx)== NULL) {
		rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if((video_dec_proc_id= vdecoder_thread->video_dec_proc_id)< 0) {
		rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	iid= vdecoder_thread->iid;

	/* **** Output loop **** */
	while(rtspThreadParam->running== true) {
		union SDL_Event evt;

		/* Receive decoded frame */
		if(proc_frame_ctx!= NULL)
			proc_frame_ctx_release(&proc_frame_ctx);
		ret_code= procs_recv_frame(procs_ctx, video_dec_proc_id,
				&proc_frame_ctx);
		if(ret_code!= STAT_SUCCESS) {
			if(ret_code== STAT_EAGAIN)
				schedule(); // Avoid closed loops
			else
				rtsperror("Error while receiving decoded frame'\n");
			continue;
		}

		/* **** Write encoded-decoded frame to output if applicable **** */

		if(proc_frame_ctx== NULL)
			continue;

		/* Write frame to input FIFO.
		 * Implementation note: we just pass the frame pointer, do not
		 * perform memory allocations.
		 */
		ret_code= fifo_put(rtspThreadParam->fifo_ctx_video_array[iid],
				(void**)&proc_frame_ctx, sizeof(void*));
		if(ret_code== STAT_SUCCESS)
			proc_frame_ctx= NULL; // succeed, avoid double referencing

		/* Push rendering event */
		memset(&evt, 0, sizeof(evt));
		evt.user.type= SDL_USEREVENT;
		evt.user.timestamp= time(0);
		evt.user.code= SDL_USEREVENT_RENDER_IMAGE;
		evt.user.data1= rtspThreadParam;
		evt.user.data2= (void*)(long long)iid;
		SDL_PushEvent(&evt);
	}

	*ref_end_code= 0; // "success status"
end:
	if(proc_frame_ctx!= NULL)
		proc_frame_ctx_release(&proc_frame_ctx);
	return (void*)ref_end_code;
}

static int audio_device_open(struct RTSPThreadParam *rtspThreadParam,
		SDL_AudioDeviceID *ref_sdl_dev)
{
	int end_code= -1; // '-1' means 'ERROR'
	SDL_AudioSpec sdl_audio_spec= {0}, sdl_audio_spec_wanted= {0};
	SDL_AudioDeviceID sdl_dev= 0;

	/* Check arguments */
	if(rtspThreadParam== NULL) {
		rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		return -1;
	}

	*ref_sdl_dev= 0;

	/* Initialize audio reproduction resources (SDL) */
	SDL_memset(&sdl_audio_spec_wanted, 0, sizeof(sdl_audio_spec_wanted));
	sdl_audio_spec_wanted.freq= rtspconf->audio_samplerate;
	sdl_audio_spec_wanted.format= -1;
	if(rtspconf->audio_device_format== AV_SAMPLE_FMT_S16) {
		sdl_audio_spec_wanted.format= AUDIO_S16SYS;
	} else {
		rtsperror("ga-client: open audio- unsupported audio device format.\n");
		goto end;
	}
	sdl_audio_spec_wanted.channels= 2; //rtspconf->audio_channels
	sdl_audio_spec_wanted.samples= 4096;
	sdl_audio_spec_wanted.callback= NULL;

	sdl_dev= SDL_OpenAudioDevice(NULL, 0, &sdl_audio_spec_wanted,
			&sdl_audio_spec, SDL_AUDIO_ALLOW_FORMAT_CHANGE);
	if(sdl_dev== 0) {
	    SDL_Log("Failed to open audio: %s", SDL_GetError());
	    goto end;
	}
	if(sdl_audio_spec.format!= sdl_audio_spec_wanted.format) {
		/* we let this one thing change. */
		SDL_Log("We didn't get expected audio format.");
		goto end;
	}
	SDL_PauseAudioDevice(sdl_dev, 0); // Start audio playing

	*ref_sdl_dev= sdl_dev;
	sdl_dev= 0; // Avoid releaseing device resources
	end_code= 0; // 'SUCCESS'
end:
	if(sdl_dev> 0)
		SDL_CloseAudioDevice(sdl_dev);
	return end_code;
}

static void audio_device_close(SDL_AudioDeviceID sdl_dev)
{
	if(sdl_dev== 0) {
		rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		return;
	}
	SDL_CloseAudioDevice(sdl_dev);
}

static void* consumer_thr_audio(void *t)
{
	int audio_dec_proc_id, ret_code, *ref_end_code= NULL;
	struct RTSPThreadParam *rtspThreadParam= (RTSPThreadParam*)t;
	procs_ctx_t *procs_ctx= NULL;
	proc_frame_ctx_t *proc_frame_ctx= NULL;
	SDL_AudioDeviceID sdl_dev= 0;

	/* Allocate return context; initialize to a default 'ERROR' value */
	if((ref_end_code= (int*)malloc(sizeof(int)))== NULL) {
		rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		return NULL;
	}
	*ref_end_code= -1; // "error status" by default

	/* Check arguments */
	if(rtspThreadParam== NULL) {
		rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}

	/* Get variables */
	if((procs_ctx= rtspThreadParam->procs_ctx)== NULL) {
		rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if((audio_dec_proc_id= rtspThreadParam->audio_dec_proc_id)< 0) {
		rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}

	if(audio_device_open(rtspThreadParam, &sdl_dev)< 0) {
		rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}

	/* **** Output loop **** */
	while(rtspThreadParam->running== true) {

		/* Receive decoded frame */
		if(proc_frame_ctx!= NULL)
			proc_frame_ctx_release(&proc_frame_ctx);
		ret_code= procs_recv_frame(procs_ctx, audio_dec_proc_id,
				&proc_frame_ctx);
		if(ret_code!= STAT_SUCCESS) {
			if(ret_code== STAT_EAGAIN)
				schedule(); // Avoid closed loops
			else
				rtsperror("Error while receiving decoded frame'\n");
			continue;
		}

		/* **** Write encoded-decoded frame to output if applicable **** */

		if(proc_frame_ctx== NULL)
			continue;

		/* Reset audio device parameters if applicable */
		if(proc_frame_ctx->proc_sampling_rate!= rtspconf->audio_samplerate) {
			rtsperror("Audio samplerate changed (%d-> %d); restoring audio "
					"device parameters.\n", rtspconf->audio_samplerate,
					proc_frame_ctx->proc_sampling_rate);
			audio_device_close(sdl_dev);
			rtspconf->audio_samplerate= proc_frame_ctx->proc_sampling_rate;
			if(audio_device_open(rtspThreadParam, &sdl_dev)< 0) {
				rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
				goto end;
			}
		}

		// Write frame directly to audio SDL reproducer
		SDL_QueueAudio(sdl_dev, proc_frame_ctx->p_data[0],
				(Uint32)proc_frame_ctx->width[0]);
	}

	*ref_end_code= 0; // "success status"
end:
	if(sdl_dev> 0)
		audio_device_close(sdl_dev);
	if(proc_frame_ctx!= NULL)
		proc_frame_ctx_release(&proc_frame_ctx);
	return (void*)ref_end_code;
}

static int launch_decoding_thread(struct RTSPThreadParam *rtspThreadParam,
		const char *sdp_mimetype, int muxer_es_id)
{
	int ret_code, dec_proc_id= -1, end_code= STAT_ERROR;
	volatile int *ref_dec_proc_id= NULL;
	char *sdp_p; // Do not release
	char *type_str= NULL, *subtype_str= NULL;
	const char *proc_name= NULL;
	pthread_t *ref_dec_thread= NULL;
	void*(*thread_routine)(void*)= NULL;
	void *thread_routine_arg= NULL;

	/* Check argumnts */
	if(rtspThreadParam== NULL || sdp_mimetype== NULL) {
		rtsperror("Error at line: %d\n", __LINE__);
		return STAT_ERROR;
	}

	/* Extract MIME type and sub-type */
	sdp_p= strchr((char*)sdp_mimetype, '/');
	if(sdp_p!= NULL && strlen(sdp_mimetype)> 0)
		type_str= strndup(sdp_mimetype, sdp_p- sdp_mimetype);
	else
		type_str= strdup("n/a");
	if(sdp_p!= NULL && strlen(sdp_p+ 1)> 0)
		subtype_str= strdup(sdp_p+ 1);
	else
		subtype_str= strdup("n/a");

	/* Open specific decoder if known */
	if(strcmp(subtype_str, "MLHE")== 0)
		proc_name= "ffmpeg_mlhe_dec";
	else if(strcmp(subtype_str, "H264")== 0)
		proc_name= "ffmpeg_x264_dec";
	else if(strcmp(subtype_str, "MP3")== 0)
		proc_name= "ffmpeg_mp3_dec";
	else {
		rtsperror("Unknown codec type '%s': not supported\n", subtype_str);
		goto end;
	}

	/* Select thread routine depending on media type (audio or video) */
	if(strcmp(type_str, "video")== 0) {
		int iid= rtspThreadParam->iid_max; //TODO: use mutex
		vdecoder_thread_t *vdecoder_thread=
				&rtspThreadParam->vdecoder_thread_array[iid];

		rtspThreadParam->iid_max= iid+ 1; // Update maximum index

		// Update parameters for this video source
		vdecoder_thread->video_muxer_es_id= muxer_es_id;
		//vdecoder_thread.video_dec_proc_id //updated below at 'procs_post()'
		//vdecoder_thread.video_dec_thread //updated below at 'pthread_create()'

		// Set decoding thread parameters
		ref_dec_proc_id= &vdecoder_thread->video_dec_proc_id;
		ref_dec_thread= &vdecoder_thread->video_dec_thread;
		thread_routine= consumer_thr_video;
		thread_routine_arg= (void*)vdecoder_thread;

	} else if(strcmp(type_str, "audio")== 0) {

		// Update parameters for the audio source (only one audio admitted)
		rtspThreadParam->audio_muxer_es_id= muxer_es_id;
		//rtspThreadParam->audio_dec_proc_id;//updated below at 'procs_post()'

		// Set decoding thread parameters
		ref_dec_proc_id= &rtspThreadParam->audio_dec_proc_id;
		ref_dec_thread= &rtspThreadParam->audio_dec_thread;
		thread_routine= consumer_thr_audio;
		thread_routine_arg= (void*)rtspThreadParam;

	} else {
		rtsperror("Unknown media type '%s': not supported\n", type_str);
		goto end;
	}

    /* Register a decoder instance and get corresponding processor Id. */
	procs_post(rtspThreadParam->procs_ctx, proc_name, "", &dec_proc_id);
	if(dec_proc_id< 0) {
		rtsperror("Failed to initialize codec type '%s'\n", subtype_str);
		goto end;
	}
	*ref_dec_proc_id= dec_proc_id;

	/* Launch specific decoder thread */
	printf("Launching elementary stream '%s' (ES-Id.: %d)\n", sdp_mimetype,
			muxer_es_id); fflush(stdout); //comment-me
	ret_code= pthread_create(ref_dec_thread, NULL, thread_routine,
			thread_routine_arg);
	if(ret_code!= 0) {
		rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}

	end_code= STAT_SUCCESS;
end:
	if(type_str!= NULL)
		free(type_str);
	if(subtype_str!= NULL)
		free(subtype_str);
	return end_code;
}

static void join_decoding_thread(struct RTSPThreadParam *rtspThreadParam)
{
	int ret_code;
	void *thread_end_code= NULL;
	procs_ctx_t *procs_ctx= NULL;

	/* Check argumnts */
	if(rtspThreadParam== NULL) {
		rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		return;
	}

	if((procs_ctx= rtspThreadParam->procs_ctx)== NULL) {
		rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		return;
	}

	/* Join video decoding threads */
	for(int i= 0; i< rtspThreadParam->iid_max; i++) {
		vdecoder_thread_t *vdecoder_thread=
				&rtspThreadParam->vdecoder_thread_array[i];

		// delete processore before joining to unblock
		ret_code= procs_opt(procs_ctx, "PROCS_ID_DELETE",
				rtspThreadParam->vdecoder_thread_array[i].video_dec_proc_id);
		if(ret_code!= STAT_SUCCESS)
			fprintf(stderr, "Error at line: %d\n", __LINE__);

		//rtsperror("Waiting video thread to join... "); //comment-me
		pthread_join(vdecoder_thread->video_dec_thread, &thread_end_code);
		if(thread_end_code!= NULL) {
			if(*((int*)thread_end_code)!= 0) {
				rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
			}
			free(thread_end_code);
			thread_end_code= NULL;
		}
		//rtsperror("joined O.K.\n"); //comment-me
	}

	/* Join audio decoding thread */
	ret_code= procs_opt(procs_ctx, "PROCS_ID_DELETE",
			rtspThreadParam->audio_dec_proc_id);
	if(ret_code!= STAT_SUCCESS)
		fprintf(stderr, "Error at line: %d\n", __LINE__);
	//rtsperror("Waiting audio thread to join... "); //comment-me
	pthread_join(rtspThreadParam->audio_dec_thread, &thread_end_code);
	if(thread_end_code!= NULL) {
		if(*((int*)thread_end_code)!= 0) {
			rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		}
		free(thread_end_code);
		thread_end_code= NULL;
	}
	//rtsperror("joined O.K.\n"); //comment-me

	return;
}

/**
 * Register (open) a processor instance:
 * 1.- Register processor with given initial settings if desired,
 * 2.- Parse JSON-REST response to get processor Id.
 */
static void procs_post(procs_ctx_t *procs_ctx, const char *proc_name,
		const char *proc_settings, int *ref_proc_id)
{
	int ret_code;
	char *rest_str= NULL;
	cJSON *cjson_rest= NULL, *cjson_aux= NULL;

	ret_code= procs_opt(procs_ctx, "PROCS_POST", proc_name, proc_settings,
			&rest_str);
	if(ret_code!= STAT_SUCCESS || rest_str== NULL) {
		fprintf(stderr, "Error at line: %d\n", __LINE__);
		exit(-1);
	}
	if((cjson_rest= cJSON_Parse(rest_str))== NULL) {
		fprintf(stderr, "Error at line: %d\n", __LINE__);
		exit(-1);
	}
	if((cjson_aux= cJSON_GetObjectItem(cjson_rest, "proc_id"))== NULL) {
		fprintf(stderr, "Error at line: %d\n", __LINE__);
		exit(-1);
	}
	if((*ref_proc_id= cjson_aux->valuedouble)< 0) {
		fprintf(stderr, "Error at line: %d\n", __LINE__);
		exit(-1);
	}
	free(rest_str); rest_str= NULL;
	cJSON_Delete(cjson_rest); cjson_rest= NULL;
}
