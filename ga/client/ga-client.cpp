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
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <SDL2/SDL.h>

#include "rtspconf.h"
#include "rtspclient.h"

#include "controller.h"
#include "ctrl-sdl.h"

#include "ga-common.h"
#include "ga-conf.h"
#include "ga-avcodec.h"

#include <map>
using namespace std;

/* MediaProcessors's library related */
extern "C" {
#include <libmediaprocsutils/stat_codes.h>
#include <libmediaprocsutils/fifo.h>
#include <libmediaprocs/proc_if.h>
#include <libmediaprocs/procs.h>
}

#define	WINDOW_TITLE		"Player Channel (%dx%d)"

static RTSPThreadParam rtspThreadParam;

static int relativeMouseMode = 0;
static int showCursor = 1;
static int windowSizeX[VIDEO_SOURCE_CHANNEL_MAX];
static int windowSizeY[VIDEO_SOURCE_CHANNEL_MAX];
// support resizable window
static int nativeSizeX[VIDEO_SOURCE_CHANNEL_MAX];
static int nativeSizeY[VIDEO_SOURCE_CHANNEL_MAX];
static map<unsigned int, int> windowId2ch;

int var_control = 0;
int event_w_Y_iput = 0;
int event_h_Y_iput = 0;

// save files
static FILE *savefp_keyts = NULL;

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
	unsigned int renderer_flags = 0;
	int renderer_index = -1;
	SDL_Window *surface = NULL;
	SDL_Renderer *renderer = NULL;
	SDL_Texture *overlay = NULL;
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

	pthread_mutex_unlock(&rtspParam->surfaceMutex[ch]);

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
	rtspParam->overlay[ch] = overlay;
	rtspParam->renderer[ch] = renderer;
	rtspParam->windowId[ch] = SDL_GetWindowID(surface);
	rtspParam->surface[ch] = surface;
	pthread_mutex_unlock(&rtspParam->surfaceMutex[ch]);
	//
	rtsperror("ga-client: window created successfully (%dx%d).\n", w, h);

	return;
}

static void render_image(struct RTSPThreadParam *rtspThreadParam, int ch)
{
	//rtsperror("entro en render image\n");
	SDL_Rect sdlRect;
	int w_Y_iput, h_Y_iput, ret_code;
	proc_frame_ctx_t *proc_frame_ctx= NULL;
	size_t fifo_elem_size= 0;
	SDL_Window *sdlWindow= NULL;
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
	if(var_control == 0){
	
		w_Y_iput= proc_frame_ctx->width[0];
		//rtsperror("w_Y_iput %d\n", w_Y_iput);
		h_Y_iput= proc_frame_ctx->height[0];
		//rtsperror("h_Y_iput %d\n", h_Y_iput);
	}else{
		w_Y_iput= event_w_Y_iput;
		//rtsperror("event w_Y_iput %d\n", w_Y_iput);
		h_Y_iput= event_h_Y_iput;
		//rtsperror("event h_Y_iput %d\n", h_Y_iput);
	}

	if(w_Y_iput<= 0 || h_Y_iput<= 0) {
		rtsperror("Invalid frame size at renderer\n");
		goto end;
	}

	/* Get rendering variables */
	sdlWindow= rtspThreadParam->surface[ch];
	sdlRenderer= rtspThreadParam->renderer[ch];
	sdlTexture= rtspThreadParam->overlay[ch];
	if(sdlWindow== NULL || sdlRenderer== NULL || sdlTexture== NULL) {
		//rtsperror("entro en render image,  if\n");
		union SDL_Event evt;

		rtspThreadParam->width[ch] = proc_frame_ctx->width[0];
		rtspThreadParam->height[ch] = proc_frame_ctx->height[0];
		rtspThreadParam->format[ch] = (AVPixelFormat)AV_PIX_FMT_YUV420P;
		bzero(&evt, sizeof(evt));
		evt.user.type = SDL_USEREVENT;
		evt.user.timestamp = time(0);
		evt.user.code = SDL_USEREVENT_CREATE_OVERLAY;
		evt.user.data1 = rtspThreadParam;
		evt.user.data2 = (void*)(long long)ch;
		SDL_PushEvent(&evt);
		goto end; // Note we are skipping this frame
	} else if(w_Y_iput!= rtspThreadParam->width[ch] ||
			h_Y_iput!= rtspThreadParam->height[ch]) {
		//rtsperror("entro en render image,  else\n");

		/* Set / backup new size */
		if(var_control==0){
			rtspThreadParam->width[ch]= w_Y_iput;
			rtspThreadParam->height[ch]= h_Y_iput;
			windowSizeY[ch]= w_Y_iput;
			windowSizeX[ch]= h_Y_iput;
			rtsperror("Resize image #%d resized: w=%d h=%d\n", ch, w_Y_iput,
					h_Y_iput);

			/* Set new window size (if applicable) and restore texture */
			pthread_mutex_lock(&rtspThreadParam->surfaceMutex[ch]);
			SDL_SetWindowSize(sdlWindow, w_Y_iput, h_Y_iput);
			SDL_DestroyTexture(sdlTexture);
			rtspThreadParam->overlay[ch]= SDL_CreateTexture(sdlRenderer,
					SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING,
					w_Y_iput, h_Y_iput);
			if(rtspThreadParam->overlay[ch]== NULL) {
				rtsperror("ga-client: create overlay (textuer) failed.\n");
				exit(-1);
			}
			pthread_mutex_unlock(&rtspThreadParam->surfaceMutex[ch]);
		}else{
			windowSizeY[ch]= event_w_Y_iput;
			windowSizeX[ch]= event_h_Y_iput;
		}
	}

	/* Specify rendering area/rectangle */
	sdlRect.x= 0;
	sdlRect.y= 0;
	//rtsperror("entro en render image - sdlRect w/h,\n");
	sdlRect.w= w_Y_iput;
	sdlRect.h= h_Y_iput;

	/* Do render */
	SDL_UpdateYUVTexture(sdlTexture, NULL,
			proc_frame_ctx->p_data[0], proc_frame_ctx->linesize[0],
			proc_frame_ctx->p_data[1], proc_frame_ctx->linesize[1],
			proc_frame_ctx->p_data[2], proc_frame_ctx->linesize[2]);
	SDL_RenderClear(sdlRenderer);
	SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, &sdlRect);
	//rtsperror("Render image - SDL_RenderCopy,\n");
	SDL_RenderPresent(sdlRenderer);
	//rtsperror("Render image - SDL_RenderPresent,\n");

end:
	if(proc_frame_ctx!= NULL)
		proc_frame_ctx_release(&proc_frame_ctx);
	return;
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
			if(showCursor)
				SDL_SetRelativeMouseMode(SDL_FALSE);
			else
				SDL_SetRelativeMouseMode(SDL_TRUE);
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
				event_w_Y_iput = w;
				event_h_Y_iput = h;
				var_control = 1;
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
		else if(event->user.code == SDL_USEREVENT_CREATE_OVERLAY) {
			long long ch = (long long) event->user.data2;
			create_overlay((struct RTSPThreadParam*) event->user.data1, (int) ch & 0x0ffffffff);
			break;
		}
		break;
	case SDL_QUIT:
		rtspThreadParam.running = false;
		return;
	default:
		// do nothing
		break;
	}
	return;
}

int
main(int argc, char *argv[]) {
	int i;
	SDL_Event event;
	pthread_t ctrlthread;
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

	/* Prepare RTSP parameters and related variables */
	bzero(&rtspThreadParam, sizeof(rtspThreadParam));
	for(i = 0; i < VIDEO_SOURCE_CHANNEL_MAX; i++) {
		pthread_mutex_init(&rtspThreadParam.surfaceMutex[i], NULL);
	}
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
