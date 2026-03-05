#ifndef MAIN_H
#define MAIN_H

#include "types.h"

// Stubs for unused features required by start.s / uhe_stub
struct options  { int test_selected; int printmode; };
struct progress { int dummy; };
struct tseq     { int dummy1, dummy2, dummy3; char *dummy4; };

extern struct options  options;
extern struct progress progress;
extern struct tseq     tseq[];
extern uint32_t mainmemory;
extern uint8_t  cputype;

void do_init(void);
void do_loader(void);

#endif // MAIN_H
