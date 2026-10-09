/* device.h for tools/hosttest -- runs the shared ramp code on a PC against a
 * model of the stock firmware (sim.c). Field offsets and polarities follow the
 * Aeris device.h; the countdown rule is switchable (see sim.c). */
#ifndef DEVICE_H
#define DEVICE_H

extern unsigned char sim_struct[256];
extern unsigned char sim_dab[64];
extern unsigned char sim_cue[16];
extern unsigned char sim_state[1024];

void  sim_pid_tick(void);
void  sim_stop(void);
short sim_div(int a, int b);
void  sim_flash_read(int addr, int len, void *buf);
void  sim_flash_erase(int addr);
void  sim_flash_write(int addr, int len, void *buf);
int   sim_notify(int handle, const unsigned char *data, int len);

#define STOCK_FN(type, addr)  ((type)(addr))
extern unsigned int sim_systick;
extern int sim_picker_closed;
#define DEV_SYS_TICK        sim_systick
#define DEV_PICKER_CLOSED() (sim_picker_closed++)

#define DEV_STRUCT          sim_struct
#define OFF_SESSION         0x00
#define OFF_REACHED         0x01
#define OFF_COUNTDOWN       0x1a
#define OFF_MEAS_F          0x28
#define OFF_MEAS_C          0x2a

#define DEV_SCALE_IS_F()    (STRUCT_BASE[0x04] != 1)
#define DEV_MODE_IS_CONC()  (STRUCT_BASE[0x06] != 1)
#define DEV_RANK(conc)      (STRUCT_BASE[(conc) ? 0x08 : 0x07])

#define DEV_PRESET_OFF(base, rank)  (((rank) + (base)) * 2)
#define TBL_FL_F            0x18
#define TBL_FL_C            0x1e
#define TBL_CO_F            0x24
#define TBL_CO_C            0x2a
#define TBL_FL_HOLD         0x30
#define TBL_CO_HOLD         0x36

#define DEV_DAB_BASE        sim_dab
#define DEV_SAVE_ARM(d)     do { (d)[31] = 200; (d)[32] = 250; } while (0)
#define DEV_END_CUE         sim_cue

#define DEV_PID_TICK        sim_pid_tick
#define DEV_STOP            sim_stop
#define DEV_ROM_DIV         sim_div

#define DEV_RAMP_FLASH      0x15000
#define DEV_FLASH_READ      sim_flash_read
#define DEV_FLASH_ERASE     sim_flash_erase
#define DEV_FLASH_WRITE     sim_flash_write
#define DEV_FLASH_WRITE_MAX 32   /* smaller than the store: every save is written in pieces */

#define DEV_RAMP_STATE      sim_state
#define RAMP_TRACE_LEN      1
#define DEV_MAX_F           600

#define DEV_NOTIFY          sim_notify
#define DEV_NOTIFY_HANDLE   27
#define DEV_ID              2

#ifndef DEV_PICK_COUNT
#define DEV_PICK_COUNT      6
#endif

#define DEV_AFTER_TICK(st)  ((void)0)
extern int sim_clock_waits;
#define DEV_CLOCK_WAITS_FOR_REACHED  (sim_clock_waits)

#endif
