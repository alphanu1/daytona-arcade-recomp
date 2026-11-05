/* Standalone ROM-free test entrypoint, never linked into the game. */
#include <enDjinn/enj_enDjinn.h>
#include <sh4zam/shz_sh4zam.h>
#include <malloc.h>
#include <stdio.h>
#include "../presentation_probe.h"

int main(void) {
    shz_xmtrx_init_scale(2.0f,-3.0f,4.0f);
    const shz_vec4_t v=shz_xmtrx_transform_vec4(shz_vec4_init(3.0f,-4.0f,5.0f,1.0f));
    if(v.x!=6.0f || v.y!=12.0f || v.z!=20.0f || v.w!=1.0f) return 1;
    enj_state_init_defaults();
    enj_state_get()->video.pvr_params.vertex_buf_size=512*1024;
    for(unsigned list=0;list<5;++list)
        enj_state_get()->video.pvr_params.opb_sizes[list]=PVR_BINSIZE_8;
    enj_state_get()->video.display_mode=DM_640x480;
    enj_state_get()->video.pixel_mode=PM_RGB888P;
    if(enj_state_startup()!=0) return 1;
    vid_set_dithering(false);
    return nm_probe_run();
}
