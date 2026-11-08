#include <unistd.h>
#include <stdio.h>
#include <sys/wait.h>
#include <string.h>
#include <stdlib.h>

#define EOL 	  	1
#define ARG 	  	2
#define AMPERSAND 	3
#define SEMICOLON 	4


#define MAXARG 		512
#define MAXBUF		512

#define FOREGROUND 	0
#define BACKGROUND	1

int userin(char *p);
int gettok(char **outptr);
int inarg(char c);
int procline(void);
int runcommand(char **cline, int where);
