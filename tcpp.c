/*
===================================================
Name        : hsvr.c  libevent wrapper
Author      : H.Hata
Version     :
Copyright   : OLT 
Description : Ansi-style
======================================================
*/
#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#define strcasecmp(x,y) _stricmp(x,y)
#define THREAD_CC   __cdecl
#define THREAD_TYPE DWORD
#define THREAD_CREATE(tid, entry, arg) do{ _beginthread((entry),0,(arg));\
            (tid)=GetCurrentThreadId();\
        }while(0)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <pthread.h>
#define THREAD_CC *
#define THREAD_TYPE pthread_t
#define THREAD_CREATE(tid, entry, arg) thread_create_linux(&(tid), (entry),     (arg))
#endif
#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/bufferevent_compat.h>
#include <event2/bufferevent_ssl.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>
#include <event2/thread.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include "tcpp.h"
#define STACK_SIZE (256*1024)
#define	SESSION_MAX		4000
#define BUFF_LEN	64000
#define PORT 3002
#define PEERPORT 80
#define PEERADDR "127.0.0.1"
#define THREAD_NUM	16
#define RECV_TIMER 60 
#define CONN_TIMER  3 
#define LOG_MSG
#define LOG_ERR 0
unsigned int accept_counter=0;
unsigned int session_highwatermark=0;
unsigned int session_gauge=0;
unsigned int session_error=0;
unsigned int discarded_octet=0;
unsigned int discarded_packet=0;
unsigned int rxoctet=0;
unsigned int rx=0;
unsigned int txoctet=0;
unsigned int tx=0;
unsigned int rxtimeout=0;
unsigned int connect_error=0;
unsigned int pool_session[THREAD_NUM]={0};


static char msg[1024];
static struct event_base *base;
static struct event_base *base_pool[THREAD_NUM];
static unsigned int session_max=SESSION_MAX;
static pthread_mutex_t free_lock;
static unsigned int thread_num=1;
static void signal_cb(evutil_socket_t sig, short events, void *ctx);
static void freeSession(SESSION *self);
static void writecb(struct bufferevent *bev,void *ctx);
static void readcb(struct bufferevent *bev,void *ctx);
static void eventcb(struct bufferevent *bev, short what, void *ctx);
static void acceptcb(struct evconnlistener *listener, evutil_socket_t fd, struct sockaddr *a, int slen, void *p);
static void fatal_cb(int err);
static void timecb(evutil_socket_t fd, short what, void *ctx);
unsigned long pthreads_thread_id(void);
void pthreads_locking_callback(int mode, int type, 
	          const char *file, int line);
static int thread_create_linux(pthread_t *tid, void *(*entry)(void *), void *arg);
void exec_s_stop(evutil_socket_t sig, short events, void *ctx);

//#define LOG_MSG
#ifdef LOG_MSG
void log_msg(int level,char *msg)
{
	puts(msg);
}
#else
extern void log_out(char *msg);
#define log_msg log_out
#endif
void exec_s_stop(evutil_socket_t sig, short events, void *ctx)
{
	int i;
	struct event_base *base = (struct event_base*)ctx;
	struct timeval delay = { 0, 100 };
	event_base_loopexit(base, &delay);
	sprintf(msg,"session_gauge=%d",session_gauge);log_msg(LOG_ERR,msg);
	sprintf(msg,"session_high=%d",session_highwatermark);log_msg(LOG_ERR,msg);
	sprintf(msg,"accept_counter=%d",accept_counter);log_msg(LOG_ERR,msg);
	sprintf(msg,"session_error=%d",session_error);log_msg(LOG_ERR,msg);
	sprintf(msg,"rxoctet=%d",rxoctet);log_msg(LOG_ERR,msg);
	sprintf(msg,"rxcount=%d",rx);log_msg(LOG_ERR,msg);
	sprintf(msg,"txoctet=%d",txoctet);log_msg(LOG_ERR,msg);
	sprintf(msg,"txcount=%d",tx);log_msg(LOG_ERR,msg);
	sprintf(msg,"rxtimeout=%d",rxtimeout);log_msg(LOG_ERR,msg);
	sprintf(msg,"discarded_octet=%d",discarded_octet);log_msg(LOG_ERR,msg);
	sprintf(msg,"discarded_packet=%d",discarded_packet);log_msg(LOG_ERR,msg);
	sprintf(msg,"connect_error=%d",connect_error);log_msg(LOG_ERR,msg);
	for(i=0;i<thread_num;i++){
		sprintf(msg,"thread %d session=%d",i,pool_session[i]);
		log_msg(LOG_ERR,msg);
	}
}
static void signal_cb(evutil_socket_t sig, short events, void *ctx)
{
	log_msg(LOG_ERR,"Caught an SIG");
	exec_s_stop(sig, events, ctx);
}

static void freeSession(SESSION *self)
{
	struct evbuffer *ev;
	struct bufferevent *bev;
	size_t len;
	SESSION *partner;
	if(0!=pthread_mutex_lock(&free_lock)){
		return ;
	}
	bev=self->ubev;
	ev=bufferevent_get_output(bev);
	/*送信データ残があるか？*/
	len=evbuffer_get_length(ev);
	if(len!=0){
		/*送信完了通知ハンドラをセットする*/
		bufferevent_setwatermark(bev,EV_WRITE,0,0);
		bufferevent_setcb(bev,readcb,writecb,eventcb,self);
		//bufferevent_disable(self->bev,EV_READ);//受信停止
		pthread_mutex_unlock(&free_lock);
		return;
	}
	/*自端を閉じる*/
	bufferevent_free(self->bev);
#ifdef DEBUG
	sprintf(msg,
		"closed session(%d)\n Rx:%zd / %zd octet\n Tx:%zd /  %zd octet",
		self->id,self->rx,self->rxoctet,self->tx,self->txoctet);
	log_msg(LOG_ERR,msg);
#endif
	rx+=(unsigned int)self->rx;
	rxoctet+=(unsigned int)self->rxoctet;
	tx+=(unsigned int)self->tx;
	txoctet+=(unsigned int)self->txoctet;
	rxtimeout+=(unsigned int)self->timeout;
//another side
	partner=self->partner;
	bufferevent_free(partner->bev);
	free(self);
	free(partner);
	session_gauge--;
	pthread_mutex_unlock(&free_lock);
}
static void writecb(struct bufferevent *bev,void *ctx)
{
#ifdef DEBUG
	log_msg(LOG_ERR,"writecb");
#endif
	freeSession((SESSION *)ctx);
}
static void readcb(struct bufferevent *bev,void *ctx)
{
	SESSION *self;
	struct evbuffer *src ;
	//size_t len;
	int n;
	unsigned char sbuff[BUFF_LEN];
	
#ifdef DEBUG
	//sprintf(msg,"readcb <<<<<<IN %u",pthread_self());
	//log_msg(LOG_ERR,msg);
#endif
	self = (SESSION *)ctx;
	src = bufferevent_get_input(bev);//自側
	//len = evbuffer_get_length(src);
	memset(sbuff,0,BUFF_LEN);
	n=evbuffer_remove(src,sbuff,BUFF_LEN-8);
	if(n<=0){
		sprintf(msg,"event_remove error %d",n);
		log_msg(LOG_ERR,msg);
		return ;
	}
#ifdef DEBUG
	if(self->side==S_CLIENT){
		sprintf(msg,"%lu RECV from CLIENT %d octet(s)",time(NULL),n);
	}else{
		sprintf(msg,"%lu RECV from SERVER %d octet(s)",time(NULL),n);
	}
	log_msg(LOG_ERR,msg);
#endif
	self->rx++;
	self->rxoctet += (unsigned int )n;
	S_send(self->partner,sbuff,(size_t)n);
	return;
}


static void eventcb(struct bufferevent *bev, short what, void *ctx)
{
	SESSION *self;
	unsigned long err;
	int err2;
	char *emsg;
	char *lib;
	char *func;

	self=ctx;
#ifdef DEBUG
	if(self->side==S_CLIENT){
		sprintf(msg,"%lu eventcb %X in CLIENT",time(NULL),what);
	}else{
		sprintf(msg,"%lu eventcb %X in SERVER",time(NULL),what);
	}
	log_msg(LOG_ERR,msg);
	if(what & 1) log_msg(LOG_ERR,"EV_READ");
	if(what & 2) log_msg(LOG_ERR,"EV_WRITE");
	if(what & 0x10) log_msg(LOG_ERR,"EV_EOF");
	if(what & 0x20) log_msg(LOG_ERR,"EV_ERR");
	if(what & 0x40) log_msg(LOG_ERR,"EV_TIMEOUT");
	if(what & 0x80) log_msg(LOG_ERR,"EV_CONNECT");
#endif
	if (what&BEV_EVENT_ERROR) {//エラー発生
		err2 = EVUTIL_SOCKET_ERROR();
		sprintf(msg, "Got an error %d (%s) on the listener. ",
						err2, evutil_socket_error_to_string(err2));
		log_msg(LOG_ERR,msg);
		//エラー出力
		while ((err = (bufferevent_get_openssl_error(bev)))) {
			emsg=(char *)ERR_reason_error_string(err);
			lib=(char*)ERR_lib_error_string(err);
			func=(char*)ERR_func_error_string(err);
			sprintf(msg,"OpenSSL Error: %s in %s %s",emsg,lib,func);
			log_msg(LOG_ERR,msg);
		}
		if (errno){
			sprintf(msg,"Connection Error errno=%d",errno);
			log_msg(LOG_ERR,msg);
		}
		//コネクションが閉じられます
		freeSession(self);
	}else if(what & BEV_EVENT_EOF){//ピア端クローズ
		//コネクションが閉じられます
		freeSession(self);
	}else if(what & BEV_EVENT_TIMEOUT){//受信タイムアウト
		self->timeout++;
		//コネクションが閉じられます
		freeSession(self);
#ifdef DEBUG
		log_msg(LOG_ERR,"Session Timeout");
#endif
	}else if (what & BEV_EVENT_CONNECTED) {
		//NOP
	}
}
static int pickupServer(char *server,uint16_t *port)
{
	//strcpy(server,"27.120.85.91"); *port=3843;
	strcpy(server,"127.0.0.1"); *port=12345;
	return 0;
}
static void acceptcb(struct evconnlistener *listener, evutil_socket_t fd, struct sockaddr *a, int slen, void *p)
{
	struct bufferevent *bev_cli;
	struct bufferevent *bev_svr;
	struct sockaddr_in *sa;
	struct sockaddr_in self;
	unsigned int tid;
	SESSION *ses_cli;
	SESSION *ses_svr;
	socklen_t len;
	int ret;

#ifdef DEBUG
	sprintf(msg,"%lu acceptcb sid=%d tid=%u",time(NULL),
			accept_counter+1,(int)pthread_self());
	log_msg(LOG_ERR,msg);
#endif
	sa=(struct sockaddr_in *)a;
	tid=accept_counter%thread_num;
	pool_session[tid]++;
	bev_cli = bufferevent_socket_new(base_pool[tid], fd,
	    BEV_OPT_CLOSE_ON_FREE|BEV_OPT_DEFER_CALLBACKS
			|BEV_OPT_THREADSAFE
			);
	/*****/
	len=sizeof(self);
	ret=getsockname(fd,(struct sockaddr*)&self,&len);
	if(ret!=0 || self.sin_family!=AF_INET){
				self.sin_family=AF_UNSPEC;
	}
	/*****/
	if(session_gauge>session_max){
//#ifdef DEBUG
		sprintf(msg," session max limit gauge=%u",session_gauge);
		log_msg(LOG_ERR,msg);
//#endif
		//セッション数過大でTCP切断
		//何らかの代理応答を返す場合はここでそのプロシージャを呼ぶ
		//Todo: ReplyError(brv_cli);
		bufferevent_free(bev_cli);
		printf("Error1\n");
		session_error++;
		//exit(0);//For debug
		return;
	}
	bev_svr = bufferevent_socket_new(base_pool[tid], -1,
	    BEV_OPT_CLOSE_ON_FREE|BEV_OPT_DEFER_CALLBACKS
			|BEV_OPT_THREADSAFE
			);
	assert(bev_cli && bev_svr);
	ses_cli=(SESSION *)malloc(sizeof(SESSION));
	if(ses_cli==NULL){
		log_msg(LOG_ERR," malloc error");
		bufferevent_free(bev_cli);
		bufferevent_free(bev_svr);
		printf("ERROR2");
		return;
	}
	ses_svr=(SESSION *)malloc(sizeof(SESSION));
	if(ses_svr==NULL){
		log_msg(LOG_ERR," malloc error");
		bufferevent_free(bev_cli);
		bufferevent_free(bev_svr);
		free(ses_cli);
		printf("ERROR2");
		return;
	}
	session_gauge++;
	if(session_highwatermark<session_gauge){
		session_highwatermark=session_gauge;
	}
	accept_counter++;
	memset(ses_cli,0,sizeof(SESSION));
	memset(ses_svr,0,sizeof(SESSION));
	ses_cli->id=accept_counter;
	ses_cli->side=S_CLIENT;
	ses_cli->ubev=bev_cli;//アンダーレイ
	ses_cli->bev=bev_cli;
	ses_cli->partner=ses_svr;
	strncpy(ses_cli->host,inet_ntoa(sa->sin_addr),31);
	ses_cli->port=ntohs((uint16_t)sa->sin_port);
	if(self.sin_family==AF_INET){
		strncpy(ses_cli->self_host,inet_ntoa(self.sin_addr),31);
		ses_cli->self_port=ntohs((uint16_t)self.sin_port);
	}
	ses_svr->id=accept_counter;
	ses_svr->side=S_SERVER;
	ses_svr->ubev=bev_svr;//アンダーレイ
	ses_svr->bev=bev_svr;
	ses_svr->partner=ses_cli;
	/****/
#ifdef DEBUG
	sprintf(msg,"session start counter=%d gauge=%d",
			accept_counter,session_gauge);
	log_msg(LOG_ERR,msg);
	sprintf(msg,"peer=%s port=%d",ses_cli->host,ses_cli->port);
	log_msg(LOG_ERR,msg);
#endif
	if(pickupServer(ses_svr->host,&ses_svr->port)!=0){
		log_msg(LOG_ERR,"no erver to connect");
		goto err;
		return;
	}
	if(S_connect(ses_svr)!=0){
		sprintf(msg,"connect error for %s:%hu",ses_svr->host,ses_svr->port);
		log_msg(LOG_ERR,msg);
		goto err;
		return;
	}
	S_settimer(ses_cli,60);
	S_settimer(ses_svr,60);
	bufferevent_setcb(ses_cli->bev, readcb, NULL, eventcb, ses_cli);
	bufferevent_setcb(ses_svr->bev, readcb, NULL, eventcb, ses_svr);
	return;
err:
	bufferevent_free(bev_cli);
	bufferevent_free(bev_svr);
	free(ses_cli);
	free(ses_svr);
}

static void fatal_cb(int err)
{
	sprintf(msg,"Err:%d\n",err);
	log_msg(LOG_ERR,msg);
	_exit(1);
}
static void timecb(evutil_socket_t fd, short what, void *ctx)
{
	return ;
}

void *thread_start(void *ctx){
	struct event_base *base = (struct event_base*)ctx;
	struct event *ev;
	struct timeval tv = {1,0};
	pthread_t pt; 
#ifdef DEBUG
	sprintf(msg,"Thread %u start",(int)pthread_self());
	log_msg(LOG_ERR,msg);
#endif
	pt=pthread_self();
	ev=event_new(base, -1,EV_TIMEOUT|EV_PERSIST, timecb,&pt);
	event_add(ev,&tv);
	event_base_dispatch(base);
	event_free(ev);
	event_base_free(base);
#ifdef DEBUG
	sprintf(msg,"Thread %d quit ",(int)pthread_self());
	log_msg(LOG_ERR,msg);
#endif
	return NULL;
}

int S_connect(SESSION *self){
	struct hostent *hent;
	struct sockaddr_in peer;
	int socklen;

	socklen=(int)sizeof(struct sockaddr_in);
	peer.sin_port=htons(self->port);
	peer.sin_addr.s_addr=inet_addr(self->host);
	peer.sin_family=AF_INET;
	if(peer.sin_addr.s_addr==INADDR_NONE){
		hent=gethostbyname(self->host);
		if(hent==NULL){
			log_msg(LOG_ERR,"inner server unknown");
			return -1;
		}
	}
	bufferevent_settimeout(self->bev,CONN_TIMER,0);
	if(bufferevent_socket_connect(
				self->bev,(struct sockaddr *)&peer,socklen)<0){
		log_msg(LOG_ERR,"bufferevent_socket_connect");
		return -1;
	}
	bufferevent_enable(self->bev,EV_READ);
	bufferevent_enable(self->partner->bev,EV_READ);
	return 0;
}

void S_settimer(SESSION *s,int t)
{
	struct bufferevent *bev;
	if(s==NULL){
		return ;
	}
	if(s->bev==NULL){
		return ;
	}
	bev=s->bev;
	bufferevent_settimeout(bev,t,0);
}
int S_send(SESSION *s,unsigned char *data,size_t len)
{
	struct bufferevent *bev;
	if(s==NULL){
		return -1;
	}
	if(s->bev==NULL){
		return -2;
	}
	bev=s->bev;
	if(data==NULL || len==0){
		return -3;
	}
	s->txoctet+=len;
	s->tx++;
	return bufferevent_write(bev,(const void *)data,len);
}
void S_close(SESSION *s)
{
	freeSession(s);
}

int S_start(
		uint16_t port,
		int limit,
		unsigned int multi,
		int tls,
		char *cert,
		char *priv)
{
	int socklen;
	struct evconnlistener *listener;
	struct sockaddr localaddr;
	struct sockaddr_in *sin;
	struct event *signal_event;
	struct sigaction sa;
	pthread_t thread;
	int i;
	if(limit<=0){
		session_max=SESSION_MAX;
	}else{
		session_max=(unsigned int)limit;
	}
	if(multi<=0){
		thread_num=1;
	}else if(multi>=THREAD_NUM){
		thread_num=THREAD_NUM;
	}else{
		thread_num=multi;
	}
	/* Ignore SIPGPIPE due to rude peers*/
	sa.sa_handler = SIG_IGN;
	sa.sa_flags = 0;
	sigemptyset(&(sa.sa_mask));
	sigaction(SIGPIPE, &sa, 0);

	pthread_mutex_init(&free_lock,NULL);
	event_set_fatal_callback(fatal_cb);
	/* Self IP Address Information */
	memset(&localaddr, 0, sizeof(struct sockaddr));
	socklen = sizeof(struct sockaddr_in);
	sin = (struct sockaddr_in*)&localaddr;
	sin->sin_port = htons(port);
	sin->sin_addr.s_addr = htonl(INADDR_ANY);
	sin->sin_family = AF_INET;
	base = event_base_new();
	if (!base) {
		perror("event_base_new()");
		return 1;
	}
	evthread_use_pthreads();
	  /* スレッド生成*/
	for(i=0;i<thread_num;i++){
		base_pool[i] = event_base_new();
		if(THREAD_CREATE(thread, thread_start,base_pool[i])  !=0)
			log_msg(LOG_ERR,"THREAD_CREATE Error");
	}
	listener = evconnlistener_new_bind(base, acceptcb, NULL,
	    LEV_OPT_CLOSE_ON_FREE|LEV_OPT_CLOSE_ON_EXEC|
			LEV_OPT_REUSEABLE,
	    -1, &localaddr, socklen);
	if (! listener) {
		log_msg(LOG_ERR,"Couldn't open listener.");
		event_base_free(base);
		return 1;
	}
	signal_event = evsignal_new(base, SIGINT, signal_cb, (void *)base);
	if (!signal_event || event_add(signal_event, NULL)<0) {
		log_msg(LOG_ERR,"Could not create/add a signal event!");
		return 1;
	}
	signal_event = evsignal_new(base, SIGTERM, signal_cb, (void *)base);
	if (!signal_event || event_add(signal_event, NULL)<0) {
		log_msg(LOG_ERR,"Could not create/add a signal event!");
		return 1;
	}
	event_base_dispatch(base);
	log_msg(LOG_ERR,"event_base_dispatch exits");
	evconnlistener_free(listener);
	event_base_free(base);
	return 0;
}

/************************************
 *  * Mutithreading Facilities         *
 *   * **********************************/
static int thread_create_linux(pthread_t *tid, void *(*entry)(void *), void *arg)
{
	pthread_attr_t  attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr,STACK_SIZE);
	pthread_attr_setdetachstate(&attr , PTHREAD_CREATE_DETACHED);
	return pthread_create(tid,&attr,entry,arg);
}




