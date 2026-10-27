/** @brief SH1's light-profiling probes (SH1_LITEPROF=1), after the HAL's (psx-to-ps2-HAL include/port/prof.h).
 * X(id, name). */

#define HAL_GAME_LITEPROF(X) \
    X(LP_MESH_LIGHT,   "mesh: light") /* func_80057344: point light (func_80057658) */ \
    X(LP_MESH_XFORM,   "mesh: transform") /* func_80057344: vertex transform (func_80057B7C) */ \
    X(LP_MESH_DRAW,    "mesh: packets") /* func_80057344: packets (Gfx_MeshDraw) */ \
    X(LP_PL_COLLECT,   "light: collect") /* Port_PointLight: pass 1 (calls = vertices) */ \
    X(LP_PL_BATCH,     "light: batch") /* Port_PointLight: Gte_RtpsDotBatch */ \
    X(LP_PL_APPLY,     "light: apply") /* Port_PointLight: pass 2 */ \
    X(LP_MAP_FX,       "map fx (grid)") /* map6_s02 func_800D2364: the effect grid, whole function */ \
    X(LP_MAP_FX_BATCH, "map fx: batch") /* its Gte_RtpBatch part (Port_GridBatch) */ \
    X(LP_SNOW_MOVE,    "particles: move") /* Particle_Update: the particle state/movement loop */ \
    X(LP_SNOW_DRAW,    "particles: draw") /* Particle_Update: the draw loop (Particle_SnowDraw / Particle_RainDraw) */ \
    X(LP_SNOW_RTPS,    "snow: rtps") /* Particle_SnowDraw: the flake's RTPS and its stores (calls = flakes drawn) */
