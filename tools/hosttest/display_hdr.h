/* Force-included (gcc -include) ahead of every source in the Carta 2 display
 * test: stock calls go through sim_fn(), which maps each stock address to a C
 * stand-in in display.c. Everything else -- addresses, glyph tables, offsets --
 * is the real carta2/device.h and ramp_display.c. */
#ifndef DISPLAY_HDR_H
#define DISPLAY_HDR_H
void *sim_fn(unsigned int addr);
#define STOCK_FN(type, addr)  ((type)sim_fn((unsigned int)(addr)))
#endif
