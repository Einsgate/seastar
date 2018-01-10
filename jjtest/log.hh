#ifndef JJLOG_HH
#define JJLOG_HH

#include "core/sstring.hh"

#define PROXY_DEBUG
//#define PROXY_LOG
#define LOG_FILE	stdout
#define DEBUG_FILE	stderr

inline void log_message(const seastar::sstring &s){
#ifdef PROXY_DEBUG
	fwrite(s.begin(), s.size(), 1, DEBUG_FILE);
#endif
#ifdef PROXY_LOG
	fwrite(s.begin(), s.size(), 1, DEBUG_FILE);
#endif
}

inline void log_message(seastar::sstring &&s){
#ifdef PROXY_DEBUG
	fwrite(s.begin(), s.size(), 1, DEBUG_FILE);
#endif
#ifdef PROXY_LOG
	fwrite(s.begin(), s.size(), 1, DEBUG_FILE);
#endif
}

inline void log_message(const char *fmt){
#ifdef PROXY_DEBUG
	fprintf(DEBUG_FILE, fmt, 0);
#endif
#ifdef PROXY_LOG
	fprintf(LOG_FILE, fmt, 0);
#endif
}

template <typename ... Args>
inline void log_message(const char *fmt, Args ... args){
#ifdef PROXY_DEBUG
	fprintf(DEBUG_FILE, fmt, args...);
#endif
#ifdef PROXY_LOG
	fprintf(LOG_FILE, fmt, args...);
#endif
}

template <typename T>
inline void log_message(T obj){
#ifdef PROXY_DEBUG
	obj();
#endif
}

#endif