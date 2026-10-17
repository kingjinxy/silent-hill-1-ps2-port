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

/* Light counters (SH1_LITEPROF=1): cycle counts around the hot parts of DrawOTag, read inline from
 * COP0 Count (a few cycles each, against the hundreds a PROF_BEGIN/END pair costs), printed with the
 * GS stats (gpu_gs.c GpuGs_Stats). Nested uses add up separately (GpuGs_Flush also runs inside the
 * polygon emit when a packet fills). */
enum
{
    LP_DRAWOTAG,   /* libgpu DrawOTag: the whole ordering table */
    LP_COMMANDS,   /* GpuGs_Commands: all GP0 packets */
    LP_POLY_PARSE, /* polygon(): reading the packet, up to the state */
    LP_POLY_STATE, /* polygon(): prim_state */
    LP_POLY_EMIT,  /* polygon(): GS packet building (emit_poly) */
    LP_RECT,       /* rectangle() */
    LP_LINES,      /* lines() */
    LP_FLUSH,      /* GpuGs_Flush: DMA to the GS, waiting for it */
    LP_STATE_TEX,  /* prim_state: tex_state (TEX0/TEXCLUT, page and CLUT caches) */
    LP_STATE_OTHER, /* prim_state: state_other (everything but the texture) */
    LP_MESH_LIGHT, /* func_80057344: point light (func_80057658) */
    LP_MESH_XFORM, /* func_80057344: vertex transform (func_80057B7C) */
    LP_MESH_DRAW,  /* func_80057344: packets (Gfx_MeshDraw) */
    LP_PL_COLLECT, /* Port_PointLight: pass 1 (calls = vertices) */
    LP_PL_BATCH,   /* Port_PointLight: Gte_RtpsDotBatch */
    LP_PL_APPLY,   /* Port_PointLight: pass 2 */
    LP_MAP_FX,     /* map6_s02 func_800D2364: the effect grid, whole function */
    LP_MAP_FX_BATCH, /* its Gte_RtpBatch part (Port_GridBatch) */
    LP_COUNT
};
#ifdef SH_PORT_LITEPROF
extern unsigned int g_LiteProf[LP_COUNT][2]; /* cycles, calls (gpu_gs.c) */
static inline unsigned int lp_now(void)
{
    unsigned int c;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    return c;
}
#define LP_BEGIN(slot) unsigned int lp_t_##slot = lp_now();
#define LP_END(slot)                                   \
    do                                                 \
    {                                                  \
        g_LiteProf[slot][0] += lp_now() - lp_t_##slot; \
        g_LiteProf[slot][1]++;                         \
    } while (0)
#else
#define LP_BEGIN(slot)
#define LP_END(slot) do { } while (0)
#endif

#endif
