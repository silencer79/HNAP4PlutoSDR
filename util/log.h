/*
 * log.h
 *
 *  Created on: 26.12.2019
 *      Author: lukas
 */

#ifndef UTIL_LOG_H_
#define UTIL_LOG_H_

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <complex.h>
#include <stdlib.h>
#include <syslog.h>

// Log level enumeration
enum {TRACE, DEBUG,INFO,WARN,ERR,NONE};

// Set the global log level
#define LOG_LEVEL INFO

// Enable/Disable timing performance evaluation
#define TIMING_ENABLE

// Enable/Disable syslog messages.
#define SYSLOG_ENABLE

// log macros with specified log level
#define LOG(level,...) do { if (level>=LOG_LEVEL) \
		{ printf(__VA_ARGS__); } } while(0)

#define PRINT_BIN(level,data,len) do { if (level>=LOG_LEVEL) \
						{ for(int i=0; i<len; i++) {printf("%02x",data[i]);}}} while(0)

// Log makros for matlab including the log level
#define LOG_MATLAB_FC(level,x,y,z) do { if (level>=LOG_LEVEL) \
										{ log_matlab_fc(x,y,z); } } while(0);
#define LOG_MATLAB_F(level,x,y,z) do { if (level>=LOG_LEVEL) \
										{ log_matlab_f(x,y,z); } } while(0);
#define LOG_MATLAB_I(level,x,y,z) do { if (level>=LOG_LEVEL) \
										{ log_matlab_i(x,y,z); } } while(0);
#define LOG_BIN(level,buf,len,filen,mode) do { if (level>=LOG_LEVEL) \
										{ log_bin(buf,len,filen,mode); } } while(0);


// Log makros for syslog
#ifdef SYSLOG_ENABLE
#define SYSLOG(level,...) syslog(level,__VA_ARGS__)
#else
#define SYSLOG(level,...)
#endif

// structure for simple timing measurements. To be used with the makros defined below
struct timecheck_s {
    char name[80];
    float avg;
    float max;
    int count;
    int avg_len;
    struct timespec start;
};

void log_matlab_fc(float complex* cpx, int num_samps, char* filename);
void log_matlab_f(float* floats, int num_samps, char* filename);
void log_matlab_i(int* buf, int num_samps, char* filename);
void log_bin(uint8_t* buf, uint buf_len, char* filename, char* mode);
void timecheck_stop(struct timecheck_s* time, int crit_delay_us);
void timecheck_info(struct timecheck_s* time);

#ifdef TIMING_ENABLE
#define TIMECHECK_CREATE(obj) struct timecheck_s* obj = NULL
#define TIMECHECK_INIT(obj,objname,avglen) if (obj==NULL) { obj = calloc(sizeof(struct timecheck_s),1); \
                                        memcpy(obj->name,objname,strlen(objname)); \
                                        obj->avg_len=avglen; }
#define TIMECHECK_START(obj) do { clock_gettime(CLOCK_MONOTONIC,&obj->start); \
                                    } while(0)
#define TIMECHECK_STOP(obj) timecheck_stop(obj,0)
#define TIMECHECK_INFO(obj) timecheck_info(obj)
#define TIMECHECK_STOP_CHECK(obj,max_delay) timecheck_stop(obj,max_delay)
#else
// set timing makros to empty statements, so we do not waste resources if we do not want to time the application
#define TIMECHECK_CREATE(name)
#define TIMECHECK_INIT(obj,name)
#define TIMECHECK_START(name)
#define TIMECHECK_STOP(name)
#define TIMECHECK_INFO(name)
#define TIMECHECK_STOP_CHECK(obj,max_delay)
#endif

#endif /* UTIL_LOG_H_ */
