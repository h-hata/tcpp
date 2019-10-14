/*
=========================================================
 Name        : main.c
 Author      : H.Hata
 Version     :
 Copyright   : OLT
 Description : Ansi-style
==========================================================
 */
#undef __UNISTD_GETOPT__
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "tcpp.h"
extern void log_setlvel(int);
extern void log_out(char *);


static void daemonize(void)
{
	int ret;
	//1回目
	fclose(stdin);
	fclose(stdout);
	fclose(stderr);
	ret=fork();
	if(ret>0){
		//親プロセス
		exit(EXIT_SUCCESS);
	}else if(ret<0){
		log_out("fork failed");
		exit(1);
	}
	//2回目
	ret=fork();
	if(ret>0){
		//親プロセス
		exit(EXIT_SUCCESS);
	}else if(ret<0){
		log_out("fork failed");
		exit(1);
	}
}


int main(int argc,char **argv)
{
	uint16_t port=1234;
	int daemon=0;
	unsigned int multi=2;
	int limit=10;

	log_out("Start");
	if(daemon==1){
		daemonize();
	}
	S_start(port,limit,multi,0,NULL,NULL);
	return EXIT_SUCCESS;
}


