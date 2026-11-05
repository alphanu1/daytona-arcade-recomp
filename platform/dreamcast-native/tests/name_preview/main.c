/* Finite presentation test using the production name callback. Not phase29. */
#include <enDjinn/enj_enDjinn.h>
#include "render/ui/name_render.h"
#include "gameplay/name_scene.h"
#include "name_data.h"
#include "labels/name_label_data.h"

static nm_pack_t models;
static nm_drive_params_t math_params; /* SH4ZAM poses do not read reference LUTs. */
static nm_name_scene_t scene;
static unsigned frames,failed;
static unsigned interactive,silent_commands;

static void label(void *unused) {
    (void)unused;
    enj_font_scale_set(1);
    enj_qfont_write(scene.finish_requested ? "END requested / preview does not save a record" :
        interactive ? "Stick: select / A: accept / arrow: delete / END: finish" :
        "Name preview / B: take control / 900 auto frames",20,450,PVR_LIST_PT_POLY);
    enj_qfont_write("B: restart/toggle auto / START+A+B+X+Y exits",20,466,PVR_LIST_PT_POLY);
}

static void update(void *unused) {
    (void)unused;
    if((!interactive && frames>=900) || failed || enj_state_get()->flags.shut_down) {
        if(!nm_name_render_release()) failed=1;
        if(!nm_name_labels_unload()) failed=1;
        nm_pack_unload(&models);
        enj_state_flag_shutdown(NULL);return;
    }
    const int16_t center[2]={248,208};
    if(frames<32) {
        if(!nm_name_scene_step(&scene,&nm_name_layout_data,0,0) ||
           !nm_name_render_enqueue_intro(&math_params,center,(unsigned)scene.draw_intro) ||
           !nm_name_labels_enqueue(-1)) failed=1;
        ++frames;return;
    }
    enj_ctrlr_state_t **controllers=enj_ctrl_get_states(),*controller=NULL;
    for(unsigned i=0;i<4;++i) if(controllers[i]) { controller=controllers[i];break; }
    if(controller && controller->button.B==ENJ_BUTTON_DOWN_THIS_FRAME) {
        interactive=!interactive;frames=32;
        if(!nm_name_scene_init(&scene,&nm_name_layout_data)) failed=1;
        scene.intro_remaining=0; /* Test toggle keeps the already-shown entrance. */
    }
    if(!interactive) {
        /* Automatic coverage, not a replay or gameplay substitute. */
        scene.entry.index=(frames/30)%29;scene.entry.count=(frames/120)%4;
        scene.entry.name[0]='A';scene.entry.name[1]='.';scene.entry.name[2]='Z';
        scene.draw_entry=scene.entry;
        if(!nm_name_marker_step(&scene.marker,nm_name_layout_data.radius[scene.entry.index],
                               nm_name_layout_data.angle[scene.entry.index])) failed=1;
    } else if(controller) {
        const int8_t steering=controller->joyx<-127 ? -127 : controller->joyx;
        if(!nm_name_scene_step(&scene,&nm_name_layout_data,steering,
                controller->button.A==ENJ_BUTTON_DOWN_THIS_FRAME)) failed=1;
        for(unsigned i=0;i<2;++i) if(scene.commands[i]) ++silent_commands;
    }
    if(!failed && !nm_name_render_enqueue(&scene.draw_entry,&scene.marker,&math_params,center,frames)) failed=1;
    if(!failed && !nm_name_labels_enqueue((int)scene.draw_seconds)) failed=1;
    enj_render_list_add(PVR_LIST_PT_POLY,label,NULL);
    ++frames;
}

int main(void) {
    if(!nm_pack_load(&models,"/pc/name.dnm")) return 1;
    if(models.header->instances!=30) { nm_pack_unload(&models);return 1; }
    for(unsigned i=0;i<30;++i) if(models.instances[i].source_address!=nm_name_model_addresses[i]) {
        nm_pack_unload(&models);return 1;
    }
    enj_state_init_defaults();
    enj_state_get()->video.pvr_params.vertex_buf_size=512*1024;
    for(unsigned list=0;list<5;++list) enj_state_get()->video.pvr_params.opb_sizes[list]=PVR_BINSIZE_8;
    enj_state_get()->video.pixel_mode=PM_RGB888P;
    if(enj_state_startup()!=0) { nm_pack_unload(&models);return 1; }
    vid_set_dithering(false);
    if(!nm_name_render_init(&models,&nm_name_layout_data,nm_name_material_headers,nm_name_colors,0)) {
        puts("FAIL name preview: model renderer init"); failed=1;
    }
    if(!failed && !nm_name_labels_load("/pc/labels/",nm_name_label_parts,NM_NAME_LABEL_TEXTURE_COUNT,
                                       NM_NAME_LABEL_STATIC_COUNT)) {
        puts("FAIL name preview: resident label load"); failed=1;
    }
    if(!failed && !nm_name_labels_record(1,1237,467,1)) {
        puts("FAIL name preview: record label preparation"); failed=1;
    }
    if(!failed && !nm_name_scene_init(&scene,&nm_name_layout_data)) {
        puts("FAIL name preview: scene init"); failed=1;
    }
    enj_mode_t mode={.name="Name renderer preview",.mode_updater=update};
    enj_mode_push(&mode);enj_state_run();
    nm_name_render_release();nm_pack_unload(&models);
    printf("Native name preview: frames=%u failed=%u manual=%u finish-request=%u silent-commands=%u; phase29/commit excluded\n",
        frames,failed,interactive,scene.finish_requested,silent_commands);
    return failed ? 1 : 0;
}
