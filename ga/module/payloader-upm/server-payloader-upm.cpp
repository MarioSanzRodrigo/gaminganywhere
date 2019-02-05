#include "server-payloader-upm.h"
#ifndef TCPSERVER_H
#define TCPSERVER_H 

#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

#include "ga-module.h"
#include "rtspconf.h"

/* Payloader's library related */
#include <libmediaprocspayloader/logger.h>
#include <libmediaprocspayloader/Interfaces.h>
#include <libmediaprocspayloader/TcpServer.h>
#include <libmediaprocspayloader/TcpConnection.h>
#include <libmediaprocspayloader/Packager.h>
#include <libmediaprocspayloader/Sender.h>
#include <libmediaprocspayloader/RtpFragmenter.h>
#include <libmediaprocspayloader/RtpHeaders.h>

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

/* PAYLOADER UPM */
namespace payloader {

typedef struct parametrosThread2
{
	int   Client;
	fd_set descriptoresLectura;
	//neosmart_event_t  evento;
}paramThread2;

class TcpServer: public PortReceiver
{
	public:
		TcpServer();
		virtual ~TcpServer();
		long int getterData();
		void tomaPuerto(long int puerto);
		char* data;
	    	long int puerto;
	   	pthread_t mithread;
	private:
		void start_accept();
		//void handle_accept(TcpConnection::pointer new_connection, const boost::system::error_code& error);

};

void* thread_function02(void* myparamThread) {

	paramThread2 *myparamThread2 = (paramThread2*)myparamThread;
	int Client = myparamThread2 -> Client;
	fd_set descriptoresLectura = myparamThread2 -> descriptoresLectura;
	//neosmart_event_t event = myparamThread2 -> evento;

	while(true)
	{
		//printf("Hebra 2 pensando...\n");
		select (Client+1, &descriptoresLectura, NULL, NULL, NULL);
		if (FD_ISSET (Client, &descriptoresLectura))
		{
			//printf("Evento de negociacion.\n");
			//SetEvent(event);
			FD_ZERO (&descriptoresLectura); 
			FD_SET (Client, &descriptoresLectura); 
		}
	}
}

void* thread_function01(void* clientSockect) {
	paramThread2 parametros;
	pthread_t mithread2;
	bool Stop = false;
	bool StreamingStarted = false;
	int Client = *(int*)clientSockect;
	char         RecvBuf[10000];                    // receiver buffer
	int          res;  
	//payloader::Streamer* m_Streamer = new payloader::Streamer();                  // our streamer for UDP/TCP based RTP transport
	//payloader::TcpConnection* new_connection = new payloader::TcpConnection(Client, m_Streamer);     // our threads RTSP session and state
	//neosmart_event_t events[2];
	//events[0] = CreateEvent(false, false); //comprobación periódica     bool manualReset, bool initialState
	//events[1] = CreateEvent(false, false); //abort manual-reset event

	//timer_start(do_something, 1000, events[0]);//Aqui hay que pasarle el evento para setevent periodicamente

	fd_set descriptoresLectura; 
	FD_ZERO (&descriptoresLectura); 
	FD_SET (Client, &descriptoresLectura); 

	parametros.Client = Client;
	parametros.descriptoresLectura = descriptoresLectura;
	//parametros.evento = events[1];
  
	pthread_create( &mithread2, NULL, &thread_function02,  &parametros);

	while (!Stop)
	{
		int index = -1;
		//WaitForMultipleEvents(events,2, false, -1, index);//Index indica si ha pasado un evento u otro, ESPERA INFINITA
		switch (index)
		{//Espera a que suceda algún evento, recivir por el socket o la comprobacion periodica
		case 1 : 
		{   
			//printf("Index: %d\n", index); 
			//printf("Negociando...\n");
			// read client socket
			//ResetEvent(events[1]);
			memset(RecvBuf,0x00,sizeof(RecvBuf));
			res = recv(Client,RecvBuf,sizeof(RecvBuf),0);

			// we filter away everything which seems not to be an RTSP command: O-ption, D-escribe, S-etup, P-lay, T-eardown
			if ((RecvBuf[0] == 'O') || (RecvBuf[0] == 'D') || (RecvBuf[0] == 'S') || (RecvBuf[0] == 'P') || (RecvBuf[0] == 'T'))
			{
				//RTSP_CMD_TYPES C = new_connection->Handle_RtspRequest(RecvBuf,res);
				//if (C == RTSP_PLAY)     StreamingStarted = true; else if (C == RTSP_TEARDOWN) Stop = true; /*else if (C == RTSP_PAUSE) StreamingStarted = false;*/
			};
            		break;      
        	};
        	case 0 : 
        	{   
			//printf("Index: %d\n", index); 
			//printf("Comprobando...\n");
			//if (StreamingStarted) m_Streamer->StartStreaming();
			break;
		};
		};
 	 };
	close(Client);
	return 0;
}

// Constructor: initialises an acceptor to listen on TCP port 8554.
TcpServer::TcpServer(){//Creamos el soocket en el puerto 8554 escuchando tcp para la conexión rtsp
	start_accept();
}
TcpServer::~TcpServer(){

}

void TcpServer::tomaPuerto(long int puerto){
	this->puerto = puerto;
}

long int TcpServer::getterData(){
	return puerto;
}

void TcpServer::start_accept(){
	int      MasterSocket;                                 		// our masterSocket(socket that listens for RTSP client connections)  
	int      ClientSocket;                                 		// RTSP socket to handle an client
	sockaddr_in ServerAddr;                                  	// server address parameters
	sockaddr_in ClientAddr;                                   	// address parameters of a new RTSP client
	socklen_t         ClientAddrLen = sizeof(ClientAddr);   
	ServerAddr.sin_family      = AF_INET;   
	ServerAddr.sin_addr.s_addr = INADDR_ANY;   
	ServerAddr.sin_port        = htons(8556);                 	// listen on RTSP port 8554
	MasterSocket               = socket(AF_INET,SOCK_STREAM,0);   
	// creates a socket
	// bind our master socket to the RTSP port and listen for a client connection
	if (bind(MasterSocket,(sockaddr*)&ServerAddr,sizeof(ServerAddr)) != 0) return;  
	if (listen(MasterSocket,5) != 0) return;
	/* MARIO
	while (true)  
	{   // loop forever to accept client connections
		ClientSocket = accept(MasterSocket,(struct sockaddr*)&ClientAddr,&ClientAddrLen);   
         	//CreateThread(NULL,0,thread_function01,&ClientSocket,0);
		pthread_create( &mithread, NULL, &thread_function01,  &ClientSocket);
		printf("Client connected. Client address: %s\r\n",inet_ntoa(ClientAddr.sin_addr));  
	}  
	close(MasterSocket);  
	*/ 
}
}
#endif // TCPSERVER_H

static int payloader_upm_server_start(void *arg)
{
printf("[server-payloader-upm.cpp] START =====================%d\n", __LINE__); fflush(stdout); //FIXME!!
	payloader::TcpServer* server =  new payloader::TcpServer(); 
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

