"""Host submission simulator and independent stream check.

Builds the real submission path (sq_prepare_batch / sq_emit_batches / sq_queue_objects of
include/render/models/model_submission.h with the real scenery stage selection, mesh and
material loaders, vertex cache and clipper) for the host. It is driven by frame tables
(per tick: car instances, world instances, view, car part boundaries; gen_frames.py writes
them), captures every word handed to the PVR and prints per frame digests of the stream.

SIM_ORACLE=1 at run time compares every frame, triangle by triangle, with the reference
renderer tests/reference_stream.h: the independent check that replaced the console's
startup stream oracle. SIM_MUTATE=n at build time makes a deliberately wrong renderer,
to show that the comparison notices.

usage: build_sim.py OUTDIR NAME [-D...]     (repository root or anywhere; needs cc and zlib)
       SIM_STAGE=0..5 [SIM_ORACLE=1] OUTDIR/NAME ASSETS_DIR FRAMES.frm OUT.txt
Generated files (tagged/, sim_parts.h, sim_light.h, the binary) go to OUTDIR.
"""
import subprocess,sys,os
from pathlib import Path
here=Path(__file__).resolve().parent
root=Path(os.environ.get('SIM_ROOT',here.parents[1])); out=Path(sys.argv[1]); name=sys.argv[2]; defs=sys.argv[3:]
out.mkdir(parents=True,exist_ok=True)
# The real submission header, unchanged unless SIM_MUTATE is set: every site that writes a vertex
# names its face and binding through SQ_VERTEX_TAG, which sim_main.c defines.
text=(root/'include/render/models/model_submission.h').read_text()
assert text.count('SQ_VERTEX_TAG(')>=6
# SIM_MUTATE=n (environment): build a deliberately wrong renderer, to show that the reference comparison notices.
MUTANTS={
 '1':('if(layer) depth_scale*=draw_layer_scale[layer]; /* see sq_emit_range */',''),                       # decal layers lose their depth factor
 '2':('    if(side<0) return baked.back==UINT16_MAX ? -1 : baked.back;','    if(side<0) return baked.front;'),  # rear side drawn with the front binding
 '3':('out->uv[v][0]=vertex->u; out->uv[v][1]=vertex->v;','out->uv[v][0]=pack->vertices[mesh->vertex_first+representative].u; out->uv[v][1]=pack->vertices[mesh->vertex_first+representative].v;'),  # a shared position keeps its representative's UV
 '4':('     * zero. */\n    return nm_float_positive(approximate-margin) ? 1 : nm_float_negative(approximate+margin) ? -1 : 0;','     * zero. */\n    return nm_float_positive(approximate-margin) ? 1 : nm_float_negative(approximate+margin) ? 1 : 0;'),      # a face decided rear is treated as front
 # level of detail (needs a world pack with NM_PACK_LOD_GROUPS)
 '6':('f=lod->first+(level ? lod->count[0] : 0u)+(level>1 ? lod->count[1] : 0u);\n            stop=f+lod->count[level]; in_group=1;',
      '{ const unsigned wrong=level ? level-1u : 1u; f=lod->first+(wrong ? lod->count[0] : 0u)+(wrong>1 ? lod->count[1] : 0u); stop=f+lod->count[wrong]; } in_group=1;'),  # the range of another level is drawn
 '7':('if(level<NM_LOD_LEVELS-1 && far_side>group->depth[level]) {','if(0) {'),  # a group never leaves its level for a farther one
 '8':('const float far_side=scaled*(1.0f/NM_LOD_HYSTERESIS),near_side=scaled*NM_LOD_HYSTERESIS;','const float far_side=scaled*1.25f,near_side=scaled*1.25f;'),  # switches a quarter too near, no hysteresis
 '5':('if(entry->stamp[rear]==light_stamp) { light=entry->light[rear]; selected=entry->binding[rear]; }','if(entry->stamp[rear]==light_stamp) { light=entry->light[rear]; selected=entry->binding[rear]^1u; }'),  # a reused class result is one binding off
 # prebuilt world streams (model_submission.h, NM_SQ_STREAM; 1, 2 and 6 above are their queue-path counterparts)
 '9':('            if(face->layer) depth*=draw_layer_scale[face->layer];\n            out->depth=depth;','            out->depth=depth;'),  # a stream face loses its layer's depth factor
 '10':('    *binding=face->back; *argb=record->argb; *selected=record->group->header;\n','    (void)record;\n'),  # the rear side of a two-luma face drawn with the front binding
 '11':('            const unsigned level=sq_lod_select(lod_first+i,row,scale);','            const unsigned chosen=sq_lod_select(lod_first+i,row,scale),level=chosen ? chosen-1u : 1u;'),  # the range of another level is drawn (and transformed)
 '12':('                out->uv[k][0]=((float)emit->uv[order[k]][0]-origin_u)*scale_u;\n                out->uv[k][1]=((float)emit->uv[order[k]][1]-origin_v)*scale_v;','                out->uv[k][0]=((float)emit->uv[k][0]-origin_u)*scale_u;\n                out->uv[k][1]=((float)emit->uv[k][1]-origin_v)*scale_v;'),  # corrupted prebuilt record: u,v of another corner
 '13':('out->corner[k]=emit->corner[order[k]];','out->corner[k]=emit->corner[k];'),  # corrupted prebuilt record: another corner's position
 '14':('                out->uv[0]=((float)in->uv[0]-mapping->origin[0])*mapping->scale[0];\n                out->uv[1]=((float)in->uv[1]-mapping->origin[1])*mapping->scale[1];','                out->uv[0]=((float)in->uv[1]-mapping->origin[0])*mapping->scale[0];\n                out->uv[1]=((float)in->uv[0]-mapping->origin[1])*mapping->scale[1];'),  # corrupted prebuilt strip vertex: u and v exchanged
 # strip runs from the strip vertex records (sq_stream_run's record colours, prepare_restrip.py packs)
 '15':('out->argb=records[sq_baked_faces[strip->face_first+member].front].argb;',
       'out->argb=records[sq_baked_faces[strip->face_first+(member+1u<strip->face_count ? member+1u : 0u)].front].argb;'),  # a strip vertex record holds another face's colour
 '16':('                const unsigned index=pack->strip_indices[strip->vertex_first+v];',
       '                const unsigned index=pack->strip_indices[strip->vertex_first+(v+1u<triangles+2u ? v : v-2u)];'),  # corrupted strip: its last vertex is its third-last
 '17':('table[strip->face_first+f].special=(uint16_t)reach;','table[strip->face_first+f].special=(uint16_t)(reach+1u);'),  # corrupted strip table: a run may take the face after its strip
 # prebuilt car streams (model_submission.h, NM_SQ_CAR_STREAM; 5 above is the queue path's light class mutant)
 '18':('out->binding=(uint16_t)selected; out->argb=record->argb; out->group=record->group;',
       'out->binding=(uint16_t)selected; out->argb=sq_emit_records[0][selected^1u].argb; out->group=record->group;'),  # a resolved light class holds another binding's colour
 '19':('        if(side<0 && !(lit->flags&SQ_LIT_TWO_SIDED)) { SQ_COUNT(draw_rear,1); SQ_CAR_COUNT(SQ_CAR_CULLED,1); continue; }',
       '        if(side>0 && !(lit->flags&SQ_LIT_TWO_SIDED)) { SQ_COUNT(draw_rear,1); SQ_CAR_COUNT(SQ_CAR_CULLED,1); continue; }'),  # the wrong side culled in the hot loop
 '20':('    for(unsigned v=low;v<=high && low!=UINT_MAX;++v) if(seen[v]) { if(out) out[count]=(uint16_t)v; ++count; seen[v]=0; }',
       '    for(unsigned v=low;v<=high && low!=UINT_MAX;++v) if(seen[v]) { if(out) out[count]=(uint16_t)(v==high ? low : v); ++count; seen[v]=0; }'),  # corrupted position list: a mesh\'s last position is never transformed
 '21':('            if(flags&SQ_LIT_CUTOUT) { if(cutouts==mesh->face_count) cutouts=f; }',
       '            if(flags&SQ_LIT_CUTOUT) { if(cutouts==mesh->face_count) cutouts=f+1u; }'),  # corrupted list split: a punch-through face drawn on the opaque list
 '22':('if(layer) depth_scale*=draw_layer_scale[layer]; /* as sq_emit_face_loop (see sq_emit_range) */',''),  # a car stream face loses its layer's depth factor (1 and 9: the queue path's and the world stream's)
 # the SH4ZAM forms (model_submission.h, NM_SH4ZAM_GFX): the reference's tolerances must still see these
 '23':('    const float shown=rear ? -dotl : dotl;','    const float shown=dotl;'),  # a class lit at the rear as if from the front
 '24':('    return (int)luminance|(rear ? 256 : 0);\n}','    return (luminance+2>255 ? 255 : (int)luminance+2)|(rear ? 256 : 0);\n}'),  # a class light two luma steps high
 '25':('                if(dn>df) { from=in[i+1u]; to=in[i]; const float swap=dn; dn=df; df=swap; }','                if(dn>df) { const float swap=dn; dn=df; df=swap; }'),  # a cut interpolated from the wrong endpoint
 '26':('                p->u=sq_lerpf(from->u,to->u,t); p->v=sq_lerpf(from->v,to->v,t);','                p->u=sq_lerpf(from->u,to->u,t); p->v=sq_lerpf(from->v,to->v,t*1.01f);'),  # a cut's v one per cent off along its edge
 '27':('        o->x=x; o->y=y; o->q=q; sq_copy_words(&o->u,&p->u,2);','        o->x=x; o->y=y; o->q=q*1.001f; sq_copy_words(&o->u,&p->u,2);'),  # a cut's depth a thousandth too near
 # catch-fence sweep (model_submission.h, NM_FENCE_SWEEP: build with -DNM_FENCE_SWEEP=1, the default is 0; needs a world
 # pack with a fence rule's stipple groups, assets-ssfar4cf3 .. assets-ssfar4eu)
 '29':('    return sq_sweep_plane[k]*(focus*(1.0f/NM_LOD_REFERENCE_FOCUS));','    return sq_sweep_plane[k]*1.01f*(focus*(1.0f/NM_LOD_REFERENCE_FOCUS));'),  # a panel split one per cent beyond its plane
 '30':('    if(!close) return beyond;','    return beyond;'),  # whole-panel switching by the centre: a panel across a plane is not split
 '31':('            p->u=sq_lerpf(from->u,to->u,t); p->v=sq_lerpf(from->v,to->v,t);\n            src_out','            p->u=sq_lerpf(from->u,to->u,t); p->v=sq_lerpf(from->v,to->v,t*1.01f);\n            src_out'),  # a band cut's v one per cent off along its edge
 # translucent effects (model_submission.h, "Translucent effects": the car effects' smoke on the translucent list)
 '32':('            while(at && order[at-1].key<nearest) { order[at]=order[at-1]; --at; }','            while(at && order[at-1].key>nearest) { order[at]=order[at-1]; --at; }'),  # sorted front to back
 '33':('            const unsigned binding=sq_lookup(draw_bindings,draw_runs,face->material,(unsigned)light&255u);','            const unsigned binding=sq_lookup(draw_bindings,draw_runs,face->material,(unsigned)light&255u)^1u;'),  # a smoke face drawn with another binding
 # make NATIVE_SMOKE_MODE=pt-screen (build with -DNM_SMOKE_MODE=2): the screen checker's u and the face's one depth
 '34':('out->u=out->u*0.5f+0.5f*(out->x+out->y)-sq_door_base;','out->u=out->u*0.5f+0.5f*(out->x+out->y)+0.25f-sq_door_base;'),  # the checker a quarter texture off (the shape half read across the halves)
 '35':('        sq_door_q=1.0f/(o->key>batch->view.near_z ? o->key : batch->view.near_z);','        sq_door_q=1.001f/(o->key>batch->view.near_z ? o->key : batch->view.near_z);'),  # the face's depth a thousandth nearer
 # make NATIVE_SMOKE_TIGHT=1 (build with -DNM_SMOKE_TIGHT=1 and NM_SMOKE_MODE 2 or 3): a piece's u' and a piece's corner
 '36':('out->u=0.5f*out->u+0.5f*(out->x+out->y)-sq_door_base;','out->u=0.5f*out->u+0.5f*(out->x+out->y)+0.25f-sq_door_base;'),  # a piece's checker a quarter texture off
 '37':('const float a=piece->a[r&1u],b=piece->b[r>>1];','const float a=piece->a[r&1u]*1.02f,b=piece->b[r>>1];'),  # a piece two per cent too wide along s (texels it should not show, or a gap)
 '28':('                if(plane==1 || plane==2) x=(view->bounds[plane-1u]-cx)*z;','                if(plane==1 || plane==2) x=(view->bounds[plane-1u]-cx)*z*1.0002f;'),  # a side cut set beyond its plane by 2e-4 of its depth
 # world objects super-sampled by instance (model_submission.h, sq_ss_select; needs a world pack whose materials carry
 # a switch depth, assets-ssfar4f since 17 October: the slot machine's reels)
 '38':('    if(level==SQ_SS_PLAIN && beyond>depth) level=SQ_SS_ON;','    if(0) level=SQ_SS_ON;'),  # never super-sampled
 '39':('    const float beyond=scaled*(1.0f/NM_LOD_HYSTERESIS),within=scaled*NM_LOD_HYSTERESIS;','    const float beyond=scaled,within=scaled;'),  # no hysteresis
 '40':('    if(!batch->supersample || (batch->supersample==SQ_SS_ON)==sq_ss_on) return;','    if(1) return;'),  # decided, but the headers never get the bit
 # the road class at another scale (model_submission.h, sq_road_depth_set; check_stream.py's road depth runs)
 '41':('            out[r].depth=layer ? depth*draw_layer_scale[layer] : depth;','            out[r].depth=depth;'),  # a rewritten stream record loses its layer's depth factor
 '42':('            out[r].depth=layer ? depth*draw_layer_scale[layer] : depth;','            (void)layer; (void)depth;'),  # the stream records keep the old scale
 '43':('    if(sq_stream_pool_bytes && sq_stream_job.mesh!=UINT_MAX)\n        sq_stream_job_start(&sq_stream_job,sq_stream_job.mesh,sq_stream_job.block);\n',''),  # a half-built mesh of the record cache goes on with the old scale in its first records
 '44':('        if(sq_surface_bindings[b]) classes[records[b].group-classes].depth=scale;','        (void)classes;'),  # the emission classes keep the old scale
}
if os.environ.get('SIM_MUTATE'):
    old,new=MUTANTS[os.environ['SIM_MUTATE']]
    assert text.count(old)==1,old
    text=text.replace(old,new)
(out/'tagged/render/models').mkdir(parents=True,exist_ok=True)
(out/'tagged/render/models/scenery_camera.h').write_text((root/'include/render/models/scenery_camera.h').read_text())
(out/'tagged/render/models/model_submission.h').write_text(text)
renderer=(root/'code/render/models/model_renderer.c').read_text()
math=(root/'include/render/models/model_math.h').read_text()
def cut(s,a,b): return s[s.index(a):s.index(b)]
state=cut(renderer,'static int draw_inspect_strips;','static int compile_draw_header(')
state=state.replace('alignas(32) static pvr_poly_hdr_t','static pvr_poly_hdr_t')
state='\n'.join(l for l in state.splitlines() if not l.startswith('_Static_assert'))+'\n'
vertex=cut(renderer,'#ifdef NATIVE_BOUNDS_VERIFY\nstatic unsigned bounds_verified;','#include "render/models/model_submission.h"')
if vertex.endswith('#ifdef NATIVE_DIRECT_SQ\n'): vertex=vertex[:-len('#ifdef NATIVE_DIRECT_SQ\n')]
light=math[math.index('static inline nm_view_t instance_view'):math.rindex('#endif')]
(out/'sim_parts.h').write_text(state+vertex)
(out/'sim_light.h').write_text(light)
if 'nm_cache_vertex_bounded' in (root/'include/geometry.h').read_text(): defs=['-DSIM_HAS_BOUNDED']+defs
if 'sq_facing_prepare' in text: defs=['-DSIM_HAS_FACING']+defs
if 'sq_emit_records_prepare' in text: defs=['-DSIM_HAS_RECORDS']+defs
if 'sq_emit_expand' in text: defs=['-DSIM_HAS_CLASSES']+defs
if 'sq_material_lists_prepare' in text: defs=['-DSIM_HAS_LISTS']+defs
if 'sq_wide_view' in text: defs=['-DSIM_HAS_WIDE']+defs
if 'sq_stream_prepare' in text: defs=['-DSIM_HAS_STREAM']+defs
if 'sq_car_stream_prepare' in text: defs=['-DSIM_HAS_CAR_STREAM']+defs
if 'sq_ss_prepare' in text: defs=['-DSIM_HAS_SS']+defs
if 'sq_live_prepare' in text: defs=['-DSIM_HAS_LIVE']+defs
cmd=['cc','-std=c11','-O2','-g','-fno-fast-math','-ffp-contract=off','-Wno-unused-function','-Wno-unused-variable',
     '-DNATIVE_DIRECT_SQ','-DENJ_XSCALE=1','-DNM_HOST_BAKED_WORLD',*defs,'-I',str(out/'tagged'),'-I',str(out),'-I',str(here),'-I',str(root/'include'),'-I',str(root/'tests'),str(here/'sim_main.c'),
     *[str(root/'code'/n) for n in ('geometry.c','meshpack.c','material_pack.c','render/models/scenery_camera.c','render/scene/track_surface.c','render/scene/damage_bodies.c')],
     '-lm','-lz','-o',str(out/name)]
subprocess.run(cmd,check=True); print('built',out/name)
