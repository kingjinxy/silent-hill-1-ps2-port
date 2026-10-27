#ifndef PORT_DEMO_REC_H
#define PORT_DEMO_REC_H

/** @brief Console replay of play recordings (src/port/demo_rec.c): the title's REPLAY entry. */

extern int g_PortReplayMenu; /* the options screen opens as the REPLAY page (title.c sets it) */
extern int g_PortReplayBits; /* the REPLAY page's Debug Options rendering bits (-1: not opened yet) */

int Port_RecReplayReady(void);       /* a recording is waiting to start (uploaded, or host:rec.txt) */
int Port_RecReplayBegin(int bits);   /* starts it from its save with these rendering bits; 0 when it couldn't */

#endif
