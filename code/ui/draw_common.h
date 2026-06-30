/*
 * draw_common.h — declares only what's not already in app.h.
 *
 * The actual prototypes for rrect / glow_dot / draw_progress /
 * draw_text / measure_text_w / hairline live in app.h since both
 * main_screen.c and menu.c need them — this header exists mainly
 * for documentation/clarity and to mark draw_common.c's role.
 */
#ifndef DRAW_COMMON_H
#define DRAW_COMMON_H

#include "app.h"
/* primitives declared in app.h, implemented in draw_common.c */

#endif /* DRAW_COMMON_H */
