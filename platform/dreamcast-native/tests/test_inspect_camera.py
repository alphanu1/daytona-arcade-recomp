"""Real SH4ZAM software-backend camera vs independent rigid-transform checks."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
sh4zam = Path(os.environ.get("SH4ZAM_ROOT", root.parents[2] / "sh4zam"))
source = r'''
#include "render/scene/inspect_camera.h"
#include <assert.h>
#include <math.h>
#include <string.h>
#include "inspect_camera_check.h"
static void near(float a,float b) { assert(fabsf(a-b)<0.0005f); }
static void check(const float view[12]) {
    shz_vec3_t p=nm_inspect_camera_position(view);
    for(unsigned r=0;r<3;++r) {
        near(view[r]*p.x+view[3+r]*p.y+view[6+r]*p.z+view[9+r],0);
        for(unsigned s=0;s<3;++s)
            near(view[r]*view[s]+view[3+r]*view[3+s]+view[6+r]*view[6+s],r==s);
    }
}
int main(void) {
    assert(nm_inspect_camera_check());
    const float identity[12]={1,0,0,0,1,0,0,0,1,0,0,0};
    nm_inspect_camera_t camera; float view[12];
    nm_inspect_camera_reset(&camera,identity);
    nm_inspect_camera_step(&camera,0,0,0,shz_vec3_init(0,0,0),view);
    assert(!memcmp(view,identity,sizeof(view)));
    nm_inspect_camera_step(&camera,0.05f,0,0,shz_vec3_init(0,0,20),view);
    near(view[11],-1); check(view);
    nm_inspect_camera_reset(&camera,identity);
    nm_inspect_camera_step(&camera,0.05f,SHZ_F_PI*10,0,shz_vec3_init(0,0,20),view);
    shz_vec3_t p=nm_inspect_camera_position(view);
    near(p.x,1);near(p.y,0);near(p.z,0);check(view);
    /* Non-identity base: preserve captured camera, compose in its local space. */
    const float base[12]={0,0,-1,0,1,0,1,0,0,-3,2,4};
    nm_inspect_camera_reset(&camera,base);
    nm_inspect_camera_step(&camera,0,0,0,shz_vec3_init(0,0,0),view);
    assert(!memcmp(base,view,sizeof(view)));
    for(unsigned i=0;i<10000;++i) {
        nm_inspect_camera_step(&camera,0.017f,1,0.7f,shz_vec3_init(0,0,0),view);
        const float cy=cosf(camera.yaw),sy=sinf(camera.yaw);
        const float cx=cosf(camera.pitch),sx=sinf(camera.pitch);
        /* Camera-to-base rotation R_y R_x, inverted independently below. */
        const float rotation[9]={cy,0,-sy,sy*sx,cx,cy*sx,sy*cx,-sx,cy*cx};
        for(unsigned c=0;c<4;++c) for(unsigned r=0;r<3;++r)
            near(view[c*3+r],rotation[r*3]*base[c*3]+rotation[r*3+1]*base[c*3+1]+rotation[r*3+2]*base[c*3+2]);
        check(view);
    }
    assert(!memcmp(camera.base.elem,base,sizeof(base)));
    /* Long stalls must not launch the free camera across the track. */
    nm_inspect_camera_reset(&camera,identity);
    nm_inspect_camera_step(&camera,30,0,0,shz_vec3_init(20,0,0),view);
    near(nm_inspect_camera_position(view).x,1);
    nm_inspect_camera_reset(&camera,base);
    nm_inspect_camera_step(&camera,0,0,0,shz_vec3_init(0,0,0),view);
    assert(!memcmp(view,base,sizeof(view)));
}
'''
with tempfile.TemporaryDirectory(prefix="daytona-inspect-camera-") as tmp:
    path = Path(tmp)
    (path / "check.c").write_text(source)
    subprocess.run([os.environ.get("HOST_CC", "cc"), "-std=gnu11", "-O1",
                    "-fsanitize=address,undefined", "-I", str(root / "include"),
                    "-I", str(root / "tests"),
                    "-I", str(sh4zam / "include"), str(path / "check.c"),
                    str(root / "code/render/scene/inspect_camera.c"),
                    str(sh4zam / "source/sw/shz_xmtrx_sw.c"), "-lm", "-o", str(path / "check")], check=True)
    subprocess.run([str(path / "check")], check=True)
print("PASS: free-camera translation, yaw/pitch, captured base, inverse, 10000 updates and reset")
