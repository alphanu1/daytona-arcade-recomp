#include "../include/indexed_texture.h"
#include <assert.h>
/* Host shared-library entry, also cross-compiled to check the SH4ZAM path. */
size_t nm_check_indexed_texture(void *out,size_t capacity,const uint32_t *header,
        const uint16_t *patterns,unsigned entries,const uint8_t *indices,
        size_t bytes,const uint16_t *palette) {
    nm_indexed_texture_t prepared;
    unsigned char original[sizeof(prepared)];
    memset(&prepared,0xa5,sizeof(prepared));memcpy(original,&prepared,sizeof(prepared));
    if(!nm_indexed_texture_prepare(&prepared,header,patterns,entries,indices,bytes)) {
        assert(!memcmp(original,&prepared,sizeof(prepared)));
        return 0;
    }
    return nm_indexed_texture_expand_prepared(out,capacity,&prepared,palette);
}
