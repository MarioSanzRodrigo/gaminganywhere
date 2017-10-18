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

#include <stdarg.h>
#include <string.h>

#include <pthread.h>
#include <SDL2/SDL.h>
#ifndef ANDROID
//#include <SDL2/SDL_ttf.h>
#endif /* ! ANDROID */
#ifndef WIN32
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif /* ! WIN32 */
#if ! defined WIN32 && ! defined __APPLE__ && ! defined ANDROID
#include <X11/Xlib.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif
#include <libavcodec/avcodec.h>
#ifdef __cplusplus
}
#endif

#include "rtspconf.h"
#include "rtspclient.h"

#include "controller.h"
#include "ctrl-sdl.h"

#include "ga-common.h"
#include "ga-conf.h"
#include "ga-avcodec.h"
#include "vconverter.h"

#include <map>
using namespace std;

/* MediaProcessors's library related */
extern "C" {
#include <libcjson/cJSON.h>
#include <libmediaprocsutils/log.h>
#include <libmediaprocsutils/stat_codes.h>
#include <libmediaprocsutils/fifo.h>
#include <libmediaprocs/proc_if.h>
#include <libmediaprocs/procs.h>
#include <libmediaprocsmuxers/live555_rtsp.h>
#include <libmediaprocscodecs/ffmpeg_x264.h>
#include <libmediaprocscodecs/ffmpeg_m2v.h>
#include <libmediaprocscodecs/ffmpeg_mp3.h>
#include <libmediaprocscodecs/ffmpeg_lhe.h>
}

#define	POOLSIZE	16

#define	IDLE_MAXIMUM_THRESHOLD		3600000	/* us */
#define	IDLE_DETECTION_THRESHOLD	 600000 /* us */

#define	WINDOW_TITLE		"Player Channel #%d (%dx%d)"

pthread_mutex_t watchdogMutex;
struct timeval watchdogTimer = {0LL, 0LL};

static RTSPThreadParam rtspThreadParam;

static int relativeMouseMode = 0;
static int showCursor = 1;
static int windowSizeX[VIDEO_SOURCE_CHANNEL_MAX];
static int windowSizeY[VIDEO_SOURCE_CHANNEL_MAX];
// support resizable window
static int nativeSizeX[VIDEO_SOURCE_CHANNEL_MAX];
static int nativeSizeY[VIDEO_SOURCE_CHANNEL_MAX];
static map<unsigned int, int> windowId2ch;

// save files
static FILE *savefp_keyts = NULL;

#if 0 //#ifndef ANDROID
#define	DEFAULT_FONT		"FreeSans.ttf"
#define	DEFAULT_FONTSIZE	24
static TTF_Font *defFont = NULL;
#endif

static void
switch_fullscreen() {
	unsigned int flags;
	SDL_Window *w = NULL;
	pthread_mutex_lock(&rtspThreadParam.surfaceMutex[0]);
	if((w = rtspThreadParam.surface[0]) == NULL)
		goto quit;
	flags = SDL_GetWindowFlags(w);
	flags = (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) ^ SDL_WINDOW_FULLSCREEN_DESKTOP;
	SDL_SetWindowFullscreen(w, flags);
quit:
	pthread_mutex_unlock(&rtspThreadParam.surfaceMutex[0]);
	return;
}

static void
switch_grab_input(SDL_Window *w) {
	SDL_bool grabbed;
	int need_unlock = 0;
	//
	if(w == NULL) {
		pthread_mutex_lock(&rtspThreadParam.surfaceMutex[0]);
		w = rtspThreadParam.surface[0];
		need_unlock = 1;
	}
	if(w != NULL) {
		grabbed = SDL_GetWindowGrab(w);
		if(grabbed == SDL_FALSE)
			SDL_SetWindowGrab(w, SDL_TRUE);
		else
			SDL_SetWindowGrab(w, SDL_FALSE);
	}
	if(need_unlock) {
		pthread_mutex_unlock(&rtspThreadParam.surfaceMutex[0]);
	}
	return;
}

static int
xlat_mouseX(int ch, int x) {
	return (1.0 * nativeSizeX[ch] / windowSizeX[ch]) * x;
}

static int
xlat_mouseY(int ch, int y) {
	return (1.0 * nativeSizeY[ch] / windowSizeY[ch]) * y;
}

static void
create_overlay(struct RTSPThreadParam *rtspParam, int ch) {
	int w, h;
	AVPixelFormat format;
	unsigned int renderer_flags = 0;
	int renderer_index = -1;
	SDL_Window *surface = NULL;
	SDL_Renderer *renderer = NULL;
	SDL_Texture *overlay = NULL;
	struct SwsContext *swsctx = NULL;
	char windowTitle[64];
	//
	pthread_mutex_lock(&rtspParam->surfaceMutex[ch]);
	if(rtspParam->surface[ch] != NULL) {
		pthread_mutex_unlock(&rtspParam->surfaceMutex[ch]);
		rtsperror("ga-client: duplicated create window request - image comes too fast?\n");
		return;
	}
	w = rtspParam->width[ch];
	h = rtspParam->height[ch];
	format = rtspParam->format[ch];
	pthread_mutex_unlock(&rtspParam->surfaceMutex[ch]);
	// swsctx
	if((swsctx = create_frame_converter(w, h, format, w, h, AV_PIX_FMT_YUV420P)) == NULL) {
		rtsperror("ga-client: cannot create swsscale context.\n");
		exit(-1);
	}

	// sdl
	int wflag = 0;
	wflag |= SDL_WINDOW_RESIZABLE;
	if(ga_conf_readbool("fullscreen", 0) != 0) {
		wflag |= SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_BORDERLESS;
	}
	if(relativeMouseMode != 0) {
		wflag |= SDL_WINDOW_INPUT_GRABBED;
	}
	snprintf(windowTitle, sizeof(windowTitle), WINDOW_TITLE, ch, w, h);
	surface = SDL_CreateWindow(windowTitle,
			SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
			w, h, wflag);
	if(surface == NULL) {
		rtsperror("ga-client: set video mode (create window) failed.\n");
		exit(-1);
	}
	//SDL_SetWindowMaximumSize(surface, w, h);
	SDL_SetWindowMinimumSize(surface, w>>2, h>>2);
	nativeSizeX[ch] = windowSizeX[ch] = w;
	nativeSizeY[ch] = windowSizeY[ch] = h;
	windowId2ch[SDL_GetWindowID(surface)] = ch;
	// move mouse to center
	SDL_WarpMouseInWindow(surface, w/2, h/2);
	if(relativeMouseMode != 0) {
		SDL_SetRelativeMouseMode(SDL_TRUE);
		showCursor = 0;
		//SDL_ShowCursor(0);
#if 0		//// XXX: EXPERIMENTAL - switch twice to make it normal?
		switch_grab_input(NULL);
		SDL_SetRelativeMouseMode(SDL_FALSE);
		switch_grab_input(NULL);
		SDL_SetRelativeMouseMode(SDL_TRUE);
#endif		////
		ga_error("ga-client: relative mouse mode enabled.\n");
	}
	//
	do {	// choose SW or HW renderer?
		// XXX: Windows crashed if there is not a HW renderer!
		int i, n = SDL_GetNumRenderDrivers();
		char renderer_name[64] = "";
		SDL_RendererInfo info;

		ga_conf_readv("video-renderer", renderer_name, sizeof(renderer_name));
		if(strcmp("software", renderer_name) == 0) {
			rtsperror("ga-client: configured to use software renderer.\n");
			renderer_flags = SDL_RENDERER_SOFTWARE;
		}

		for(i = 0; i < n; i++) {
			if(SDL_GetRenderDriverInfo(i, &info) < 0)
				continue;
			if(strcmp(renderer_name, info.name) == 0)
				renderer_index = i;
			rtsperror("ga-client: renderer#%d - %s (%s%s%s%s)%s\n",
				i, info.name,
				info.flags & SDL_RENDERER_SOFTWARE ? "SW" : "",
				info.flags & SDL_RENDERER_ACCELERATED? "HW" : "",
				info.flags & SDL_RENDERER_PRESENTVSYNC ? ",vsync" : "",
				info.flags & SDL_RENDERER_TARGETTEXTURE ? ",texture" : "",
				i != renderer_index ? "" : " *");
			if(renderer_flags != SDL_RENDERER_SOFTWARE && info.flags & SDL_RENDERER_ACCELERATED)
				renderer_flags = SDL_RENDERER_ACCELERATED;
		}
	} while(0);
	//
	renderer = SDL_CreateRenderer(surface, renderer_index, renderer_flags);
			//rtspconf->video_renderer_software ?
			//	SDL_RENDERER_SOFTWARE : renderer_flags);
	if(renderer == NULL) {
		rtsperror("ga-client: create renderer failed.\n");
		exit(-1);
	}
	//
	overlay = SDL_CreateTexture(renderer,
			SDL_PIXELFORMAT_YV12,
			SDL_TEXTUREACCESS_STREAMING,
			w, h);
	if(overlay == NULL) {
		rtsperror("ga-client: create overlay (textuer) failed.\n");
		exit(-1);
	}
	//
	pthread_mutex_lock(&rtspParam->surfaceMutex[ch]);
	rtspParam->swsctx[ch] = swsctx;
	rtspParam->overlay[ch] = overlay;
	rtspParam->renderer[ch] = renderer;
	rtspParam->windowId[ch] = SDL_GetWindowID(surface);
	rtspParam->surface[ch] = surface;
	pthread_mutex_unlock(&rtspParam->surfaceMutex[ch]);
	//
	rtsperror("ga-client: window created successfully (%dx%d).\n", w, h);
	// initialize watchdog
	pthread_mutex_lock(&watchdogMutex);
	gettimeofday(&watchdogTimer, NULL);
	pthread_mutex_unlock(&watchdogMutex);
	//
	// RAL: Should implement "release" code in case of error...
	// There is no destroying renderer, window, etc...!!
	return;
}

static void
open_audio(struct RTSPThreadParam *rtspParam, AVCodecContext *adecoder) {
	SDL_AudioSpec wanted, spec;
	//
	wanted.freq = rtspconf->audio_samplerate;
	wanted.format = -1;
	if(rtspconf->audio_device_format == AV_SAMPLE_FMT_S16) {
		wanted.format = AUDIO_S16SYS;
	} else {
		rtsperror("ga-client: open audio - unsupported audio device format.\n");
		return;
	}
	wanted.channels = rtspconf->audio_channels;
	wanted.silence = 0;
	wanted.samples = SDL_AUDIO_BUFFER_SIZE;
	wanted.callback = NULL; //audio_buffer_fill_sdl; //FIXME!! //RAL
	wanted.userdata = adecoder;
	//
	pthread_mutex_lock(&rtspParam->audioMutex);
	if(rtspParam->audioOpened == true) {
		pthread_mutex_unlock(&rtspParam->audioMutex);
		return;
	}
	if(SDL_OpenAudio(&wanted, &spec) < 0) {
		pthread_mutex_unlock(&rtspParam->audioMutex);
		rtsperror("ga-client: open audio failed - %s\n", SDL_GetError());
		return;
	}
	//
	rtspParam->audioOpened = true;
	//
	SDL_PauseAudio(0);
	pthread_mutex_unlock(&rtspParam->audioMutex);
	rtsperror("ga-client: audio device opened.\n");
	return;
}

// negative x or y means centering-x and centering-y, respectively
static void
render_text(SDL_Renderer *renderer, SDL_Window *window, int x, int y, int line, const char *text) {
#if 1 //#ifdef ANDROID
	// not supported
#else
	SDL_Color textColor = {255, 255, 255};
	SDL_Surface *textSurface = TTF_RenderText_Solid(defFont, text, textColor);
	SDL_Rect dest = {0, 0, 0, 0}, boxRect;
	SDL_Texture *texture;
	int ww, wh;
	//
	if(window == NULL || renderer == NULL) {
		rtsperror("render_text: Invalid window(%p) or renderer(%p) received.\n",
			window, renderer);
		return;
	}
	//
	SDL_GetWindowSize(window, &ww, &wh);
	// centering X/Y?
	if(x >= 0) {	dest.x = x; }
	else {		dest.x = (ww - textSurface->w)/2; }
	if(y >= 0) {	dest.y = y; }
	else {		dest.y = (wh - textSurface->h)/2; }
	//
	dest.y += line * textSurface->h;
	dest.w = textSurface->w;
	dest.h = textSurface->h;
	//
	boxRect.x = dest.x - 6;
	boxRect.y = dest.y - 6;
	boxRect.w = dest.w + 12;
	boxRect.h = dest.h + 12;
	//
	if((texture = SDL_CreateTextureFromSurface(renderer, textSurface)) != NULL) {
		SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
		SDL_RenderFillRect(renderer, &boxRect);
		SDL_RenderCopy(renderer, texture, NULL, &dest);
		SDL_DestroyTexture(texture);
	} else {
		rtsperror("render_text: failed on creating text texture: %s\n", SDL_GetError());
	}
	//
	SDL_FreeSurface(textSurface);
#endif
	return;
}

static void render_image(struct RTSPThreadParam *rtspThreadParam, int ch)
{
#if 1 //RAL
	SDL_Rect sdlRect;
	int w_Y_iput, h_Y_iput, ret_code;
	proc_frame_ctx_t *proc_frame_ctx= NULL;
	size_t fifo_elem_size= 0;
	SDL_Renderer *sdlRenderer= NULL;
	SDL_Texture *sdlTexture= NULL;

	/* Check arguments */
	if(rtspThreadParam== NULL) {
		rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		return;
	}
	if(ch< 0 || ch> VIDEO_SOURCE_CHANNEL_MAX) {
		rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		return;
	}

	/* Get input frame */
	ret_code= fifo_get(rtspThreadParam->fifo_ctx_video_array[ch],
			(void**)&proc_frame_ctx, &fifo_elem_size);
	if(ret_code!= STAT_SUCCESS || proc_frame_ctx== NULL) {
		if(ret_code!= STAT_EAGAIN)
			rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}

	/* Get and check input frame with and height */
	w_Y_iput= proc_frame_ctx->width[0];
	h_Y_iput= proc_frame_ctx->height[0];
	if(w_Y_iput<= 0 || h_Y_iput<= 0) {
		rtsperror("Invalid frame size at renderer\n");
		goto end;
	}

	/* Get rendering variables */
	sdlRenderer= rtspThreadParam->renderer[ch];
	sdlTexture= rtspThreadParam->overlay[ch];
	if(sdlRenderer== NULL || sdlTexture== NULL) {
		union SDL_Event evt;

		rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__); fflush(stderr);
		//pthread_mutex_lock(&rtspThreadParam->surfaceMutex[ch]);
		//if(rtspThreadParam->swsctx[iid] == NULL) {
			rtspThreadParam->width[ch] = proc_frame_ctx->width[0];
			rtspThreadParam->height[ch] = proc_frame_ctx->height[0];
			rtspThreadParam->format[ch] = (AVPixelFormat)AV_PIX_FMT_YUV420P;
rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__); fflush(stderr);
rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__); fflush(stderr);
			//pthread_mutex_unlock(&rtspThreadParam->surfaceMutex[iid]);
			bzero(&evt, sizeof(evt));
			evt.user.type = SDL_USEREVENT;
			evt.user.timestamp = time(0);
			evt.user.code = SDL_USEREVENT_CREATE_OVERLAY;
			evt.user.data1 = rtspThreadParam;
			evt.user.data2 = (void*) ch;
			SDL_PushEvent(&evt);
			// skip the initial frame:
			// for event handler to create/setup surfaces
			//continue; //goto skip_frame;
		//}
		//pthread_mutex_unlock(&rtspThreadParam->surfaceMutex[ch]);
			rtsperror("'%s' failed. Line %d\n", __FUNCTION__, __LINE__); fflush(stderr);
		goto end; // Note we are skipping this frame
	}

	/* Specify rendering area/rectangle */
	sdlRect.x= 0;
	sdlRect.y= 0;
	sdlRect.w= w_Y_iput;
	sdlRect.h= h_Y_iput;

	/* Do render */
	SDL_UpdateYUVTexture(sdlTexture, NULL,
			proc_frame_ctx->p_data[0], proc_frame_ctx->linesize[0],
			proc_frame_ctx->p_data[1], proc_frame_ctx->linesize[1],
			proc_frame_ctx->p_data[2], proc_frame_ctx->linesize[2]);
	SDL_RenderClear(sdlRenderer);
	SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, &sdlRect);
	SDL_RenderPresent(sdlRenderer);

end:
	if(proc_frame_ctx!= NULL)
		proc_frame_ctx_release(&proc_frame_ctx);
	return;
#else
	dpipe_buffer_t *data;
	AVPicture *vframe;
	SDL_Rect rect;
	unsigned char *pixels;
	int pitch;

	//
	if((data = dpipe_load_nowait(rtspParam->pipe[ch])) == NULL) {
		return;
	}
	vframe = (AVPicture*) data->pointer;
	//
	if(SDL_LockTexture(rtspParam->overlay[ch], NULL, (void**) &pixels, &pitch) == 0) {
		bcopy(vframe->data[0], pixels, rtspParam->width[ch] * rtspParam->height[ch]);
		bcopy(vframe->data[1], pixels+((pitch*rtspParam->height[ch]*5)>>2), rtspParam->width[ch] * rtspParam->height[ch] / 4);
		bcopy(vframe->data[2], pixels+pitch*rtspParam->height[ch], rtspParam->width[ch] * rtspParam->height[ch] / 4);
		SDL_UnlockTexture(rtspParam->overlay[ch]);
	} else {
		rtsperror("ga-client: lock textture failed - %s\n", SDL_GetError());
	}
	dpipe_put(rtspParam->pipe[ch], data);
	rect.x = 0;
	rect.y = 0;
	rect.w = rtspParam->width[ch];
	rect.h = rtspParam->height[ch];

	SDL_RenderCopy(rtspParam->renderer[ch], rtspParam->overlay[ch], NULL, NULL);
	SDL_RenderPresent(rtspParam->renderer[ch]);

	//
	image_rendered = 1;
	//
#endif
}

void
ProcessEvent(SDL_Event *event) {
	sdlmsg_t m;
	map<unsigned int,int>::iterator mi;
	int ch;
	struct timeval tv;
	//
	switch(event->type) {
	case SDL_KEYUP:
		if(event->key.keysym.sym == SDLK_BACKQUOTE
		&& relativeMouseMode != 0) {
			showCursor = 1 - showCursor;
			//SDL_ShowCursor(showCursor);
			switch_grab_input(NULL);
#if 1
			if(showCursor)
				SDL_SetRelativeMouseMode(SDL_FALSE);
			else
				SDL_SetRelativeMouseMode(SDL_TRUE);
#endif
		}
		// switch between fullscreen?
		if((event->key.keysym.sym == SDLK_RETURN)
		&& (event->key.keysym.mod & KMOD_ALT)) {
			// do nothing
		} else
		//
		if(rtspconf->ctrlenable) {
		sdlmsg_keyboard(&m, 0,
			event->key.keysym.scancode,
			event->key.keysym.sym,
			event->key.keysym.mod,
			0/*event->key.keysym.unicode*/);
		ctrl_client_sendmsg(&m, sizeof(sdlmsg_keyboard_t));
		}
		if(savefp_keyts != NULL) {
			gettimeofday(&tv, NULL);
			ga_save_printf(savefp_keyts, "KEY-UP: %u.%06u scan 0x%04x sym 0x%04x mod 0x%04x\n",
				tv.tv_sec, tv.tv_usec,
				event->key.keysym.scancode,
				event->key.keysym.sym,
				event->key.keysym.mod);
		}
		break;
	case SDL_KEYDOWN:
		// switch between fullscreen?
		if((event->key.keysym.sym == SDLK_RETURN)
		&& (event->key.keysym.mod & KMOD_ALT)) {
			switch_fullscreen();
		} else
		//
		if(rtspconf->ctrlenable) {
		sdlmsg_keyboard(&m, 1,
			event->key.keysym.scancode,
			event->key.keysym.sym,
			event->key.keysym.mod,
			0/*event->key.keysym.unicode*/);
		ctrl_client_sendmsg(&m, sizeof(sdlmsg_keyboard_t));
		}
		if(savefp_keyts != NULL) {
			gettimeofday(&tv, NULL);
			ga_save_printf(savefp_keyts, "KEY-DN: %u.%06u scan 0x%04x sym 0x%04x mod 0x%04x\n",
				tv.tv_sec, tv.tv_usec,
				event->key.keysym.scancode,
				event->key.keysym.sym,
				event->key.keysym.mod);
		}
		break;
	case SDL_MOUSEBUTTONUP:
		mi = windowId2ch.find(event->button.windowID);
		if(mi != windowId2ch.end() && rtspconf->ctrlenable) {
			ch = mi->second;
			sdlmsg_mousekey(&m, 0, event->button.button,
				xlat_mouseX(ch, event->button.x),
				xlat_mouseY(ch, event->button.y));
			ctrl_client_sendmsg(&m, sizeof(sdlmsg_mouse_t));
		}
		break;
	case SDL_MOUSEBUTTONDOWN:
		mi = windowId2ch.find(event->button.windowID);
		if(mi != windowId2ch.end() && rtspconf->ctrlenable) {
			ch = mi->second;
			sdlmsg_mousekey(&m, 1, event->button.button,
				xlat_mouseX(ch, event->button.x),
				xlat_mouseY(ch, event->button.y));
			ctrl_client_sendmsg(&m, sizeof(sdlmsg_mouse_t));
		}
		break;
	case SDL_MOUSEMOTION:
		mi = windowId2ch.find(event->motion.windowID);
		if(mi != windowId2ch.end() && rtspconf->ctrlenable && rtspconf->sendmousemotion) {
			ch = mi->second;
			sdlmsg_mousemotion(&m,
				xlat_mouseX(ch, event->motion.x),
				xlat_mouseY(ch, event->motion.y),
				xlat_mouseX(ch, event->motion.xrel),
				xlat_mouseY(ch, event->motion.yrel),
				event->motion.state,
				relativeMouseMode == 0 ? 0 : 1);
			ctrl_client_sendmsg(&m, sizeof(sdlmsg_mouse_t));
		}
		break;
#if 1	// only support SDL2
	case SDL_MOUSEWHEEL:
		if(rtspconf->ctrlenable && rtspconf->sendmousemotion) {
			sdlmsg_mousewheel(&m, event->motion.x, event->motion.y);
			ctrl_client_sendmsg(&m, sizeof(sdlmsg_mouse_t));
		}
		break;
	case SDL_WINDOWEVENT:
		if(event->window.event == SDL_WINDOWEVENT_CLOSE) {
			rtspThreadParam.running = false;
			return;
		} else if(event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
			mi = windowId2ch.find(event->window.windowID);
			if(mi != windowId2ch.end()) {
				int w, h, ch = mi->second;
				char title[64];
				w = event->window.data1;
				h = event->window.data2;
				windowSizeX[ch] = w;
				windowSizeY[ch] = h;
				snprintf(title, sizeof(title), WINDOW_TITLE, ch, w, h);
				SDL_SetWindowTitle(rtspThreadParam.surface[ch], title);
				rtsperror("event window #%d(%x) resized: w=%d h=%d\n",
					ch, event->window.windowID, w, h);
			}
		}
		break;
	case SDL_USEREVENT:
		if(event->user.code == SDL_USEREVENT_RENDER_IMAGE) {
			long long ch = (long long) event->user.data2;
			render_image((struct RTSPThreadParam*) event->user.data1, (int) ch & 0x0ffffffff);
			break;
		}
		if(event->user.code == SDL_USEREVENT_CREATE_OVERLAY) {
			long long ch = (long long) event->user.data2;
			create_overlay((struct RTSPThreadParam*) event->user.data1, (int) ch & 0x0ffffffff);
			break;
		}
		if(event->user.code == SDL_USEREVENT_OPEN_AUDIO) {
			open_audio(
				(struct RTSPThreadParam*) event->user.data1,
				(AVCodecContext*) event->user.data2);
			break;
		}
		if(event->user.code == SDL_USEREVENT_RENDER_TEXT) {
			//SDL_SetAlpha()
			SDL_SetRenderDrawColor(rtspThreadParam.renderer[0], 0, 0, 0, 192/*SDL_ALPHA_OPAQUE/2*/);
			//SDL_RenderFillRect(rtspThreadParam.renderer[0], NULL);
			render_text(rtspThreadParam.renderer[0],
				rtspThreadParam.surface[0],
				-1, -1, 0, (const char *) event->user.data1);
			SDL_RenderPresent(rtspThreadParam.renderer[0]);
			break;
		}
		break;
#endif /* SDL_VERSION_ATLEAST(2,0,0) */
	case SDL_QUIT:
		rtspThreadParam.running = false;
		return;
	default:
		// do nothing
		break;
	}
	return;
}

static void *
watchdog_thread(void *args) {
	static char idlemsg[128];
	struct timeval tv;
	SDL_Event evt;
	//
	rtsperror("watchdog: launched, waiting for audio/video frames ...\n");
	//
	while(true) {
#ifdef WIN32
		Sleep(1000);
#else
		sleep(1);
#endif
		pthread_mutex_lock(&watchdogMutex);
		gettimeofday(&tv, NULL);
		if(watchdogTimer.tv_sec != 0) {
			long long d;
			d = tvdiff_us(&tv, &watchdogTimer);
			if(d > IDLE_MAXIMUM_THRESHOLD) {
				rtspThreadParam.running = false;
				break;
			} else if(d > IDLE_DETECTION_THRESHOLD) {
				// update message and show
				snprintf(idlemsg, sizeof(idlemsg),
					"Audio/video stall detected, waiting for %d second(s) to terminate ...",
					(int) (IDLE_MAXIMUM_THRESHOLD - d) / 1000000);
				//
				bzero(&evt, sizeof(evt));
				evt.user.type = SDL_USEREVENT;
				evt.user.timestamp = time(0);
				evt.user.code = SDL_USEREVENT_RENDER_TEXT;
				evt.user.data1 = idlemsg;
				evt.user.data2 = NULL;
				SDL_PushEvent(&evt);
				//
				rtsperror("watchdog: %s\n", idlemsg);
			} else {
				// do nothing
			}
		} else {
			rtsperror("watchdog: initialized, but no frames received ...\n");
		}
		pthread_mutex_unlock(&watchdogMutex);
	}
	//
	rtsperror("watchdog: terminated.\n");
	exit(-1);
	//
	return NULL;
}

int
main(int argc, char *argv[]) {
	int i;
	SDL_Event event;
	pthread_t ctrlthread;
	pthread_t watchdog;
	char savefile_keyts[128];

	if(argc < 3) {
		rtsperror("usage: %s config url\n", argv[0]);
		return -1;
	}
	//
	if(ga_init(argv[1], argv[2]) < 0) {
		rtsperror("cannot load configuration file '%s'\n", argv[1]);
		return -1;
	}

	// enable logging
	ga_openlog();
	//
	if(ga_conf_readbool("control-relative-mouse-mode", 0) != 0) {
		rtsperror("*** Relative mouse mode enabled.\n");
		relativeMouseMode = 1;
	}
	//
	if(ga_conf_readv("save-key-timestamp", savefile_keyts, sizeof(savefile_keyts)) != NULL) {
		savefp_keyts = ga_save_init_txt(savefile_keyts);
		rtsperror("*** SAVEFILE: key timestamp saved fo '%s'\n",
			savefp_keyts ? savefile_keyts : "NULL");
	}
	//
	rtspconf = rtspconf_global();
	if(rtspconf_parse(rtspconf) < 0) {
		rtsperror("parse configuration failed.\n");
		return -1;
	}
	//
#if ! defined WIN32 && ! defined __APPLE__ && ! defined ANDROID
	if(XInitThreads() == 0) {
		rtsperror("XInitThreads() failed, client terminated.\n");
		return -1;
	}
#endif
#if 0 //#ifndef ANDROID
	// init fonts
	if(TTF_Init() != 0) {
		rtsperror("cannot initialize SDL_ttf: %s\n", SDL_GetError());
		return -1;
	}
	if((defFont = TTF_OpenFont(DEFAULT_FONT, DEFAULT_FONTSIZE)) == NULL) {
		rtsperror("open font '%s' failed: %s\n",
			DEFAULT_FONT, SDL_GetError());
		return -1;
	}
#endif
	//
	rtspconf_resolve_server(rtspconf, rtspconf->servername);
	rtsperror("Remote server @ %s[%s]:%d\n",
		rtspconf->servername,
		inet_ntoa(rtspconf->sin.sin_addr),
		rtspconf->serverport);
	//
	if(SDL_Init(SDL_INIT_EVERYTHING) < 0) {
		rtsperror("SDL init failed: %s\n", SDL_GetError());
		return -1;
	}
	if(rtspconf->video_renderer_software == 0) {
		ga_error("SDL: prefer opengl hardware renderer.\n");
		SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
	}

	// launch controller?
	do if(rtspconf->ctrlenable) {
		if(ctrl_queue_init(32768, sizeof(sdlmsg_t)) < 0) {
			rtsperror("Cannot initialize controller queue, controller disabled.\n");
			rtspconf->ctrlenable = 0;
			break;
		}
		if(pthread_create(&ctrlthread, NULL, ctrl_client_thread, rtspconf) != 0) {
			rtsperror("Cannot create controller thread, controller disabled.\n");
			rtspconf->ctrlenable = 0;
			break;
		}
		pthread_detach(ctrlthread);
	} while(0);
	// launch watchdog
	pthread_mutex_init(&watchdogMutex, NULL);
	/*if(ga_conf_readbool("enable-watchdog", 1) == 1) {
		if(pthread_create(&watchdog, NULL, watchdog_thread, NULL) != 0) {
			rtsperror("Cannot create watchdog thread.\n");
			return -1;
		}
		pthread_detach(watchdog);
	} else {
		ga_error("watchdog disabled.\n");
	}*/ //FIXME!! //RAL

	/* Prepare RTSP parameters and related variables */
	bzero(&rtspThreadParam, sizeof(rtspThreadParam));
	for(i = 0; i < VIDEO_SOURCE_CHANNEL_MAX; i++) {
		pthread_mutex_init(&rtspThreadParam.surfaceMutex[i], NULL);
	}
	pthread_mutex_init(&rtspThreadParam.audioMutex, NULL);
	rtspThreadParam.url = strdup(argv[2]);

	/* Launch demultiplexing thread */
	if(rtsp_client_init(&rtspThreadParam)!= 0) {
		rtsperror("Cannot create rtsp client thread.\n");
		return -1;
	}

	while(rtspThreadParam.running) {
		if(SDL_WaitEvent(&event)) {
			ProcessEvent(&event);
		}
	}
	//
	rtspThreadParam.quitLive555 = 1;
	rtsperror("terminating ...\n");
	//
	rtsp_client_deinit(&rtspThreadParam);
	if(rtspconf->ctrlenable)
		pthread_cancel(ctrlthread);
	pthread_cancel(watchdog);
	//SDL_WaitThread(thread, &status);
	//
	if(savefp_keyts != NULL) {
		ga_save_close(savefp_keyts);
		savefp_keyts = NULL;
	}
	SDL_Quit();

	ga_deinit();
	exit(0);
	//
	return 0;
}
