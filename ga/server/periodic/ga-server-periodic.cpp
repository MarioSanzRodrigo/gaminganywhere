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
#include <stdlib.h>
#include <unistd.h>

#include "ga-common.h"
#include "ga-conf.h"
#include "ga-module.h"
#include "rtspconf.h"
#include "controller.h"
#include "encoder-common.h"

/* MediaProcessors's library related */
extern "C" {
#include <libmediaprocs/procs.h>
}

// configurations:
static char *imagepipefmt= (char*)"video-%d";
static char *filterpipefmt= (char*)"filter-%d";
static char *imagepipe0= (char*)"video-0";
static char *filterpipe0= (char*)"filter-0";
static char *filter_param[]= { imagepipefmt, filterpipefmt };

static struct gaRect *prect = NULL;
static struct gaRect rect;

static ga_module_t *m_vsource, *m_filter, *m_vencoder, *m_asource, *m_aencoder, *m_ctrl, *m_server;
static procs_ctx_t *procs_ctx= NULL;
static rtsp_server_arg_t rtsp_server_arg= {
		.rtsp_conf= NULL,
		.procs_ctx= NULL,
		.muxer_proc_id= -1
};
static vencoder_arg_t vencoder_arg= {
		.rtsp_conf= NULL,
		.mime= NULL,
		.pipefmt= NULL,
		.procs_ctx= NULL,
		.muxer_proc_id= -1,
		.flag_is_initialized= 0,
		.flag_has_started= 0
};
static aencoder_arg_t aencoder_arg= {
		.rtsp_conf= NULL,
		.mime= NULL,
		.procs_ctx= NULL,
		.muxer_proc_id= -1,
		.flag_is_initialized= 0,
		.flag_has_started= 0
};

int
load_modules() {
	if((m_vsource = ga_load_module("mod/vsource-desktop", "vsource_")) == NULL)
		return -1;
	if((m_filter = ga_load_module("mod/filter-rgb2yuv", "filter_RGB2YUV_")) == NULL)
		return -1;
	if((m_vencoder = ga_load_module("mod/encoder-video", "vencoder_")) == NULL)
		return -1;
	if(ga_conf_readbool("enable-audio", 1) != 0) {
	//////////////////////////
#ifndef __APPLE__
	if((m_asource = ga_load_module("mod/asource-system", "asource_")) == NULL)
		return -1;
#endif
	if((m_aencoder = ga_load_module("mod/encoder-audio", "aencoder_")) == NULL)
		return -1;
	//////////////////////////
	}
	if((m_ctrl = ga_load_module("mod/ctrl-sdl", "sdlmsg_replay_")) == NULL)
		return -1;
	if((m_server = ga_load_module("mod/server-live555", "live_")) == NULL)
		return -1;
	return 0;
}

int
init_modules() {
	struct RTSPConf *conf = rtspconf_global();
	//static const char *filterpipe[] = { imagepipe0, filterpipe0 };
	if(conf->ctrlenable) {
		ga_init_single_module_or_quit("controller", m_ctrl, (void *) prect);
	}
	// controller server is built-in - no need to init
	// note the order of the two modules ...
	ga_init_single_module_or_quit("video-source", m_vsource, (void*) prect);
	ga_init_single_module_or_quit("filter", m_filter, (void*) filter_param);

	/* Create PROCS module insance to be used for audio and video codecs */
	if((procs_ctx= procs_open(NULL))== NULL) {
		ga_error("Could not instantiate processors module.\n");
		exit(-1);
	}

	/* Initialize server module (before encoding modules) */
	rtsp_server_arg.rtsp_conf= conf;
	rtsp_server_arg.procs_ctx= procs_ctx;
	rtsp_server_arg.muxer_proc_id= -1; // Initialize to invlid value
	ga_init_single_module_or_quit("server-live555", m_server,
			(void*)&rtsp_server_arg);

	/* Initialize video encoders */
	vencoder_arg.rtsp_conf= conf;
	vencoder_arg.mime= (m_vencoder->mimetype!= NULL)?
			strdup(m_vencoder->mimetype): (char*)"video/NONE";
	vencoder_arg.pipefmt= filterpipefmt;
	vencoder_arg.procs_ctx= procs_ctx;
	vencoder_arg.muxer_proc_id= rtsp_server_arg.muxer_proc_id;
	ga_init_single_module_or_quit("video-encoder", m_vencoder,
			(void*)&vencoder_arg);

	/* Initialize audio encoders */
	if(ga_conf_readbool("enable-audio", 1) != 0) {
		aencoder_arg.rtsp_conf= conf;
		aencoder_arg.mime= (m_aencoder->mimetype!= NULL)?
				strdup(m_aencoder->mimetype): (char*)"audio/NONE";
		aencoder_arg.procs_ctx= procs_ctx;
		aencoder_arg.muxer_proc_id= rtsp_server_arg.muxer_proc_id;
#ifndef __APPLE__
		ga_init_single_module_or_quit("audio-source", m_asource, NULL);
#endif
		ga_init_single_module_or_quit("audio-encoder", m_aencoder,
				(void*)&aencoder_arg);
	}
	return 0;
}

int
run_modules() {
	struct RTSPConf *conf = rtspconf_global();
	static const char *filterpipe[] =  { imagepipe0, filterpipe0 };
	// controller server is built-in, but replay is a module
	if(conf->ctrlenable) {
		ga_run_single_module_or_quit("control server", ctrl_server_thread, conf);
		// XXX: safe to comment out?
		//ga_run_single_module_or_quit("control replayer", m_ctrl->threadproc, conf);
	}
	// video
	//ga_run_single_module_or_quit("image source", m_vsource->threadproc, (void*) imagepipefmt);
	if(m_vsource->start(prect) < 0)		exit(-1);
	//ga_run_single_module_or_quit("filter 0", m_filter->threadproc, (void*) filterpipe);
	if(m_filter->start(filter_param) < 0)	exit(-1);
	encoder_register_vencoder(m_vencoder, (void*)&vencoder_arg);
	// audio
	if(ga_conf_readbool("enable-audio", 1) != 0) {
	//////////////////////////
#ifndef __APPLE__
	//ga_run_single_module_or_quit("audio source", m_asource->threadproc, NULL);
	if(m_asource->start(NULL) < 0)		exit(-1);
#endif
	encoder_register_aencoder(m_aencoder, (void*)&aencoder_arg);
	//////////////////////////
	}
	// start video encoder
	if(m_vencoder != NULL && m_vencoder->start != NULL) {
		if(m_vencoder->start((void*)&vencoder_arg) < 0) {
			ga_error("video encoder: start failed.\n");
			exit(-1);
		}
	}
	// start audio encoder
	if(ga_conf_readbool("enable-audio", 1)!= 0 && m_aencoder!= NULL &&
			m_aencoder->start!= NULL) {
		if(m_aencoder->start((void*)&aencoder_arg) < 0) {
			ga_error("audio encoder: start failed.\n");
			exit(-1);
		}
	}
	// server
	if(m_server->start(NULL) < 0) exit(-1);
	//
	return 0;
}

void
handle_netreport(ctrlmsg_system_t *msg) {
	ctrlmsg_system_netreport_t *msgn = (ctrlmsg_system_netreport_t*) msg;
	ga_error("net-report: capacity=%.3f Kbps; loss-rate=%.2f%% (%u/%u); overhead=%.2f [%u KB received in %.3fs (%.2fKB/s)]\n",
		msgn->capacity / 1024.0,
		100.0 * msgn->pktloss / msgn->pktcount,
		msgn->pktloss, msgn->pktcount,
		1.0 * msgn->pktcount / msgn->framecount,
		msgn->bytecount / 1024,
		msgn->duration / 1000000.0,
		msgn->bytecount / 1024.0 / (msgn->duration / 1000000.0));
	return;
}

int
main(int argc, char *argv[]) {
	int notRunning = 0;

	if(argc < 2) {
		fprintf(stderr, "usage: %s config-file\n", argv[0]);
		return -1;
	}
	//
	if(ga_init(argv[1], NULL) < 0)	{ return -1; }
	//
	ga_openlog();
	//
	if(rtspconf_parse(rtspconf_global()) < 0)
					{ return -1; }
	//
	prect = NULL;
	//
	if(ga_crop_window(&rect, &prect) < 0) {
		return -1;
	} else if(prect == NULL) {
		ga_error("*** Crop disabled.\n");
	} else if(prect != NULL) {
		ga_error("*** Crop enabled: (%d,%d)-(%d,%d)\n", 
			prect->left, prect->top,
			prect->right, prect->bottom);
	}
	//
	if(load_modules() < 0)	 	{ return -1; }
	if(init_modules() < 0)	 	{ return -1; }
	if(run_modules() < 0)	 	{ return -1; }
	// enable handler to monitored network status
	ctrlsys_set_handler(CTRL_MSGSYS_SUBTYPE_NETREPORT, handle_netreport);
	//
	while(1) {
		usleep(5000000);
	}
	// alternatively, it is able to create a thread to run rtspserver_main:
	//	pthread_create(&t, NULL, rtspserver_main, NULL);
	//
	ga_deinit();
	//
	return 0;
}

