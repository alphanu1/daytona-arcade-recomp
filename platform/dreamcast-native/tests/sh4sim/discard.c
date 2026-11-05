/* Host-side check harness support (tools/sh4_gameplay_sim.py), not console code. Linked with
 * -Wl,--wrap=fopen,--wrap=fprintf,--wrap=fputs,--wrap=fputc: a file opened for writing whose name ends in
 * ".discard" is a real (empty) file, but nothing is formatted or written to it, so the simulator's instruction
 * count of a trace_race_native run is the race and not the 176 MB text trace. The program logic is unchanged:
 * every row is still built, only the formatting is skipped. Every other stream works normally. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

FILE *__real_fopen(const char *path,const char *mode);
int __real_fputs(const char *text,FILE *stream);
int __real_fputc(int c,FILE *stream);
FILE *__wrap_fopen(const char *path,const char *mode);
int __wrap_fprintf(FILE *stream,const char *format,...);
int __wrap_fputs(const char *text,FILE *stream);
int __wrap_fputc(int c,FILE *stream);

static FILE *discarded;
FILE *__wrap_fopen(const char *path,const char *mode) {
    FILE *file=__real_fopen(path,mode);
    const size_t n=strlen(path);
    if(file && mode[0]=='w' && n>8 && !strcmp(path+n-8,".discard")) discarded=file;
    return file;
}
int __wrap_fprintf(FILE *stream,const char *format,...) {
    if(stream && stream==discarded) return 1;
    va_list args;va_start(args,format);
    const int n=vfprintf(stream,format,args);
    va_end(args);return n;
}
int __wrap_fputs(const char *text,FILE *stream) { return stream && stream==discarded ? 1 : __real_fputs(text,stream); }
int __wrap_fputc(int c,FILE *stream) { return stream && stream==discarded ? c : __real_fputc(c,stream); }
