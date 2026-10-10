#include "game.h"

#include "port/prof.h"

#include <psyq/libetc.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/demo.h"
#include "bodyprog/events/events_main.h"
#include "bodyprog/game_boot/game_boot.h"
#include "bodyprog/item_screens.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/screen/screen_draw.h"
#include "bodyprog/screen/vsync.h"
#include "bodyprog/sys/joy.h"
#include "bodyprog/text/text_draw.h"
#include "bodyprog/math/math.h"
#include "bodyprog/memcard.h"
#include "bodyprog/sound/sound_system.h"
#include "bodyprog/sys/game_main.h"
#include "screens/b_konami/b_konami.h"
#include "screens/credits/credits.h"
#include "screens/options.h"
#include "screens/saveload.h"
#include "screens/stream/stream.h"

// ========================================
// GLOBAL VARIABLES
// ========================================

s32 g_Demo_FrameCount = 0;
s32 g_WarmBootTimer   = 0;

static s32 g_PrevVBlanks = 0;

// Audio task for `SD_Call` meant to load base VAB audios.
static u16 g_baseVabAudiosTaskId[] = {
   160,
   162,
   0
};

static void (*g_GameStateUpdateFuncs[])(void) = {
    GameState_Init_Update,
    GameState_KonamiLogo_Update,
    GameState_KcetLogo_Update,
    GameState_MovieIntroFadeIn_Update,
    GameState_AutoLoadSavegame_Update,
    GameState_MovieIntroAlternate_Update,
    GameState_MovieIntro_Update,
    GameState_MainMenu_Update,
    GameState_LoadSavegameScreen_Update,
    GameState_MovieOpening_Update,
    GameState_LoadScreen_Update,
    GameState_InGame_Update,
    GameState_MapEvent_Update,
    GameState_ExitMovie_Update,
    GameState_ItemScreens_Update,
    GameState_PaperMapScreen_Update,
    GameState_LoadSavegameScreen_Update,
    GameState_DebugMoviePlayer_Update,
    GameState_Options_Update,
    GameState_LoadStatusScreen_Update,
    GameState_LoadMapScreen_Update,
    GameState_Credits_Update
};

// ========================================
// MAIN LOOP
// ========================================

void GameState_Init_Update(void) // 0x80032D1C
{
    s32 gameState;
    s32 VabAudioTaskId;

    switch (g_GameWork.gameStateSteps[0])
    {
        case 0:
            g_GameWork.background2dColor.r = 0;
            g_GameWork.background2dColor.g = 0;
            g_GameWork.background2dColor.b = 0;

            Screen_Init(SCREEN_WIDTH, false);
            Game_StateStepIncrement(0);
            break;

        case 1:
            if (Sd_AudioStreamingCheck() == AudioStreamingState_None)
            {
                VabAudioTaskId = g_baseVabAudiosTaskId[g_GameWork.gameStateSteps[1]];
                if (VabAudioTaskId != 0)
                {
                    SD_Call(VabAudioTaskId);
                    g_GameWork.gameStateSteps[1]++;
                }
                else
                {
                    Game_StateStepIncrement(0);
                }
            }
            break;

        case 2:
            Fs_QueueStartReadTim(FILE_1ST_FONT16_TIM, FS_BUFFER_1, &g_Font16AtlasImg);
            Fs_QueueStartReadTim(FILE_1ST_KONAMI_TIM, FS_BUFFER_1, &g_KonamiLogoImg);

            ScreenFade_Start(true, false, false);
            g_GameWork.gameStateSteps[0]++;
            break;

        case 3:
            if (ScreenFade_IsFinished())
            {
                Fs_QueueWaitForEmpty();

                Game_StateSetNext(g_GameWork.gameState + 1);
            }
            break;
    }

    MemCard_ElementsUpdate();
    Screen_BackgroundImgDraw(&g_MainImg0);
    func_80089090(1);
}

void MainLoop(void) // 0x80032EE0
{
    #define TICKS_PER_SECOND_MIN              (TICKS_PER_SECOND / 4)
    #define H_BLANKS_PER_SECOND               15780
    #define H_BLANKS_PER_TICK                 (H_BLANKS_PER_SECOND / TICKS_PER_SECOND)                    // 263
    #define H_BLANKS_TO_SEC_CONVERSION_FACTOR ((float)Q12(1.0f) / (float)H_BLANKS_PER_SECOND)             // 0.25956907477f
    #define H_BLANKS_PER_FRAME_MIN            (H_BLANKS_PER_SECOND / TICKS_PER_SECOND_MIN)                // 1052
    #define H_BLANKS_Q12_TO_SEC_SCALE         (s32)(H_BLANKS_TO_SEC_CONVERSION_FACTOR * (float)Q12(1.0f)) // 1063
    #define H_BLANKS_GRAVITY_SCALE            Q12(9.8f * H_BLANKS_TO_SEC_CONVERSION_FACTOR)               // 10419
    #define V_BLANKS_MAX                      4

    s32 vBlanks;
    s32 vCount;
    s32 vCountCopy;
    s32 interval;

    // Initialize engine.
    GsInitVcount();
    MemCard_SysInit();
    MemCard_SysEnable();
    MemCard_InitStatus();
    Joy_Init();
    VSyncCallback(&Screen_VSyncCallback);

    // NTSC-J moves these calls into the `HP_SAFE1`/`S__SAFE2` anti-modchip overlays,
    // likely to make sure those overlays wouldn't be patched out by pirates.
#if !VERSION_REGION_IS(NTSCJ)
    InitGeom();
    ItemScreen_TmdGsFCallInit();
    func_800890B8();
#endif

    SD_Init();

    // Run game.
    while (true)
    {
        g_TickCount++;

        // Update input.
        Joy_ReadP1();
        Demo_ControllerDataUpdate();
        Joy_ControllerDataUpdate();

        if (MainLoop_ShouldWarmReset() == 2)
        {
            Game_WarmBoot();
            continue;
        }

        g_ActiveBufferIdx = GsGetActiveBuff();

        if (g_GameWork.gameState == GameState_MainLoadScreen ||
            g_GameWork.gameState == GameState_InGame)
        {
            GsOUT_PACKET_P = (PACKET*)(TEMP_MEMORY_ADDR + (g_ActiveBufferIdx << 17));
        }
        else if (g_GameWork.gameState == GameState_InventoryScreen)
        {
            GsOUT_PACKET_P = (PACKET*)(TEMP_MEMORY_ADDR + (g_ActiveBufferIdx * 40000));
        }
        else
        {
            GsOUT_PACKET_P = (PACKET*)(TEMP_MEMORY_ADDR + (g_ActiveBufferIdx << 15));
        }

        GsClearOt(0, 0, &g_OrderingTable0[g_ActiveBufferIdx]);
        GsClearOt(0, 0, &g_OrderingTable2[g_ActiveBufferIdx]);

        g_SysWork.bgmStatusFlags = BgmStatusFlag_None;

        // Call update function for current game state.
        PROF_BEGIN("MainLoop: game state update")
        g_GameStateUpdateFuncs[g_GameWork.gameState]();
        PROF_END("MainLoop: game state update")
#ifdef SH_PORT
        {
            extern void Port_DemoTick(void);
            Port_DemoTick(); // Demo menu cutscene: back to the list once it's over (src/port/demo_menu.c)
        }
#endif

        Demo_Update();
        Demo_GameRandSeedSet();

        if (MainLoop_ShouldWarmReset() == 2)
        {
            Game_WarmBoot();
            continue;
        }

        PROF_BEGIN("MainLoop: fade, memory card, sound, files, pad, camera")
        PROF_BEGIN("ml: Screen_FadeUpdate")
        Screen_FadeUpdate();
        PROF_END("ml: Screen_FadeUpdate")
        PROF_BEGIN("ml: MemCard_Update")
        MemCard_Update();
        PROF_END("ml: MemCard_Update")

        // Update sound.
        PROF_BEGIN("ml: Sd_TaskPoolExecute")
        Sd_TaskPoolExecute();
        PROF_END("ml: Sd_TaskPoolExecute")
        if (Sd_AudioStreamingCheck() == AudioStreamingState_None)
        {
            PROF_BEGIN("ml: Fs_QueueUpdate")
            Fs_QueueUpdate();
            PROF_END("ml: Fs_QueueUpdate")
        }

        PROF_BEGIN("ml: func_80089128")
        func_80089128();
        PROF_END("ml: func_80089128")
        PROF_BEGIN("ml: func_8008D78C")
        func_8008D78C(); // Camera update?
        PROF_END("ml: func_8008D78C")
        PROF_END("MainLoop: fade, memory card, sound, files, pad, camera")
        DrawSync(SyncMode_Wait);

        // Handle V sync.
        if (g_SysWork.sysFlags & SysFlag_DemoActive)
        {
            vBlanks   = VSync(SyncMode_Count);
            g_VBlanks = vBlanks - g_PrevVBlanks;

            Demo_PresentIntervalUpdate();

            interval      = g_Demo_VideoPresentInterval;
            g_PrevVBlanks = vBlanks;

            if (interval < g_IntervalVBlanks)
            {
                interval = g_IntervalVBlanks;
            }

#if defined(SH_PORT) && defined(SH_PORT_BENCH)
            {
                // Port benchmark (`SH1_BENCH=1`): the demo runs uncapped, but with its own timing. No
                // waiting for vertical blanks; the vertical blank callback (demo frame counter, state
                // timers) runs here once per simulated blank instead of from the interrupt; the frame's
                // time step is the demo's own (vCount below). The demo plays the same frames as at
                // normal speed, as fast as the PS2 manages: frames per second = headroom.
                extern void Port_VSyncCallbackManual(int on);
                extern void Port_VSyncCallbackRun(void);
                s32         k;
                Port_VSyncCallbackManual(1);
                for (k = 0; k < interval; k++)
                {
                    Port_VSyncCallbackRun();
                }
                g_VBlanks = interval;
            }
#else
            do
            {
                VSync(SyncMode_Wait);
                g_VBlanks++;
                g_PrevVBlanks++;
            }
            while (g_VBlanks < interval);
#endif

            g_UncappedVBlanks = g_VBlanks;
            g_VBlanks         = MIN(g_VBlanks, 4);

            vCount     = g_Demo_VideoPresentInterval * H_BLANKS_PER_TICK;
            vCountCopy = g_UncappedVBlanks * H_BLANKS_PER_TICK;
            g_VBlanks  = g_Demo_VideoPresentInterval;
#if defined(SH_PORT) && (defined(SH_PORT_BENCH) || defined(SH_PORT_DEMO_TRACE))
            {
                // Demo sync trace and deterministic frame dumps, by the demo's own playback position
                // (g_Demo_DemoStep, the recorded input in use; frames spent waiting for the disc don't
                // advance it): Harry's position every 60 steps; identical lines between a normal and
                // an uncapped (SH1_BENCH=1) run mean the demo plays the same. Benchmark builds dump
                // the screen (host:frame_*.ppm) at steps 100, 200, ... 1000 for exact renderer
                // comparisons (tools/port/bench_frames.sh).
                static u32 nextTrace = 60, nextDump = 100;
                if (g_Demo_DemoStep >= nextTrace)
                {
                    printf("demo trace: step %d, Harry at %d %d %d\n", g_Demo_DemoStep, g_SysWork.playerWork.player.position.vx,
                           g_SysWork.playerWork.player.position.vy, g_SysWork.playerWork.player.position.vz);
                    nextTrace = g_Demo_DemoStep + 60 - g_Demo_DemoStep % 60;
                }
#ifdef SH_PORT_BENCH
                if (g_Demo_DemoStep >= nextDump && nextDump <= 1000)
                {
                    extern void Display_RequestDump(void);
                    Display_RequestDump();
                    nextDump += 100;
                }
#endif
            }
#endif
        }
        else
        {
#if defined(SH_PORT) && defined(SH_PORT_BENCH)
            {
                extern void Port_VSyncCallbackManual(int on);
                Port_VSyncCallbackManual(0); /* not a demo: the interrupt calls it again */
            }
#endif
#ifdef SH_PORT
            {
                // Test runs (pcsx2_run.py --gameplay) time unbroken stretches of gameplay.
                static s32 inGameplay;
                s32        now = g_GameWork.gameState == GameState_InGame && g_SysWork.sysState == SysState_Gameplay;
                if (now != inGameplay)
                {
                    printf(now ? "port: gameplay\n" : "port: gameplay ended\n");
                    inGameplay = now;
                }
            }
#endif
            if (g_SysWork.sysState != SysState_Gameplay)
            {
                g_VBlanks     = VSync(SyncMode_Count) - g_PrevVBlanks;
                g_PrevVBlanks = VSync(SyncMode_Count);
#if defined(SH_PORT) && SH_PORT_FPS == 60
#ifndef SH_PORT_CUTSCENE_VBLANKS
#define SH_PORT_CUTSCENE_VBLANKS 1
#endif
                // Port option (`SH1_FPS=60`): as in gameplay, wait only when the frame took under a
                // vertical blank, so a frame running slightly late doesn't drop to 30 fps.
                while (g_VBlanks < SH_PORT_CUTSCENE_VBLANKS)
                {
                    VSync(SyncMode_Wait);
                    g_VBlanks++;
                    g_PrevVBlanks++;
                }
#else
                VSync(SyncMode_Wait);
#endif
            }
            else
            {
                if (!ScreenFade_IsNone())
                {
                    VSync(SyncMode_Wait);
                }

                g_VBlanks     = VSync(SyncMode_Count) - g_PrevVBlanks;
                g_PrevVBlanks = VSync(SyncMode_Count);

                while (g_VBlanks < g_IntervalVBlanks)
                {
                    VSync(SyncMode_Wait);
                    g_VBlanks++;
                    g_PrevVBlanks++;
                }
            }

            // Update V blanks.
            g_UncappedVBlanks = g_VBlanks;
            g_VBlanks         = MIN(g_VBlanks, V_BLANKS_MAX);

            // Update V count.
            vCount     = MIN(GsGetVcount(), H_BLANKS_PER_FRAME_MIN); // NOTE: Will call `GsGetVcount` twice.
            vCountCopy = vCount;
        }

        // Update delta time.
        g_DeltaTime    = Q12_MULT(vCount, H_BLANKS_Q12_TO_SEC_SCALE);
        g_DeltaTimeRaw = Q12_MULT(vCountCopy, H_BLANKS_Q12_TO_SEC_SCALE);
        g_GravitySpeed = Q12_MULT(vCount, H_BLANKS_GRAVITY_SCALE);
        GsClearVcount();

        // Draw objects?
        PROF_BEGIN("MainLoop: swap and draw")
        GsSwapDispBuff();
        GsSortClear(g_GameWork.background2dColor.r, g_GameWork.background2dColor.g, g_GameWork.background2dColor.b, &g_OrderingTable0[g_ActiveBufferIdx]);
        GsDrawOt(&g_OrderingTable0[g_ActiveBufferIdx]);
        GsDrawOt(&g_OrderingTable2[g_ActiveBufferIdx]);
        PROF_END("MainLoop: swap and draw")
    }

    #undef TICKS_PER_SECOND_MIN
    #undef H_BLANKS_PER_SECOND
    #undef H_BLANKS_PER_TICK
    #undef H_BLANKS_TO_SEC_CONVERSION_FACTOR
    #undef H_BLANKS_PER_FRAME_MIN
    #undef H_BLANKS_Q12_TO_SEC_SCALE
    #undef H_BLANKS_GRAVITY_SCALE
    #undef V_BLANKS_MAX
}
