#ifndef PORT_DEMO_MENU_H
#define PORT_DEMO_MENU_H

/** @brief The port's Demo menu (src/port/demo_menu.c): plays an in-game cutscene from the main menu. */

typedef struct
{
    const char* name;   /** Event function name (decomp). */
    short       mapIdx; /** `e_MapIdx` */
    short       event;  /** Index into the map's `mapEvents`. */
} PortCutscene;

extern const PortCutscene g_PortCutscenes[];  /* build/port/demo_cutscenes.c (tools/port/cutscene_list.py) */
extern const int          g_PortCutsceneCount;

int  Port_DemoMenuUpdate(void);   /* -2: nothing, -1: back, >= 0: cutscene chosen */
void Port_DemoMenuDraw(void);
int  Port_DemoChosen(void);       /* the cutscene being started or played (-1: none) */
void Port_DemoBegin(int idx);
int  Port_DemoEventStart(void);   /* Event_Update: starts the chosen event; 1 when it did */
void Port_DemoTick(void);         /* once per frame, after the game state update */
int  Port_DemoReturnToList(void); /* main menu: open the Demo list (once after a cutscene) */
void Port_DemoRemote(int idx);    /* network request (agent_ps2.c "DM", tools/port/ps2_ctl.py demo) */
int  Port_DemoRemoteTake(void);   /* main menu: the requested cutscene, or -1 */
int  Port_DemoRemotePending(void);
void Port_NewGameRemote(void);    /* network request (agent_ps2.c "NG", tools/port/ps2_ctl.py newgame) */
int  Port_NewGameRemoteTake(void); /* main menu: 1 once after a request */

#endif
