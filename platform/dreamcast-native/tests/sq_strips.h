/* Included by test_sq_quads.py after the actual SQ implementation. */
static unsigned expand_sq_strips(const pvr_vertex_t *captured,unsigned count,
        pvr_vertex_t triangles[64][3]) {
    unsigned first=0,vertices=0,total=0;
    for(unsigned i=0;i<count;++i) {
        if(captured[i].flags<PVR_CMD_VERTEX) { assert(!vertices); continue; }
        assert(captured[i].flags==PVR_CMD_VERTEX || captured[i].flags==PVR_CMD_VERTEX_EOL);
        if(!vertices) first=i;
        ++vertices;
        if(vertices>=3) {
            assert(total<64);
            const unsigned t=vertices-3;
            const unsigned corners[3]={first+t+(t&1),first+t+!(t&1),i};
            for(unsigned v=0;v<3;++v) {
                triangles[total][v]=captured[corners[v]];
                /* Termination belongs to the transport strip, not its triangle. */
                triangles[total][v].flags=PVR_CMD_VERTEX;
            }
            ++total;
        }
        if(captured[i].flags==PVR_CMD_VERTEX_EOL) { assert(vertices>=3); vertices=0; }
    }
    assert(!vertices);
    return total;
}

static void check_offline_strips(FILE *output) {
    nm_vertex_t vertices[8]={
        {{-30,20,1},80,96},{{-30,-20,1.125f},88,112},
        {{-10,20,1.25f},96,128},{{-10,-20,1.5f},104,144},
        {{10,20,1},112,160},{{10,-20,1.75f},120,176},
        {{30,20,1.5f},128,192},{{30,-20,2},136,208}};
    nm_face_t faces[4]={
        {.indices={2,0,1,3},.count=4},{.indices={4,2,3,5},.count=4},
        {.indices={4,5,6},.count=3},{.indices={6,5,7},.count=3}};
    const uint16_t indices[8]={0,1,2,3,4,5,6,7};
    const nm_strip_t strip={.face_first=0,.face_count=4,.vertex_count=8,.vertex_first=0};
    nm_header_t header={.instances=1,.meshes=1,.faces=4};
    nm_mesh_t mesh={.vertex_count=8,.face_count=4};
    nm_pack_t pack={.data=&header,.header=&header,.meshes=&mesh,.vertices=vertices,.faces=faces,.strip_indices=indices};
    nm_texture_record_t texture={.width=64,.height=32};
    nm_material_binding_t bindings[2]={
        {.resource=0,.color=3u|(4u<<16)}, {.resource=0,.color=4u|(4u<<16)}};
    uint32_t modulation[2]={0xff123456,0xffabcdef};
    /* Offline strips are emitted for palette (flat shaded) packs only. */
    const uint8_t palette_index[4]={0,0,0,0};
    nm_material_pack_t materials={.textures=&texture,.bindings=bindings,.modulation=modulation,.palette_index=palette_index};
    nm_draw_material_t cache[2]={ {.header={.cmd=0x80840000,.mode1=7}},
                                {.header={.cmd=0x80840000,.mode1=7}} };
    cache[0].uv=nm_material_uv(&materials,bindings);
    cache[1].uv=nm_material_uv(&materials,bindings+1);
    /* Equal canonical headers must not make different bindings joinable. */
    const pvr_poly_hdr_t *shared[2]={&cache[0].header,&cache[0].header};
    nm_cached_vertex_t points[8];
    uint32_t entries[4];
    nm_sq_batch_t batch={.mesh=&mesh,.view={{248,192},{0,496,0,384},0.01f},
        .points=points,.bindings=entries,.prepared=1,.instance_index=2};
    draw_pack=&pack; draw_materials=&materials; draw_material_cache=cache;
    world_draw.pack=NULL; sq_shared_headers[0]=shared;
    sq_sources[0].batches=&batch; sq_sources[0].count=1;
    sq_failed=0;
    /* Position of each face in its strip, as the renderer prepares it at load. */
    pack.strips=&strip; mesh.padding[3]=1;
    assert(sq_flat_strips_prepare(0,&pack,&materials) && sq_flat_strip_triangle[0]);
    unsigned cases=0;
    /* 0 whole strip; 1..5 one clipped corner; 8 untextured; 10 queue out of strip order. */
    static const unsigned variants[]={0,1,2,3,4,5,8,10};
    for(unsigned pass=0;pass<2;++pass) for(unsigned n=0;n<sizeof(variants)/sizeof(variants[0]);++n) {
        const unsigned variant=variants[n];
        const unsigned list=draw_lists[pass];
        const int joined=variant==0 || variant==8;
        draw_untextured=variant==8;
        for(unsigned v=0;v<8;++v) {
            nm_view_vertex_t point={vertices[v].position[0],vertices[v].position[1],
                vertices[v].position[2],vertices[v].u,vertices[v].v};
            if(v==4) {
                if(variant==1) point.z=-1;
                if(variant==2) point.x=-1000;
                if(variant==3) point.x=1000;
                if(variant==4) point.y=-1000;
                if(variant==5) point.y=1000;
            }
            nm_cache_vertex(&batch.view,&point,points+v);
        }
        if(variant>=1 && variant<=5) assert(nm_cached_outside(points+4));
        unsigned selected=0;
        for(unsigned f=0;f<4;++f) {
            const unsigned ordered=variant==10 && (f==1 || f==2) ? 3-f : f;
            entries[pass ? 3-selected : selected]=ordered<<16;
            ++selected;
        }
        batch.opaque_count=pass ? 0 : selected; batch.cutout_count=pass ? selected : 0;
        pack.strips=NULL; mesh.padding[3]=0;
        reset(); sq_joined_strips=sq_strip_vertices_saved=0;
        assert(sq_emit(list));
        const unsigned baseline_writes=writes,baseline_count=draw_count;
        pvr_vertex_t baseline[128],candidate[128];
        assert(writes<128); memcpy(baseline,stream,writes*sizeof(stream[0]));
        pvr_vertex_t expected[64][3],actual[64][3];
        assert(expand_sq_strips(baseline,writes,expected)==baseline_count);
        pack.strips=&strip; mesh.padding[3]=1;
        reset(); sq_joined_strips=sq_strip_vertices_saved=0;
        assert(sq_emit(list) && draw_count==baseline_count);
        const unsigned candidate_writes=writes;
        assert(writes<128); memcpy(candidate,stream,writes*sizeof(stream[0]));
        assert(draw_opaque==(pass ? 0 : baseline_count) && draw_cutout==(pass ? baseline_count : 0));
        assert(expand_sq_strips(candidate,writes,actual)==baseline_count);
        if(joined) {
            assert(baseline_writes==15 && writes==9 && draw_count==6 && draw_header_count==1);
            assert(sq_joined_strips==1 && sq_strip_vertices_saved==6);
            assert(!memcmp(candidate,baseline,sizeof(pvr_poly_hdr_t)));
            for(unsigned t=0;t<baseline_count;++t) {
                unsigned rotation=0;
                for(;rotation<3;++rotation) {
                    unsigned v=0;
                    for(;v<3;++v) {
                        pvr_vertex_t got=actual[t][v],want=expected[t][(rotation+v)%3];
                        if(memcmp(&got,&want,sizeof(got))) break;
                    }
                    if(v==3) break;
                }
                assert(rotation<3);
            }
            for(unsigned v=1;v<9;++v) {
                assert(candidate[v].flags==(v==8 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX));
                if(draw_untextured) assert(!candidate[v].u && !candidate[v].v && candidate[v].argb==modulation[0]);
            }
        } else {
            assert(!sq_joined_strips && !sq_strip_vertices_saved && writes==baseline_writes);
            assert(!memcmp(candidate,baseline,writes*sizeof(stream[0])));
        }
        reset(); queued=0; sq_joined_strips=sq_strip_vertices_saved=0;
        assert(sq_queue_objects(NULL,0,0) && queued==1 && queue[0].list==list);
        queue[0].call(queue[0].data);
        assert(!sq_failed && writes==candidate_writes && draw_count==baseline_count);
        assert(!memcmp(stream,candidate,writes*sizeof(stream[0])));
        assert(sq_joined_strips==(unsigned)joined && sq_strip_vertices_saved==(joined ? 6u : 0u));
        assert(fwrite(&writes,sizeof(writes),1,output)==1);
        assert(fwrite(stream,sizeof(stream[0]),writes,output)==writes);
        ++cases;
    }
    draw_untextured=draw_inspect_strips=0;
    for(unsigned v=0;v<8;++v) {
        const nm_view_vertex_t point={vertices[v].position[0],vertices[v].position[1],
            vertices[v].position[2],vertices[v].u,vertices[v].v};
        nm_cache_vertex(&batch.view,&point,points+v);
    }
    for(unsigned f=0;f<4;++f) entries[f]=f<<16;
    batch.opaque_count=4; batch.cutout_count=0;
    reset(); sq_joined_strips=sq_strip_vertices_saved=0;
    draw_count=NM_DRAW_TRIANGLES-5;
    memset(stream,0xa5,sizeof(stream));
    pvr_vertex_t untouched; memcpy(&untouched,stream,sizeof(untouched));
    assert(!sq_emit(0) && !writes && !draw_header_count && !sq_joined_strips && !sq_strip_vertices_saved);
    assert(!memcmp(stream,&untouched,sizeof(untouched)));
    reset(); draw_count=NM_DRAW_TRIANGLES-6;
    assert(sq_emit(0) && writes==9 && draw_count==NM_DRAW_TRIANGLES);
    /* DNM6: a strip is one layer; every vertex carries that layer's depth
     * factor and nothing else changes. */
    reset(); sq_joined_strips=sq_strip_vertices_saved=0;
    assert(sq_emit(0) && writes==9 && sq_joined_strips==1);
    pvr_vertex_t unlayered[9]; memcpy(unlayered,stream,sizeof(unlayered));
    draw_layer_scale_init();
    for(unsigned f=0;f<4;++f) faces[f].layer=2;
    reset(); sq_joined_strips=sq_strip_vertices_saved=0;
    assert(sq_emit(0) && writes==9 && sq_joined_strips==1);
    assert(!memcmp(stream,unlayered,sizeof(pvr_poly_hdr_t)));
    for(unsigned v=1;v<9;++v) {
        pvr_vertex_t got=stream[v].vertex;
        assert(got.z==points[v-1].q*(1.0f+2.0f*NM_LAYER_DEPTH_STEP) && got.z>unlayered[v].z);
        got.z=unlayered[v].z;
        assert(!memcmp(&got,unlayered+v,sizeof(got)));
    }
    for(unsigned f=0;f<4;++f) faces[f].layer=0;
    sq_shared_headers[0]=NULL;
    free(sq_flat_strip_triangle[0]); sq_flat_strip_triangle[0]=NULL;
    assert(cases==16);
    puts("PASS: offline strip as one flat-shaded run, mixed nonplanar chain, 16 OP/PT cases; ordered byte-exact triangles, clipping/queue-order fallback, untextured mode and pre-write capacity; DNM6 layered strip depth");
}
