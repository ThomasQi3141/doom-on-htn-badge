// Boot menu for the badge: Singleplayer / Multiplayer, shown in place of the
// attract-demo loop the badge cannot run.

#ifndef BADGE_BOOT_H
#define BADGE_BOOT_H

#include "doomtype.h"
#include "d_event.h"

typedef enum
{
    BOOT_NONE,      // the engine owns the screen (in a level, intermission...)
    BOOT_MENU,      // Singleplayer / Multiplayer
    BOOT_CONNECT,   // waiting for another badge
    BOOT_STARTING,  // a level has been asked for and does not exist yet
} bootscreen_t;

extern bootscreen_t bootscreen;

// Show the menu. Replaces D_StartTitle, so End Game also lands here.
void Boot_Start(void);

// Responder chain hook: true if the event was consumed.
boolean Boot_Responder(event_t *ev);

// Called from G_Ticker while gamestate is GS_DEMOSCREEN.
void Boot_Ticker(void);

// Called from D_Display while gamestate is GS_DEMOSCREEN.
void Boot_Drawer(void);

// A level now exists, so the boot screens are done. From G_DoLoadLevel.
//
// The screen cannot be given up at the point the level is *asked* for:
// gameaction runs a tic later, and a co-op start waits for the other badge
// on top of that, so frames are drawn in between. Without a boot screen up
// they land in D_PageDrawer, which has no page to draw.
void Boot_Finish(void);

#endif
