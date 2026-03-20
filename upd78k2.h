#ifndef UPD78K2_H_
#define UPD78K2_H_
#include <stdio.h>
#include "backend.h"
#include "tern.h"

typedef struct upd78k2_options upd78k2_options;

typedef struct upd78k2_context upd78k2_context;
uint8_t upd78237_sfr_read(uint32_t address, void *context);
void *upd78237_sfr_write(uint32_t address, void *context, uint8_t value);
void init_upd78k2_opts(upd78k2_options *opts, memmap_chunk const *chunks, uint32_t num_chunks);
upd78k2_context *init_upd78k2_context(upd78k2_options *opts);
void upd78k2_sync_cycle(upd78k2_context *upd, uint32_t target_cycle);
typedef void (upd_io_fun)(upd78k2_context *upd, uint8_t offset);
typedef void (upd_io_write_fun)(upd78k2_context *upd, uint8_t offset, uint8_t value, uint8_t mode);
typedef void (upd_edge_fun)(upd78k2_context *upd, uint8_t offset, uint32_t cycle);
void upd78k2_adjust_cycles(upd78k2_context *upd, uint32_t deduction);
void upd78k2_schedule_port2_transition(upd78k2_context *upd, uint32_t cycle, uint8_t bit, uint8_t level, upd_edge_fun *next_transition);
typedef void (upd_fun)(upd78k2_context *upd);
void upd78k2_insert_breakpoint(upd78k2_context *upd, uint32_t address, upd_fun *handler);
void upd78k2_remove_breakpoint(upd78k2_context *upd, uint32_t address);
void upd78k2_calc_next_int(upd78k2_context *upd);

struct upd78k2_options {
	cpu_options gen;
	FILE* address_log;
};

struct upd78k2_context {
	upd78k2_options *opts;
	tern_node *breakpoints;
	uint8_t *mem_pointers[4];
	upd_edge_fun *edge_next[7];
	upd_io_fun *io_read;
	upd_io_write_fun *io_write;
	upd_fun *sio_handler;
	upd_fun *sio_extclock;
	void *system;
	uint32_t edge_cycles[7];
	uint32_t tm1_cycle;
	uint32_t tm0_cycle;
	uint32_t sync_cycle;
	uint32_t sio_divider;
	uint32_t sio_cycle;
	uint32_t scratch2;
	uint32_t scratch1;
	uint32_t int_cycle;
	uint32_t cycles;
	uint16_t tm0;
	uint16_t sp;
	uint16_t pr0;
	uint16_t pc;
	uint16_t mk0;
	uint16_t ism0;
	uint16_t if0;
	uint16_t cr02;
	uint16_t cr01;
	uint16_t cr00;
	uint8_t iram[256];
	uint8_t port_mode[8];
	uint8_t port_input[8];
	uint8_t port_data[8];
	uint8_t main[8];
	uint8_t edge_value[7];
	uint8_t edge_int[7];
	uint8_t zflag;
	uint8_t tmc1;
	uint8_t tmc0;
	uint8_t tm1;
	uint8_t stbc;
	uint8_t sio_counter;
	uint8_t sio;
	uint8_t rbs;
	uint8_t puo;
	uint8_t psw;
	uint8_t prm1;
	uint8_t pmc3;
	uint8_t mm;
	uint8_t ist;
	uint8_t intm1;
	uint8_t intm0;
	uint8_t int_priority_flag;
	uint8_t int_enable;
	uint8_t csim;
	uint8_t crc1;
	uint8_t crc0;
	uint8_t cr11;
	uint8_t cr10;
	uint8_t chflags;
};

void upd78k2_execute(upd78k2_context *context, uint32_t target_cycle);
#endif //UPD78K2_H_
