#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <pwd.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>

#include <arpa/inet.h> // socket
#include <sys/socket.h>
#include <sys/un.h> // UNIX socket

#include <pthread.h>
#include <sys/prctl.h>

#include "constants.h"



char** ARGV;
int* ARGC;

static unsigned char SIG_EXTENDED;
static unsigned int  SIG_FEISTY;
static volatile char SIG_LOCK;
static volatile char SIG_ACQUIRE;


/************ TYPEDEFS ********** *
 * ****************************** */
typedef struct SEQ_META	seqmeta_t;
typedef struct BUCKETS	buck_t;
typedef struct ARGS		args_t;
typedef struct BUFFER	buff_t;


pthread_t WDogThread;
pthread_t FlusherThread;

/*********** GENERAL PURPOSE FUNCTIONS ********* *
 * ********************************************* */

void 			printBin   (char n);

void 			memcopy    (char* dest, char* src, unsigned int length);
void* 			memSet     (void *b, int c, unsigned int len);

char* 			strTok     (char* s, char* delim);
seqmeta_t* 		seqTok     (seqmeta_t* container, char* first, char* last, char* delim, unsigned int delimlength);
unsigned char 	seqCmp     (char* s1, char* s2, unsigned int len);

int 			findChar   (char* src, char c);
char* 			joinStrings(char* str1, char* str2);
unsigned int	strLength  (char* str);
char*			strNullTerm(char* str, unsigned int strLen);

int 			strToInt   (char* str);
double 			strToDouble(char* str);

time_t 			timestamp  ();



struct passwd* s_passwd;
struct timeval time_values;

/************ STRUCTURES ********** *
 * ******************************** */
struct BUCKETS {
	char* lbl;
	char* dat;
	unsigned int   mtd;
	unsigned char  mod;
	char* loc;
	unsigned int port;
	
	unsigned int lblLen;
	unsigned int locLen;
	unsigned int datLen;
	
	unsigned char activated;
	pthread_t FLUSHER_ID;
	int fd;
	
	struct timeval flushAt;
	unsigned int flushAfter;
	unsigned char active;
	char  delete_at_flush;
	unsigned char immediate;
	
	struct BUCKETS* start;
	struct BUCKETS* last;
	
	struct BUCKETS* next;
	struct BUCKETS* previous;
	
	volatile unsigned char ACQUIRED;
	volatile unsigned char LOCKED;
};


struct SEQ_META{
	char* address;
	unsigned int length; // hopefully nobody will try to add >4GB data... or >64MB on x16 processor-drives machines..
};


struct FLAGS {
	char RUNNING;
	char DAEMON;
	char DAEMON_RUNNING;
} F;

struct HELP {
	int LOADED;
	const char* SHORT;
	const char* INFO;
	const char* USAGE;
	const char* PARAMS;
	const char* CONF;
} help;

struct BUS {
	
	unsigned int EMERG_BUFF_SZ;
	char* EMERG_BUFF;
	
	char*  logMSG;
	char* _logMSG;
	char* _stream;
	
	char* iofilePath;
	char* initFilePath;
	char* confFilePath;
	char* logFilePath;
	char* OWNER_name;
	
	char* queryDelim;
	unsigned int qDelimLength;
	
	unsigned int fd_io;
	unsigned int fd_log;
	
	unsigned int readBuffSz;

	unsigned int WDogTOut;
	unsigned int lsnTOut;
	unsigned int lsnReadDelay;
	unsigned int TTD;
	unsigned int ttd_dieAt;
	unsigned int OWNER_UID;
	unsigned int PID;
	
	
} bus;


struct ARGS {
	int elements_ct;
	char** elements;
};

struct BUFFER {
	unsigned int length;
	void** data;
};

typedef struct ReturnSet {
	unsigned int code;
	void* data;
} retset_t;




/* ###################### Bucket-managing functions ##########################
 * ###########################################################################
 * #    Bucket is a structure containing stored data and its metadata.       #
 * #-------------------------------------------------------------------------#
 * #  Buckets can be:                                                        #
 * #      * bucketsInit()       - initiated (a very first bucket in the list)#
 * #      * bucketCreateNew()   - created                                    #
 * #      * bucketAddNew()      - added next to other buckets                #
 * #      * bucketFindByLabel() - found knowing only a label linked to it    #
 * #      * bucketDelByLabel()  - deleted knowing only a label linked to it  #
 * #                                                                         #
 * ###########################################################################
 * */


buck_t* bucketFindByLabel(	buck_t* chain, 
							seqmeta_t* setLbl
							){
	buck_t* b = chain->start;


	while(b) { // eventually NEXT will be NULL
		if(	b->lbl )
			if( b->lblLen == setLbl->length ){
				//printf("b->lbl[%s] && setLbl->address[%s]\n", b->lbl, setLbl->address);
				if (seqCmp(b->lbl, setLbl->address, setLbl->length) )
					return b;
			}
		b = b->next;
	}

	return NULL;
}
						
buck_t* bucketCreateNew(seqmeta_t* setLbl,
						seqmeta_t* setDat,
						seqmeta_t* setLoc,
						int mtd
						){
	buck_t* b = malloc(sizeof(buck_t));
	
	b->lbl = malloc(sizeof(char)*setLbl->length);
	b->dat = malloc(sizeof(char)*setDat->length);
	b->loc = malloc(sizeof(char)*setLoc->length);
	
	memcpy(b->lbl, setLbl->address, setLbl->length);
	memcpy(b->dat, setDat->address, setDat->length);
	memcpy(b->loc, setLoc->address, setLoc->length);
	
	b->lblLen = setLbl->length;
	b->datLen = setDat->length;
	b->locLen = setLoc->length;
	
	//b->lbl = setLbl->address; b->lblLen = setLbl->length;
	//b->dat = setDat->address; b->datLen = setDat->length;
	//b->loc = setLoc->address; b->locLen = setLoc->length;
	b->mtd = mtd;
	b->port = 0;
	
	b->delete_at_flush = 1;
	b->flushAfter = 0;
	b->immediate = 0;
	b->fd = -1;
	
	b->active = 0;
	
	b->LOCKED   = 0;
	b->ACQUIRED = 0;
	
	return b;
}


int bucketAddNew(buck_t* chain, 
				 buck_t* new
				 ){
	
	if(! chain) return -2;

	if(! chain->start->lbl){ // if first element of the chain is not set - make 'new' the first one
		buck_t* b = chain->start;
		//*
		b->lbl = new->lbl; b->lblLen = new->lblLen;
		b->dat = new->dat; b->datLen = new->datLen;
		b->loc = new->loc; b->locLen = new->locLen;
		b->mtd = new->mtd; //*/
		b->mod = new->mod;
		b->flushAfter 		= new->flushAfter;
		b->delete_at_flush 	= new->delete_at_flush;
		b->immediate 		= new->immediate;
		b->port 			= new->port;
		//*b = *new;

		return 0;
	}
	
	seqmeta_t* setLbl = malloc(sizeof(seqmeta_t));
	setLbl->address = new->lbl;
	setLbl->length  = new->lblLen;
	
	if(bucketFindByLabel(chain, setLbl)) {
		free(new);
		return -1;
	}
	
	free(setLbl);
	
	chain->start->last->next = new;
	new->previous = chain->start->last;
	new->next = NULL;
	chain->start->last = new;
	
	new->start = chain->start;
	
	return 0;
}



buck_t* bucketsInit(){
	//buck_t* b = malloc(sizeof(buck_t));
	seqmeta_t* dummy = malloc(sizeof(seqmeta_t)); // memory leak in ADD<...>
	dummy->address = NULL;
	dummy->length = 0;
	
	buck_t* b = bucketCreateNew(dummy, dummy, dummy,0);
	b->start = b;
	b->last = b;
	b->next = NULL;
	b->previous = NULL;
	//b->delete_at_flush = 1;
	
	return b;
}


int bucketDelByLabel(buck_t* chain, 
					 seqmeta_t* setLbl
					){
	// TODO cut on memory leaks 
	buck_t* b = bucketFindByLabel(chain, setLbl);
	
	//printf("-----------\n");
	
	if(b){
		//printf("ADDR (if these are equal - it's the first member): b[%p], b->start[%p], chain[%p]\n", b, b->start, chain);
		if(b->start == b){ // only one element
			//printf("THE ONLY\n");
			//b=bucketsInit();
			*chain = *bucketsInit();//*b
		}
		
		else {
			if(b->next){ // not last element
				b->next->previous = b->previous;
			} else { // last element
				b->start->last = b->previous;
			}
			if(b->previous){ // not first element
				b->previous->next = b->next;
			} else { // first element
				b->next->last = b->last;
				b->next->start = b->next;
				b->next->previous = b->previous;
			}
			free(b);
		}
		
		return 0;
	} 
	return -1;
}



//struct USER usr;

void initBus(){
	
	
	bus.fd_log   = 2;
	bus.fd_io    = 0;
	bus.lsnTOut  = 10;//sec
	bus.WDogTOut = POW10_6; //µsec
	bus.lsnReadDelay = 0.5; // sec
	bus.readBuffSz = 1024; //Bytes
	
	
	bus.EMERG_BUFF_SZ = 2048;
	bus.EMERG_BUFF = malloc(sizeof(char) * bus.EMERG_BUFF_SZ);
	
	bus.logMSG  = malloc(sizeof(char) * LOG_MSG_LENGTH);
	bus._logMSG = malloc(sizeof(char) * MAX_LOG_MSG_LENGTH);
	bus._stream = malloc(sizeof(char) * bus.readBuffSz    );
	
	memSet( bus.logMSG,    0,     LOG_MSG_LENGTH);
	memSet(bus._logMSG,    0, MAX_LOG_MSG_LENGTH);
	memSet(bus.EMERG_BUFF, 0, bus.EMERG_BUFF_SZ );
	memSet(bus._stream,    0, bus.readBuffSz    );
	
	
	bus.iofilePath	 = NULL;
	bus.initFilePath = NULL;
	bus.confFilePath = NULL;
	bus.logFilePath	 = NULL;
	bus.OWNER_name	 = NULL;
	bus.queryDelim	 = NULL;
	
	
	struct passwd* pwd = getpwuid(getuid());
	
	
	bus.OWNER_UID  = pwd->pw_uid;
	bus.OWNER_name = pwd->pw_name;
	
	bus.qDelimLength = 1;
	bus.queryDelim = malloc(sizeof(char)*bus.qDelimLength);
	memcpy(bus.queryDelim, "\x01", 1);
}


time_t timestamp(){
	gettimeofday(&time_values, NULL);
	return time_values.tv_sec;
}



unsigned int strLength(char* str){
	size_t i=0;
	if(str != NULL)
		while(str[i] != '\0')
			i = i+1;
	
	return i;
}


void printBin(char n){
	printf("number: %d\n", n);
	while (n) {
		if (n & 1)	write(1,"1",1);
		else 		write(1,"0",1);
		n >>= 1;
	}
	write(1, "\n", 1);

}


void memcopy(char* dest, char* src, unsigned int length){
	int i=0;
	
	for(i=0; i<length; i=i+1)
		dest[i]=src[i];
}


void* memSet(void *b, int c, unsigned int len){
	unsigned char *p = b;
	while(len > 0){
		*p = c;
		p=p+1;
		len=len-1;
	}
	return b;
}

char* strTok(char* s, char* delim){
	static char* old_s = 0;
	
	if(s == 0) s = old_s;
	if(s == 0) return NULL;
	
	/*In case string starts with a delimiter OR old_s does - bypass that delimiter part*/
	char* ps = s;
	for(; *ps != 0; ps=ps+1){
		const char* pd = delim;
		for(; *pd != 0; pd=pd+1)
			if(*ps == *pd) break;
		if(*pd == 0) break;
	}
	
	s = ps;
	if(*s == 0) {
		old_s = s;
		return 0;
	}
	
	/*Look for another delimiter in the (remaining) string*/
	for(ps=ps+1; *ps!=0; ps=ps+1){
		const char* pd = delim;
		for(; *pd!=0; pd=pd+1)
			if(*ps == *pd) break;
		if(*pd != 0) break;
	}
	
	old_s = ps; // save remainings to the static address
	if(*ps != 0){ // ...and if it was not the end of the string
		old_s = old_s + 1;
		*ps = '\0'; // ... add a NULL at the end so that we'd have a nice stringlet.
	}
	//printf("str: '%s'\n", s);
	return s;
}


char* strTok_r(char* s, char* delim, char** old_s){
	//static char* old_s = 0;
	
	if(s == 0) s = *old_s;
	if(s == 0) return NULL;
	
	/*In case string starts with a delimiter OR old_s does - bypass that delimiter part*/
	char* ps = s;
	for(; *ps != 0; ps=ps+1){
		const char* pd = delim;
		for(; *pd != 0; pd=pd+1)
			if(*ps == *pd) break;
		if(*pd == 0) break;
	}
	
	s = ps;
	if(*s == 0) {
		*old_s = s;
		return 0;
	}
	
	/*Look for another delimiter in the (remaining) string*/
	for(ps=ps+1; *ps!=0; ps=ps+1){
		const char* pd = delim;
		for(; *pd!=0; pd=pd+1)
			if(*ps == *pd) break;
		if(*pd != 0) break;
	}
	
	*old_s = ps; // save remainings to the static address
	if(*ps != 0){ // ...and if it was not the end of the string
		*old_s = *old_s + 1;
		*ps = '\0'; // ... add a NULL at the end so that we'd have a nice stringlet.
	}
	//printf("str: '%s'\n", s);
	return s;
}


seqmeta_t* seqTok(seqmeta_t* container, char* first, char* last, char* delim, unsigned int delimlength){
	static char* old_s = 0;
	int dpos = 0; // delimiter position marker
	char* temp;
	char match = 0;
	
	//seqmeta_t* container = malloc(sizeof(seqmeta_t));
	
	if(first == NULL) first=old_s; // means we should proceed on previous string
	if(first == last) return NULL;
	
	//printf("SEQTOK] :: Delimiter[%d]='%s', STRING: '%s'\n", delimlength, delim, first);
	
	container->address = first;
	
	while(first < last){
		
		if(*first == delim[0]){ 
			temp = first;
			dpos=0;
			while(*temp == delim[dpos] && temp < last){ // if main sequence element maches respective element of the delimiting sequence
				//printf("[%c] ?= [%c]\n", *temp, delim[dpos]);
				if(dpos == delimlength-1) { // and if many enough delimiting elements have mached - break
					first = temp;
					match = 1;
					break; 
				}
				temp=temp+1;
				dpos=dpos+1;
			}
			if(match) {
				break;
			}
		}
		first = first+1;
	}
	
	container->length = (first - container->address)+1 - (first<last?delimlength:0);
	old_s = first+(first<last?1:0); // as we broke out of the loop before iterating the pointer

	/*
	printf("Token [%d] is: \n", container->length);
	write(1, container->address, container->length);
	write(1, "\n", 1);
	// */
	return container;
}


seqmeta_t* seqTok_r(seqmeta_t* container, char* first, char* last, char* delim, unsigned int delimlength, char** old_s){
	//static char* old_s = 0;
	int dpos = 0; // delimiter position marker
	char* temp;
	char match = 0;
	
	//seqmeta_t* container = malloc(sizeof(seqmeta_t));
	
	if(first == NULL) first=*old_s; // means we should proceed on previous string
	if(first == last) {
		container->address = NULL;
		return NULL;
	}
	
	container->address = first;
	
	while(first < last){
		
		if(*first == delim[0]){ 
			temp = first;
			dpos=0;
			while(*temp == delim[dpos] && temp < last){ // if main sequence element maches respective element of the delimiting sequence
				//printf("[%c] ?= [%c]\n", *temp, delim[dpos]);
				if(dpos == delimlength-1) { // and if many enough delimiting elements have mached - break
					first = temp;
					match = 1;
					break; 
				}
				temp=temp+1;
				dpos=dpos+1;
			}
			if(match) {
				break;
			}
		}
		first = first+1;
	}
	
	container->length = (first - container->address)+1 - (first<last?delimlength:0);
	*old_s = first+(first<last?1:0); // as we broke out of the loop before iterating the pointer

	/*
	printf("Token [%d] is: \n", container->length);
	write(1, container->address, container->length);
	write(1, "\n", 1);
	// */
	return container;
}



unsigned char seqCmp(char* s1, char* s2, unsigned int len){
	char retVal = 1; // 1 = match
	int i=0;
	while(i<len && retVal){
		//printf("%d ?= %d\n",s1[i] ,s2[i]);
		if(s1[i] != s2[i]) retVal = 0;
		else i=i+1;
	}
	return retVal;
}

int findChar(char* src, char c){
	int pos=-1;
	int cnt=0;
	while(src[cnt] != '\0'){
		if(src[cnt] == c){
			pos=cnt;
			break;
		}
		cnt=cnt+1;
	}
	
	return pos;
}

int seqReplace(char* where, unsigned int seqLen, char what, char withWhat){
	int cnt = 0;
	int l=0;
	
	while(l<seqLen){
		if(where[l] == what){
			where[l]=withWhat;
			cnt=cnt+1;
		}
		l=l+1;
	}
	
	return cnt;
}

char* joinStrings( char* str1, char* str2){
	char* result;
	size_t len1 = strLength(str1);
	size_t len2 = strLength(str2);
	
	result = (char*) malloc(len1+len2+1);
	
	memcopy(result, str1, len1);
	memcopy(result+len1, str2, len2+1); // including the \0
	
	return result;
}

int strToInt(char* str){
	int i=-1;
	int multipl=1;
	int temp;
	int neg = 0;
	int result = 0;
	
	int l = strLength(str)-1;
	
	if(l < 0) return 0;
	
	if(str[0] == '-'){
		neg = 1;
		i   = 0;
	}
	
	for(; i<l; l=l-1){
		temp = str[l] - 48;
		if(temp < 0 || temp > 9) return 0;
		
		result = result + multipl * temp;
		multipl = multipl * 10;
	}
	
	if(neg) result = 0 - result;
	
	return result;
}

double strToDouble(char* str){
	int i=-1;
	double multipl=1;
	double multDelta = 10;
	char neg = 0;
	double temp;
	double result = 0;

	int l = strLength(str)-1;
	int l2 = l;

	if(l < 0) return 0;

	if(str[0] == '-'){
		neg	= 1;
		i	= 0;
	}

	for(; i<l; l=l-1){
		if(str[l] == ',' || str[l] == '.') {
			multipl = 1;
			int d=0;
			for(; d<l2-l; d=d+1)
				result = result/multDelta;
			continue;
		}
		temp = (double)str[l] - 48;
		if(temp < 0 || temp > 9) return 0;
		//printf("RESULT: %lf + %lf * %lf = ", result, multipl, temp);
		result = result + multipl * temp;
		//printf("%lf\n", result);
		multipl = multipl * multDelta;
	}

	if(neg) result = 0 - result;

	return result;
}

char* strNullTerm(char* str, unsigned int strLen){

	char* result = malloc(sizeof(char) * (strLen+1) );
	memcpy(result, str, strLen);
	result[strLen] = '\0';
	return result;
}

char* strTrim(char* str){
	int length = strLength(str);
	int offs_s = 0;
	int offs_e = length-1;
	if(length > 0){
		while(offs_s < length && (str[offs_s] == ' ' || str[offs_s] == '\t')) offs_s = offs_s+1;
		while(offs_e > 0      && (str[offs_e] == ' ' || str[offs_e] == '\t' 
			                   || str[offs_e] =='\r' || str[offs_e] == '\n')) offs_e = offs_e-1;
	}
	str[offs_e+1] = '\0';
	return str+offs_s;
}



void initHelpPage(){
	help.SHORT = "\n\
 Please specify which topic you're interested in. Available topics:\n\
	 short  - this message\n\
	 info   - general information abaout the BUS\n\
	 usage  - basic usage-related information\n\
	 params - CLI parameters with brief explanation\n\
	 conf   - information about configuration file, available entries\n\
	 --------\n\
	 all    - show all the information\n\n";
	 help.INFO = "\
	 \nINFO:\n\
		This ir an IPC daemon that can store some chunks of information in memory and pass them to other processes run by the same user via socket, file or pipe.\n\
	 BUS can be run in either of the following modes:\n\
		* Daemon   -- will run in background and completely detatch from cTTY; \n\
		* Terminal -- will run on active terminal and will be possible to terminate by keyboard. \n\
	 By default BUS runs in Terminal mode. All the output will be redirected to STDERR. \n\n\
	 To start the BUS one must specify at least management file path. This must be a pipe-file. Other processes will be able to store, retrieve and remove data from the BUS by communicating to it via this file. \
	 \n\n";
	 
	 help.USAGE = "\
	 \nUSAGE:\n\
	To store, retrieve or delete data from BUS a string of the following pattern must be sent to mgmt file:\n\
	COMMAND$LABEL$DATA$TRANSFER METHOD$TRANSFER MODE::LOCATION[|:port] \n\n\
	The '$' sign represents a separator between distinct parts of a query (default is 01)\n\
 * COMMAND - command for the BUS, i.e. what you want to do. Can be one of the following: \n\
	add - adds data to BUS \n\
	get - passes data from BUS to a location, specified by 'add' \n\
	del - removes data from the BUS\n\
	die - terminates the BUS\n\
 * LABEL - a label to be assigned to the DATA. All labels must be unique. \n\
 * DATA - the actual data to be stored in a BUS\n\
	DATA can be dynamic. There are a few DATA field tags that allow DYNAMIC data:\n\
		* [stdin]  --  will allow user to enter data via stdin, unless BUS is running in DAEMON mode (w/o terminal);\n\
		* [stdin+secret] -- will ask user to enter data just like with '[stdin]' tag, except data will not appear on screen (just like when entering passwords);\n\
		* [file]/somedir/somefile -- will load data from file. Make sure to set a buffer big enough to fit all the data required. Read delay (-B) should be also considered for bigger files or slower systems.\n\
 * TRANSFER METHOD - how the data will be retrieved from BUS: FILE, SOCK, MMAP. \n\
 * TRANSFER MODE - Optional. By default data will be transfered to a normal file and left there. MODE can be a combination of one or more values listed below:\n\
	FileMode:\n\
		C - create file if it does not exist yet \n\
		A - append to the file when sending data; \n\
		T - truncate the file when sending data; \n\
		X - exclusive file mode, i.e. file MUST not be there when transfering data.\n\
	FileType:\n\
		N - normal file (default); \n\
		P - pipe file (should be set with timeout).\n\
	Timeout (i.e. how soon data will be removed from the location):\n\
		0 - no timeout (default);\n\
		positive number - number of seconds after which file will be removed;\n\
		negative number - number of seconds after which file will be emptied.\n\
	\n\
	SocketMode:\n\
		C - Client socket\n\
		S - Server socket\n\
		(disabled) A - Pick between client/server automatically\n\
		\n\
		N - Network socket\n\
		P - pipe-file socket\n\
		H - Hidden socket (aplies for pipe-file sockets)\n\
		_______\n\
		I - Immediate mode. Useful for pipes, sockets and other blocking mechanizms. If data has been flushed before timeout, LOCATION will be housekept (deleted/drained&closed) immediatelly.\n\
 Mode must be separated from LOCATION by a double colon ('::'). If mode is not provided then colon separation is not required.\n\
 * LOCATION - location to where data will be transfered. Currently only file paths are available (though in future modifications support for sockets and shm might be added).\n\
	The following DATA transfer types are supported:\n\
		* normal file;\n\
		* pipe-file;\n\
		* UNIX socket (client/server);\n\
		* abstract UNIX socket (client/server);\n\
		* network socket - TCP (client/server) (location = [adress]:[port]).\n\n\
 EXAMPLE:\n\
	add$MyLabel1$this is some random data$FILE$CNA-15::/tmp/outputFile	\n\n\
	this query will add new data entry with label 'Label1' and set it to be pushed to the end of a file on /tmp (create the file if it's not there). \n\
	After 15 seconds since 'get' query is sent the file will be wiped.\n\
	Only the owner of process 1234 will be able to manipulate this data.\
	 \n\n";
	
	help.PARAMS = "\
	 \nCLI PARAMETERS:\n\
	The following parameters can be passed to the BUS:\n\
	-i, --iofile     [path] 		- the management file through which the BUS can be communicated. MANDATORY!\n\
	-c, --config     [path] 		- path to configuration file\n\
	-I, --initFile	 [path] 		- path to file containing initial values that should be loaded to BUS on startup\n\
	-l, --log        [path] 		- path to LOG file. Defaults: daemon -- /dev/null; shell -- STDERR\n\
	-d, --daemon    		- to daemonize the BUS\n\
	-D, --delimiter  [sequence] 	- delimiter for query strings\n\
	-t, --ttd        [seconds] 	- time until script death (divisible by -T)\n\
	-T, --timeout    [seconds] 	- how long BUS will wait for interaction before re-iterating listening.\n\
	-b, --readBuffer [Bytes] 	- Listener's read buffer size(B). It will be able to read queries this long. \n\
						Default: 1024.\n\
	-B, --buffDelay  [seconds] 	- Listener will wait for this many seconds bor buffer to fill-up before reading it.\n\
						Default: 0.5\n\
	-w, --wclock	 [seconds] 	- float. Sets watchdog clock speed. Higher values will increase CPU load, \n\
			  but will also cause better watchdog precision. Default is 1 (1 iteration per second).\n\
	 \n";
	help.CONF = "\
	\nCONFIGURATION FILE:\n\
	BUS can be configured to load certain values from a custom configuration file. Config file lines can be commented-out by adding hashtag '#' as the first symbol on the line.\n\
	Valid configuration entries look as follows:\n\
		VARIABLE = VALUE\n\n\
	Available variables:\n\
		Delimiter      		- separator for distinct fields of a query. Default is 01\n\
		InitFile        	- path to file containing initial queries. These queries will be loaded during start-up of the BUS\n\
		ListenerFile   		- path to management file\n\
		LogFile        		- path to log file. Defaults: daemon - /dev/null; shell - stderr\n\
		TTD            		- TimeTillDeath. After this many seconds BUS will die. Value should be divisible by ListenTimeout\n\
		ListenTimeout  		- after this many seconds listening on mgmt file will be re-iterated. You don't normally need this value to be big. Default is 10\n\
		ReadBufferSize 		- Size of buffer filled by mgmt file. Longest estimated query should be shorted than the buffer size.\n\
		ReadBufferDelay		- Delay in seconds to allow read query buffer to fill-up before flushing it. Bigger data chunks might need longer delays. Def.: 0.5\n\
		WatchDogClockRate 	- determines how often watchdog will check BUS integrity. Higher values will cause higher CPU usage. \n\
					  Default is 1.0 (float, delimited by point [not comma])\n\
		ExtendedSigHandler	- enables extended signal handler [in some rare cases might cause glitches or stack overfill].\n\
					  By default it's enabled. Possible config values: [yes|no] (lowercase)\n\
		Feisty         		- If ExtendedSigHandler is enabled, BUS can act feisty. If any process tries to send a signal to BUS, that process will be sent a signal, \n\
					  specified as 'Feisty' value (integer)\n\
		\n\n";
			
	help.LOADED = 1;
}

void printHelpPage(const char* page){
	if(&help.LOADED == NULL)
		return;
	
	if     (strcmp(page, "short") == 0)
		printf("%s", help.SHORT);
	else if(strcmp(page, "info") == 0)
		printf("%s", help.INFO);
	else if(strcmp(page, "usage") == 0)
		printf("%s", help.USAGE);
	else if(strcmp(page, "params") == 0)
		printf("%s", help.PARAMS);
	else if(strcmp(page, "conf") == 0)
		printf("%s", help.CONF);
	else if(strcmp(page, "all") == 0)
		printf("%s\n%s\n%s\n%s", help.CONF, help.USAGE, help.PARAMS, help.CONF);
	else
		printf("Unknown help page '%s'.\n%s", page, help.SHORT);
	exit(0);
}

//const 
/*
const char* RET_CODE[] = {  
		
	"OK",
	
	"EMERGENCY",
	"LifespanExpired",
	
	"UNKNOWN_ERROR",
	
	"WrongSyntax",
	"UnknownCommand",
	"WrongLabel",
	"CannotTransfer",
	"UnknownTransferMethod",
	"FileNotFound",
	"PIDNotAccessible",
	"PIDandBUSownershipMismatch",
	"Timeout",
	};
//*/

void LOG(const char* msg){
	//char* message = malloc(sizeof(char)*MAX_LOG_MSG_LENGTH);
	timestamp();
	memSet(bus._logMSG, 0, MAX_LOG_MSG_LENGTH);
	sprintf(bus._logMSG, "%ld.%06ld nBUS[%d] %s",(long int)time_values.tv_sec, (long int)time_values.tv_usec, getpid(), msg);
	write(bus.fd_log, bus._logMSG, MAX_LOG_MSG_LENGTH);
	//write(bus.fd_log, bus._logMSG, strLength(message));
	//free(message);
}





int setListenerFile(char* path){
	int len = strLength(path);
	int fd = -1;
	
	if(access(path, F_OK) == -1){
		if(mkfifo(path, 0600) == 0) 
			LOG("Listener file has been created successfully\n");
		else 	LOG("Failed to create a Listener file\n");
	}
	
	if(access(path, R_OK) != -1){
		struct stat* buf = malloc(sizeof(struct stat));
		stat(path, buf);
		if(S_ISFIFO(buf->st_mode)){
			fd = open(path, O_RDWR);
			if(fd > 0){
				//bus.fd_io = fd;
				LOG("Listener file has been opened successfully\n");
			} else LOG("Failed to open Listener file\n");
		} else LOG("Listener file is not a FIFO\n");
		
		free(buf);
	} else LOG("Listener file has wrong permissions\n");
	
	
	if(fd > -1){
		
		if(bus.iofilePath) {
			free(bus.iofilePath);
			bus.iofilePath=NULL;
		}
		bus.iofilePath = malloc(sizeof(char) * len);
		memcpy(bus.iofilePath, path, len);
		
		if(bus.fd_io > -1) close(bus.fd_io);
		bus.fd_io = fd;
		
	}
	
	return fd;
}

int setLogFile(char* path){
	int len = strLength(path);
	int fd = -1;
	int mode = 0|O_APPEND;
	int set_mode = 0;
	int good = 1;
	
	
	
	if(access(path, F_OK) != 0) {
		mode=mode|O_WRONLY|O_CREAT;
		set_mode = 1;
	} else
	if(access(path, R_OK) == 0){
		mode=mode|O_WRONLY;
		
		struct stat* buf = malloc(sizeof(struct stat));
		stat(path, buf);
		
		if(S_ISFIFO(buf->st_mode)){
			LOG("LOG file is a pipe\n");
			
			if(access(path, W_OK) == 0){
				mode = mode|O_RDWR;
			} else {
				good = 0;
				LOG("LOG pipe-file must have WRITE flag set but it's not there. LOG stream will not be changed\n");
			}
			
		}
		
		free(buf);
		
	} else {
		good=0;
		LOG("LOG file must be writable but it's not. LOG stream will not be changed\n");
	}
	
	if(good){
		fd = open(path, mode);
		if(fd > -1){
			
			if(set_mode) fchmod(fd, 0644);
			
			if(bus.logFilePath) {
				free(bus.logFilePath);
				bus.logFilePath=NULL;
			}
			bus.logFilePath = malloc(sizeof(char) * len);
			memcpy(bus.logFilePath, path, len);
			
			if(bus.fd_log > -1) close(bus.fd_log);
			bus.fd_log = fd;
			
		} else
			LOG("Could not open LOG file. LOG stream will not be changed\n");
	}
	
	return fd;
}

int setQueryDelimiter(char* str){
	if(bus.queryDelim) {
		free(bus.queryDelim);
		bus.queryDelim = NULL;
	}
	
	bus.qDelimLength = strLength(str);
	bus.queryDelim = malloc(sizeof(char)*bus.qDelimLength);
	memcpy(bus.queryDelim, str, bus.qDelimLength);
	
	return bus.qDelimLength;
}

int setListenerReadBufferSize(unsigned int size){
	bus.readBuffSz = size;
	
	return size;
}

int setListenerReadBufferDelay(unsigned int delay){
	bus.lsnReadDelay = delay;
	
	return delay;
}

int setListenerReadWait(unsigned int timeout){
	bus.lsnTOut = timeout;
	
	return timeout;
}

int setWatchDogTickerRate(double rate){
	bus.WDogTOut = (unsigned int) (1 * POW10_6 / rate);
	
	return bus.WDogTOut;
	
}





void DAEMONIZE(){
	
	if(F.DAEMON_RUNNING) return; // daemon cannot be 2nd level daemon... It's not a Saiyan.
	
	F.DAEMON_RUNNING=1;
	
	LOG("--------DAEMON MODE-------\n");
	
	pid_t pid = fork();
	if(pid > 0) exit(0);
	close(0);
	close(1);
	close(2);
	
	setsid();
	pid = fork();
	if(pid > 0) exit(0);
	
	F.DAEMON  = 1;
	F.RUNNING = 1;
	
	
	if(!bus.logFilePath){
		setLogFile("/dev/null");
	}
	
}


void LEAVE(int code){
	F.RUNNING = 0;
	
	//char* msg = malloc(LOG_MSG_LENGTH);
	sprintf(bus.logMSG, "Leaving the BUS. Reason for leaving - '%d'. Shredding all the data before quit.\n", code);
	LOG(bus.logMSG);
	
	free(bus.logMSG);
	free(bus._logMSG);
	free(bus.EMERG_BUFF);
	free(bus.queryDelim);
	free(bus._stream);
	
	exit(code);
}

void DO_NOTHING(int signum){
	
	//return;
}

void SIGHANDLER(int sig, siginfo_t* info, void* nil){
	
	
	
	if(SIG_EXTENDED){
		
		/** SMART signal handler.
		 * 
		 * However smart it is - it's not perfect. It's likely to overflow the stack
		 * hence the critical section...
		 */
		
	/* ######### CRITICAL SECTION ######### */
		int cnt=3;                //####### */
		if(SIG_LOCK)              //####### */
			while(SIG_ACQUIRE){   //####### */
				if(! cnt) return; //####### */
				sleep(1);         //####### */
				cnt = cnt-1;      //####### */
			}                     //####### */
	/* #################################### */
		
		SIG_ACQUIRE = 1;
		SIG_LOCK = 1;      
		
		
		
		int fd = -1;
		int acc = -1;
		memset(bus.EMERG_BUFF, 0, bus.EMERG_BUFF_SZ);
		
		sprintf(bus.EMERG_BUFF, "/proc/%d/cmdline", info->si_pid);
		acc = access(bus.EMERG_BUFF, F_OK);
		
		
		if(acc == 0){
			acc = access(bus.EMERG_BUFF, R_OK);
			if(acc == 0){
				fd = open(bus.EMERG_BUFF, O_RDONLY);
			}
		} else {
			
			memset(bus.EMERG_BUFF, 0, bus.EMERG_BUFF_SZ);
			sprintf(bus.EMERG_BUFF, "/proc/%d/comm", info->si_pid);
			acc = access(bus.EMERG_BUFF, F_OK);
			
			if(acc == 0){
				acc = access(bus.EMERG_BUFF, R_OK);
				if(acc == 0)
					fd = open(bus.EMERG_BUFF, O_RDONLY);
			}
		}
		
		memset(bus.EMERG_BUFF, 0, bus.EMERG_BUFF_SZ);
		if(fd > -1) {
			read(fd, bus.EMERG_BUFF, bus.EMERG_BUFF_SZ-1);
			close(fd);
		}
		
		
		memset(bus.logMSG, 0, LOG_MSG_LENGTH);
		sprintf(bus.logMSG, "SIGNAL: si_signo[%d], si_pid[%d], si_procn[%s], si_uid[%d], si_fd[%d], si_addr[%p]\n",
				info->si_signo, info->si_pid, bus.EMERG_BUFF, info->si_uid, info->si_fd, info->si_addr);
		LOG(bus.logMSG);
		
		if(SIG_FEISTY && info->si_pid){
			kill(info->si_pid, SIG_FEISTY);
		}
		
		memset(bus.logMSG, 0, LOG_MSG_LENGTH);
		memset(bus.EMERG_BUFF, 0, bus.EMERG_BUFF_SZ);
		
		
		SIG_ACQUIRE = 0;
		SIG_LOCK = 0;
	} // if(SIG_EXTENDED) //
	
	switch(sig){
		case SIGHUP:	// 1
			DAEMONIZE();
		break;
		case SIGINT:	// 2
			
		break;
		case SIGQUIT:	// 3
			LEAVE(sig);
		break;
		case SIGILL:	// 4
			LEAVE(sig);
		break;
		case SIGTRAP:	// 5
			
		break;
		case SIGABRT:	// 6
			
		break;
		case SIGBUS:	// 7
			LEAVE(sig);
		break;
		case SIGFPE:	// 8
			
		break;
		case SIGKILL:	// 9
			
		break;
		case SIGUSR1:	// 10
			
		break;
		case SIGSEGV:	// 11
			LEAVE(sig);
		break;
		case SIGUSR2:	// 12
			
		break;
		case SIGPIPE:	// 13
			
		break;
		case SIGALRM:	// 14
			
		break;
		case SIGTERM:	// 15
			LEAVE(sig);
		break;
		
	}
}

void TrapSignals(){
	
	
	SIG_LOCK = 0;
	SIG_ACQUIRE = 0;
	
	struct sigaction action;
		action.sa_flags = action.sa_flags | SA_SIGINFO;
		action.sa_sigaction = SIGHANDLER;
		sigaction(SIGHUP,  &action, NULL); // 1
		sigaction(SIGINT,  &action, NULL); // 2
		sigaction(SIGQUIT, &action, NULL); // 3
		sigaction(SIGILL,  &action, NULL); // 4
		sigaction(SIGTRAP, &action, NULL); // 5
		sigaction(SIGABRT, &action, NULL); // 6
		sigaction(SIGBUS,  &action, NULL); // 7
		sigaction(SIGFPE,  &action, NULL); // 8
		sigaction(SIGKILL, &action, NULL); // 9
		sigaction(SIGUSR1, &action, NULL); // 10
		sigaction(SIGSEGV, &action, NULL); // 11
		sigaction(SIGUSR2, &action, NULL); // 12
		sigaction(SIGPIPE, &action, NULL); // 13
		sigaction(SIGALRM, &action, NULL); // 14
		sigaction(SIGTERM, &action, NULL); // 15
	
	/*
	struct sigaction action;
		action.sa_handler = LEAVE;
		sigaction(SIGQUIT, &action, NULL); 			// 3  - quit (die and dump)
		sigaction(SIGILL,  &action, NULL); 			// 4  - illegal instruction
		sigaction(SIGKILL, &action, NULL); 			// 9  - kill (furious ninja attack)
		sigaction(SIGSEGV, &action, NULL); 			// 11 - SegFault
		sigaction(SIGTERM, &action, NULL); 			// 15 - terminate
		
	struct sigaction action_IGN;
		action_IGN.sa_handler = DO_NOTHING;
		sigaction(SIGPIPE, &action_IGN, NULL); 		// 2  - interrupt. DON'T!
		sigaction(SIGPIPE, &action_IGN, NULL); 		// 13 - broken pipe
		
	struct sigaction action_DAEMON;
		action_DAEMON.sa_handler = DAEMONIZE;
		sigaction(SIGHUP, &action_DAEMON, NULL); 	// 1  - HangUp (parents are divorcing - now you're on your own)
	//*/
	
}



int interpretConfig(char* confFile){
	//TODO make config interpreter

	int fd = -1;
	int size = 0;
	
	char* buffer = NULL;
	//char* msg    = NULL;
	int bytes_read = 0;
	
	//msg = malloc(sizeof(char)*LOG_MSG_LENGTH);
	memSet(bus.logMSG, 0, size);
	
	sprintf(bus.logMSG, "Loading configuration file: '%s'\n", confFile);
	LOG(bus.logMSG);
	memSet(bus.logMSG, 0, size);
	
	if(access(confFile, F_OK) != 0) {
		LOG("Cannot find config file\n");
		return -1;
	}
	if(access(confFile, R_OK) != 0) {
		LOG("Cannot open config file to READ\n");
		return -1;
	}
	
	
	fd=open(confFile, O_RDONLY);
	if(fd < 0){
		LOG("Failed to open config file\n");
		return -1;
	}
	
	
	size = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	if(size < 0){
		LOG("Unable to seek() to the end of config file\n");
		close(fd);
		//free(msg);
		return -1;
	}
	
	
	int namelen = strLength(confFile);
	if(bus.confFilePath) {
		free(bus.confFilePath);
		bus.confFilePath=NULL;
	}
	bus.confFilePath = malloc(sizeof(char) * namelen);
	memcpy(bus.confFilePath, confFile, namelen);


	buffer = malloc(sizeof(char)*size);
	memSet(buffer, 0, size);
	
		
	seqmeta_t* LINE = malloc(sizeof(seqmeta_t));
	
	seqmeta_t* LINE_orig = LINE;
	
	char* substr;
	char* value;
	char** tempToken = malloc(sizeof(char*)); // we'll need 2 tokenizers: 1 for lines, another - for variables' declarations and values
	char** tempTokenLine = malloc(sizeof(char*)); // we'll have to use strtok in streamParser if config intends to load InitFile later....
	
	while((bytes_read = read(fd, buffer, size)) > 0){
		
		//int x=0; while(x<bytes_read){ printf("LINE_CHAR[%d]{%p} = '%c'(%d)\n", x, buffer+x, buffer[x], buffer[x]); x=x+1;} // DEBUG
		
		/*     Since read() returns a sequence rather than a string (\0 terminated sequence) 
		 * it'd be easier if we turned our buffer into sequence of strings.
		 * \0 - terminated sequences are easier to parse by strTok().
		 * 
		 * */
		seqReplace(buffer, bytes_read, '\n', '\0'); 
		
		seqTok_r(LINE, buffer, buffer+bytes_read-1, "\0", 1, tempTokenLine); // now read our sequence string-by-string (i.e. split by '\0')
		
		
		while(LINE->address){
			//printf("LINE=[%s]{%p}\n", LINE->address, LINE->address);
			//int x=0; while(x<LINE->length){ printf("LINE_CHAR[%d]{%p} = '%c'(%d)\n", x, LINE->address+x, LINE->address[x], LINE->address[x]); x=x+1;} // DEBUG
			LINE->address = strTrim(LINE->address);
			
			if(LINE->address[0] == '\0' || LINE->address[0] == '#' || (LINE->address[0] == '/' && LINE->address[1] == '/')){ // commented-out line
				// lose the rest of the LINE
				seqTok_r(LINE, NULL, buffer+bytes_read-1, "\0", 1, tempTokenLine);
				continue;
			}

			substr = strTok_r(LINE->address, "=", tempToken);

			seqTok_r(LINE, NULL, buffer+bytes_read-1, "\0", 1, tempTokenLine);
			if(substr) substr=strTrim(substr);
			//else continue;
			
			
			
			if((value = strTok_r(NULL, "\0", tempToken)) ){
				value = strTrim(value);
			}
			
			//if(substr == NULL) continue;
			
			//printf("________\nsubstr[%p]{%s}, tempToken[%p]{%s}, value[%p]{%s}\n", substr, substr, *tempToken, *tempToken, value, value);
			
			//* <BODY> *//
			memset(bus.logMSG, 0, LOG_MSG_LENGTH);
			if(strcmp(substr, "ListenerFile") == 0 && value){
				sprintf(bus.logMSG, "CONFIG :: Setting Listener file path: '%s'\n", value); LOG(bus.logMSG);
				
				setListenerFile(value);
				
			} else 
			if(strcmp(substr, "LogFile") == 0 && value){
				sprintf(bus.logMSG, "CONFIG :: Setting LOG file path: '%s'\n", value); LOG(bus.logMSG);

				setLogFile(value);

			} else 
			if(strcmp(substr, "InitFile") == 0 && value){
				sprintf(bus.logMSG, "CONFIG :: Setting init file path: '%s'\n", value); LOG(bus.logMSG);

				interpretINITfile(value);

			} else 
			if(strcmp(substr, "ListenTimeout") == 0 && value){
				sprintf(bus.logMSG, "CONFIG :: Setting ListeningTickerTimeout: '%s'\n", value); LOG(bus.logMSG);
				
				setListenerReadWait(strToInt(value));
				
			} else 
			if(strcmp(substr, "TTD") == 0 && value){
				sprintf(bus.logMSG, "CONFIG :: Setting Timeout_unTil_Death: '%s'\n", value); LOG(bus.logMSG);
				bus.ttd_dieAt = strToInt(value);
			} else 
			if(strcmp(substr, "WatchDogClockRate") == 0 && value){
				sprintf(bus.logMSG, "CONFIG :: Setting WatchdogTickerTimeout: '%s'\n", value); LOG(bus.logMSG);
				
				setWatchDogTickerRate(strToDouble(value));
				
			} else 
			if(strcmp(substr, "ReadBufferSize") == 0 && value){
				sprintf(bus.logMSG, "CONFIG :: Setting Listener Buffer size '%s'\n", value); LOG(bus.logMSG);
				
				setListenerReadBufferSize(strToInt(value));
				
			} else
			if(strcmp(substr, "ReadBufferDelay") == 0 && value){
				sprintf(bus.logMSG, "CONFIG :: Setting Listener Buffer delay '%s'\n", value); LOG(bus.logMSG);
				
				setListenerReadBufferDelay(strToInt(value));
				
			} else
			if(strcmp(substr, "Delimiter") == 0 && value){
				sprintf(bus.logMSG, "CONFIG :: Setting delimiter: '%s'\n", value); LOG(bus.logMSG);
				
				setQueryDelimiter(value);
				
			} else 
			if(strcmp(substr, "ExtendedSigHandler") == 0 && value){
				sprintf(bus.logMSG, "CONFIG :: Setting Extended Signal Handler state: '%s'\n", value); LOG(bus.logMSG);
				
				if (strcmp(value, "yes") == 0){
					SIG_EXTENDED = 1;
				} else
				if (strcmp(value, "no") == 0){
					SIG_EXTENDED = 0;
				}
				
			} else
			if(strcmp(substr, "Feisty") == 0 && value){
				sprintf(bus.logMSG, "CONFIG :: Setting Feistiness '%s'\n", value); LOG(bus.logMSG);
				
				SIG_FEISTY = strToInt(value);
				
			} else
			if(strcmp(substr, "Daemon") == 0){
				sprintf(bus.logMSG, "CONFIG :: Setting flag to enable DAEMON mode\n"); LOG(bus.logMSG);
				F.DAEMON = 1;
			} else {
				sprintf(bus.logMSG, "Unknown config entry: '%s'\n", substr);
				LOG(bus.logMSG);
				
			}
			
			//* </BODY> *//

		}

		memSet(buffer, 0, bytes_read);
	}
	
	close(fd);
	
	free(LINE_orig);
	free(buffer);
	//free(msg);
	free(tempToken);
	free(tempTokenLine);
	
	return 0;
}


void argsParser(int argc, char** argv, args_t* free_args, args_t* pair_args, args_t* flag_args){
	
	char*  arg = NULL;
	char*  temp = NULL;
	
	char get_pair = 0; // chars are less expensive -- only 1 byte.
	
	int i=0;
	//int length=0;
	
	int free_ct=0;
	int pair_ct=0;
	int flag_ct=0;
	
	
	char** elements_fl=NULL;
	char** elements_fr=NULL;
	char** elements_pr=NULL;
	
	
	
	for(i=1; i<argc; i=i+1){
		arg=argv[i];

		if(arg[0] == '-'){
			if(get_pair){
				//length=(int)strLength(temp);
				
				elements_fl = (char**) realloc(elements_fl, sizeof(elements_fl) * (flag_ct+1)); // malloc & memcpy & free
				//elements_fl[flag_ct] = (char*)malloc(sizeof(char)*length+1); // +1 because include \0
				elements_fl[flag_ct] = temp;

				flag_ct = flag_ct + 1;
				
				//if(temp){ free(temp); temp=NULL; }
				
				//temp=malloc(sizeof(char)); // we must not overwrite the same region of memory!
				
			}
			if(i+1 == argc){
				
				//length=(int)strLength(arg);
				elements_fl = (char**) realloc(elements_fl, sizeof(elements_fl) * (flag_ct+1)); // malloc & memcpy & free
				//elements_fl[flag_ct] = (char*)malloc(sizeof(char)*length+1); // +1 because include \0
				elements_fl[flag_ct] = arg;

				flag_ct = flag_ct + 1;

			} else {
				get_pair = 1;
				temp = arg;
			}
			
			
		} else {
			if(get_pair){
				
				elements_pr = (char**) realloc(elements_pr, sizeof(elements_pr) * (pair_ct+2)); // malloc & memcpy & free
				
				//length=(int)strLength(temp);
				//elements_pr[pair_ct] = (char*)malloc(sizeof(char)*length+1); // +1 because include \0
				elements_pr[pair_ct] = temp;

				//length=(int)strLength(arg);
				//elements_pr[pair_ct+1] = (char*)malloc(sizeof(char)*length+1); // +1 because include \0
				elements_pr[pair_ct+1] = arg;
								
				pair_ct = pair_ct + 2;

				get_pair=0;
			} else {
				//length=(int)strLength(arg);
				
				elements_fr = (char**) realloc(elements_fr, sizeof(elements_fr) * (free_ct+1)); // malloc & memcpy & free
				//elements_fr[free_ct] = (char*)malloc(sizeof(char)*length+1); // +1 because include \0
				elements_fr[free_ct] = arg;
				
				free_ct = free_ct + 1;

			}
		}
	}
	
	
	free_args->elements = elements_fr;
	free_args->elements_ct = free_ct;
	
	pair_args->elements = elements_pr;
	pair_args->elements_ct = pair_ct;
	
	flag_args->elements = elements_fl;
	flag_args->elements_ct = flag_ct;
	
	
	//args_t* tmparg;

	//tmparg = (args_t*)((args_t**)buff->data)[2];
	//printf("from array: %s\n", tmparg->elements[0]);
	//printf("from array: %d\n", tmparg->elements_ct);
	
	//if(temp) free(temp);
	
}



void interpretArgs(args_t* pairArgs, args_t* flagArgs, args_t* freeArgs){
	int i;
	
	//char* msg = (char*)malloc(sizeof(char)*MAX_LOG_MSG_LENGTH);
	
	// getting pairs first
	
	for(i=0; i<pairArgs->elements_ct; i=i+1){
		//printf("pairArg[%d]: %s\n", i, (char*)pairArgs->elements[i]);
		memSet(bus.logMSG, 0, LOG_MSG_LENGTH);
		if(strcmp(pairArgs->elements[i], "-h") == 0){
			sprintf(bus.logMSG, "ARGS[pair] :: showing help page: '%s'\n", pairArgs->elements[i+1]); LOG(bus.logMSG);
			
			printHelpPage(pairArgs->elements[i+1]);
			
			i=i+1;
		}
		else if(strcmp(pairArgs->elements[i], "-i") == 0){
			sprintf(bus.logMSG, "ARGS[pair] :: Setting Listener file path: '%s'\n", pairArgs->elements[i+1]); LOG(bus.logMSG);
			
			setListenerFile(pairArgs->elements[i+1]);
			
			i=i+1;
		}
		else if(strcmp(pairArgs->elements[i], "-l") == 0){
			sprintf(bus.logMSG, "ARGS[pair] :: Setting LOG file path: '%s'\n", pairArgs->elements[i+1]); LOG(bus.logMSG);
			
			setLogFile(pairArgs->elements[i+1]);
			
			i=i+1;
		}
		else if(strcmp(pairArgs->elements[i], "-c") == 0){
			sprintf(bus.logMSG, "ARGS[pair] :: Setting config file path: '%s'\n", pairArgs->elements[i+1]); LOG(bus.logMSG);

			interpretConfig(pairArgs->elements[i+1]);

			i=i+1;
		}
		else if(strcmp(pairArgs->elements[i], "-I") == 0){
			sprintf(bus.logMSG, "ARGS[pair] :: Setting init file path: '%s'\n", pairArgs->elements[i+1]); LOG(bus.logMSG);
			
			interpretINITfile(pairArgs->elements[i+1]);
			
			i=i+1;
		}
		else if(strcmp(pairArgs->elements[i], "-e") == 0){
			sprintf(bus.logMSG, "ARGS[pair] :: Showing meaning for ExitCode(%s)\n", pairArgs->elements[i+1]); LOG(bus.logMSG);
			i=i+1;
		}
		else if(strcmp(pairArgs->elements[i], "-D") == 0){
			sprintf(bus.logMSG, "ARGS[pair] :: Setting delimiter: '%s'\n", pairArgs->elements[i+1]); LOG(bus.logMSG);

			setQueryDelimiter(pairArgs->elements[i+1]);
			
			i=i+1;
		}
		else if(strcmp(pairArgs->elements[i], "-b") == 0){
			sprintf(bus.logMSG, "ARGS[pair] :: Setting Listener Buffer size: '%s'\n", pairArgs->elements[i+1]); LOG(bus.logMSG);
			
			setListenerReadBufferSize(strToInt(pairArgs->elements[i+1]));
			
			i=i+1;
		}
		else if(strcmp(pairArgs->elements[i], "-B") == 0){
			sprintf(bus.logMSG, "ARGS[pair] :: Setting Buffer Fill-up delay: '%s'\n", pairArgs->elements[i+1]); LOG(bus.logMSG);
			
			setListenerReadBufferDelay(strToInt(pairArgs->elements[i+1]));
			
			i=i+1;
		}
		else if(strcmp(pairArgs->elements[i], "-T") == 0){
			sprintf(bus.logMSG, "ARGS[pair] :: Setting ListeningTickerTimeout: '%s'\n", pairArgs->elements[i+1]); LOG(bus.logMSG);
			
			setListenerReadWait(strToInt(pairArgs->elements[i+1]));
			
			i=i+1;
		}
		else if(strcmp(pairArgs->elements[i], "-t") == 0){
			sprintf(bus.logMSG, "ARGS[pair] :: Setting TimeTillDeath: '%s'\n", pairArgs->elements[i+1]); LOG(bus.logMSG);
			bus.TTD = strToInt(pairArgs->elements[i+1]);
			i=i+1;
		}
		else if(strcmp(pairArgs->elements[i], "-w") == 0){
			sprintf(bus.logMSG, "ARGS[pair] :: Setting WatchdogTickerTimeout: '%s'\n", pairArgs->elements[i+1]); LOG(bus.logMSG);
			
			setWatchDogTickerRate(strToDouble(pairArgs->elements[i+1]) );
			
			i=i+1;
		}
		else {
			sprintf(bus.logMSG, "ARGS[pair] :: Unknown pair: '%s %s'\n", pairArgs->elements[i], pairArgs->elements[i+1]); LOG(bus.logMSG);
			i=i+1;
		}
	}

	for(i=0; i<flagArgs->elements_ct; i=i+1){
		
		memSet(bus.logMSG, 0, LOG_MSG_LENGTH);
		//printf("flagArg[%d]: %s\n", i, (char*)flagArgs->elements[i]);
		if(strcmp(flagArgs->elements[i], "-h") == 0){
			LOG("ARGS[flag] :: HELP flag detected\n");
			printHelpPage("short");
		}
		else if(strcmp(flagArgs->elements[i], "-d") == 0){
			LOG("ARGS[flag] :: Daemon flag - DAEMONIZING\n");

			F.DAEMON = 1;
		}
		else {
			sprintf(bus.logMSG, "ARGS[flag] :: Unknown flag: '%s'\n", flagArgs->elements[i]); LOG(bus.logMSG);
		}

	}

	for(i=0; i<freeArgs->elements_ct; i=i+1){
		
		memSet(bus.logMSG, 0, LOG_MSG_LENGTH);
		sprintf(bus.logMSG, "ARGS[free] :: Free arg detected: '%s'\n", freeArgs->elements[i]); LOG(bus.logMSG);
		//printf("freeArg[%d]: %s\n", i, (char*)freeArgs->elements[i]);
		
	}
	
	
	//free(msg);
}



void renameProcess(char* newName){
	//*name[1] = *newName;
	int i=0;
	
	if(newName == NULL){
		for(;i<*ARGC; i=i+1){ // nulling all the parameters
			memSet(ARGV[i], 0, strLength(ARGV[i]));
		}
	}
	memSet(*ARGV, 0, strLength(*ARGV));
	memcpy(*ARGV, newName, strLength(newName));
}




/**
 * @RETURN	OK  - File Descriptor
 * 			NOK - NULL pointer
 */
int deparser(buck_t* b){
	int mode = 0;
	int fd;
	
	char* tempStr = NULL;
	
	umask(022);
	//printBin(b->mod);
	
	if(b->active){
		b->flushAt.tv_sec = (int)timestamp() + b->flushAfter;
		return -1;
	}
	
	if(b->mtd&F_MTD_FILE){
		//printf("FILE  \n");
		if(access(b->loc, F_OK) == 0){
			//printf("EXISTS  \n");
			if(b->mod & F_MODE_FILE_EXCL){ // pipefile does NOT exist
				LOG("File exists and this is unexpected. Skipping flush\n");
				return -1;
			}
		} else {
			//printf("NEXIST  \n");
			if(!(b->mod & F_MODE_FILE_CREAT)) {
				LOG("File does not exist. Skipping flush\n");
				return -1;
			}
		}
		
		
		
		if(b->mod & F_MODE_FILE_ABNORMAL){ // pipe-file
			//printf("PIPE  \n");
			if(access(b->loc, F_OK) != 0){
				mkfifo(b->loc, 0720);
			}
			
			mode = mode|O_WRONLY;
		}
		else { // normal-file
			if(b->mod & F_MODE_FILE_APPEND) mode = mode|O_APPEND;
			mode = mode|O_WRONLY|O_CREAT;
		}
		
		

		char* loc = strNullTerm(b->loc, b->locLen);
		//printf("[DEPARSER]OPENING....\n");
		b->active = 1; //printf("[D]Setting___________________ACTIVE\n");
		b->fd = open(loc, mode);
		//printf("[DEPARSER]OPEN returns fd='%d'\n", b->fd);
		chmod(loc, 0700);


		//printf("LOCATION = '%s'\n", loc);
		free(loc);
	} else
	
	
	
	if(b->mtd&F_MTD_SOCKET){
		//printf("SOCKET  \n");

		struct sockaddr_in addr_in;
		struct sockaddr_un addr_un;
		
		memSet(&addr_un, 0, sizeof(addr_un));
		memSet(&addr_in, 0, sizeof(addr_in));

		if(b->mod & F_MODE_SOCK_SERVER){ // SERVER

			if(b->mod & F_MODE_FILE_ABNORMAL){ // socket FILE
				if(b->locLen > sizeof(addr_un.sun_path)-1){
					LOG("Location segment is too long. Stick to sizeof(addr_un.sun_path-1). Shrinking location segment.\n");
					b->locLen = sizeof(addr_un.sun_path)-1;
				}
				tempStr = strNullTerm(b->loc, b->locLen);
				unlink(tempStr);
				free(tempStr); tempStr=NULL;
				
				fd = socket(AF_UNIX, SOCK_STREAM, 0);
				
				addr_un.sun_family = AF_UNIX;
				
				memcpy(addr_un.sun_path, b->loc, b->locLen);
				
				bind(fd, (struct sockaddr*) &addr_un, b->locLen + ((void*)&addr_un.sun_path - (void*)&addr_un));
			}
			else { // network socket
				
				fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
				//int opt=1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
				addr_in.sin_family = AF_INET;
				//addr_in.sin_addr.s_addr = INADDR_ANY;
				addr_in.sin_addr.s_addr = inet_addr(strNullTerm(b->loc, b->locLen));
				addr_in.sin_port = htons(b->port);

				bind(fd, (struct sockaddr*) &addr_in, sizeof(addr_in));
			}
			
			if(listen(fd, 1) == 0){ // Clever accept() w/ timeout
				b->active = 1;

				int slct;
				struct timeval tv;
				fd_set rfds;
				FD_ZERO(&rfds);
				FD_SET(fd, &rfds);
				
				tv.tv_sec = b->flushAfter;

				slct = select(fd+1, &rfds, NULL, NULL, &tv);
				if(slct > 0 || b->flushAfter == 0){
					//printf("SELECT()'ed\n");
					b->fd = accept(fd, NULL, NULL);
				} else {
					//printf("noSELECT()'ed\n");
					close(fd);
					close(b->fd);
					b->fd = -1;
				}
				if(b->fd < 0){
					b->active = 0;
					free(tempStr);
					tempStr = strNullTerm(b->loc, b->locLen);
					
					unlink(tempStr);
					
					free(tempStr); tempStr=NULL;

				}
				close(fd);
			}
		}
		
		
		
		else { // CLIENT

			if(b->mod & F_MODE_FILE_ABNORMAL){ // socket FILE
				if(b->locLen > sizeof(addr_un.sun_path)-1){
					LOG("Location segment is too long. Stick to sizeof(addr_un.sun_path-1). Shrinking location segment.\n");
					b->locLen = sizeof(addr_un.sun_path)-1;
				}


				fd = socket(AF_UNIX, SOCK_STREAM, 0);

				addr_un.sun_family = AF_UNIX;
				
				memcpy(addr_un.sun_path, b->loc, b->locLen);

				if( connect(fd, (struct sockaddr*) &addr_un, b->locLen + ((void*)&addr_un.sun_path - (void*)&addr_un)) == 0){
					b->fd = fd;
					b->active = 1;
				} else {
					b->fd = -1;
					b->active = 0;
					close(fd);
				}
			}
			else { // network socket

				fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
				//setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, 1, 4);
				addr_in.sin_family = AF_INET;
				//addr_in.sin_addr.s_addr = INADDR_ANY;
				addr_in.sin_addr.s_addr = inet_addr(strNullTerm(b->loc, b->locLen));

				addr_in.sin_port = htons(b->port);

				if(connect(fd, (struct sockaddr*) &addr_in, sizeof(addr_in)) == 0){
					b->fd = fd;
					b->active = 1;
				} else {
					b->fd = -1;
					b->active = 0;
					close(fd);
				}
			}
			
		}
		
	} 
	
	else {
		LOG("Unknown method\n");
	}
	//printf("[deparser] returns fd: '%d'\n", b->fd);
	
	if(b->active == 1) b->flushAt.tv_sec = (int)timestamp() + b->flushAfter;
	return b->fd;
	
}



void* _WATCHDOG_THREAD(void* b){
	F.RUNNING = 1;
	unsigned int TIME = 0;
	
	char* tempStr = NULL;
	
	LOG("Launching WDog\n");
	
	//pthread_setname_np(pthread_self(), "[sBUS WDog]");
	//prctl(PR_SET_NAME,"[sBUS WDog]",0,0,0);

	buck_t* START = (buck_t*) b;
	buck_t* B  = START;
	
	int fd;
	int bufLen = 256;
	char* buff = malloc(sizeof(char)*bufLen);
	
	

	while(F.RUNNING == 1){
		B = START;
		
		TIME=timestamp();
		while(B){ // While there are elements on our list
			//printf("B FD='%d', flushTime is: '%d', flushAfter is '%d', NOW is: '%d' \n", B->fd, (int)(B->flushAt.tv_sec), B->flushAfter, TIME); 
			if(B->flushAt.tv_sec && B->flushAt.tv_sec < TIME){  // If it should have been flushed by now
				//printf("DRAIN TIME!!!!!\n");
				if(!B->LOCKED) // Pick one and see if it's locked
					if(! B->ACQUIRED){   // Now check whether lock has been acquired...
						B->ACQUIRED = 1; // If not - acquire a lock 
						B->LOCKED   = 1; // and lock the object
						
					
						if(B->delete_at_flush){
							shutdown(B->fd, SHUT_WR); /* send */
							shutdown(B->fd, SHUT_RD); /* recv */
							close(B->fd);
							
							free(tempStr);
							tempStr = strNullTerm(B->loc, B->locLen);
							
							unlink(tempStr);
							
							free(tempStr); tempStr=NULL;
						}
						else {
							if(B->mtd & F_MTD_FILE)
								if(B->mtd & F_MODE_FILE_ABNORMAL){ // pipe file
									//printf("[WDOG]stealing data from pipe\n");
									
									free(tempStr);tempStr = NULL;
									tempStr = strNullTerm(B->loc, B->locLen);
									fd = open(tempStr, O_RDWR|O_NONBLOCK);
									
									free(tempStr);tempStr = NULL;
									
									while(fd > 0 && read(fd, buff, bufLen) >0); // draining the pipe
									close(fd);
								}
								else { // if it's a normal file
									ftruncate(B->fd, 0);
								}
							else if(B->mtd & F_MTD_SOCKET){
								//printf("[WDOG]closing socket\n");
								shutdown(B->fd, SHUT_WR); /* send */
								shutdown(B->fd, SHUT_RD); /* recv */
								close(B->fd);
								
								free(tempStr); tempStr = NULL;
								tempStr = strNullTerm(B->loc, B->locLen);
								unlink(tempStr);
								
								free(tempStr); tempStr = NULL;
							}
						}
						
					B->flushAt.tv_sec = 0;
					
					B->LOCKED   = 0;
					B->ACQUIRED = 0;
				}
			}
			B = B->next;
		}

		usleep(bus.WDogTOut);
	}
	
	F.RUNNING = 0;
	free(buff);
	LOG("Leaving WDog\n");
	free(tempStr);
	return NULL;
}

/**
 * Will sleep for half the µseconds given as '*curr', or for 'min' seconds, if '*curr'/2 < 'min'.
 * This will cause dynamic sleep. This sleep ir more precise as it checks time quite often enough to detect 
 * any changes, e.g. sleep time has been increased or decreased while asleep.
 * It also won't overdrive CPU as this sleep won't cause interrupts more often than every 'min'th µsecond.
 * 
 * @EXAMPLE 
 *     while(minSleep = 100000 && (usleepVal = (wakeUpAt.tv_sec - timestamp())*1000000) > 0) dynusleep(&usleepVal, minSleep);
 */
void dynusleep(unsigned int* curr, unsigned int min){
	*curr = *curr/2;
	if(*curr<=min) *curr = min;
	usleep(*curr);
}

void* _FLUSHER_THREAD(void* buck){
	buck_t* b = (buck_t*) buck;
	
	char* tempStr = NULL;
	
	// Using dynamic sleep to reduce CPU usage on polling
	unsigned int usleepVal=0;
	unsigned int minSleep = POW10_5; // µseconds. usleepVal cannot be less than this
	
	int fd = 0;
	//int Bfd = 0; // bucket FD
	int bufLen = 256;
	int deparsetReturn = 0;
	char* buff = malloc(sizeof(char)*bufLen);
	
	deparsetReturn = deparser(b);
	
	if(b->fd > 0){
		write(b->fd, b->dat, b->datLen);
		
		if(deparsetReturn == -1) return NULL; // make it re-flush, but do not bring another thread up
		
		
		if(b->flushAfter > 0) {

			usleepVal = (int)b->flushAfter*POW10_6;
			//printf("[FLUSHER]Sleep until: '%d', timestamp='%d'\n", (int)b->flushAt.tv_sec, (int)timestamp());
			if(!b->immediate) while(b->flushAt.tv_sec && (usleepVal = (b->flushAt.tv_sec - timestamp())*POW10_6) > 0) dynusleep(&usleepVal, minSleep);
			while(b && b->ACQUIRED && b->LOCKED){sleep(1);} // waiting for the object to be unlocked. Possibly watchdog got it...
			if(b){
				b->LOCKED = 1; b->ACQUIRED = 1;
				
				if(b->delete_at_flush){
					shutdown(b->fd, SHUT_WR); /* send */
					shutdown(b->fd, SHUT_RD); /* recv */
					close(b->fd);
					
					free(tempStr);tempStr = NULL;
					tempStr = strNullTerm(b->loc, b->locLen);
					unlink(tempStr);
					
					free(tempStr);tempStr = NULL;
				}
				else {
					if(b->mtd & F_MTD_FILE)
						if(b->mtd & F_MODE_FILE_ABNORMAL){ // pipe file
							free(tempStr);tempStr = NULL;
							tempStr = strNullTerm(b->loc, b->locLen);

							fd = open(tempStr, O_RDWR|O_NONBLOCK);
							
							free(tempStr);tempStr = NULL;
							
							while(fd > 0 && read(fd, buff, bufLen) > 0); // draining the pipe
							close(fd);
						}
						else { // if it's a normal file
							ftruncate(b->fd, 0);
						}
					else if(b->mtd & F_MTD_SOCKET){
						shutdown(b->fd, SHUT_WR); /* send */
						shutdown(b->fd, SHUT_RD); /* recv */
						close(b->fd);
						free(tempStr); tempStr=NULL;
						tempStr = strNullTerm(b->loc, b->locLen);
						
						unlink(tempStr);
						
						free(tempStr); tempStr=NULL;
					}
				}
				
				b->LOCKED = 0; b->ACQUIRED = 0;
			
			} else return 0;
			
		}
		//printf("[FLUSHER]CLOSING thread\n");
		b->flushAt.tv_sec = 0;
		
		shutdown(b->fd, SHUT_RD); /* recv */
		shutdown(b->fd, SHUT_WR); /* send */
		close(b->fd); // these are in critical section too... need to do something about it :/
		b->fd = -1;
		b->active = 0; 
		//printf("LEAVING THREAD\n");
	}
	


	free(buff);
	free(tempStr);
	//b->active = 0; 
	b->FLUSHER_ID = 0;
	pthread_exit(NULL);
	//return NULL;
}


int launchWatchdog(void* b){
	int runs = 1;
	int attempts = 3;
	unsigned int timeout = 1;
	
	F.RUNNING = 1;

	while(runs != 0 && attempts > 0){
		runs = pthread_create(&WDogThread, NULL, &_WATCHDOG_THREAD, b);
		attempts = attempts - 1;
		sleep(timeout);
	}
	
	return runs;
}


int launchFlusher(void* buck){
	
	
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	
	
	
	return pthread_create(&((buck_t*)buck)->FLUSHER_ID, NULL, &_FLUSHER_THREAD, buck);
	
}



