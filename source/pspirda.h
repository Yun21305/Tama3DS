/*
 * 3DS port of PspIrDA's platform header.
 * Original by Mathieu Albinet (see COPYRIGHT). Globals changed to extern
 * so that modern GCC (-fno-common) links correctly.
 */
#ifndef _PSPIRDA_H
#define _PSPIRDA_H

#include <stdio.h>

#define IRDA_DEBUG 5
#define IRDA_DUMP  1

extern int irda_debug;
extern FILE *debug_fd;
extern int irda_dump;
extern FILE *dump_fd;

void dump(unsigned char data, int send_or_receive);

#ifdef IRDA_DEBUG
#  define DEBUG(n, format, args...) do { if (debug_fd && irda_debug >= (n)) { fprintf(debug_fd, "%s(): " format, __FUNCTION__, ##args); fflush(debug_fd); } } while (0)
#else
#  define DEBUG(n, format, args...)
#endif

#ifdef IRDA_DUMP
#  define DUMP(data, send_or_receive) do { if (irda_dump == 1) dump((data), (send_or_receive)); } while (0)
#else
#  define DUMP(data, send_or_receive)
#endif

#define MAXPATH 1024
#define MAXNAME 256
#define MAX_ENTRY 256

#endif
