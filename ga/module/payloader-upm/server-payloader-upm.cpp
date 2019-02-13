#include "server-payloader-upm.h"

#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

#include "ga-module.h"
#include "rtspconf.h"

/* Payloader's library related */

#include "TcpServer.h"
#include "Interfaces.h"
#include "logger.h"
#include "pevents.h"
#include "RtpFragmenter.h"
#include "RtpHeaders.h"
#include "Streamer.h"
#include "TcpConnection.h"
#include "Packager.h"
#include "Unpackager.h"


/* MediaProcessors's library related */
extern "C" {
#include <libcjson/cJSON.h>
#include <libmediaprocsutils/stat_codes.h>
#include <libmediaprocs/procs.h>
}

/* Prototypes */
static int payloader_upm_server_deinit(void *arg);

/* Prototipos Payloader */



/* Implementations */
static ga_module_t ga_module= {0};

static int payloader_upm_server_init(void *arg)
{
printf("server-payloader init =====================%d\n", __LINE__); fflush(stdout); //FIXME!!

	char *p_stream_name;
	int ret_code, end_code= -1;
	rtsp_server_arg_t *rtsp_server_2_arg= (rtsp_server_arg_t*)arg;
	struct RTSPConf *rtspconf= NULL;
	procs_ctx_t *procs_ctx= NULL;
	char proc_settings[512]= {0};
	char *rest_str= NULL;
	cJSON *cjson_rest= NULL, *cjson_aux= NULL;

	/* Check arguments */
	if(rtsp_server_2_arg== NULL) {
		ga_error("Bad arguments at 'payloader_upm_server_init()'\n");
		return -1;
	}

	if((rtspconf= rtsp_server_2_arg->rtsp_conf)== NULL) {
		ga_error("server: no configuration found\n");
		return -1;
	}
	if((procs_ctx= rtsp_server_2_arg->procs_ctx)== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		return -1;
	}
printf("server-payloader init =====================%d\n", __LINE__); fflush(stdout); //FIXME!!
	/* Compose server initial settings */
	p_stream_name= rtspconf->object;
	if(p_stream_name!= NULL && strlen(p_stream_name+ 1)> 0){
		p_stream_name+= 1;
printf("server-payloader init =====================%d\n", __LINE__); fflush(stdout); //FIXME!!
	}else
		p_stream_name= (char*)"ga";
	snprintf(proc_settings, sizeof(proc_settings), "rtsp_port=%d"
			"&rtsp_streaming_session_name=%s",
			rtspconf->serverport> 0? rtspconf->serverport: 8554, p_stream_name);

	/* Register RTSP multiplexer in PROCS module */
printf("server-payloader init =====================%d\n", __LINE__); fflush(stdout); //FIXME!!
	ret_code= procs_opt(procs_ctx, "PROCS_POST", "payloader_upm_rtsp_mux",
			proc_settings, &rest_str);
printf("server-payloader init =====================%d\n", __LINE__); fflush(stdout); //FIXME!!
printf("[server-payloader init] ret code es: %d\n", ret_code);
	if(ret_code!= STAT_SUCCESS || rest_str== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if(ret_code!= STAT_SUCCESS || rest_str== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if((cjson_rest= cJSON_Parse(rest_str))== NULL) {
printf("server-payloader init =====================%d\n", __LINE__); fflush(stdout); //FIXME!!
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if((cjson_aux= cJSON_GetObjectItem(cjson_rest, "proc_id"))== NULL) {
printf("server-payloader init =====================%d\n", __LINE__); fflush(stdout); //FIXME!!
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	if((rtsp_server_2_arg->muxer_proc_id= cjson_aux->valuedouble)< 0) {
printf("server-payloader init =====================%d\n", __LINE__); fflush(stdout); //FIXME!!
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		goto end;
	}
	free(rest_str); rest_str= NULL;
	cJSON_Delete(cjson_rest); cjson_rest= NULL;

	end_code= 0; // "SUCCESS"
end:
printf("server-payloader init =====================%d\n", __LINE__); fflush(stdout); //FIXME!!
printf(" @@@@@ el end_code es: %d\n", end_code);
	if(cjson_rest!= NULL)
		cJSON_Delete(cjson_rest);
	if(rest_str!= NULL)
		free(rest_str);
	if(end_code!= 0) // error occurred
		payloader_upm_server_deinit(arg);
	return end_code;
}

static int payloader_upm_server_start(void *arg)
{
printf("[server-payloader-upm.cpp] START =====================%d\n", __LINE__); fflush(stdout); //FIXME!!
	payloader::TcpServer* server =  new payloader::TcpServer(); 
	//TcpServer* server =  new TcpServer(); 
	return 0;
}

static int payloader_upm_server_stop(void *arg)
{
printf("[server-payloader-upm.cpp] STOP =====================%d\n", __LINE__); fflush(stdout); //FIXME!!
	return 0;
}



static int payloader_upm_server_deinit(void *arg)
{
	rtsp_server_arg_t *rtsp_server_2_arg= (rtsp_server_arg_t*)arg;
	procs_ctx_t *procs_ctx= NULL;

	/* Check arguments */
	if(rtsp_server_2_arg== NULL) {
		ga_error("Bad arguments at 'payloader_upm_server_deinit()'\n");
		return -1;
	}

	if((procs_ctx= rtsp_server_2_arg->procs_ctx)== NULL) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		return -1;
	}

	if(procs_opt(procs_ctx, "PROCS_ID_DELETE", rtsp_server_2_arg->muxer_proc_id)
			!= STAT_SUCCESS) {
		ga_error("'%s' failed. Line %d\n", __FUNCTION__, __LINE__);
		return -1;
	}
	rtsp_server_2_arg->muxer_proc_id= -1; // Set to invalid value

	return 0;
}

ga_module_t* module_load()
{
	bzero(&ga_module, sizeof(ga_module));
	ga_module.type= GA_MODULE_TYPE_SERVER;
	ga_module.name= strdup("payloader-upm-rtsp-server");
	ga_module.init= payloader_upm_server_init;
	ga_module.start= payloader_upm_server_start;
	ga_module.stop= payloader_upm_server_stop;
	ga_module.deinit= payloader_upm_server_deinit;
	ga_module.send_packet= NULL;
	return &ga_module;
}

