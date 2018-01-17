#ifndef JJLOG_HH
#define JJLOG_HH

#include "core/sstring.hh"

//#define PROXY_DEBUG
//#define PROXY_LOG
#define LOG_FILE	"/home/net/jjtest/log"
#define DEBUG_FILE	stderr

inline void log_message(const seastar::sstring &s){
#ifdef PROXY_DEBUG
	fwrite(s.begin(), s.size(), 1, DEBUG_FILE);
#endif
#ifdef PROXY_LOG
	FILE *fp = fopen(LOG_FILE, "a");
	if(!fp) return;
	fwrite(s.begin(), s.size(), 1, fp);
	fclose(fp);
#endif
}

inline void log_message(seastar::sstring &&s){
#ifdef PROXY_DEBUG
	fwrite(s.begin(), s.size(), 1, DEBUG_FILE);
#endif
#ifdef PROXY_LOG
	FILE *fp = fopen(LOG_FILE, "a");
	if(!fp) return;
	fwrite(s.begin(), s.size(), 1, fp);
	fclose(fp);
#endif
}

inline void log_message(const char *fmt){
#ifdef PROXY_DEBUG
	fprintf(DEBUG_FILE, fmt, 0);
#endif
#ifdef PROXY_LOG
	FILE *fp = fopen(LOG_FILE, "a");
	if(!fp) return;
	fprintf(fp, fmt, 0);
	fclose(fp);
#endif
}

template <typename ... Args>
inline void log_message(const char *fmt, Args ... args){
#ifdef PROXY_DEBUG
	fprintf(DEBUG_FILE, fmt, args...);
#endif
#ifdef PROXY_LOG
	FILE *fp = fopen(LOG_FILE, "a");
	if(!fp) return;
	fprintf(fp, fmt, args...);
	fclose(fp);
#endif
}

template <typename T>
inline void log_message(T obj){
#ifdef PROXY_DEBUG
	obj();
#endif
}

#endif