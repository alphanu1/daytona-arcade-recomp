"""The drawn level of a car (render only): textured while it is NM_FLAT_CAR_PX rows or more, in every scene."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
text = (root / "code/render/scene/practice.c").read_text()
rule = "#ifndef NM_SMALL_CAR_PX" + text.split("#ifndef NM_SMALL_CAR_PX", 1)[1].split("int nm_practice_render_parts", 1)[0]
source = r'''
#include <assert.h>
typedef struct { int attract_active; } nm_practice_scene_t;
typedef struct { float matrix[12],focus[2]; } nm_instance_t;
''' + rule + r'''
/* rows = 480/384 * 1.28 * focus / depth */
static unsigned drawn(int attract,unsigned car,unsigned level,float focus,float depth) {
    const nm_practice_scene_t scene={.attract_active=attract};
    nm_instance_t body={.focus={focus,focus}};
    body.matrix[11]=depth;
    return practice_drawn_level(&scene,car,level,&body);
}
int main(void) {
    const float follow=1.6f*12.0f*28.0f/1.6f/12.0f*(384.0f/480.0f)/1.28f*12.0f; /* 12 rows at depth 28 */
    assert(NM_SMALL_CAR_PX==12 && NM_FLAT_CAR_PX==6);
    /* The race's follow view: as before. */
    assert(drawn(0,1,3,follow,20)==3 && drawn(0,1,4,follow,10)==4);
    assert(drawn(0,1,3,follow,30)==2 && drawn(0,1,2,follow,50)==2);
    assert(drawn(0,1,2,follow,60)==1 && drawn(0,1,1,follow,120)==1);
    assert(drawn(0,0,1,follow,120)==1 && drawn(0,0,4,follow,60)==4); /* the player */
    assert(drawn(0,1,0,follow,10)==0 && drawn(1,1,0,follow*8,10)==0); /* not listed */
    /* A long lens (the podium's and the attract's cameras): level 1 by depth, tens of rows tall. */
    assert(drawn(1,0,1,follow*8,120)==2 && drawn(1,5,1,follow*8,250)==2);
    assert(drawn(1,5,1,follow*8,460)==1);       /* under 6 rows */
    assert(drawn(1,5,1,follow*8,-120)==1);      /* behind the camera */
    assert(drawn(0,5,1,follow*8,120)==2);       /* the race's own long lenses too */
    /* The cinematics' higher levels stay the arcade's. */
    assert(drawn(1,5,2,follow,60)==2 && drawn(1,5,3,follow,60)==3 && drawn(1,5,4,follow,200)==4);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="daytona-drawn-level-") as tmp:
    path = Path(tmp)
    (path / "check.c").write_text(source)
    subprocess.run([os.environ.get("HOST_CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(path / "check.c"), "-o", str(path / "check")], check=True)
    subprocess.run([str(path / "check")], check=True)
print("PASS: level 1 is drawn textured at 6 rows or more in every scene; the race's rule and the cinematics' levels as before")
