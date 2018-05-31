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
#include <mongoose.h> // HTTP-server facilities
#include <libmediaprocs/procs_api_http.h> // HTTP-REST facilities
#include <libmediaprocs/procs.h>
}

/**
 * HTTP-server data.
 */
typedef struct http_server_thr_ctx_s {
	volatile int *ref_flag_exit;
	const char *ref_listening_port;
	procs_ctx_t *procs_ctx;
} http_server_thr_ctx_t;


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

static volatile int flag_app_exit= 0;

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

static void http_event_handler(struct mg_connection *c, int ev, void *p)
{
#define URI_MAX 4096
#define METH_MAX 16
#define BODY_MAX 4096000

	if(ev== MG_EV_HTTP_REQUEST) {
		register size_t uri_len= 0, method_len= 0, qs_len= 0, body_len= 0;
		const char *uri_p, *method_p, *qs_p, *body_p;
		struct http_message *hm= (struct http_message*)p;
		char *url_str= NULL, *method_str= NULL, *str_response= NULL,
				*qstring_str= NULL, *body_str= NULL;
		http_server_thr_ctx_t *http_server_thr_ctx= (http_server_thr_ctx_t*)
				c->user_data;

		if((uri_p= hm->uri.p)!= NULL && (uri_len= hm->uri.len)> 0 &&
				uri_len< URI_MAX) {
			url_str= (char*)calloc(1, uri_len+ 1);
			if(url_str!= NULL)
				memcpy(url_str, uri_p, uri_len);
		}
		if((method_p= hm->method.p)!= NULL && (method_len= hm->method.len)> 0
				 && method_len< METH_MAX) {
			method_str= (char*)calloc(1, method_len+ 1);
			if(method_str!= NULL)
				memcpy(method_str, method_p, method_len);
		}
		if((qs_p= hm->query_string.p)!= NULL &&
				(qs_len= hm->query_string.len)> 0 && qs_len< URI_MAX) {
			qstring_str= (char*)calloc(1, qs_len+ 1);
			if(qstring_str!= NULL)
				memcpy(qstring_str, qs_p, qs_len);
		}
		if((body_p= hm->body.p)!= NULL && (body_len= hm->body.len)> 0
				&& body_len< BODY_MAX) {
			body_str= (char*)calloc(1, body_len+ 1);
			if(body_str!= NULL)
				memcpy(body_str, body_p, body_len);
		}

		/* Process HTTP request */
		if(url_str!= NULL && method_str!= NULL)
			procs_api_http_req_handler(http_server_thr_ctx->procs_ctx, url_str,
					qstring_str, method_str, body_str, body_len, &str_response);
		/* Send response */
		if(str_response!= NULL && strlen(str_response)> 0) {
			//printf("str_response: %s (len: %d)\n", str_response,
			//		(int)strlen(str_response)); //comment-me
			mg_printf(c, "%s", "HTTP/1.1 200 OK\r\n");
			mg_printf(c, "Content-Length: %d\r\n", (int)strlen(str_response));
			mg_printf(c, "\r\n");
			mg_printf(c, "%s", str_response);
		} else {
			mg_printf(c, "%s", "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
		}

		if(str_response!= NULL)
			free(str_response);
		if(url_str!= NULL)
			free(url_str);
		if(method_str!= NULL)
			free(method_str);
		if(qstring_str!= NULL)
			free(qstring_str);
		if(body_str!= NULL)
			free(body_str);
	} else if(ev== MG_EV_RECV) {
		mg_printf(c, "%s", "HTTP/1.1 202 ACCEPTED\r\nContent-Length: 0\r\n");
	} else if(ev== MG_EV_SEND) {
		mg_printf(c, "%s", "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
	}
#undef URI_MAX 4096
#undef METH_MAX 16
#undef BODY_MAX 4096000
}

/*
 * Runs HTTP server thread, listening to the given port.
 */
static void* http_server_thr(void *t)
{
	struct mg_mgr mgr;
	struct mg_connection *c;
	http_server_thr_ctx_t *http_server_thr_ctx= (http_server_thr_ctx_t*)t;
	struct mg_bind_opts opts;
	const char *error_str= NULL;

	/* Check argument */
	if(http_server_thr_ctx== NULL) {
		fprintf(stderr, "Bad argument '%s'\n", __FUNCTION__);
		exit(1);
	}

	/* Create and configure the server */
	mg_mgr_init(&mgr, NULL);

	memset(&opts, 0, sizeof(opts));
	opts.error_string= &error_str;
	opts.user_data= http_server_thr_ctx;
	c= mg_bind_opt(&mgr, http_server_thr_ctx->ref_listening_port,
			http_event_handler, opts);
	if(c== NULL) {
		fprintf(stderr, "mg_bind_opt(localhost:%s) failed: %s\n",
				http_server_thr_ctx->ref_listening_port, error_str);
		exit(EXIT_FAILURE);
	}
	mg_set_protocol_http_websocket(c);

	while(*http_server_thr_ctx->ref_flag_exit== 0)
		mg_mgr_poll(&mgr, 1000);

	mg_mgr_free(&mgr);
	return NULL;
}

static void stream_proc_quit_signal_handler(int)
{
	printf("signaling application to finalize...\n"); fflush(stdout);
	flag_app_exit= 1;
}

int main(int argc, char *argv[])
{
	sigset_t set;
	char *endptr, *http_server_port_p= NULL, *http_server_port= "8080";
	char conf_buf[64]= {0};
	http_server_thr_ctx_t http_server_thr_ctx= {0};

	/* Set SIGNAL handlers to this process */
	sigfillset(&set);
	sigdelset(&set, SIGINT);
	pthread_sigmask(SIG_SETMASK, &set, NULL);
	signal(SIGINT, stream_proc_quit_signal_handler);

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

	/* Set HTTP-server listening port */
	if((http_server_port_p= ga_conf_readv("http-server-port", conf_buf,
			sizeof(conf_buf)))!= NULL) {
		int val= strtol(http_server_port_p, &endptr, 10);
		if(endptr!= http_server_port_p) {
			http_server_port= http_server_port_p;
			//printf("Http-port is %ld\n", val); fflush(stdout); //comment-me
		}
	}

	/* Launch HTTP-server */
	if(procs_ctx== NULL) { // Sanity check
		ga_error("PROCS module should be initialized previously.\n");
		exit(-1);
	}
	http_server_thr_ctx.ref_flag_exit= &flag_app_exit;
	http_server_thr_ctx.ref_listening_port= http_server_port;
	http_server_thr_ctx.procs_ctx= procs_ctx;
	printf("Starting server...\n"); fflush(stdout);
	http_server_thr(&http_server_thr_ctx);
	//while(1) {
	//	usleep(5000000);
	//}
	// alternatively, it is able to create a thread to run rtspserver_main:
	//	pthread_create(&t, NULL, rtspserver_main, NULL);
	//
	ga_deinit();
	//
	return 0;
}

