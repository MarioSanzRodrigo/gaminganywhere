/*
 * Copyright (c) 2013-2015 Chun-Ying Huang
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

#include "server-live555.h"

#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

#include "ga-module.h"
#include "rtspconf.h"

/* MediaProcessors's library related */
extern "C" {
#include <libcjson/cJSON.h>
#include <libmediaprocsutils/stat_codes.h>
#include <libmediaprocs/procs.h>
}

/* Prototypes */
static int live_server_deinit(void *arg);

/* Implementations */
static ga_module_t ga_module= {0};

static int live_server_init(void *arg)
{
	char *p_stream_name;
	int ret_code, end_code= -1;
	rtsp_server_arg_t *rtsp_server_arg= (rtsp_server_arg_t*)arg;
	struct RTSPConf *rtspconf= NULL;
	procs_ctx_t *procs_ctx= NULL;
	char proc_settings[512]= {0};
	char *rest_str= NULL;
	cJSON *cjson_rest= NULL, *cjson_aux= NULL;

	/* Check arguments */
	if(rtsp_server_arg== NULL) {
		ga_error("Bad arguments at 'live_server_init()'\n");
		return -1;
	}

	if((rtspconf= rtsp_server_arg->rtsp_conf)== NULL) {
		ga_error("server: no configuration found\n");
		return -1;
	}
	if((procs_ctx= rtsp_server_arg->procs_ctx)== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		return -1;
	}

	/* Compose server initial settings */
	p_stream_name= rtspconf->object;
	if(p_stream_name!= NULL && strlen(p_stream_name+ 1)> 0)
		p_stream_name+= 1;
	else
		p_stream_name= (char*)"ga";
	snprintf(proc_settings, sizeof(proc_settings), "rtsp_port=%d"
			"&rtsp_streaming_session_name=%s",
			rtspconf->serverport> 0? rtspconf->serverport: 8554, p_stream_name);

	/* Register RTSP multiplexer in PROCS module */
	ret_code= procs_opt(procs_ctx, "PROCS_POST", "live555_rtsp_mux",
			proc_settings, &rest_str);
	if(ret_code!= STAT_SUCCESS || rest_str== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
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
	if((rtsp_server_arg->muxer_proc_id= cjson_aux->valuedouble)< 0) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	free(rest_str); rest_str= NULL;
	cJSON_Delete(cjson_rest); cjson_rest= NULL;

	end_code= 0; // "SUCCESS"
end:
	if(cjson_rest!= NULL)
		cJSON_Delete(cjson_rest);
	if(rest_str!= NULL)
		free(rest_str);
	if(end_code!= 0) // error occurred
		live_server_deinit(arg);
	return end_code;
}

static int live_server_start(void *arg)
{
	return 0;
}

static int live_server_stop(void *arg)
{
	return 0;
}

static int live_server_deinit(void *arg)
{
	rtsp_server_arg_t *rtsp_server_arg= (rtsp_server_arg_t*)arg;
	procs_ctx_t *procs_ctx= NULL;

	/* Check arguments */
	if(rtsp_server_arg== NULL) {
		ga_error("Bad arguments at 'live_server_deinit()'\n");
		return -1;
	}

	if((procs_ctx= rtsp_server_arg->procs_ctx)== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		return -1;
	}

	if(procs_opt(procs_ctx, "PROCS_ID_DELETE", rtsp_server_arg->muxer_proc_id)
			!= STAT_SUCCESS) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		return -1;
	}
	rtsp_server_arg->muxer_proc_id= -1; // Set to invalid value

	return 0;
}

ga_module_t* module_load()
{
	bzero(&ga_module, sizeof(ga_module));
	ga_module.type= GA_MODULE_TYPE_SERVER;
	ga_module.name= strdup("live555-rtsp-server");
	ga_module.init= live_server_init;
	ga_module.start= live_server_start;
	ga_module.stop= live_server_stop;
	ga_module.deinit= live_server_deinit;
	ga_module.send_packet= NULL;
	return &ga_module;
}
