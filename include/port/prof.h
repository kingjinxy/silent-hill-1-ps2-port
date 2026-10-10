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
#elif defined(SH_PORT_MSG_DEBUG)
/* Spike test (SH1_MSG_DEBUG=1): a section taking over 30 ms in one go is printed. */
void Port_Spike(const char* name, unsigned int ms); /* src/port/demo_menu.c */
#define PROF_BEGIN(name) { unsigned int prof_t_; __asm__ volatile("mfc0 %0, $9" : "=r"(prof_t_));
#define PROF_END(name)                                                                            \
    {                                                                                             \
        unsigned int prof_e_;                                                                     \
        __asm__ volatile("mfc0 %0, $9" : "=r"(prof_e_));                                          \
        if (prof_e_ - prof_t_ > 30u * 294912u)                                                    \
            Port_Spike(name, (prof_e_ - prof_t_) / 294912u);                                     \
    } }
#else
#define PROF_BEGIN(name) {
#define PROF_END(name)   }
#endif

#endif
