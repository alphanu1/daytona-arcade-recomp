/* Offline partition/BVH experiment and loaded DNM3 visibility check.
 * clang -std=c11 -O2 -fno-fast-math -ffp-contract=off tests/check_face_groups.c
 * code/{drive,contact,road,path,geometry,meshpack,visibility}.c -lm -o check-groups
 * check-groups drive.dnd course0.dnc course0.dnr course0-route0.dnp world.dnm scenery.dnv
 * No hardware access; actual native fixed ticks and target logical projection. */
#undef NDEBUG
#include "../include/pose.h"
#include "../include/lighting.h"
#include "../include/visibility.h"
#include "practice_script.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

enum { CANDIDATES=10 };
/* Count real submitted world triangles by clipping work, after facing/material
 * and prepared-group rejection. Host-only evidence, not a new render path. */
static uint64_t clip_paths[4], side_output_triangles, side_guardband;
static float side_max_screen;
/* Host-only inventory of per-plane inside proofs using the same prepared
 * coefficients/error envelope; target per-vertex masks did not improve timing. */
static unsigned uncertain_planes(const nm_bounds_planes_t *planes,const float bounds[6]) {
    if(!planes->valid) return 31;
    unsigned mask=0;
    for(unsigned p=0;p<5;++p) {
        float minimum=planes->plane[p].e[3],error=planes->error[p].e[3];
        for(unsigned a=0;a<3;++a) {
            minimum+=planes->plane[p].e[a]*bounds[a+(planes->plane[p].e[a]<0 ? 3 : 0)];
            error+=planes->error[p].e[a]*fmaxf(fabsf(bounds[a]),fabsf(bounds[a+3]));
        }
        if(!isfinite(minimum) || !isfinite(error) || minimum<=error) mask|=1u<<p;
    }
    return mask;
}
static const unsigned sizes[CANDIDATES]={16,32,64,128,256,16,32,64,128,256};
typedef struct { unsigned first,count; float bounds[6]; } group_t;
/* Host prototype: contiguous preorder nodes, one32B record per node. Leaves
 * refer to existing spatial groups; no face reorder or duplicated geometry.
 * ponytail: balanced existing order, not SAH; compare counts before exporting. */
typedef struct { float bounds[6]; uint32_t first,count; } bvh_node_t;
_Static_assert(sizeof(bvh_node_t)==32,"one cache line per BVH node");
static void bvh_build(bvh_node_t *node,const nm_pack_t *pack,unsigned first,unsigned count) {
    assert(count);
    node->first=first;node->count=count;
    memcpy(node->bounds,nm_pack_group_bounds(pack,first),sizeof(node->bounds));
    for(unsigned g=first+1;g<first+count;++g) for(unsigned a=0;a<3;++a) {
        const float *bounds=nm_pack_group_bounds(pack,g);
        node->bounds[a]=fminf(node->bounds[a],bounds[a]);
        node->bounds[a+3]=fmaxf(node->bounds[a+3],bounds[a+3]);
    }
    if(count>1) {
        const unsigned left=count/2;
        bvh_build(node+1,pack,first,left);
        bvh_build(node+2*left,pack,first+left,count-left);
    }
}
static uint64_t bvh_tests,flat_group_tests,bvh_faces,bvh_inside;
static void bvh_classify(const bvh_node_t *node,const nm_bounds_planes_t *planes,uint8_t *classes) {
    ++bvh_tests;
    const int result=nm_bounds_planes_classify(planes,node->bounds);
    if(result!=1 || node->count==1) {
        memset(classes+node->first,result,node->count);
        return;
    }
    bvh_classify(node+1,planes,classes);
    bvh_classify(node+2*(node->count/2),planes,classes);
}
typedef struct {
    group_t *groups;
    unsigned *offsets,*order,count;
    uint64_t tested,rejected,faces,vertices;
} candidate_t;

typedef struct { float center[3];unsigned face; } face_center_t;
/* Host-only qsort context. No target state or parallel invocation. */
static unsigned sort_axis;
static int compare_centers(const void *aa,const void *bb) {
    const face_center_t *a=aa,*b=bb;
    if(a->center[sort_axis]!=b->center[sort_axis]) return a->center[sort_axis]<b->center[sort_axis] ? -1 : 1;
    return (a->face>b->face)-(a->face<b->face);
}
static void spatial_order(face_center_t *faces,unsigned count,unsigned size) {
    if(count<=size) return;
    float low[3],high[3];memcpy(low,faces->center,sizeof(low));memcpy(high,low,sizeof(high));
    for(unsigned f=1;f<count;++f) for(unsigned a=0;a<3;++a) {
        if(faces[f].center[a]<low[a]) low[a]=faces[f].center[a];
        if(faces[f].center[a]>high[a]) high[a]=faces[f].center[a];
    }
    sort_axis=0;
    for(unsigned a=1;a<3;++a) if(high[a]-low[a]>high[sort_axis]-low[sort_axis]) sort_axis=a;
    qsort(faces,count,sizeof(*faces),compare_centers);
    /* Split on a group boundary: each final contiguous block has <=size faces. */
    const unsigned half=((count/size+1)/2)*size;
    assert(half && half<count);
    spatial_order(faces,half,size);spatial_order(faces+half,count-half,size);
}

static void prepare(candidate_t *c,const nm_pack_t *pack,unsigned size,int spatial) {
    c->offsets=calloc(pack->header->meshes+1,sizeof(*c->offsets));assert(c->offsets);
    for(unsigned m=0;m<pack->header->meshes;++m)
        c->offsets[m+1]=c->offsets[m]+(pack->meshes[m].face_count+size-1)/size;
    c->count=c->offsets[pack->header->meshes];
    c->groups=calloc(c->count,sizeof(*c->groups));assert(c->groups);
    c->order=calloc(pack->header->faces,sizeof(*c->order));assert(c->order);
    for(unsigned m=0;m<pack->header->meshes;++m) {
        const nm_mesh_t *mesh=pack->meshes+m;
        face_center_t *centers=calloc(mesh->face_count,sizeof(*centers));assert(centers);
        for(unsigned f=0;f<mesh->face_count;++f) {
            centers[f].face=f;
            const nm_face_t *face=pack->faces+mesh->face_first+f;
            for(unsigned v=0;v<face->count;++v) for(unsigned a=0;a<3;++a)
                centers[f].center[a]+=pack->vertices[mesh->vertex_first+face->indices[v]].position[a]/face->count;
        }
        if(spatial) spatial_order(centers,mesh->face_count,size);
        unsigned char *seen=calloc(mesh->face_count,1);assert(seen);
        for(unsigned f=0;f<mesh->face_count;++f) {
            assert(centers[f].face<mesh->face_count && !seen[centers[f].face]);
            seen[centers[f].face]=1;c->order[mesh->face_first+f]=centers[f].face;
        }
        free(seen);
        free(centers);
        for(unsigned g=c->offsets[m];g<c->offsets[m+1];++g) {
            group_t *out=c->groups+g;
            out->first=(g-c->offsets[m])*size;
            out->count=mesh->face_count-out->first;
            if(out->count>size) out->count=size;
            for(unsigned a=0;a<3;++a) {out->bounds[a]=INFINITY;out->bounds[a+3]=-INFINITY;}
            for(unsigned f=out->first;f<out->first+out->count;++f) {
                const nm_face_t *face=pack->faces+mesh->face_first+c->order[mesh->face_first+f];
                for(unsigned v=0;v<face->count;++v) {
                    const float *p=pack->vertices[mesh->vertex_first+face->indices[v]].position;
                    for(unsigned a=0;a<3;++a) {
                        if(p[a]<out->bounds[a]) out->bounds[a]=p[a];
                        if(p[a]>out->bounds[a+3]) out->bounds[a+3]=p[a];
                    }
                }
            }
        }
    }
}

int main(int argc,char **argv) {
    assert(argc==7);
    nm_drive_params_t *math=nm_drive_load(argv[1]);
    nm_contact_pack_t contact={0};nm_road_pack_t road={0};nm_path_pack_t path={0};
    nm_pack_t pack={0};nm_scenery_pack_t scenery={0};
    assert(math && nm_contact_load(&contact,argv[2]) && nm_road_load(&road,argv[3]) &&
        nm_path_load(&path,argv[4]) && nm_pack_load(&pack,argv[5]) && nm_scenery_load(&scenery,argv[6]));
    assert(scenery.instances==pack.header->instances && scenery.slot==0);
    assert(nm_pack_has_groups(&pack));
    bvh_node_t *tree=calloc(2*pack.header->reserved,sizeof(*tree));
    unsigned *roots=calloc(pack.header->meshes,sizeof(*roots));
    uint8_t *tree_classes=calloc(pack.header->reserved,1);
    assert(tree && roots && tree_classes);
    unsigned node_count=0;
    for(unsigned m=0;m<pack.header->meshes;++m) {
        const nm_mesh_t *mesh=pack.meshes+m;
        assert(mesh->padding[1]);roots[m]=node_count;
        bvh_build(tree+node_count,&pack,mesh->padding[0],mesh->padding[1]);
        node_count+=2*mesh->padding[1]-1;
    }
    assert(node_count<=2*pack.header->reserved);
    candidate_t candidates[CANDIDATES]={0};
    for(unsigned c=0;c<CANDIDATES;++c) prepare(candidates+c,&pack,sizes[c],c>=5);
    nm_road_marks_t marks={.count=road.profile_count-1};marks.values=calloc(marks.count,1);assert(marks.values);
    nm_practice_t state={0};assert(nm_practice_reset(&state,math,&contact,&road,&path,&marks));
    /* Mirrors practice.instance plus instance_view(), not the older 640x480
     * host practice trace. Scenery is authored world-space: use follow view. */
    const nm_view_t camera={{248,176},{0,496,0,384},0.01f};
    const float focus[2]={310,320};
    uint64_t faces=0,vertices=0,visible=0,instances=0;
    uint64_t light_faces=0,light_normals=0,light_points=0,shared_points=0,draw_points=0;
    uint64_t prepared_faces=0,prepared_inside=0;
    uint64_t plane_vertices=0,plane_tests_current=0,plane_tests_masked=0;
    uint64_t plane_histogram[6]={0};
    uint64_t op_points=0,pt_points=0,both_lists=0;
    unsigned frame_cache_peak=0,frame_light_peak=0;
    /* Host-only evidence for offline normal sharing. Bitwise keys preserve
     * signed zero; reset demand per instance, never share across poses. */
    unsigned *normal_refs=calloc(pack.header->faces,sizeof(*normal_refs));assert(normal_refs);
    for(unsigned m=0;m<pack.header->meshes;++m) {
        const nm_mesh_t *mesh=pack.meshes+m;
        /* ponytail: quadratic offline scan; sort exact keys if export size grows. */
        for(unsigned f=0;f<mesh->face_count;++f) {
            const unsigned index=mesh->face_first+f;normal_refs[index]=f;
            for(unsigned p=0;p<f;++p)
                if(!memcmp(pack.faces[index].normal,pack.faces[mesh->face_first+p].normal,12)) {
                    normal_refs[index]=p;break;
                }
            assert(!memcmp(pack.faces[index].normal,
                pack.faces[mesh->face_first+normal_refs[index]].normal,12));
        }
    }
    for(unsigned tick=0;tick<NM_PRACTICE_SCRIPT_TICKS;++tick) {
        unsigned frame_cache=0,frame_lights=0;
        assert(nm_practice_script_advance(&state,math,&contact,&road,&path,&marks,NM_PRACTICE_TICK_US));
        float view[12],origin[2],light[3];int16_t heading;uint16_t cell;
        nm_scenery_cells_t cells;
        assert(nm_follow_view(math,&state.car,view,&heading) &&
            nm_follow_scenery_origin(math,&state.car,heading,origin,&cell) &&
            nm_scenery_select(&scenery,math,cell,(uint16_t)state.car.phase[0],origin[0],origin[1],heading,&cells));
        for(unsigned a=0;a<3;++a)
            light[a]=nm_race_world_light[0]*view[a]+nm_race_world_light[1]*view[3+a]+nm_race_world_light[2]*view[6+a];
        nm_bounds_planes_t prepared;
        nm_bounds_planes_prepare(&prepared,&camera,view,focus);
        assert(prepared.valid);
        for(unsigned ci=0;ci<cells.draw_count;++ci) {
          const nm_scenery_cell_t *cell=scenery.cells+cells.draw[ci];
          for(unsigned i=0;i<cell->count;++i) {
            const unsigned mi=pack.instances[cell->first+i].mesh;
            const nm_mesh_t *mesh=pack.meshes+mi;
            if(!nm_bounds_visible(&camera,mesh->bounds,view,focus)) continue;
            const int prepared_instance=nm_bounds_planes_classify(&prepared,mesh->bounds);
            if(prepared_instance==1) {
                flat_group_tests+=mesh->padding[1];
                bvh_classify(tree+roots[mi],&prepared,tree_classes);
            } else memset(tree_classes+mesh->padding[0],prepared_instance,mesh->padding[1]);
            /* Cost of retaining every selected instance's projected cache
             * until PT, instead of the existing one-mesh reusable scratch. */
            frame_cache+=mesh->vertex_count*sizeof(nm_cached_vertex_t);
            frame_lights+=mesh->face_count*sizeof(int16_t);
            assert(mesh->mode<=1);++instances;faces+=mesh->face_count;
            uint16_t *keep=calloc(mesh->face_count,sizeof(*keep));
            uint16_t *needed=calloc(mesh->vertex_count,sizeof(*needed));
            float (*points)[3]=calloc(mesh->vertex_count,sizeof(*points));
            nm_cached_vertex_t *cache=calloc(mesh->vertex_count,sizeof(*cache));
            uint8_t *plane_demand=malloc(mesh->vertex_count);
            assert(keep && needed && points && cache && plane_demand);
            memset(plane_demand,31,mesh->vertex_count);
            unsigned char *normal_seen=calloc(mesh->face_count,1);
            unsigned char *point_seen=calloc(mesh->vertex_count,1);
            assert(normal_seen && point_seen);
            for(unsigned c=0;c<CANDIDATES;++c) {
                candidate_t *test=candidates+c;
                for(unsigned g=test->offsets[mi];g<test->offsets[mi+1];++g) {
                    const group_t *group=test->groups+g;++test->tested;
                    if(!nm_bounds_visible(&camera,group->bounds,view,focus)) {++test->rejected;continue;}
                    test->faces+=group->count;
                    for(unsigned f=group->first;f<group->first+group->count;++f)
                        keep[test->order[mesh->face_first+f]]|=1u<<c;
                }
            }
            for(unsigned v=0;v<mesh->vertex_count;++v) {
                const nm_vertex_t *vertex=pack.vertices+mesh->vertex_first+v;const float *p=vertex->position;
                for(unsigned a=0;a<3;++a) points[v][a]=p[0]*view[a]+p[1]*view[3+a]+p[2]*view[6+a]+view[9+a];
                const nm_view_vertex_t raw={points[v][0]*focus[0],points[v][1]*focus[1],points[v][2],vertex->u,vertex->v};
                nm_cache_vertex(&camera,&raw,cache+v);
            }
            /* Only the rejection mask uses spatial order. Submission would
             * still visit the original faces, with original winding and UVs. */
            for(unsigned f=0;f<mesh->face_count;++f) {
                const nm_face_t *face=pack.faces+mesh->face_first+f;
                const int packed_visible=!nm_pack_has_groups(&pack) || nm_bounds_visible(&camera,
                    nm_pack_group_bounds(&pack,face->group),view,focus);
                const int prepared_class=prepared_instance ? (nm_pack_has_groups(&pack) ?
                    nm_bounds_planes_classify(&prepared,nm_pack_group_bounds(&pack,face->group)) : prepared_instance) : 0;
                prepared_faces+=prepared_class!=0;
                prepared_inside+=prepared_class==2;
                const unsigned tree_class=tree_classes[face->group];
                bvh_faces+=tree_class!=0;bvh_inside+=tree_class==2;
                if(tree_class==2) for(unsigned v=0;v<face->count;++v)
                    assert(nm_cached_outside(cache+face->indices[v])==0);
                if(prepared_class==2) for(unsigned v=0;v<face->count;++v)
                    assert(nm_cached_outside(cache+face->indices[v])==0);
                if(packed_visible) {
                    ++light_faces;
                    const unsigned n=normal_refs[mesh->face_first+f];
                    if(!normal_seen[n]) {normal_seen[n]=1;++light_normals;}
                    const unsigned p=face->indices[2];
                    const unsigned ref=pack.position_refs ? pack.position_refs[mesh->vertex_first+p] : p;
                    if(!(point_seen[ref]&1)) {point_seen[ref]|=1;++light_points;}
                }
                for(unsigned t=1;t+1<face->count;++t) {
                    const nm_cached_vertex_t *input[3]={cache+face->indices[0],cache+face->indices[t],cache+face->indices[t+1]};
                    nm_screen_vertex_t output[NM_CLIP_VERTICES];
                    const unsigned count=nm_project_cached_triangle(&camera,input,output);
                    if(count) {assert(tree_class && prepared_class && packed_visible && keep[f]==(1u<<CANDIDATES)-1);++visible;}
                }
                const nm_material_t *material=pack.materials+face->material;
                float normal[3],face_points[3][3];
                for(unsigned a=0;a<3;++a) {
                    normal[a]=face->normal[0]*view[a]+face->normal[1]*view[3+a]+face->normal[2]*view[6+a];
                    const float *source=pack.faces[mesh->face_first+normal_refs[mesh->face_first+f]].normal;
                    const float shared=source[0]*view[a]+source[1]*view[3+a]+source[2]*view[6+a];
                    assert(!memcmp(&shared,normal+a,sizeof(shared)));
                    for(unsigned v=0;v<3;++v) face_points[v][a]=points[face->indices[v]][a];
                }
                const int luma=nm_face_lighting(mesh->mode,normal,face_points,light,material);assert(luma>=0);
                if((!(face->attribute&(1u<<17)) && (luma&256)) ||
                    !(face->attribute&(3u<<8)) || (material->header[0]&0x8000)) continue;
                if(prepared_class) {
                    const unsigned mask=uncertain_planes(&prepared,nm_pack_has_groups(&pack) ?
                        nm_pack_group_bounds(&pack,face->group) : mesh->bounds);
                    for(unsigned v=0;v<face->count;++v) {
                        const unsigned index=face->indices[v];
                        const unsigned rep=pack.position_refs ? pack.position_refs[mesh->vertex_first+index] : index;
                        /* Any containing group may prove a shared XYZ inside
                         * a plane. AND unresolved masks; retain old full-inside
                         * proof separately (bit5), demanded in bit6. */
                        plane_demand[rep]=(plane_demand[rep]&mask) | 64 |
                            (plane_demand[rep]&32) | (prepared_class==2 ? 32 : 0);
                        assert(!(nm_cached_outside(cache+index)&~mask));
                    }
                }
                if(prepared_class) for(unsigned t=1;t+1<face->count;++t) {
                    const nm_cached_vertex_t *input[3]={cache+face->indices[0],cache+face->indices[t],cache+face->indices[t+1]};
                    nm_screen_vertex_t output[NM_CLIP_VERTICES];
                    const unsigned count=nm_project_cached_triangle(&camera,input,output);
                    const unsigned mask=nm_cached_outside(input[0])|nm_cached_outside(input[1])|nm_cached_outside(input[2]);
                    ++clip_paths[!count ? 0 : !mask ? 1 : (mask&1) ? 2 : 3];
                    if(count && mask && !(mask&1)) {
                        float maximum=0;
                        for(unsigned v=0;v<3;++v) {
                            const nm_view_vertex_t p=input[v]->view;
                            assert(p.z>=camera.near_z);
                            const float x=(camera.center[0]+p.x/p.z)*(640.0f/496.0f);
                            const float y=(camera.center[1]-p.y/p.z)*(480.0f/384.0f);
                            maximum=fmaxf(maximum,fmaxf(fabsf(x),fabsf(y)));
                        }
                        assert(isfinite(maximum));
                        side_max_screen=fmaxf(side_max_screen,maximum);
                        side_guardband+=maximum<=2048;
                        side_output_triangles+=count-2;
                    }
                }
                for(unsigned v=0;v<face->count;++v) {
                    const unsigned index=face->indices[v];
                    const unsigned representative=pack.position_refs ? pack.position_refs[mesh->vertex_first+index] : index;
                    needed[representative]|=keep[f]|(1u<<CANDIDATES);
                    if(packed_visible) {
                        point_seen[representative]|=2;
                        point_seen[representative]|=((material->header[0]>>13)&3)==3 ? 8 : 4;
                    }
                }
            }
            for(unsigned v=0;v<mesh->vertex_count;++v) {
                if(plane_demand[v]&64) {
                    const unsigned mask=plane_demand[v]&31;
                    unsigned tests=0;
                    for(unsigned p=0;p<5;++p) tests+=(mask>>p)&1;
                    assert(!(nm_cached_outside(cache+v)&~mask));
                    ++plane_vertices; ++plane_histogram[tests];
                    plane_tests_current+=(plane_demand[v]&32) ? 0 : 5;
                    plane_tests_masked+=tests;
                }
                draw_points+=(point_seen[v]&2)!=0;
                shared_points+=(point_seen[v]&3)==3;
                op_points+=(point_seen[v]&4)!=0;
                pt_points+=(point_seen[v]&8)!=0;
                both_lists+=(point_seen[v]&12)==12;
                vertices+=(needed[v]>>CANDIDATES)&1;
                for(unsigned c=0;c<CANDIDATES;++c) candidates[c].vertices+=(needed[v]>>c)&1;
            }
            free(normal_seen);free(point_seen);
            free(plane_demand);free(cache);free(points);free(needed);free(keep);
          }
        }
        if(frame_cache>frame_cache_peak) frame_cache_peak=frame_cache;
        if(frame_lights>frame_light_peak) frame_light_peak=frame_lights;
    }
    assert(nm_practice_script_digest(&state)==0xcf4dadbc);
    printf("BVH nodes=%u bytes=%zu flat-group-tests=%llu tree-node-tests=%llu retained-faces=%llu inside-faces=%llu (host counts, NOT target timing)\n",
        node_count,node_count*sizeof(*tree), (unsigned long long)flat_group_tests,
        (unsigned long long)bvh_tests,(unsigned long long)bvh_faces,(unsigned long long)bvh_inside);
    free(tree_classes);free(roots);free(tree);
    printf("PLANE unique demanded world XYZ=%llu current-tests=%llu masked-tests=%llu histogram0..5=",
        (unsigned long long)plane_vertices,(unsigned long long)plane_tests_current,
        (unsigned long long)plane_tests_masked);
    for(unsigned p=0;p<6;++p) printf(" %llu",(unsigned long long)plane_histogram[p]);
    puts(" (host opportunity estimate, not target timing; per-plane proofs checked)");
    printf("CLIP prepared/drawable world: rejected=%llu inside=%llu near=%llu side-only=%llu; side-output=%llu within-2048px=%llu max-unclipped-pixel=%g\n",
        (unsigned long long)clip_paths[0],(unsigned long long)clip_paths[1],
        (unsigned long long)clip_paths[2],(unsigned long long)clip_paths[3],
        (unsigned long long)side_output_triangles,(unsigned long long)side_guardband,(double)side_max_screen);
    printf("Prepared-plane world faces retained=%llu inside=%llu; no false rejection/inside classification\n",
        (unsigned long long)prepared_faces,(unsigned long long)prepared_inside);
    if(nm_pack_has_groups(&pack)) printf("PASS: %u loaded DNM3/4 groups retain every visible triangle\n",pack.header->reserved);
    printf("PASS: 2400 fixed ticks cf4dadbc; %llu selected instances, %llu visible source triangles retained by EVERY candidate\n",
        (unsigned long long)instances,(unsigned long long)visible);
    printf("BASE faces=%llu unique_position_transforms=%llu\n",(unsigned long long)faces,(unsigned long long)vertices);
    assert(light_normals<=light_faces && light_points<=light_faces && shared_points<=light_points && shared_points<=draw_points);
    assert(op_points+pt_points==draw_points+both_lists);
    printf("LISTS loaded-groups OP_positions=%llu PT_positions=%llu shared=%llu; per-instance projected-cache peak=%u light-cache peak=%u bytes (world only, sampled route)\n",
        (unsigned long long)op_points,(unsigned long long)pt_points,(unsigned long long)both_lists,
        frame_cache_peak,frame_light_peak);
    printf("LIGHTING loaded-groups faces=%llu unique_normals=%llu unique_p2=%llu draw_positions=%llu shared_p2_draw=%llu (transform counts, NOT timing)\n",
        (unsigned long long)light_faces,(unsigned long long)light_normals,(unsigned long long)light_points,
        (unsigned long long)draw_points,(unsigned long long)shared_points);
    free(normal_refs);
    for(unsigned c=0;c<CANDIDATES;++c) {
        const candidate_t *test=candidates+c;
        printf("GROUP policy=%s size=%u count=%u bounds_bytes=%zu mapping_bytes=%zu tests=%llu rejected=%llu retained_faces=%llu retained_transforms=%llu\n",
            c>=5 ? "spatial" : "source",sizes[c],test->count,
            test->count*sizeof(group_t)+(pack.header->meshes+1)*sizeof(unsigned),
            c>=5 ? pack.header->faces*sizeof(unsigned) : 0,
            (unsigned long long)test->tested,(unsigned long long)test->rejected,
            (unsigned long long)test->faces,(unsigned long long)test->vertices);
        assert(test->faces<=faces && test->vertices<=vertices && test->rejected);
        free(test->groups);free(test->offsets);free(test->order);
    }
    free(marks.values);free(math);nm_contact_unload(&contact);nm_road_unload(&road);
    nm_path_unload(&path);nm_pack_unload(&pack);nm_scenery_unload(&scenery);
    return 0;
}
