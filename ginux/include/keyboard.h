#ifndef KEYBOARD_H
#define KEYBOARD_H

void keyboard_init(void);
int  keyboard_haskey(void);
char keyboard_getchar(void); /* blocking */

#endif
