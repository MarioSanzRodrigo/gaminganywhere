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

#include <stdio.h>
#include <unistd.h>

#include "asource.h"
#include "rtspconf.h"
#include "encoder-common.h"
#include "ga-common.h"
#include "ga-conf.h"
#include "ga-module.h"

/* MediaProcessors's library related */
extern "C" {
#include <libcjson/cJSON.h>
#include <libmediaprocsutils/stat_codes.h>
#include <libmediaprocsutils/schedule.h>
#include <libmediaprocs/proc_if.h>
#include <libmediaprocs/procs.h>
}

typedef struct aencoder_thread_s {
	aencoder_arg_t *aencoder_arg;
	int enc_proc_id;
	int muxer_es_id;
	pthread_t enc_thread;
	pthread_t enc2mux_thread;
} aencoder_thread_t;

static struct aencoder_thread_s aencoder_thread;

/* Prototypes */
static int aencoder_deinit(void *arg);

static void* es_mux_thr(void *arg)
{
	int iid, ret_code, *ref_end_code= NULL;
	aencoder_thread_t *aencoder_thread= (aencoder_thread_t*)arg;
	aencoder_arg_t *aencoder_arg= NULL;
	struct RTSPConf *rtspconf= NULL;
	procs_ctx_t *procs_ctx= NULL;
	proc_frame_ctx_t *proc_frame_ctx= NULL;

	/* Allocate return context; initialize to a default 'ERROR' value */
	if((ref_end_code= (int*)malloc(sizeof(int)))== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		return NULL;
	}
	*ref_end_code= -1; // "error status" by default

	/* Check arguments */
	if(aencoder_thread== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}

	/* Get variables */
	if((aencoder_arg= aencoder_thread->aencoder_arg)== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if((rtspconf= aencoder_arg->rtsp_conf)== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if((procs_ctx= aencoder_arg->procs_ctx)== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}

	/* Get frame from encoder and send to multiplexer */
	while(aencoder_arg->flag_has_started== 1) {

		/* Receive encoded frame */
		if(proc_frame_ctx!= NULL)
			proc_frame_ctx_release(&proc_frame_ctx);
		ret_code= procs_recv_frame(procs_ctx, aencoder_thread->enc_proc_id,
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
		//ga_error("Got audio frame!! (%dx%d)\n", proc_frame_ctx->width[0],
		//		proc_frame_ctx->height[0]); //comment-me

		proc_frame_ctx->es_id= aencoder_thread->muxer_es_id;
		ret_code= procs_send_frame(procs_ctx, aencoder_arg->muxer_proc_id,
				proc_frame_ctx);
		if(ret_code!= STAT_SUCCESS) {
			if(ret_code== STAT_EAGAIN)
				schedule(); // Avoid closed loops
			else
				fprintf(stderr, "Error while multiplexing frame'\n");
			continue;
		}
	}

	*ref_end_code= 0; // "success status"
end:
	if(proc_frame_ctx!= NULL)
		proc_frame_ctx_release(&proc_frame_ctx);
	return (void*)ref_end_code;
}

static int aencoder_init(void *arg)
{
	char *mime;
	int ret_code, end_code= -1;
	aencoder_arg_t *aencoder_arg= (aencoder_arg_t*)arg;
	struct RTSPConf *rtspconf= NULL;
	procs_ctx_t *procs_ctx= NULL;
	char *rest_str= NULL;
	cJSON *cjson_rest= NULL, *cjson_aux= NULL;
	char proc_settings[128]= {0};

	/* Check arguments */
	if(aencoder_arg== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		return -1;
	}

	if((rtspconf= aencoder_arg->rtsp_conf)== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if((procs_ctx= aencoder_arg->procs_ctx)== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if(aencoder_arg->flag_is_initialized!= 0) {
		end_code= 0;
		goto end;
	}

	/* Check if encoder name exists */
	if(rtspconf->audio_encoder_name[0]== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}

	/* Register encoder in PROCS module */
	aencoder_thread.aencoder_arg= aencoder_arg;
	aencoder_thread.enc_proc_id= -1;
	aencoder_thread.muxer_es_id= -1;
	snprintf(proc_settings, sizeof(proc_settings),
			"bit_rate_output=%d&sample_rate_output=%d",
			rtspconf->audio_bitrate, rtspconf->audio_samplerate);
	//ga_error("proc_settings: '%s'\n", proc_settings); //comment-me
	ret_code= procs_opt(procs_ctx, "PROCS_POST",
			rtspconf->audio_encoder_name[0], proc_settings, &rest_str);
	if(ret_code!= STAT_SUCCESS || rest_str== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if((cjson_rest= cJSON_Parse(rest_str))== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if((cjson_aux= cJSON_GetObjectItem(cjson_rest, "proc_id"))== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if((aencoder_thread.enc_proc_id= cjson_aux->valuedouble)< 0) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	free(rest_str); rest_str= NULL;
	cJSON_Delete(cjson_rest); cjson_rest= NULL;

	/* Register an elementary stream for the multiplexer */
	mime= (aencoder_arg->mime!= NULL)? aencoder_arg->mime:
			(char*)"audio/NONE";
	snprintf(proc_settings, sizeof(proc_settings), "sdp_mimetype=%s", mime);
	ret_code= procs_opt(procs_ctx, "PROCS_ID_ES_MUX_REGISTER",
			aencoder_arg->muxer_proc_id, proc_settings, &rest_str);
	if(ret_code!= STAT_SUCCESS || rest_str== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if((cjson_rest= cJSON_Parse(rest_str))== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if((cjson_aux= cJSON_GetObjectItem(cjson_rest,
			"elementary_stream_id"))== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if((aencoder_thread.muxer_es_id= cjson_aux->valuedouble)< 0) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	free(rest_str); rest_str= NULL;
	cJSON_Delete(cjson_rest); cjson_rest= NULL;

	aencoder_arg->flag_is_initialized= 1;
	ga_error("audio encoder: initialized.\n");
	end_code= 0;
end:
	if(cjson_rest!= NULL)
		cJSON_Delete(cjson_rest);
	if(rest_str!= NULL)
		free(rest_str);
	if(end_code!= 0) // error occurred
		aencoder_deinit(aencoder_arg);
	return end_code;
}

static int aencoder_deinit(void *arg)
{
	int ret_code;
	aencoder_arg_t *aencoder_arg= (aencoder_arg_t*)arg;

	/* Check arguments */
	if(aencoder_arg== NULL) {
		ga_error("Bad arguments at '%s'\n", __FUNCTION__);
		return -1;
	}

	if(procs_opt(aencoder_arg->procs_ctx, "PROCS_ID_DELETE",
			aencoder_thread.enc_proc_id)!= STAT_SUCCESS) {
		ga_error("audio encoder: deinitialization failed: could not "
				"delete encoder instance.\n");
	} else {
		aencoder_thread.enc_proc_id= -1;
	}

	aencoder_arg->flag_is_initialized= 0;
	ga_error("audio encoder: deinitialized.\n");
	return 0;
}

static void *aencoder_threadproc(void *arg)
{
	int64_t frame_period_usec, frame_period_90KHz;
	int ret_code, frame_size_samples= 0, frame_size_bytes= 0,
			frames_size_bytes= 0;
	aencoder_thread_t *aencoder_thread= (aencoder_thread_t*)arg;
	aencoder_arg_t *aencoder_arg= NULL; // Alias
	struct RTSPConf *rtspconf= NULL; // Alias
	procs_ctx_t *procs_ctx= NULL; // Alias
	char *rest_str= NULL;
	cJSON *cjson_rest= NULL, *cjson_aux= NULL;
	proc_frame_ctx_s proc_frame_ctx= {0};
	int r, frameunit;
	// buffer used to store captured data
	unsigned char *samples = NULL;
	int nsamples= 0, samplebytes= 0, buffer_purged= 0;
	int offset;
	audio_buffer_t *ab = NULL;

	/* Check arguments */
	if(aencoder_thread== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		return NULL;
	}

	/* Get variables */
	if((aencoder_arg= aencoder_thread->aencoder_arg)== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if((rtspconf= aencoder_arg->rtsp_conf)== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if((procs_ctx= aencoder_arg->procs_ctx)== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}

	/* Get frame size requested by audio encoder. We need this data to
	 * know how many bytes to send to the encoder.
	 */
	ret_code= procs_opt(procs_ctx, "PROCS_ID_GET",
			aencoder_thread->enc_proc_id, &rest_str);
	if(ret_code!= STAT_SUCCESS || rest_str== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	//printf("PROCS_ID_GET: '%s'\n", rest_str); fflush(stdout); //comment-me
	if((cjson_rest= cJSON_Parse(rest_str))== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if((cjson_aux= cJSON_GetObjectItem(cjson_rest,
			"expected_frame_size_iput"))== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if((frame_size_bytes= cjson_aux->valuedouble* sizeof(uint16_t))< 0) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	free(rest_str); rest_str= NULL;
	cJSON_Delete(cjson_rest); cjson_rest= NULL;

	frame_size_samples= frame_size_bytes>> 1; // 16-bit samples
	frames_size_bytes= frame_size_bytes * audio_source_channels();

	/* Capture buffer related */
	if((ab = audio_source_buffer_init()) == NULL) {
		ga_error("audio encoder: cannot initialize audio source buffer.\n");
		return NULL;
	}
	audio_source_client_register(ga_gettid(), ab);

	if((samples = (unsigned char*) malloc(frames_size_bytes)) == NULL) {
		ga_error("audio encoder: cannot allocate sample buffer (%d bytes), "
				"terminated.\n", frames_size_bytes);
		goto end;
	}

	frameunit = audio_source_channels() * audio_source_bitspersample() / 8;

	// start encoding
	ga_error("audio encoding started\n");

    frame_period_usec= (1000000* frame_size_samples)/
    		rtspconf->audio_samplerate; //usecs
    frame_period_90KHz= (frame_period_usec/1000/*[msec]*/)*
    		90/*[ticks/msec]*/; //ticks

	while(aencoder_arg->flag_has_started!= 0) {
		struct timespec time_curr;
		int64_t pts;

		if(buffer_purged == 0) {
			audio_source_buffer_purge(ab);
			buffer_purged = 1;
		}
		// read audio frames //FIXME!! Bad implemented GA!
		r = audio_source_buffer_read(ab, samples + samplebytes,
				frame_size_samples- nsamples);
		if(r <= 0) {
			usleep(1000);
			continue;
		}

		// encode
		nsamples += r;
		samplebytes += r*frameunit;
		offset = 0;

		/* Get time-stamp base for this chunk */
		clock_gettime(CLOCK_MONOTONIC, &time_curr);
		pts= (int64_t)((int64_t)time_curr.tv_sec*1000000000+
				(int64_t)time_curr.tv_nsec); //[nsecs]
		pts= (pts*90)/1000000; //[nsec*clk_90KHz/nsec= clk_90KHz]

		while(nsamples >= frame_size_samples) {
			unsigned char *srcbuf= samples+ offset;

			/* Prepare audio frame to be encoded */
			proc_frame_ctx.data= srcbuf;
			proc_frame_ctx.p_data[0]= srcbuf;
			proc_frame_ctx.width[0]= proc_frame_ctx.linesize[0]=
					frame_size_bytes<< 1;
			proc_frame_ctx.height[0]= 1;
			proc_frame_ctx.proc_sample_fmt= PROC_IF_FMT_S16;
			proc_frame_ctx.pts= pts;
			//printf("pts: %"PRId64"\n", pts); fflush(stdout); //comment-me

			/* Send audio frame to encoder */
			ret_code= procs_send_frame(procs_ctx, aencoder_thread->enc_proc_id,
					&proc_frame_ctx);
			if(ret_code!= STAT_SUCCESS && ret_code!= STAT_EAGAIN) {
				ga_error("'%s' failed. Line %d code: %d\n", __FUNCTION__,
						__LINE__, ret_code);
			}

			/* Update input data references */
			nsamples-= frame_size_samples;
			offset+= frame_size_samples* frameunit;
			pts+= frame_period_90KHz;
		}

		// if something has been processed
		if(offset > 0) {
			if(samplebytes-offset > 0) {
				bcopy(&samples[offset], samples, samplebytes-offset);
			}
			samplebytes -= offset;
		}
	}

end:
	audio_source_client_unregister(ga_gettid());
	audio_source_buffer_deinit(ab);
	//
	if(samples)	free(samples);
	aencoder_deinit(NULL);
	ga_error("audio encoder: thread terminated (tid=%ld).\n", ga_gettid());
	//
	if(cjson_rest!= NULL)
		cJSON_Delete(cjson_rest);
	if(rest_str!= NULL)
		free(rest_str);
	return NULL;
}

static int aencoder_start(void *arg)
{
	int ret_code;
	aencoder_arg_t *aencoder_arg= (aencoder_arg_t*)arg;

	/* Check arguments */
	if(aencoder_arg== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		return -1;
	}

	if(aencoder_arg->flag_has_started!= 0)
		return 0;
	aencoder_arg->flag_has_started= 1; // set before launching threads

	/* Start encoders to multiplexers threads */
	ret_code= pthread_create(&aencoder_thread.enc2mux_thread,
			NULL, es_mux_thr, (void*)&aencoder_thread);
	if(ret_code!= 0) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		aencoder_arg->flag_has_started= 0;
		return -1;
	}

	/* Start encoding thread */
	ret_code= pthread_create(&aencoder_thread.enc_thread,
			NULL, aencoder_threadproc, (void*)&aencoder_thread);
	if(ret_code!= 0) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		aencoder_arg->flag_has_started= 0;
		return -1;
	}

	return 0;
}

static int aencoder_stop(void *arg)
{
	aencoder_arg_t *aencoder_arg= (aencoder_arg_t*)arg;
	void *thread_end_code= NULL;

	/* Check arguments */
	if(aencoder_arg== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		return -1;
	}

	if(aencoder_arg->flag_has_started== 0)
		return 0;
	aencoder_arg->flag_has_started= 0;

	/* Join encoders to multiplexers threads */
	//ga_error("Waiting thread to join... "); //comment-me
	pthread_join(aencoder_thread.enc2mux_thread, &thread_end_code);
	if(thread_end_code!= NULL) {
		if(*((int*)thread_end_code)!= 0) {
			ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		}
		free(thread_end_code);
		thread_end_code= NULL;
	}
	//ga_error("joined O.K.\n"); //comment-me

	pthread_join(aencoder_thread.enc_thread, &thread_end_code);
	if(thread_end_code!= NULL) {
		if(*((int*)thread_end_code)!= 0) {
			ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		}
		free(thread_end_code);
		thread_end_code= NULL;
	}

	return 0;
}

ga_module_t *module_load()
{
	static ga_module_t m;
	char mime[64];
	bzero(&m, sizeof(m));
	m.type = GA_MODULE_TYPE_AENCODER;
	m.name = strdup("ffmpeg-audio-encoder");
	if(ga_conf_readv("audio-mimetype", mime, sizeof(mime)) != NULL) {
		m.mimetype = strdup(mime);
	}
	m.init = aencoder_init;
	m.start = aencoder_start;
	//m.threadproc = aencoder_threadproc;
	m.stop = aencoder_stop;
	m.deinit = aencoder_deinit;
	return &m;
}

