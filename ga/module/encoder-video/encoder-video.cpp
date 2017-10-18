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

/* MediaProcessors's library related */
extern "C" {
#include <libcjson/cJSON.h>
#include <libmediaprocsutils/log.h>
#include <libmediaprocsutils/stat_codes.h>
#include <libmediaprocsutils/schedule.h>
#include <libmediaprocs/proc_if.h>
#include <libmediaprocs/procs.h>
}

/* Maximum parameter length (e.g. "pipe" names) */
#define	MAXPARAMLEN	64

/* CRC-32C (iSCSI) polynomial in reversed bit order. */
#define POLY 0x82f63b78

typedef struct vencoder_thread_s {
	vencoder_arg_t *vencoder_arg;
	int iid;
	int enc_proc_id;
	int muxer_es_id;
	pthread_t enc_thread;
	pthread_t enc2mux_thread;
} vencoder_thread_t;

/* Prototypes */
static int vencoder_deinit(void *arg);

static struct vencoder_thread_s vencoder_thread_array[VIDEO_SOURCE_CHANNEL_MAX];

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

static void* es_mux_thr(void *arg)
{
	int iid, ret_code, *ref_end_code= NULL;
	vencoder_thread_t *vencoder_thread= (vencoder_thread_t*)arg;
	vencoder_arg_t *vencoder_arg= NULL;
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
	if(vencoder_thread== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}

	/* Get variables */
	if((vencoder_arg= vencoder_thread->vencoder_arg)== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if((rtspconf= vencoder_arg->rtsp_conf)== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if((procs_ctx= vencoder_arg->procs_ctx)== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	iid= vencoder_thread->iid;

	/* Get frame from encoder and send to multiplexer */
	while(vencoder_arg->flag_has_started== 1) {

		/* Receive encoded frame */
		if(proc_frame_ctx!= NULL)
			proc_frame_ctx_release(&proc_frame_ctx);
		ret_code= procs_recv_frame(procs_ctx, vencoder_thread->enc_proc_id,
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
		ga_error("Got frame!! (%dx%d)\n", proc_frame_ctx->width[0],
				proc_frame_ctx->height[0]); //comment-me
		proc_frame_ctx->es_id= vencoder_thread->muxer_es_id;
		ret_code= procs_send_frame(procs_ctx, vencoder_arg->muxer_proc_id,
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

static int vencoder_init(void *arg)
{
	int iid, ret_code, end_code= -1;
	vencoder_arg_t *vencoder_arg= (vencoder_arg_t*)arg;
	struct RTSPConf *rtspconf= NULL;
	char *pipefmt= NULL;
	procs_ctx_t *procs_ctx= NULL;
	char *rest_str= NULL;
	cJSON *cjson_rest= NULL, *cjson_aux= NULL;

	/* Check arguments */
	if(vencoder_arg== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		return -1;
	}

	if((rtspconf= vencoder_arg->rtsp_conf)== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if((procs_ctx= vencoder_arg->procs_ctx)== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if((pipefmt= vencoder_arg->pipefmt)== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if(vencoder_arg->flag_is_initialized!= 0) {
		end_code= 0;
		goto end;
	}

	/* Check encoder name exists (we will only use the first one) */
	if(rtspconf->video_encoder_name[0]== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}

	for(iid= 0; iid< video_source_channels(); iid++) {
		char *mime;
		int outputW= video_source_out_width(iid);
		int outputH= video_source_out_height(iid);
		char proc_settings[128]= {0};

		/* Register video encoder in PROCS module */
		vencoder_thread_array[iid].vencoder_arg= vencoder_arg;
		vencoder_thread_array[iid].iid= iid;
		vencoder_thread_array[iid].enc_proc_id= -1;
		vencoder_thread_array[iid].muxer_es_id= -1;
		snprintf(proc_settings, sizeof(proc_settings),
				"width_output=%d&height_output=%d", outputW, outputH);
		//ga_error("proc_settings: '%s'\n", proc_settings); //comment-me
		ret_code= procs_opt(procs_ctx, "PROCS_POST",
				rtspconf->video_encoder_name[0], proc_settings, &rest_str);
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
		if((vencoder_thread_array[iid].enc_proc_id= cjson_aux->valuedouble)<
				0) {
			ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
			goto end;
		}
		free(rest_str); rest_str= NULL;
		cJSON_Delete(cjson_rest); cjson_rest= NULL;

		/* Register an elementary stream for the multiplexer */
		mime= (vencoder_arg->mime!= NULL)? vencoder_arg->mime:
				(char*)"video/NONE";
		snprintf(proc_settings, sizeof(proc_settings), "sdp_mimetype=%s", mime);
		ret_code= procs_opt(procs_ctx, "PROCS_ID_ES_MUX_REGISTER",
				vencoder_arg->muxer_proc_id, proc_settings, &rest_str);
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
		if((vencoder_thread_array[iid].muxer_es_id= cjson_aux->valuedouble)
				< 0) {
			ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
			goto end;
		}
		free(rest_str); rest_str= NULL;
		cJSON_Delete(cjson_rest); cjson_rest= NULL;
	}

	vencoder_arg->flag_is_initialized= 1;
	ga_error("video encoder: initialized.\n");
	end_code= 0;
end:
	if(cjson_rest!= NULL)
		cJSON_Delete(cjson_rest);
	if(rest_str!= NULL)
		free(rest_str);
	if(end_code!= 0) // error occurred
		vencoder_deinit(NULL);
	return end_code;
}

static int vencoder_deinit(void *arg)
{
	int iid, ret_code;
	vencoder_arg_t *vencoder_arg= (vencoder_arg_t*)arg;
	procs_ctx_t *procs_ctx= NULL;

	/* Check arguments */
	if(vencoder_arg== NULL) {
		ga_error("Bad arguments at 'vencoder_deinit()'\n");
		return -1;
	}

	procs_ctx= vencoder_arg->procs_ctx;

	for(iid = 0; iid < video_source_channels(); iid++) {
		if(procs_opt(procs_ctx, "PROCS_ID_DELETE",
				vencoder_thread_array[iid].enc_proc_id)!= STAT_SUCCESS) {
			ga_error("video encoder: deinitialization failed: could not "
					"delete encoder instance.\n");
		} else {
			vencoder_thread_array[iid].enc_proc_id= -1;
		}
	}

	vencoder_arg->flag_is_initialized= 0;
	ga_error("video encoder: deinitialized.\n");
	return 0;
}

static void *vencoder_threadproc(void *arg)
{
	int iid, ret_code;
	vencoder_thread_t *vencoder_thread= (vencoder_thread_t*)arg;
	vencoder_arg_t *vencoder_arg= NULL; // Alias
	struct RTSPConf *rtspconf= NULL; // Alias
	procs_ctx_t *procs_ctx= NULL; // Alias
	char *pipefmt= NULL; // Alias
	char pipename[MAXPARAMLEN]= {0};
	dpipe_t *pipe= NULL;
	int outputW = video_source_out_width(iid);
	int outputH = video_source_out_height(iid);
/////////////////////////////////////////////
	vsource_frame_t *frame = NULL;

	dpipe_buffer_t *data = NULL;
	AVFrame *pic_in = NULL;
	unsigned char *pic_in_buf = NULL;
	int pic_in_size;
	long long basePts = -1LL, newpts = 0LL, pts = -1LL, ptsSync = 0LL;

	/* Check arguments */
	if(vencoder_thread== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		return NULL;
	}

	/* Get variables */
	if((vencoder_arg= vencoder_thread->vencoder_arg)== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if((rtspconf= vencoder_arg->rtsp_conf)== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if((procs_ctx= vencoder_arg->procs_ctx)== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if((pipefmt= vencoder_arg->pipefmt)== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	iid= vencoder_thread->iid;

	/* Get "pipe" */
	snprintf(pipename, sizeof(pipename), pipefmt, iid);
	if((pipe = dpipe_lookup(pipename)) == NULL) {
		ga_error("video encoder: pipe %s is not found\n", pipename);
		goto end;
	}
	ga_error("video encoder: video source #%d from '%s' (%dx%d).\n",
			iid, pipe->name, outputW, outputH);

	encoder_pts_clear(iid);

	if((pic_in = av_frame_alloc()) == NULL) {
		ga_error("video encoder: picture allocation failed, terminated.\n");
		goto end;
	}
	pic_in->width = outputW;
	pic_in->height = outputH;
	pic_in->format = AV_PIX_FMT_YUV420P;
	pic_in_size = avpicture_get_size(AV_PIX_FMT_YUV420P, outputW, outputH);
	if((pic_in_buf = (unsigned char*) av_malloc(pic_in_size)) == NULL) {
		ga_error("video encoder: picture buffer allocation failed, terminated.\n");
		goto end;
	}
	avpicture_fill((AVPicture*) pic_in, pic_in_buf,
			AV_PIX_FMT_YUV420P, outputW, outputH);
	//ga_error("video encoder: linesize = %d|%d|%d\n", pic_in->linesize[0], pic_in->linesize[1], pic_in->linesize[2]);
	// start encoding
	ga_error("video encoding started: tid=%ld %dx%d@%dfps, pic_in_size=%d.\n",
		ga_gettid(),
		outputW, outputH, rtspconf->video_fps, pic_in_size);
	//

	while(vencoder_arg->flag_has_started!= 0) {
		proc_frame_ctx_t proc_frame_ctx= {0};
		struct timeval tv;
		struct timespec to;
		gettimeofday(&tv, NULL);
		to.tv_sec = tv.tv_sec+1;
		to.tv_nsec = tv.tv_usec * 1000;

		/* Release frame pool of past iteration (frame was consumed) */
		if(data!= NULL)
			dpipe_put(pipe, data);
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
			goto end;
		}
		tv = frame->timestamp;

		// pts must be monotonically increasing
		if(newpts > pts) {
			pts = newpts;
		} else {
			pts++;
		}
		// encode
		encoder_pts_put(iid, pts, &tv);
		pic_in->pts = pts;

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
        ret_code= procs_send_frame(procs_ctx, vencoder_thread->enc_proc_id,
        		&proc_frame_ctx);
        if(ret_code!= STAT_SUCCESS && ret_code!= STAT_EAGAIN) {
        	ga_error("'%s' failed. Line %d code: %d\n", __FUNCTION__, __LINE__,
        			ret_code);
        }
	}

end:
	if(data!= NULL)
		dpipe_put(pipe, data);
	if(pipe)
		pipe= NULL;
	if(pic_in_buf)
		av_free(pic_in_buf);
	if(pic_in)
		av_free(pic_in);
	ga_error("video encoder: thread terminated (tid=%ld).\n", ga_gettid());
	return NULL;
}


static int vencoder_start(void *arg)
{
	int iid, ret_code;
	vencoder_arg_t *vencoder_arg= (vencoder_arg_t*)arg;

	/* Check arguments */
	if(vencoder_arg== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		return -1;
	}

	if(vencoder_arg->flag_has_started!= 0)
		return 0;
	vencoder_arg->flag_has_started= 1; // set before launching threads

	for(iid= 0; iid< video_source_channels(); iid++) {
		vencoder_thread_t *vencoder_thread= &vencoder_thread_array[iid];

		/* Start encoders to multiplexers threads */
		ret_code= pthread_create(&vencoder_thread_array[iid].enc2mux_thread,
				NULL, es_mux_thr, (void*)vencoder_thread);
		if(ret_code!= 0) {
			ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
			vencoder_arg->flag_has_started= 0;
			return -1;
		}

		/* Start encoding thread */
		ret_code= pthread_create(&vencoder_thread_array[iid].enc_thread,
				NULL, vencoder_threadproc, (void*)vencoder_thread);
		if(ret_code!= 0) {
			ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
			vencoder_arg->flag_has_started= 0;
			return -1;
		}
	}

	ga_error("video encdoer: all started (%d)\n", iid);
	return 0;
}

static int vencoder_stop(void *arg)
{
	int iid;
	void *ignored;
	vencoder_arg_t *vencoder_arg= (vencoder_arg_t*)arg;

	/* Check arguments */
	if(vencoder_arg== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		return -1;
	}

	if(vencoder_arg->flag_has_started== 0)
		return 0;
	vencoder_arg->flag_has_started= 0;

	for(iid= 0; iid< video_source_channels(); iid++) {
		void *thread_end_code= NULL;

		/* Join encoders to multiplexers threads */
		//ga_error("Waiting thread to join... "); //comment-me
		pthread_join(vencoder_thread_array[iid].enc2mux_thread,
				&thread_end_code);
		if(thread_end_code!= NULL) {
			if(*((int*)thread_end_code)!= 0) {
				ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
			}
			free(thread_end_code);
			thread_end_code= NULL;
		}
		//ga_error("joined O.K.\n"); //comment-me

		pthread_join(vencoder_thread_array[iid].enc_thread, &thread_end_code);
		if(thread_end_code!= NULL) {
			if(*((int*)thread_end_code)!= 0) {
				ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
			}
			free(thread_end_code);
			thread_end_code= NULL;
		}
	}
	ga_error("video encdoer: all stopped (%d)\n", iid);
	return 0;
}

ga_module_t* module_load()
{
	static ga_module_t m;
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
	m.raw = NULL;
	m.ioctl = NULL; // Not used
	return &m;
}
