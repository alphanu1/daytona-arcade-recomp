/* UI placement check, not a gameplay or SH4ZAM algorithm test.
 * cc -std=c11 -I platform/dreamcast-native/include this_file.c -lm -o /tmp/name-pose
 */
#include <assert.h>
#include "render/ui/name_draw.h"

int main(void) {
    static nm_drive_params_t params;
    params.sine[16383]=UINT32_C(0x3f800000); /* Axis endpoint for reference path. */
    float pose[12];
    assert(nm_name_initial_pose(&params,0.25f,0,pose));
    assert(pose[0]==0.075f && pose[4]==0.075f && pose[8]==0.075f);
    assert(pose[9]==0.25f && pose[10]==-0.2f && pose[11]==1.0f);
    assert(nm_name_initial_pose(&params,-0.25f,16384,pose));
    assert(pose[2]==0.075f && pose[6]==-0.075f);
    assert(pose[9]==-0.25f && pose[10]==-0.2f && pose[11]==1.0f);
    assert(!nm_name_initial_pose(&params,NAN,0,pose));
    assert(!nm_name_initial_pose(NULL,0,0,pose));
    assert(!nm_name_initial_pose(&params,0,0,NULL));
    const nm_name_layout_t layout={.initial_x={-0.25f,0,0.25f}};
    const nm_name_marker_t marker={0};
    nm_name_entry_t entry={.name={'A','.','Z'}};
    unsigned binding;
    for(unsigned count=0;count<=3;++count) {
        entry.count=count;
        assert(nm_name_draw_count(&entry)==30+count+(count<3));
        for(unsigned selected=0;selected<29;++selected) {
            entry.index=selected;
            for(unsigned i=0;i<nm_name_draw_count(&entry);++i) {
                assert(nm_name_draw_pose(&params,&layout,&entry,&marker,0,i,&binding,pose));
                if(i<29) assert(binding==i && pose[11]==1.0f);
                else if(i<29+count) {
                    const unsigned letters[]={0,26,25};
                    assert(binding==letters[i-29] && pose[9]==layout.initial_x[i-29]);
                } else if(i==29+count) assert(binding==29 && pose[11]==1.035f);
                else assert(binding==selected && pose[9]==layout.initial_x[count]);
            }
            assert(!nm_name_draw_pose(&params,&layout,&entry,&marker,0,
                nm_name_draw_count(&entry),&binding,pose));
        }
    }
    entry.count=0;entry.index=7;
    for(unsigned frame=0;frame<64;++frame) {
        float expected[12];
        const uint16_t spin=(frame&31)<16 ? (uint16_t)((frame&31)<<12) : 0;
        assert(nm_name_draw_pose(&params,&layout,&entry,&marker,frame,7,&binding,pose));
        assert(nm_name_ring_pose(&params,0,0,spin,expected));
        assert(!memcmp(pose,expected,sizeof(pose)) && binding==7);
        assert(nm_name_draw_pose(&params,&layout,&entry,&marker,frame,30,&binding,pose));
        assert(nm_name_initial_pose(&params,layout.initial_x[0],spin,expected));
        assert(!memcmp(pose,expected,sizeof(pose)) && binding==7);
    }
    entry.count=4;assert(!nm_name_draw_count(&entry));
    entry.count=1;entry.name[0]='{';
    assert(!nm_name_draw_pose(&params,&layout,&entry,&marker,0,29,&binding,pose));
    assert(!nm_name_draw_pose(&params,&layout,NULL,&marker,0,0,&binding,pose));
    return 0;
}
