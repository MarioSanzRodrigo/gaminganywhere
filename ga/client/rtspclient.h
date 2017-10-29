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

#ifndef __RTSPCLIENT_H__
#define __RTSPCLIENT_H__

#include <SDL2/SDL.h>
#include <pthread.h>

#include "rtspconf.h"
#include "vsource.h"

#define	SDL_USEREVENT_CREATE_OVERLAY	0x0001
#define	SDL_USEREVENT_OPEN_AUDIO	0x0002
#define	SDL_USEREVENT_RENDER_IMAGE	0x0004

#define	RTSP_VIDEOSTATE_NULL	0

/* FIFO size, in number of frames, for decoders -> renderer interfacing */
#define RENDERER_FIFO_SIZE 60

/* Forward declarations */
typedef struct fifo_ctx_s fifo_ctx_t;
typedef struct procs_ctx_s procs_ctx_t;

typedef struct vdecoder_thread_s {
	volatile int iid;
	volatile int video_dec_proc_id;
	volatile int video_muxer_es_id;
	pthread_t video_dec_thread;
	struct RTSPThreadParam *volatile rtspThreadParam;
} vdecoder_thread_t;

struct RTSPThreadParam {
	const char *url;
	bool running;
	bool rtpOverTCP;
	char quitLive555;
	// video
	int width[VIDEO_SOURCE_CHANNEL_MAX];
	int height[VIDEO_SOURCE_CHANNEL_MAX];
	AVPixelFormat format[VIDEO_SOURCE_CHANNEL_MAX];
	pthread_mutex_t surfaceMutex[VIDEO_SOURCE_CHANNEL_MAX];
	fifo_ctx_t *fifo_ctx_video_array[VIDEO_SOURCE_CHANNEL_MAX];
#if 1	// only support SDL2
	unsigned int windowId[VIDEO_SOURCE_CHANNEL_MAX];
	SDL_Window *surface[VIDEO_SOURCE_CHANNEL_MAX];
	SDL_Renderer *renderer[VIDEO_SOURCE_CHANNEL_MAX];
	SDL_Texture *overlay[VIDEO_SOURCE_CHANNEL_MAX];
#endif
	// **** MediaProcessors's library related ****
	// General
	pthread_t rtspthread;
	int dmux_proc_id;
	procs_ctx_t *procs_ctx;
	// Video
	volatile int iid_max;
	vdecoder_thread_t vdecoder_thread_array[VIDEO_SOURCE_CHANNEL_MAX];
	// Audio
	volatile int audio_dec_proc_id;
	volatile int audio_muxer_es_id;
	pthread_t audio_dec_thread;
};

extern struct RTSPConf *rtspconf;

void rtsperror(const char *fmt, ...);

int rtsp_client_init(struct RTSPThreadParam *rtspThreadParam);
void rtsp_client_deinit(struct RTSPThreadParam *rtspThreadParam);

#endif
