#include <sys/types.h>
#include <sys/time.h>
//#include <sys/select.h>
#include <stdio.h>
#include <termios.h> // for secret input from STDIN

#include "constants.h"
#include "bus.h"




void Listener();
int streamParser(char* stream, int length, char* delim, int delimLen);
buck_t* parseModesEXT(buck_t* b);
int interpretINITfile(char* path);


buck_t* bucket;
// d=$'\x01\x01\x01'; a="add${d}MyLabel${d}MyData${d}FILE${d}CNA-5::/tmp/datafile"; echo -ne "$a" >/tmp/iofile








int main(int argc, char* argv[]){
	
	ARGV = argv;
	ARGC = &argc;
	
	//SIG_EXTENDED = 1;
	
	initHelpPage();
	initBus();
	
	bucket = bucketsInit();
	
	
	args_t* free_args = (args_t*)malloc(sizeof(args_t));
	args_t* pair_args = (args_t*)malloc(sizeof(args_t));
	args_t* flag_args = (args_t*)malloc(sizeof(args_t));
	
	argsParser(argc, argv, free_args, pair_args, flag_args);
	
	//interpretConfig(DEFAULT_CONFIG);

	interpretArgs(pair_args, flag_args, free_args);
	
	free(pair_args->elements);
	free(flag_args->elements);
	free(free_args->elements);
	
	
	free(pair_args);
	free(flag_args);
	free(free_args);
	
	if(F.DAEMON) DAEMONIZE();
	
	renameProcess(NULL);
	renameProcess("[sBUS]");
	
	
	TrapSignals();
	
	//F.RUNNING = 1;
	
	if (launchWatchdog(bucket) != 0)
		LEAVE(-1);
	
	Listener();
	
	return 0;
}


/**
 * This function contains the main loop. It listens on the i/o file for new command-streams. 
 * Once data stream is received - the loop unblocks and brings the stream contents to streamParser();
 * It's possible to set a timeout before loop unblock and actually retrieving data. One might want
 * this if either BUS is running on a slow machine, or w/ HI CPU load or if one's expecting a LOT of data.
 * */
void Listener(){
	chdir("/tmp");
	
	fd_set rfds;
	struct timeval timeout;
	int selected, nfds;
	//char* stream = NULL;
	int   stream_length=0;
	
	
	//char* mesg = malloc(sizeof(char) * LOG_MSG_LENGTH);

	memSet(bus.logMSG,   0, LOG_MSG_LENGTH);
	//memSet(stream, 0, bus.readBuffSz);
	
	sprintf(bus.logMSG, "BUS started by UID[%d] UNAME[%s]\n", bus.OWNER_UID, bus.OWNER_name);
	LOG(bus.logMSG);
	
	memSet(bus.logMSG, 0, LOG_MSG_LENGTH);
	
	if(bus.fd_io < 0){
		LOG("Cannot see a lsnrFile open. Terminating...\n");
		LEAVE(-1);
	}
	
	timeout.tv_sec  = bus.lsnTOut;
	timeout.tv_usec = 0;
	
	
	//stream = malloc(sizeof(char)*bus.readBuffSz);
	
	while(F.RUNNING){
		timeout.tv_sec  = bus.lsnTOut;
		timeout.tv_usec = 0;
		
		FD_ZERO(&rfds);
		FD_SET(bus.fd_io, &rfds);
		nfds = bus.fd_io+1;
		
		
		//printf("fd_io=%d, rfds=%d\n", bus.fd_io, rfds);
		selected = select(nfds, &rfds, NULL, NULL, &timeout);
		
		if(selected == -1) LOG("LISTENER :: select() interrupted [-1]\n");
		else if(selected) {
			//printf("selected!\n");
			if(FD_ISSET(bus.fd_io, &rfds)){
				
				
				sleep(bus.lsnReadDelay);
				stream_length = read(bus.fd_io, bus._stream, bus.readBuffSz);
				//printf("READ[%d]: '%s'\n", stream_length, stream);
				streamParser(bus._stream, stream_length, NULL, 0);
				
				memSet(bus._stream, 0, bus.readBuffSz);
				//free(stream); stream = NULL;
			}
		}
		//else printf("Timeout...\n");
		
		
	}
	
	//free(mesg);
	//free(stream);
	LEAVE(0);
}


/**
 * This function is responsible for an important part of BUCKET preparation. It will determine how BUS should flush the data.
 * 
 * User gives those instructions as charset. Comparing charsets might be expensive - working with bitflags is cheaper 
 * and faster. This function will convert bytesets (a part of LOCATION field before "::") into respective bitflags and store 
 * them in BUCKET. Once it's time to flush the data - BUS will need to get rid of it ASAP. Bitflags will save some time.
 * 
 * */

buck_t* parseModesEXT(buck_t* b){ // returns parsed mode and alters loc if required
	unsigned char MODE = 0;
	int c=0;
	char C;
	
	char* tempStr = NULL;
	
	//char* msg = NULL;
	//int i=0;
	unsigned int flushAfter = 0;
	unsigned int multipl = 1;
	
	
	seqmeta_t* MODSEQ  = malloc(sizeof(seqmeta_t)); seqmeta_t* MODSEQ_orig  = MODSEQ;
	seqmeta_t* LOCSEQ  = malloc(sizeof(seqmeta_t)); seqmeta_t* LOCSEQ_orig  = LOCSEQ;
	seqmeta_t* portSEQ = malloc(sizeof(seqmeta_t)); seqmeta_t* portSEQ_orig = portSEQ;
	
	MODSEQ->address  = NULL;
	LOCSEQ->address  = NULL;
	portSEQ->address = NULL;
	
	//msg = malloc(LOG_MSG_LENGTH);
	memSet(bus.logMSG, 0, LOG_MSG_LENGTH);
	
	seqTok(MODSEQ, b->loc, b->loc + b->locLen, field_delimiter_2, strLength(field_delimiter_2));
	seqTok(LOCSEQ, NULL,   b->loc + b->locLen, field_delimiter_2, strLength(field_delimiter_2));\
	//printf("LOCSEQ='%s'\n", LOCSEQ->address);

	if(LOCSEQ == NULL || LOCSEQ->address == NULL){
		
		b->loc = MODSEQ->address;
		b->locLen = MODSEQ->length;
		b->mod = 0;
		if(b->mtd == F_MTD_SOCKET){ 
		
			seqTok(NULL, b->loc, b->loc + b->locLen, ":", 1); // location
			seqTok(portSEQ, NULL,  b->loc + b->locLen, ":", 1);
			
			if(portSEQ){
				free(tempStr); tempStr=NULL;
				tempStr = strNullTerm(portSEQ->address, portSEQ->length);
				
				b->port = strToInt(tempStr);
				b->locLen = b->locLen-1-portSEQ->length; // removing port and ':'
			}
		}
		
		//free(msg);
		free(MODSEQ_orig);
		free(LOCSEQ_orig);
		free(portSEQ_orig);
		free(tempStr);
		
		return b;
	}
	
	b->loc = LOCSEQ->address;

	b->locLen = b->locLen - strLength(field_delimiter_2);
	//printf("MODE_STR='%s'\n", MODE_STR);
	
	if(b->mtd == F_MTD_FILE){
		while(c < MODSEQ->length){
			b->locLen = b->locLen-1;
			C = *(MODSEQ->address+c);
			if(C == '-'){
				b->delete_at_flush = 0;
			}
			else if(C <= '9' && C >= '0'){
				flushAfter = flushAfter*multipl + (C-48);
				multipl = multipl*10;
				
			}
			else
				switch(C){
					case 'I':
						b->immediate = 1;
					break;
					case 'N':
						MODE = MODE&~F_MODE_FILE_ABNORMAL;
					break;
					case 'P':
						MODE = MODE|F_MODE_FILE_ABNORMAL;
					break;
					case 'T':
						MODE = MODE&~F_MODE_FILE_APPEND;
					break;
					case 'A':
						MODE = MODE|F_MODE_FILE_APPEND;
					break;
					case 'C':
						MODE = MODE|F_MODE_FILE_CREAT;
					break;
					case 'X':
						MODE = MODE|F_MODE_FILE_EXCL;
					break;
					default:
						sprintf(bus.logMSG, "Unknown FILE MODE flag: '%c'\n", C);
						LOG(bus.logMSG);
						memSet(bus.logMSG, 0, LOG_MSG_LENGTH);
					break;
				}
			c=c+1;
		}
		
	}
	else if(b->mtd == F_MTD_SOCKET){
		
		
		while(c < MODSEQ->length){
			b->locLen = b->locLen-1;
			C = *(MODSEQ->address+c); 
			if(C == '-'){
				b->delete_at_flush = 0;
			}
			else if(C <= '9' && C >= '0'){
				flushAfter = flushAfter*multipl + (C-48);
				multipl = multipl*10;
			}else
			
			switch(C){
				case 'I':
					b->immediate = 1;
				break;
				case 'N': // network socket
					MODE = MODE&~F_MODE_FILE_ABNORMAL;
					
				break;
				case 'P': // pipe-file socket
					MODE = MODE|F_MODE_FILE_ABNORMAL;
				break;
				case 'H': // Hidden socket file
					b->loc = b->loc-1;
					b->loc[0] = '\0';
					//b->locLen = b->locLen+1;
				break;
				
				case 'A': // Automatically pick between Client and Server modes
					MODE = MODE|F_MODE_SOCK_AUTO;
				break;
				case 'S': // Server socket
					MODE = MODE|F_MODE_SOCK_SERVER;
				break;
				case 'C': // Client socket
					MODE = MODE&~F_MODE_SOCK_SERVER;
				break;
				
				default:
					sprintf(bus.logMSG, "Unknown SOCK MODE flag: '%c'\n", C);
					LOG(bus.logMSG);
					memSet(bus.logMSG, 0, LOG_MSG_LENGTH);
				break;
			}
			c=c+1;
		}
		
		if(!(MODE & F_MODE_FILE_ABNORMAL) && findChar(b->loc, ':') > -1 ){ // NETWORK SOCKET
		//printf("÷÷÷÷÷÷÷÷÷÷÷÷÷÷÷÷÷÷÷÷÷÷÷÷÷\n");
			seqTok(NULL, b->loc, b->loc + b->locLen, ":", 1); // location

			seqTok(portSEQ, NULL, b->loc + b->locLen, ":", 1);
			if(portSEQ && portSEQ->address){
				free(tempStr); tempStr=NULL;
				tempStr = strNullTerm(portSEQ->address, portSEQ->length);
				
				b->port = strToInt(tempStr);
				
				b->locLen = portSEQ->address - b->loc - 2; // removing port and ':'
				//printf("ADDRESS=[%s], length=[%d]\n", strNullTerm(b->loc, b->locLen), b->locLen);
				//printf("...And the port is.... NUM[%d], STR[%s]\n", b->port, strNullTerm(portSEQ->address, portSEQ->length));
			}
		}
		
	}
	
	b->flushAfter = flushAfter;
	b->mod = MODE;
	
	b->locLen = b->locLen+1; // for either preceding or terminating '\0'
	//printf("flushAfter has been init'ed to: '%d'\n", b->flushAfter);
	//printBin(MODE);
	//printf("ADDRESS=[%s]\n", strNullTerm(b->loc, b->locLen));
	
	//free(msg);
	free(MODSEQ_orig);
	free(LOCSEQ_orig);
	free(portSEQ_orig);
	free(tempStr);
	return b;
}



/**
 * This function performs the initial parsing of the stream. It will determine what user wants (to add/get/... data),
 * get the label and other required data. Parser will split the stream into main parts: CMD, LBL, DAT, MTD, [MOD+LOC].
 * The [MOD+LOC] will be sent to parseModesEXT() to get figured out some details about LOCATION.
 * */

int streamParser(char* stream, int length, char* delim, int delimLen){
	
	char* tempDelim;
	char* tempStr = NULL;
	buck_t* b = NULL;
	
	if(delimLen == 0){
		delimLen = bus.qDelimLength;
		tempDelim = bus.queryDelim;
	} else tempDelim = delim;
	
	int tokCount = 0;
	//char* log_msg = malloc(sizeof(char)*LOG_MSG_LENGTH);
	memSet(bus.logMSG, 0, LOG_MSG_LENGTH);
	seqmeta_t* TOKEN = malloc(sizeof(seqmeta_t)); TOKEN->address = NULL;
	seqmeta_t* pieces[query_token_ct];
	
	
	
	seqTok(TOKEN, stream, stream+bus.readBuffSz, tempDelim, bus.qDelimLength);

	while(TOKEN->address != NULL && tokCount < query_token_ct){
		pieces[tokCount] = TOKEN;
		tokCount = tokCount + 1;
		TOKEN = malloc(sizeof(seqmeta_t)); TOKEN->address = NULL;
		seqTok(TOKEN, NULL, stream+bus.readBuffSz, tempDelim, bus.qDelimLength);
	}
	
	if(tokCount != query_token_ct){
		sprintf(bus.logMSG, "Incorrect number of tokens: '%d', should be '%d'\n", tokCount, query_token_ct);
		LOG(bus.logMSG);
		memSet(bus.logMSG, 0, LOG_MSG_LENGTH);
		return -1;
	}
	
	//printf("COMMAND='%s'\n", pieces[tok_field_cmd]->address);
	
	if(seqCmp("add", pieces[tok_field_cmd]->address, 3)){
		//printf("Adding stuff\n");
		free(tempStr); tempStr = NULL;
		tempStr = strNullTerm(pieces[tok_field_lbl]->address, pieces[tok_field_lbl]->length);
		
		sprintf(bus.logMSG, "Adding label: '%s'\n", tempStr);
		LOG(bus.logMSG);
		memSet(bus.logMSG, 0, LOG_MSG_LENGTH);
		
		unsigned char mtd = 0; 
		     if(seqCmp(pieces[tok_field_mtd]->address, "FILE", 4)) mtd=F_MTD_FILE;
		else if(seqCmp(pieces[tok_field_mtd]->address, "SOCK", 4)) mtd=F_MTD_SOCKET;
		
		if(seqCmp("[stdin", pieces[tok_field_dat]->address, 6)){
			// user enters data via STDIN. Check if that's possible... (daemon mode?)
			if(!isatty(0)){
				LOG("Cannot open STDIN to enter data -- NOT A TERMINAL!\n");
				return -1; // not a tty = no stdin
			}
			free(tempStr); tempStr=NULL;
			tempStr = strNullTerm(pieces[tok_field_lbl]->address, pieces[tok_field_lbl]->length);
			
			printf("Enter value for label '%s' > ", tempStr);
			fflush(stdout);
			
			struct termios oldt, newt;
			tcgetattr(0, &oldt);
			tcgetattr(0, &newt);
			
			if(seqCmp("+secret", pieces[tok_field_dat]->address+6, 7)){
			// user enters data via STDIN w/o ECHO. Check if that's possible... (daemon mode?)
				newt.c_lflag &= ~ECHO;
				tcsetattr(0, TCSAFLUSH, &newt);
			} // do not disable ECHO on stdin
			if(*(pieces[tok_field_dat]->address+6) == ']' || *(pieces[tok_field_dat]->address+13) == ']'){
				int len = 0;
				char* input = malloc(sizeof(char) * bus.readBuffSz);
				
				if((len = read(0, input, bus.readBuffSz)) > 0){
					//pieces[tok_field_dat]->address = input;
					if(len > 0){
						char* actualdata = malloc(sizeof(char) * len-1); // loose the '\n'
						//printf("___\n");
						memcpy(actualdata, input, len-1);
						//free(pieces[tok_field_dat]->address);
						pieces[tok_field_dat]->address = actualdata;
						pieces[tok_field_dat]->length = len-1;
						free(actualdata);
					}
				} else {
					free(input);
				}
			}
			
			tcsetattr(0, TCSAFLUSH, &oldt);
				
				
		} else
		if(seqCmp("[file]", pieces[tok_field_dat]->address, 6)){

			// data will be taken from file (if it exists)
			
			
			char* datafile = strNullTerm(pieces[tok_field_dat]->address+6, pieces[tok_field_dat]->length-6);
			int fd = 0;
			if(access(datafile, F_OK) == 0){
				if(access(datafile, R_OK) == 0){
					if((fd = open(datafile, O_RDONLY)) > -1){
						char* fileBuffer = malloc(sizeof(char)*bus.readBuffSz);
						int len = 0;
						if( ( len = read(fd, fileBuffer, bus.readBuffSz) ) >0 ){
							pieces[tok_field_dat]->address = fileBuffer;
							pieces[tok_field_dat]->length = len;
							close(fd);
						} else {
							free(fileBuffer);
							//free(datafile);
						}
						
					} //else printf("Failed to open\n");// failed to open
				} //else printf("Not readable\n");// not readable file
			} //else printf("Non-existant\n");// non-existant file
			
			free(datafile);
			
		}
		
		b = bucketCreateNew(pieces[tok_field_lbl], pieces[tok_field_dat], pieces[tok_field_loc], mtd); 
		
		b->locLen = length - (pieces[tok_field_loc]->address - pieces[0]->address);

		parseModesEXT(b);

		//printf("datLen = '%d'\n", b->datLen);
		if(b) switch(bucketAddNew(bucket, b)){
				case 0:
					LOG("New bucket has been added\n");
					//printf("DATA=[%s]\n", b->dat);
				break;
				case -1:
					LOG("ERR :: Could not add a new bucket - label already exists\n");
				break;
				case -2:
					LOG("ERR :: Error in BUS - bucket is not initialized.\n");
				break;
			}
		
	}
	
	else if(seqCmp("get", pieces[tok_field_cmd]->address, 3)){
		//printf("Getting stuff\n");
		free(tempStr); tempStr=NULL;
		
		tempStr = strNullTerm(pieces[tok_field_lbl]->address, pieces[tok_field_lbl]->length);

		sprintf(bus.logMSG, "Retrieving label: '%s'\n", tempStr);
		LOG(bus.logMSG);
		memSet(bus.logMSG, 0, LOG_MSG_LENGTH);

		b = bucketFindByLabel(bucket, pieces[tok_field_lbl]);
		
		if(b){
			launchFlusher(b);
		} else {
			LOG("Could not find bucket\n");
		}
	}
	
	else if(seqCmp("del", pieces[tok_field_cmd]->address, 3)){
		//printf("Deleting stuff\n");
		free(tempStr); tempStr = NULL;
		tempStr = strNullTerm(pieces[tok_field_lbl]->address, pieces[tok_field_lbl]->length);
		
		sprintf(bus.logMSG, "Deleting label: '%s'\n", tempStr);
		LOG(bus.logMSG);
		memSet(bus.logMSG, 0, LOG_MSG_LENGTH);
		switch(bucketDelByLabel(bucket, pieces[tok_field_lbl] ) ){
			case 0:
				LOG("Bucket has been deleted\n");
			break;
			case -1:
				LOG("Bucket was not deleted\n");
			break;
		}
		
	}

	else if(seqCmp("die", pieces[tok_field_cmd]->address, 3)){
		printf("Exitting...\n");
		
		LEAVE(0);
	}

	else{
		//sprintf(bus.logMSG, "Unknown command: \n", tokCount, query_token_ct);
		LOG("Unknown command\n");
		//memSet(bus.logMSG, 0, LOG_MSG_LENGTH);
		
	}
	
	while(tokCount > 0){
		//free(pieces[tokCount-1]->address);
		free(pieces[tokCount-1]);
		tokCount = tokCount-1;
	}
	
	free(TOKEN);
	//free(log_msg);
	free(tempStr);
	return 0;
}



/**
 * This function will load an INIT file. This file can define queries that should be loaded on BUS start-up.
 * These queries will be interpreted and add to buckets and/or flushed.
 * */

int interpretINITfile(char* path){
	
	
	
	int fd = -1;
	int size = 0;
	
	char* buffer = NULL;
	//char* msg    = NULL;
	int bytes_read = 0;
	
	//msg = malloc(sizeof(char)*LOG_MSG_LENGTH);
	memSet(bus.logMSG, 0, size);
	
	sprintf(bus.logMSG, "Loading configuration file: '%s'\n", path);
	LOG(bus.logMSG);
	memSet(bus.logMSG, 0, size);
	
	if(access(path, F_OK) != 0) {
		LOG("Cannot find config file\n");
		return -1;
	}
	if(access(path, R_OK) != 0) {
		LOG("Cannot open config file to READ\n");
		return -1;
	}
	
	
	fd=open(path, O_RDONLY);
	if(fd < 0){
		LOG("Failed to open config file\n");
		close(fd);
		//free(msg);
		return -1;
	}
	
	
	size = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	if(size < 0){
		LOG("Unable to seek() to the end of config file\n");
		close(fd);
		return -1;
	}
	
	
	
	buffer = malloc(sizeof(char)*size);
	memSet(buffer, 0, size);
	
	seqmeta_t* LINE = malloc(sizeof(seqmeta_t));
	seqmeta_t* LINE_orig = LINE;
	
	char* str_left;
	char* str_right;
	char** tempToken = malloc(sizeof(char*)); // we'll need 2 tokenizers: 1 for lines, another - for variables' declarations and values
	char** tempTokenLine = malloc(sizeof(char*)); // we'll have to use strtok in streamParser later....
	
	
	char* tempDelim=NULL;
	int tempDelimLen=0;
	
	while((bytes_read = read(fd, buffer, size)) > 0){
		seqReplace(buffer, bytes_read, '\n', '\0'); 
		
		seqTok_r(LINE, buffer, buffer+bytes_read-1, "\0", 1, tempTokenLine); // now read our sequence string-by-string (i.e. split by '\0')
		
		while(LINE->address){
			LINE->address = strTrim(LINE->address);
			
			if(LINE->address[0] == '#' || (LINE->address[0] == '/' && LINE->address[0] != '\0' && LINE->address[1] == '/')){ // commented-out line
				// lose the rest of the LINE
				seqTok_r(LINE, NULL, buffer+bytes_read-1, "\0", 1, tempTokenLine);
				continue;
			}

			str_left = strTok_r(LINE->address, "=", tempToken);

			seqTok_r(LINE, NULL, buffer+bytes_read-1, "\0", 1, tempTokenLine);
			
			if(str_left) 
				str_left=strTrim(str_left);
			
			if((str_right = strTok_r(NULL, "\0", tempToken)) ) 
				str_right = strTrim(str_right);
			
			//* <BODY> *//
			if(str_right && strcmp(str_left, "delimiter") == 0){
				
				if(tempDelim){
					free(tempDelim);
					tempDelim = NULL;
				}
				
				
				tempDelimLen = strLength(str_right);
				tempDelim = malloc(tempDelimLen);
				memcpy(tempDelim, str_right, tempDelimLen);
				
			} else
			if(str_left){
				streamParser(str_left, strLength(str_left)-1, tempDelim, tempDelimLen);
				
			}
			//* </BODY> *//
		}
		
		memSet(buffer, 0, bytes_read);
	}
	
	close(fd);
	
	free(LINE_orig);
	free(buffer);
	//free(msg);
	if(tempDelim) free(tempDelim);
	free(tempToken);
	free(tempTokenLine);

	////////////////////////////////////////////////////////////////
	return 0;
}


