/* Pulse Meter Interface program */

#include <stdio.h>	// for FILE
#include <stdlib.h>	// for timeval
#include <strings.h>	// for strlen etc
#include <time.h>	// for ctime
#include <sys/types.h>	// for fd_set
#include<sys/mman.h>

// #include <sys/socket.h>
// #include <netinet/in.h>
#include <netdb.h>	// for sockaddr_in 
#include <fcntl.h>	// for O_RDWR
//#include <termios.h>	// for termios
#include <unistd.h>		// for getopt
#include <pthread.h>
#ifdef linux
#include <errno.h>		// for Linux
#include <linux/nvram.h>
#include <errno.h>		// for Linux
#else
#define NVRAM_INIT 0
#endif

#define REVISION "$Revision: 1.7 $"
/*  1.0 01/10/2007 Initial version copied from Steca
	1.2 14/10/2008 Version like pulseserver uses NVRAM and sends kwh
	1.3 12/10/2009 Changed save to NVRAM from an hour to 10 mins.
	1.4 22/06/2011 Logon as Meter
	1.5 17/02/2012 Permit setting pulses/kwh for each channel. If chan 1 set, also sets chan 2 to same value. 
	1.6 2012/05/18 Permit fractional rate - Staples claimes to be 0.2 pulses per kwh.
    1.7 2012/05/20 "Read" command now issues a Message not just a meter data line.
*/

static char* id="@(#)$Id: pulse.c,v 1.7 2012/05/29 17:37:29 martin Exp $";

#define PORTNO 10010
#define LOGFILE "/tmp/pulse.log"
#define PROGNAME "Pulse"
#define progname "meter"
#define METERDAT "/tmp/meter.dat"

// Severity levels.  ERROR and FATAL terminate program
#define INFO	0
#define	WARN	1
#define	ERROR	2
#define	FATAL	3
// Socket retry params
#define NUMRETRIES 3
#define RETRYDELAY	1000000	/* microseconds */
// Steca values expected
#define NUMPARAMS 15
// Set to if(0) to disable debugging
#define DEBUG if(debug >= 1)
#define DEBUG2 if (debug > 1)
#define DEBUG3 if (debug > 2)

// If defined, send fixed data instead of timeout message
// #define DEBUGCOMMS

/* SOCKET CLIENT */

/* Command line params: 
1 - device name
2 - device timeout. Default to 60 seconds
3 - optional 'nolog' to suppress writing to local logfile
*/

#ifndef linux
extern
#endif
int errno;  

#define DIO0 0x01
#define DIO1 0x02
#define DIO2 0x04
#define DIO3 0x08
#define DIO4 0x10
#define DIO5 0x20

// Procedures in this file
void sockSend(const char * msg);	// send a string
int processSocket(void);			// process server message
void logmsg(int severity, char *msg);	// Log a message to server and file
void usage(void);					// standard usage message
char * getversion(void);
char * binary(int v);
time_t timeMod(time_t t);
void readCMOS(void);			// Call at startup to load counters
void writeCMOS(void);			// Call every hour to save counters
void * count_pulses(void *);	// Simply count pulses as they occur - thread worker function
void decode(char * msg);
void writemeter();				// write to /tmp/meter.dat

/* GLOBALS */
FILE * logfp = NULL;
int sockfd = 0;
int debug = 0;
int noserver = 0;		// prevents socket connection when set to 1
float rate1 = 10, rate2 = 10;		// Pulses per kWh
unsigned int count1, count2;
pthread_mutex_t mutex1 = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex2 = PTHREAD_MUTEX_INITIALIZER;
volatile unsigned int *pbdata, *pbddr;
int delay = 1000;		// usec to sleep (but it rounds up to 20mSec anyway)

//#define BUFSIZE 256	/* should be longer than max possible line of text from Pulse */
//char serialbuf[BUFSIZE];	// data accumulates in this global
// char * serbufptr = &serialbuf[0];
int controllernum = -1;	//	only used in logon message

/********/
/* MAIN */
/********/
int main(int argc, char *argv[])
// arg1: controller number
{
	int nolog = 0;

    struct sockaddr_in serv_addr;
    struct hostent *server;

    char buffer[256];
	int run = 1;		// set to 0 to stop main loop
	fd_set readfd; 
	int numfds;
	int interval = 60;
	int tmout = 90;	// seconds to wait in select()
	int saveInterval = 600;	// Save to NVRAM every 10 mins
	int logerror = 0;
	int option; 
	int delay = 1000;		// usec to sleep (but it rounds up to 20mSec anyway
	time_t update = 0, now;
	pthread_t tid;
	time_t nextSave;

	// Command line arguments
	
	opterr = 0;
	while ((option = getopt(argc, argv, "di:slVZ1:2:")) != -1) {
		switch (option) {
			case 's': noserver = 1; break;
			case 'l': nolog = 1; break;
			case '?': usage(); exit(1);
			case 'i': interval = atoi(optarg); break;
			case 'd': debug++; break;
			case '1': rate1 = rate2 = atof(optarg);	break;
			case '2': rate2 = atof(optarg);	break;
			case 'V': printf("Version %s %s\n", getversion(), id); exit(0);
			case 'Z': decode("(b+#Gjv~z`mcx-@ndd`rxbwcl9Vox=,/\x10\x17\x0e\x11\x14\x15\x11\x0b\x1a" 
							 "\x19\x1a\x13\x0cx@NEEZ\\F\\ER\\\x19YTLDWQ'a-1d()#!/#(-9' >q\"!;=?51-??r"); exit(0);
		}
	}
	
	DEBUG printf("Debug %d. optind %d argc %d\n", debug, optind, argc);
	
	if (optind < argc) controllernum = atoi(argv[optind]);	// get optional controller number: parameter 1
	
	sprintf(buffer, LOGFILE, controllernum);
	
	if (!nolog) if ((logfp = fopen(buffer, "a")) == NULL) logerror = errno;	
	
	// There is no point in logging the failure to open the logfile
	// to the logfile, and the socket is not yet open.

	sprintf(buffer, "STARTED %s as %d interval %d %s", argv[0], controllernum, interval, nolog ? "nolog" : "");
	logmsg(INFO, buffer);
	
	// Set up socket 
	if (!noserver) {
		sockfd = socket(AF_INET, SOCK_STREAM, 0);
		if (sockfd < 0) 
			logmsg(FATAL, "FATAL " PROGNAME " Creating socket");
		server = gethostbyname("localhost");
		if (server == NULL) {
			logmsg(FATAL, "FATAL " PROGNAME " Cannot resolve localhost");
		}
		bzero((char *) &serv_addr, sizeof(serv_addr));
		serv_addr.sin_family = AF_INET;
		bcopy((char *)server->h_addr, 
			 (char *)&serv_addr.sin_addr.s_addr,
			 server->h_length);
		serv_addr.sin_port = htons(PORTNO);
		if (connect(sockfd,(struct sockaddr *) &serv_addr,sizeof(serv_addr)) < 0) {
			sockfd = 0;
			logmsg(ERROR, "ERROR " PROGNAME " Connecting to socket");
		}	
		
		if (flock(fileno(logfp), LOCK_EX | LOCK_NB) == -1) {
			logmsg(FATAL, "FATAL " PROGNAME " is already running, cannot start another one");
		}
	
		// Logon to server as meter
		sprintf(buffer, "logon " progname " %s %d %d", getversion(), getpid(), controllernum);
		sockSend(buffer);
	}
	else	sockfd = 1;		// noserver: use stdout
	
	// If we failed to open the logfile and were NOT called with nolog, warn server
	// Obviously don't use logmsg!
	if (logfp == NULL && nolog == 0) {
		sprintf(buffer, "event WARN " PROGNAME " %d could not open logfile %s: %s", controllernum, LOGFILE, strerror(logerror));
		sockSend(buffer);
	}
		
	// Set up hardware
	
	int mem = open("/dev/mem", O_RDWR);
	unsigned char * start = mmap(0, getpagesize(), PROT_READ|PROT_WRITE, MAP_SHARED, mem, 0x80840000);
	pbdata = (unsigned int*)(start + 0x04);
	pbddr = (unsigned int *)(start + 0x14);
	// All inputs
	*pbddr = DIO4 | DIO5;  // DIO 4/5 outputs.
	*pbdata = 0xff;		// set to all 1's

	numfds = sockfd + 1;		// nfds parameter to select. One more than highest descriptor

	// Main Loop
	count1 = count2 = 0;
	readCMOS();
	DEBUG fprintf(stderr, "Got values from NVRAM: %d %d (%f %f)\n",  
				  count1, count2, (count1 / rate1), (count2 / rate2));

	// Start count thread
	if (pthread_create(&tid, NULL, count_pulses, NULL) < 0)
		perror("count_pulse");
	DEBUG fprintf(stderr, "Thread started as %d (0x%x)\n", tid, tid);

	FD_ZERO(&readfd); 

	update = timeMod(interval);
	nextSave = timeMod(saveInterval);
	while(run) {
		struct timeval tv1;
		now = time(NULL);
		if (now > nextSave) {
			DEBUG fprintf(stderr, "Saving to NVRAM ");
			writeCMOS();
			nextSave = timeMod(saveInterval);
			DEBUG {
				struct tm * t;
				t = localtime(&nextSave);
				strftime(buffer, sizeof(buffer), "%F %T", t);
				fprintf(stderr, "Next save at %s\n", buffer);
			}
		}

		if (now > update) {	// message time.  Will always send out 0 0 at startup.
			int fd;
			sprintf(buffer, "meter 2 %.1f %.1f", (count1 / rate1), (count2 / rate2));
			sockSend(buffer);
			update = timeMod(interval);
			// Write to /tmp/pulse
			fd = open("/tmp/pulse", O_RDWR | O_CREAT | O_TRUNC);
			if (fd < 0) perror("/tmp/pulse");
			if (fd > 0) {
				strcat(buffer, "\n");
				write(fd, buffer, strlen(buffer));
				close(fd);
			}
		}
		
		FD_SET(sockfd, &readfd);
		tv1.tv_sec = 0;
		tv1.tv_usec = tmout;
		if (select(sockfd + 1, &readfd, NULL, NULL, &tv1) && sockfd > 1)	// anything from server?
			run = processSocket();	// the server may request a shutdown by setting run to 0
		usleep(delay);
	}
	logmsg(INFO,"INFO " PROGNAME " Shutdown requested");
	close(sockfd);
	close(mem);
	munmap(start, getpagesize());

	return 0;
}

/*********/
/* USAGE */
/*********/
void usage(void) {
	printf("Usage: pulse [-i interval] [-l] [-s] [-d] [-V] [-1 pulse/kwh] [-2 pulse/kwh] controllernum\n");
	printf("-l: no log  -s: no server  -d: debug on\n -V version");
	return;
}

/**********/
/* LOGMSG */
/**********/
void logmsg(int severity, char *msg)
// Write error message to logfile and socket if possible and abort program for ERROR and FATAL
// Truncate total message including timestamp to 200 bytes.

// Globals used: sockfd logfp

// Due to the risk of looping when you call this routine due to a problem writing on the socket,
// set sockfd to 0 before calling it.

{
	char buffer[200];
	time_t now;
	if (strlen(msg) > 174) msg[174] = '\0';		// truncate incoming message string
	now = time(NULL);
	strcpy(buffer, ctime(&now));
	buffer[24] = ' ';	// replace newline with a space
	strcat(buffer, msg);
	strcat(buffer, "\n");
	if (logfp) {
		fputs(buffer, logfp);
		fflush(logfp);
	} 
	if (sockfd > 0) {
		strcpy(buffer, "event ");
		strcat(buffer, msg);
		sockSend(buffer);
	}
    if (severity > WARN) {		// If severity is ERROR or FATAL terminate program
		if (logfp) fclose(logfp);
		if (sockfd) close(sockfd);
		exit(severity);
	}
}

/************/
/* SOCKSEND */
/************/
void sockSend(const char * msg) {
// Send the string to the server.  May terminate the program if necessary
	short int msglen, written;
	int retries = NUMRETRIES;
	
	if(noserver) {	// shortcut when in test mode
		puts(msg);
		return;
	}

	msglen = strlen(msg);
	written = htons(msglen);

	if (write(sockfd, &written, 2) != 2) { // Can't even send length ??
		sockfd = 0;		// prevent logmsg trying to write to socket!
		logmsg(ERROR, "ERROR " PROGNAME " Can't write a length to socket");
	}
	while ((written = write(sockfd, msg, msglen)) < msglen) {
		// not all written at first go
			msg += written; msglen =- written;
			printf("Only wrote %d; %d left \n", written, msglen);
			if (--retries == 0) {
				logmsg(WARN, "WARN " PROGNAME " Timed out writing to server"); 
				return;
			}
			usleep(RETRYDELAY);
	}
}

/*****************/
/* PROCESSSOCKET */
/*****************/
int processSocket(void){
// Deal with commands from MCP.  Return to 0 to do a shutdown
	short int msglen, numread;
	char buffer[128], buffer2[192];	// about 128 is good but rather excessive since longest message is 'truncate'
	char * cp = &buffer[0];
	int retries = NUMRETRIES;
		
	if (read(sockfd, &msglen, 2) != 2) {
		logmsg(WARN, "WARN " PROGNAME " Failed to read length from socket");
		return 1;
	}
	msglen =  ntohs(msglen);
	while ((numread = read(sockfd, cp, msglen)) < msglen) {
		cp += numread;
		msglen -= numread;
		if (--retries == 0) {
			logmsg(WARN, "WARN " PROGNAME " Timed out reading from server");
			return 1;
		}
		usleep(RETRYDELAY);
	}
	cp[numread] = '\0';	// terminate the buffer 
	
	if (strcmp(buffer, "exit") == 0)
		return 0;	// Terminate program
	if (strcmp(buffer, "Ok") == 0)
		return 1;	// Just acknowledgement
	if (strcmp(buffer, "truncate") == 0) {
		if (logfp) {
		// ftruncate(logfp, 0L);
		// lseek(logfp, 0L, SEEK_SET);
			freopen(NULL, "w", logfp);
			logmsg(INFO, "INFO " PROGNAME " Truncated log file");
		} else
			logmsg(INFO, "INFO " PROGNAME " Log file not truncated as it is not open");
		return 1;
	}
	if (strcmp(buffer, "debug 0") == 0) {	// turn off debug
		debug = 0;
		return 1;
	}
	if (strcmp(buffer, "debug 1") == 0) {	// enable debugging
		debug = 1;
		return 1;
	}
	else if (strcasecmp(buffer, "read") == 0) {
		sprintf(buffer2, "INFO meter 2 %.1f %.1f", (count1 / rate1), (count2 / rate2));
		DEBUG fprintf(stderr, "Message: %s\n", buffer2);
		sockSend(buffer2);
		writemeter();
		return 1;
	}
	else if (strcasecmp(buffer, "help") == 0) {
		logmsg(INFO, "INFO " PROGNAME " Commands: exit, truncate, debug 0|1, save, read, set 1|2");
		return 1;
	}
	else if (strcasecmp(buffer, "save") ==0) {
		writeCMOS();
		return 1;
	}
	else if (strncmp(buffer, "set ", 4) == 0) {	// It's a set
		int n, num;
		float val;
		n = sscanf(buffer, "set %d %f", & num, & val);
		if (n != 2) {
			logmsg(WARN, "WARN " PROGNAME ": failed to get number and value");
			return 1;
		}
		if (num < 1 || num > 2) {
			logmsg(WARN, "WARN " PROGNAME ": number needs to to 1 or 2");
			return 1;
		}
		sprintf(buffer2, "INFO " PROGNAME " setting meter %d to %f", num, val);
		logmsg(INFO, buffer2);
		if (num == 1) {
			pthread_mutex_lock(&mutex1);
			count1 = (val * rate1);
			pthread_mutex_unlock(&mutex1);
		}
		else {
			pthread_mutex_lock(&mutex2);
			count2 = (val * rate2);
			pthread_mutex_unlock(&mutex2);
		}
		DEBUG fprintf(stderr, "Setting: %d %d\n", count1, count2);
		return 1;
	}

	strcpy(buffer2, "INFO " PROGNAME " Unknown message from server: ");
	strcat(buffer2, buffer);
	logmsg(INFO, buffer2);	// Risk of loop: sending unknown message straight back to server
	
	return 1;	
};

////////////////
/* GETVERSION */
////////////////
char *getversion(void) {
// return pointer to version part of REVISION macro
	static char version[10] = "";	// Room for xxxx.yyyy
	if (!strlen(version)) {
		strcpy(version, REVISION+11);
		version[strlen(version)-2] = '\0';
	}
return version;
}

char * binary(int v) {
	static char r[9];
	int i;
	for (i = 0; i < 8; i++)
		if ((v >> i) & 1 == 1) r[i] = '1'; else r[i] = '0';
	r[8] = '\0';
	return r;
}

/***********/
/* TIMEMOD */
/***********/

time_t timeMod(time_t interval) {
	// Return a time in the future at modulus t;
	// ie, if t = 3600 (1 hour) the time returned
	// will be the next time on the hour.
	//	time_t now = time(NULL);
	char buffer[20];
	if (interval == 0) interval = 600;
	time_t t = time(NULL);
	time_t newt =  (t / interval) * interval + interval;
	DEBUG {
		struct tm * tm;
		tm = localtime(&t);
		strftime(buffer, sizeof(buffer), "%F %T", tm);
		fprintf(stderr,"TimeMod now = %s delta = %d ", buffer, interval);
		tm = localtime(&newt);
		strftime(buffer, sizeof(buffer), "%F %T", tm);
		fprintf(stderr, "result %s\n", buffer);
	} 
	return newt;
}

void decode(char * msg) {
 // Algorithm - each byte X-ored' with a successively higher integer.
 char * cp;
 char i = 0;
 for (cp = msg; *cp; cp++) putchar(*cp ^ i++);
 putchar('\n');
 }


 /************/
/* READCMOS */
/************/
void readCMOS() {
	// Read the 8 bytes at offset 100 into count1 and count2
	int fd;
	if ((fd = open("/dev/misc/nvram", O_RDONLY)) == -1) {	// failed to open device
		logmsg(WARN, "WARN " PROGNAME " Failed to open /dev/misc/nvram. nvram.o probably not loaded\n");
		return;
	}
	if (lseek(fd, 100, SEEK_SET) == -1) {
		logmsg(WARN, PROGNAME ": readCMOS failed to seek");
		return;
	}
	pthread_mutex_lock( &mutex1 );
	if (read(fd, &count1, 4) != 4) {			// possibly not initialised
		logmsg(WARN, "WARN " PROGNAME " ReadCMOS: Initialising /dev/misc/nvram\n");
		if (ioctl(fd, NVRAM_INIT, 0) == -1) {
			perror("Error initialising NVRAM: ");
			pthread_mutex_unlock(&mutex1);
			return;
		}
		// try again
		if (read(fd, &count1, 4) != 4) {	// still can't read it
			perror("Error reading NVRAM after successful initialisation");
			pthread_mutex_unlock(&mutex1);
			return;
		}
	}
	pthread_mutex_unlock(&mutex1);
	pthread_mutex_lock(&mutex2);
	read(fd, &count2, 4);
	pthread_mutex_unlock(&mutex2);
	close(fd);
	DEBUG fprintf(stderr, "ReadCMOS got %d and %d\n", count1, count2);
}

/*************/
/* WRITECMOS */
/*************/
void writeCMOS(void) {
	// Write count1 and count2 to NVRAM at location 100
	int fd;
	if ((fd = open("/dev/misc/nvram", O_RDWR)) == -1) {	// failed to open device
		logmsg(WARN, "WARN " PROGNAME " Failed to open /dev/misc/nvram. nvram.o probably not loaded\n");
		return;
	}
	if (lseek(fd, 100, SEEK_SET) == -1) {
		logmsg(WARN, PROGNAME ": writeCMOS failed to seek");
		return;
	}
	
	if (write(fd, &count1, 4) != 4) {			// possibly not initialised
		logmsg(WARN, "WARN " PROGNAME " WriteCMOS: Initialising /dev/misc/nvram\n");
		if (ioctl(fd, NVRAM_INIT, 0) == -1) {
			perror("Error initialising NVRAM: ");
			return;
		}
		// try again
		if (write(fd, &count1, 4) != 4) {	// still can't write it
			perror("Error writing NVRAM after successful initialisation");
			return;
		}
	}
	write(fd, &count2, 4);
	close(fd);
	
	DEBUG fprintf(stderr, "WriteCMOS wrote %d and %d\n", count1, count2);
	return;
}

/****************/
/* COUNT_PULSES */
/****************/ 
void * count_pulses(void *arg) {	// Simply count pulses as they occur
	struct timeval tv1, tv2;
	int pulse, stuck;
	time_t now;
	while(1) {
		now = time(NULL);
		gettimeofday(&tv1, NULL);
		// If not using realtime, for the pulse to be stuck at 100 * 20mSec = 2.0 sec is too long.
		for (stuck = 100; (*pbdata & DIO0) == 0 && stuck; stuck--) {usleep(1);}
		if (stuck == 0) {
			fprintf(stderr, " ERROR - input 0 stuck at %d\n", stuck);
		}
		else if ((*pbdata & DIO2) == 0) {	// record pulse
			pthread_mutex_lock(&mutex1);
			count1++;
			pthread_mutex_unlock(&mutex1);
			DEBUG2 fprintf(stderr,"pulse1 ");
			*pbdata = ~(DIO4 | DIO2);
			while((*pbdata & DIO2) == 0) { usleep(1); pulse++; }
			*pbdata = ~0;
			DEBUG3 {
				int deltasec, deltausec;
				deltasec = tv1.tv_sec - tv2.tv_sec;
				deltausec= tv1.tv_usec - tv2.tv_usec;
				if (deltausec < 0) {deltasec -= 1; deltausec += 1000000;}
				fprintf(stderr, "PULSE 0 %06d %06d %d.%06d (%d.%06d) %s\n", 
					count1, count2, tv1.tv_sec, tv1.tv_usec, deltasec, deltausec, binary(*pbdata));
				tv2.tv_sec = tv1.tv_sec; tv2.tv_usec = tv1.tv_usec;
			}
		}
		for (stuck = 100; (*pbdata & DIO1) == 0 && stuck; stuck--) {usleep(1);}
		if (stuck == 0) {
			fprintf(stderr, " ERROR - input 1 stuck at %d\n", stuck);
		} 
		else if ((*pbdata & DIO3) == 0) {	// record pulse
			pthread_mutex_lock(&mutex2);
			count2++;
			pthread_mutex_unlock(&mutex2);
			DEBUG2 fprintf(stderr,"pulse2 ");
			*pbdata = ~(DIO5 | DIO3);
			while((*pbdata & DIO3) == 0) { usleep(1); pulse++; }
			*pbdata = ~0;
			DEBUG3 {
				int deltasec, deltausec;
				deltasec = tv1.tv_sec - tv2.tv_sec;
				deltausec= tv1.tv_usec - tv2.tv_usec;
				if (deltausec < 0) {deltasec -= 1; deltausec += 1000000;}
				fprintf(stderr, "PULSE 1 %06d %06d %d.%06d (%d.%06d) %s\n", 
				count1, count2, tv1.tv_sec, tv1.tv_usec, deltasec, deltausec, binary(*pbdata));
				tv2.tv_sec = tv1.tv_sec; tv2.tv_usec = tv1.tv_usec;
			}
		}
		usleep(delay);
	}
	return (void *)0;
}

/**************/
/* WRITEMETER */
/**************/
void writemeter() {
	// Write the /tmp/meter.dat file with current time and date, and the meter readings
	FILE * f;
	char c[60];
	struct tm * tmptr;
	if (!(f = fopen(METERDAT, "w"))) {
		fprintf(stderr, "Failed to open " METERDAT " for writing\n");
		return;
	}
	time_t now = time(NULL);
	tmptr = localtime(&now);
	strftime(c, 60, "date='%A %e %B %Y'\n", tmptr);		fputs(c, f);
	strftime(c, 60, "time='%T'\n", tmptr);				fputs(c, f);
	fprintf(f, "meter1=%.1f\nmeter2=%.1f\n", (count1 / rate1), (count2 / rate2));
	fclose(f);
};
