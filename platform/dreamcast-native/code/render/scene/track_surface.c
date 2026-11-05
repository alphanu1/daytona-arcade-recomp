#include "render/scene/track_surface.h"
#include "scene_loader.h"
#include <stdalign.h>
#include <stdio.h>
#include <string.h>

/* Beginner pavement/markings, curbs and immediate barriers, audited against
 * XYZ/normals and contact sheets. Match binding headers, not export indices.
 * shortcut: course0 only; audit another course before extending this selection. */
alignas(32) static const uint16_t surface_headers[][4]={
    {17609,0,2988,35200}, /* curb M0 */
    {17609,0,2988,35392}, /* curb M1 */
    {17408,0,6875,52608}, /* rail M2 */
    {17600,0,6875,52608}, /* rail M3 */
    {32768,0,0,39808},   /* rail back M5 */
    {17627,0,7168,39744}, {17627,0,7168,40000},
    {17609,0,2988,35264}, /* curb M30 */
    {17627,0,7168,35904}, {17435,0,7168,40000},
    {17435,0,7168,35904}, {17435,0,7168,39744},
    {17435,0,7168,39040}, {17435,0,6656,40000},
    {17435,0,6656,35904}, {17435,0,6656,39744},
    {17425,0,3588,39744}, {17425,0,3588,40000},
    {17425,0,3588,35904}, {17627,0,7680,39744},
    {17627,0,7680,40000}, {17627,0,7680,35904},
    {25603,0,6168,35904}, /* finish-line overlay M127 */
    {17627,0,7680,39040}, {17435,0,6656,39040},
    {17627,0,6656,35904},
    {17417,0,2988,35392}, /* curb M196 */
    {17424,0,3591,40000}, /* white edge M205 */
    {17625,0,2080,38528}, /* retaining wall M211 */
    {17627,0,6656,39744}, {17627,0,6656,39040},
    {17627,0,6656,40000}, {17424,0,3590,35904},
    {25817,0,6684,35904}, /* rail M225 */
    {17627,0,7168,39040}, {17627,0,7168,37824},
    {17625,0,2080,41536}, /* retaining wall M232 */
    {17625,0,2080,41472}, /* retaining wall M233 */
    {25625,0,6684,35904}, /* rail M234 */
    {17627,0,7168,39936}
};

static void track_surface_compact(nm_pack_t *pack,const uint8_t *keep,
        unsigned *selected,unsigned *empty,unsigned *strips);

int nm_track_surface_select(nm_pack_t *pack) {
    if(!pack || !pack->data || !pack->header || pack->header->materials>1024) return 0;
    alignas(32) uint8_t keep[1024]={0};
    for(unsigned s=0;s<sizeof(surface_headers)/sizeof(*surface_headers);++s) {
        unsigned matches=0;
        for(unsigned m=0;m<pack->header->materials;++m)
            if(!memcmp(surface_headers[s],pack->materials[m].header,sizeof(*surface_headers))) {
                keep[m]=1;++matches;
            }
        if(matches!=1) return 0; /* Wrong/ambiguous source: never silently hide the road. */
    }
    unsigned selected=0,empty=0,strips=0;
    track_surface_compact(pack,keep,&selected,&empty,&strips);
    /* Existing group bounds/cones remain conservative for every retained face
     * (face.group and face.layer travel with the face). */
    printf("Native track surface: %u/%lu faces, %u strips, %u empty meshes; pavement/curbs/barriers only\n",
        selected,(unsigned long)pack->header->faces,strips,empty);
    return selected!=0;
}

/* Keep the faces of the marked materials, in place, in pack order. */
static void track_surface_compact(nm_pack_t *pack,const uint8_t *keep,
        unsigned *selected_out,unsigned *empty_out,unsigned *strips_out) {
    unsigned selected=0,empty=0,strips=0;
    nm_strip_t *records=(nm_strip_t *)pack->strips;
    for(unsigned m=0;m<pack->header->meshes;++m) {
        nm_loader_yield(0); /* a load in another screen's frames (scene_loader.h) */
        nm_mesh_t *mesh=(nm_mesh_t *)pack->meshes+m;
        nm_face_t *faces=(nm_face_t *)pack->faces+mesh->face_first;
        unsigned count=0,kept=0,strip=mesh->padding[2];
        const unsigned strip_end=records ? strip+mesh->padding[3] : strip;
        /* Level-of-detail groups (meshpack.h): each range keeps its retained
         * faces, which stay consecutive; a range, or a whole group, may end
         * up empty and then draws nothing. Before the faces move. */
        if(pack->lod_groups) {
            nm_lod_group_t *group=(nm_lod_group_t *)pack->lod_groups+(mesh->padding[4]&0xffffu);
            unsigned scan=0,before=0;
            for(unsigned g=mesh->padding[4]>>16;g;--g,++group) {
                const unsigned first=group->face_first-mesh->face_first;
                for(;scan<first;++scan) before+=keep[faces[scan].material];
                group->face_first=mesh->face_first+before;
                for(unsigned level=0;level<NM_LOD_LEVELS;++level) {
                    unsigned retained=0;
                    for(unsigned n=group->face_count[level];n;--n,++scan) retained+=keep[faces[scan].material];
                    group->face_count[level]=(uint16_t)retained; before+=retained;
                }
            }
        }
        for(unsigned f=0;f<mesh->face_count;++f) {
            /* Strip records follow face order and cover one material each,
             * so a strip is retained or dropped whole with its first face.
             * Only its face range moves; its vertex indices are unchanged. */
            const int starts=strip<strip_end && records[strip].face_first==mesh->face_first+f;
            if(keep[faces[f].material]) {
                if(starts) {
                    nm_strip_t moved=records[strip];
                    moved.face_first=mesh->face_first+count;
                    records[mesh->padding[2]+kept++]=moved;
                }
                faces[count++]=faces[f];
            }
            strip+=starts;
        }
        if(records) mesh->padding[3]=kept;
        mesh->face_count=count;selected+=count;empty+=count==0;strips+=kept;
    }
    *selected_out=selected;*empty_out=empty;*strips_out=strips;
}

/* Scenery stages (track_surface.h). A pack with scenery classes carries the
 * stage of every material (tools/prepare_face_order.py --scenery-classes:
 * the audited list above is stage 0); the selection is that comparison. A
 * pack without classes knows only the reduced view and the complete pack. */
int nm_scenery_stage_select(nm_pack_t *pack,unsigned stage) {
    if(!pack || !pack->data || !pack->header || pack->header->materials>1024 ||
       stage>NM_SCENERY_STAGE_LAST) return 0;
    if(!(nm_pack_flags(pack)&NM_PACK_SCENERY_CLASSES)) {
        if(!stage) return nm_track_surface_select(pack);
        printf("Native scenery stage %u: pack without classes, every face kept\n",stage);
        return 1;
    }
    alignas(32) uint8_t keep[1024]={0};
    unsigned by_stage[NM_SCENERY_STAGE_LAST+1]={0},materials=0;
    for(unsigned m=0;m<pack->header->materials;++m) {
        keep[m]=(pack->materials[m].padding[0]&NM_SCENERY_STAGE_MASK)<=stage;
        materials+=keep[m];
    }
    for(unsigned m=0;m<pack->header->meshes;++m) {
        const nm_mesh_t *mesh=pack->meshes+m;
        for(unsigned f=0;f<mesh->face_count;++f)
            ++by_stage[pack->materials[pack->faces[mesh->face_first+f].material].padding[0]&NM_SCENERY_STAGE_MASK];
    }
    unsigned selected=0,empty=0,strips=0;
    track_surface_compact(pack,keep,&selected,&empty,&strips);
    printf("Native scenery stage %u: %u/%lu faces of %u materials, %u strips, %u empty meshes; "
           "pack faces by stage: reduced %u, opaque %u, ground %u, cutout %u, checker %u, objects %u\n",
        stage,selected,(unsigned long)pack->header->faces,materials,strips,empty,
        by_stage[0],by_stage[1],by_stage[2],by_stage[3],by_stage[4],by_stage[5]);
    return selected!=0;
}
