typedef enum{
	S_CLIENT,
	S_SERVER
}SSTYPE;
typedef enum{
	TYP_ACCEPT,
	TYP_READ,
	TYP_TERM,
	TYP_TIMEOUT
}CTYPE;
typedef struct _session_t{
	unsigned int id;
	SSTYPE	side;
	struct bufferevent *bev;
	struct bufferevent *ubev;
	int status;
	size_t txoctet;
	size_t tx;
	size_t rxoctet;
	size_t rx;
	size_t timeout;
	char host[32];
	unsigned short port;
	char self_host[32];
	unsigned short self_port;
	char *usr;
	struct _session_t *partner;
}SESSION;
typedef void(*CALLBACK_T)(CTYPE type,SESSION *);
extern int S_start(
	uint16_t port,
	int limit,
	unsigned int multi,
	int tls,
	char *cert,
	char *priv) ;
extern void S_close(SESSION *);
extern int S_send(SESSION *,unsigned char *buff, size_t len);
extern void S_settimer(SESSION *s,int sec);
extern int S_connect(SESSION *s);
extern SESSION* S_search(char *ip,uint16_t port);



