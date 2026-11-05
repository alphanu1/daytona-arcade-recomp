/* Every float bit pattern: nm_drive_round_i32 equals its roundf reference in
 * both the return value and, when accepted, the integer. */
#include "drive.h"
#include <stdio.h>
int main(void) {
    uint32_t bits=0; unsigned long long accepted=0;
    do {
        float value; memcpy(&value,&bits,sizeof(value));
        int32_t a=123456789,b=123456789;
        const int ra=nm_drive_round_i32_reference(value,&a),rb=nm_drive_round_i32(value,&b);
        if(ra!=rb || a!=b) { printf("FAIL: bits %08lx reference %d/%ld inline %d/%ld\n",(unsigned long)bits,ra,(long)a,rb,(long)b); return 1; }
        accepted+=ra!=0;
    } while(++bits);
    printf("PASS: all 4294967296 float patterns; %llu accepted\n",accepted);
    return 0;
}
