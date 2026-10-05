#ifndef _PORT_PROF_H
#define _PORT_PROF_H

/** @brief Port profiling (build with SH1_PROF=1): named cycle counters, inclusive (a counter's time
 * includes the counters started inside it). Prof_Report() prints them, most expensive first. */

#ifdef SH_PORT_PROF
void Prof_Begin(const char* name, unsigned int* start);
void Prof_End(const char* name, unsigned int start);
void Prof_Report(unsigned int frames);
#define PROF_BEGIN(name) { unsigned int prof_t_; Prof_Begin(name, &prof_t_);
#define PROF_END(name)   Prof_End(name, prof_t_); }
#else
#define PROF_BEGIN(name) {
#define PROF_END(name)   }
#endif

#endif
