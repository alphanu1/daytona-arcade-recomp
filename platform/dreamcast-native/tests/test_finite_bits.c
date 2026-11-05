/* Every float bit pattern: the bit tests of finite.h equal the compiler's
 * isfinite/isnan. Build without finite.h's macros in effect for the reference. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static int reference_finite(float v) { return __builtin_isfinite(v); }
static int reference_nan(float v) { return __builtin_isnan(v); }
#include "finite.h"
int main(void) {
    uint32_t bits=0; unsigned long long finite=0,nan=0;
    do {
        float value; memcpy(&value,&bits,sizeof(value));
        const int f=isfinite(value),n=isnan(value);
        if(!!f!=!!reference_finite(value) || !!n!=!!reference_nan(value) ||
           f!=nm_float_is_finite(value) || n!=nm_float_is_nan(value)) {
            printf("FAIL: bits %08lx finite %d/%d nan %d/%d\n",(unsigned long)bits,f,reference_finite(value),n,reference_nan(value));
            return 1;
        }
        if(nm_float_negative(value)!=(value<0) || nm_float_positive(value)!=(value>0)) {
            printf("FAIL: bits %08lx sign tests\n",(unsigned long)bits);
            return 1;
        }
        finite+=f!=0; nan+=n!=0;
    } while(++bits);
    const double d=1e300*1e300;
    if(isfinite(d) || !isfinite(1.0) || isnan(1.0) || !isnan(d-d)) { puts("FAIL: double arguments"); return 1; }
    printf("PASS: all 4294967296 float patterns (finite, nan, <0, >0); finite=%llu nan=%llu; double arguments keep the builtin\n",finite,nan);
    return finite==4278190080ull && nan==16777214ull ? 0 : 1;
}
