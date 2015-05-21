#define POW10_1  10
#define POW10_2  100
#define POW10_3  1000
#define POW10_4  10000
#define POW10_5  100000
#define POW10_6  1000000
#define POW10_7  10000000
#define POW10_8  100000000
#define POW10_9  1000000000
#define POW10_10 10000000000


#define query_token_ct 5 //  how many tokens should the query contain





#define     LOG_MSG_LENGTH 256
#define MAX_LOG_MSG_LENGTH 300
#define field_delimiter_2 "::"   // Delimiter user to separate actual LOC from MOD (last element of the query)

#define tok_field_cmd 0
#define tok_field_lbl 1
#define tok_field_dat 2
#define tok_field_mtd 3
#define tok_field_loc 4





#define F_MTD_FILE 		 0x01
#define F_MTD_SOCKET 	 0x02
#define F_MTD_MEMORY 	 0x04
#define F_MTD_UNUSED 	 0x08 // not used yet


/* We manage to stuff all flags into 8 bits - we can use a char for them */
#define F_MODE_FILE_ABNORMAL 	0x01
#define F_MODE_FILE_PIPE		0x02
#define F_MODE_SOCK_SERVER 		0x04
#define F_MODE_SOCK_AUTO 		0x08


#define F_MODE_FILE_APPEND 		0x10
#define F_MODE_FILE_CREAT 		0x20
#define F_MODE_FILE_EXCL 		0x40
#define F_MODE_FILE_UNUSEDFLAG 	0x80 // not used yet


