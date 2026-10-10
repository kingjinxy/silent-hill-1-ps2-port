/** sh1spu's messages go through log.c: a write never waits for the console (see there). Formatted
 * with sprintf (sysclib's vsprintf printed garbage on hardware). */
#ifndef SH1SPU_LOG_H
#define SH1SPU_LOG_H
void spu_log_init(void);
void spu_log_put(const char* text, int n);
#define printf(...)                                \
    do                                             \
    {                                              \
        char spu_log_line_[256];                   \
        int  spu_log_n_ = sprintf(spu_log_line_, __VA_ARGS__); \
        spu_log_put(spu_log_line_, spu_log_n_);    \
    } while (0)
#endif
