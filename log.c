#include <stdio.h>
#include <time.h>
#include <string.h>
#include <syslog.h>

#define	TAG	"ylb"
#define	LOG_FILE	"lb"
#define	DEBUG_FILE	"debuglog"


#define DBG	if(debug==1){
#define	DEND	}
#define	BUFF_MAX	4096


static int loglevel=0;


void log_setlvel(int l)
{
	loglevel=l;
}

void log_out(char *msg)
{
	if(loglevel==0){
		puts(msg);
	}else{
		openlog(TAG, LOG_CONS | LOG_PID, LOG_USER);
		syslog(LOG_INFO, "%s",msg);
		closelog();
	}
}

void logging(int level,char *msg)
{
	log_out(msg);
}

