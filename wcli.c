#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <event2/dns.h>
#include <event2/bufferevent.h>
#include <event2/bufferevent_compat.h>
#include <event2/buffer.h>
#include <event2/util.h>
#include <event2/event.h>
static	struct event_base *base=NULL;
typedef struct{
	struct event_base *base;
	struct evdns_base *dns_base;
	char host[260];
	char resource[260];
	int result;
	int code;
	int len;
	int blen;
	int rlen;
	char response[1024];
}UDATA;

void eventrd(struct bufferevent *bev, void *ptr)
{
	char buff[1024];
	int n;
	UDATA *udata;
	int room;
	int copylen;
	udata=(UDATA *)ptr;
	if(udata!=NULL){
		udata->result=2;
	}
	struct evbuffer *input=bufferevent_get_input(bev);
	if(input==NULL){
		return;
	}
	while((n=evbuffer_remove(input,buff, sizeof(buff)))>0){
		if(udata!=NULL){
			udata->rlen+=n;
			room=udata->blen - udata->len;
			if(room>0){
				if(n<room){
					copylen=n;
				}else{
					copylen=room;
				}
				memcpy(&udata->response[udata->len],buff,(size_t)copylen);
				udata->len+=copylen;
			}
		}
	}
}

void eventcb(struct bufferevent *bev,short events, void *ptr)
{
	char sbuff[1024];
	UDATA *udata;
	udata=(UDATA *)ptr;
	if(events & BEV_EVENT_CONNECTED){
		//sprintf(sbuff,"GET %s \r\n\r\n",udata->resource);
		sprintf(sbuff,"GET %s HTTP/1.1\r\nConnection:Close\r\nHost:%s\r\n\r\n",
				udata->resource,udata->host);
		evbuffer_add_printf(bufferevent_get_output(bev),"%s",sbuff);
		udata->result=1;
	}else if (events & (BEV_EVENT_ERROR|BEV_EVENT_EOF) || 
			events& BEV_EVENT_TIMEOUT){
		struct event_base *base=udata->base;
		struct evdns_base *dns_base=udata->dns_base;
		if(events & BEV_EVENT_TIMEOUT){
			udata->result+=10;
		} else if(events & BEV_EVENT_ERROR){
			udata->result=-1;
			int err=bufferevent_socket_get_dns_error(bev);
			if(err){
				udata->result=-2;
			}
		}else{
			if(udata->result==2){
				udata->result=3;
			}
		}
		bufferevent_free(bev);
		evdns_base_free(dns_base,0);
		event_base_loopexit(base,NULL);
	}
}

int query(char *host,ushort port,char *res)
{
	struct bufferevent *bev;
	struct evdns_base *dns_base;
	UDATA udata;
	char tmp[256];
	char tmp2[256];
	int n;

	memset(&udata,0,sizeof(UDATA));
	if(base==NULL)
		base = event_base_new();
	strncpy(udata.host,host,256);
	strncpy(udata.resource,res,256);
	udata.blen=1000;
	udata.base=base;
	if(base==NULL){
		return -1;
	}
	dns_base=evdns_base_new(base,1);
	if(dns_base==NULL){
		event_base_free(base);
		return -1;
	}
	udata.dns_base=dns_base;
	bev=bufferevent_socket_new(base,-1,
			 BEV_OPT_DEFER_CALLBACKS|BEV_OPT_CLOSE_ON_FREE);
	if(bev==NULL){
		evdns_base_free(dns_base,0);
		event_base_free(base);
		return -1;
	}
	bufferevent_setcb(bev,eventrd,NULL,eventcb,&udata);
	bufferevent_enable(bev,EV_READ|EV_WRITE);
	bufferevent_settimeout(bev,10,0);
	bufferevent_socket_connect_hostname(bev,dns_base,AF_UNSPEC,host,port);
	event_base_dispatch(base);
	if(udata.result>=2){
		memset(tmp,0,256);
		memcpy(tmp,udata.response,16);
		tmp[17]='\0';
		if(strncmp(tmp,"HTTP/1.1",8)==0){
				n=sscanf(udata.response,"%s %d",tmp2,&udata.code);
				if(n!=2){
					udata.code=-1;
				}
		}else{
			udata.code=-1;
		}
	}
	return udata.code;
}
#ifdef MAIN
static void usage(char *cmd)
{
	printf("Trivial HTTP 0.x client\n"
			"Syntax: %s [hostname] [port] [resource]\n"
			"Example: %s www.google.com 80 /\n",cmd,cmd);
	return ;
}
int main(int argc, char **argv)
{
	int port;
	int n;
	if(argc!=4){
		usage(argv[0]);
		return 1;
	}
	port=atoi(argv[2]);
	if(port==0){
		usage(argv[0]);
		return 1;
	}
	n=query(argv[1],(ushort)port,argv[3]);
	printf("return query result=%d\n",n);
	exit(0);
	return 0;
}
#endif
