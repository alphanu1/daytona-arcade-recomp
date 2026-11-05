#ifndef DAYTONA_NATIVE_PRESENTATION_PROBE_H
#define DAYTONA_NATIVE_PRESENTATION_PROBE_H
/* Standalone bring-up modes only; never included by the driving game build. */
#ifdef NATIVE_DISC
#define NM_AUTO_EXIT_FRAMES 0
#else
#define NM_AUTO_EXIT_FRAMES 600
#endif
#ifdef NATIVE_TRANSFER_BENCH
#include "transfer_bench.h"
#endif
#ifdef NATIVE_CAMERA
#include "camera.h"
#endif
#ifdef NATIVE_MODELS
#include "render/models/model_renderer.h"
#endif
#ifdef NATIVE_MATERIALS
#include "render/materials/materials.h"
#endif

typedef struct {
    pvr_poly_hdr_t header;
    float x, y;
    unsigned frames;
    uint64_t register_sum, render_sum, register_max, render_max;
    unsigned samples, over_budget, register_max_frame, render_max_frame;
    int failed;
#ifdef NATIVE_CAMERA
    float camera[3];
    uint64_t camera_time;
#endif
} probe_t;

#ifndef NATIVE_RENDER
static void render_probe(void *data) {
    const probe_t *probe = data;
    const float corners[4][2] = {{-1, 1}, {-1, -1}, {1, 1}, {1, -1}};
    pvr_poly_hdr_t *header = pvr_dr_target();
    *header = probe->header;
    pvr_dr_commit(header);

    shz_xmtrx_init_scale(24.0f, 24.0f, 1.0f);
    for (unsigned i = 0; i < 4; ++i) {
        const shz_vec4_t v = shz_xmtrx_transform_vec4(
            shz_vec4_init(corners[i][0], corners[i][1], 1.0f, 1.0f));
        pvr_vertex_t *vertex = pvr_dr_target();
        *vertex = (pvr_vertex_t){
            .flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,
            .x = v.x + probe->x, .y = v.y + probe->y, .z = v.z,
            .argb = 0xff40c0ff,
        };
        pvr_dr_commit(vertex);
    }
}

static void render_labels(void *data) {
    (void)data;
    enj_font_scale_set(2);
    enj_qfont_write("Daytona / Dreamcast native", 24, 24, PVR_LIST_PT_POLY);
    enj_font_scale_set(1);
    enj_qfont_write("Foundation only - no game code imported", 24, 68,
                    PVR_LIST_PT_POLY);
    enj_qfont_write("Analog stick: move SH4ZAM probe", 24, 400, PVR_LIST_PT_POLY);
    enj_qfont_write("START+A+B+X+Y: return to loader", 24, 424, PVR_LIST_PT_POLY);
}
#endif

static void update(void *data) {
    probe_t *probe = data;
#ifdef NATIVE_RENDER
    if (enj_state_get()->flags.shut_down ||
        (NM_AUTO_EXIT_FRAMES && probe->frames == NM_AUTO_EXIT_FRAMES)) {
#ifdef NATIVE_CAPTURE
        /* Diagnostic-only readback AFTER the quiet timing window. Use a new
         * mapped directory: never overwrite another captured frame. */
        if (probe->frames == 600) {
            pvr_wait_ready();
            pvr_wait_render_done();
            uint8_t *pixels = NULL;
            const size_t bytes = vid_screen_shot_data(&pixels);
            FILE *file = bytes ? fopen("/pc/frame.rgb", "wbx") : NULL;
            int ok = file && bytes == 640u*480u*3u;
            if (file) {
                if (fwrite(pixels, 1, bytes, file) != bytes) ok = 0;
                if (fclose(file)) ok = 0;
            }
            free(pixels);
            printf("Native framebuffer RGB24 capture: %zu bytes %s\n", bytes, ok ? "PASS" : "FAIL");
            if (!ok) probe->failed = 1;
        }
#endif
        nm_render_release();
        nm_materials_unload();
        printf("Native draw released before enDjinn shutdown: PVR free=%lu bytes\n",
               (unsigned long)pvr_mem_available());
        enj_state_flag_shutdown(NULL);
        return;
    }
#endif
    probe->x = 320.0f;
    probe->y = 240.0f;
    enj_ctrlr_state_t **controllers = enj_ctrl_get_states();
#ifdef NATIVE_CAMERA
    float motion[3] = {0};
    int reset_camera = 0;
#endif
    for (size_t i = 0; i < enj_ctrl_states_length(); ++i) {
        if (controllers[i]) {
            probe->x += controllers[i]->joyx;
            probe->y += controllers[i]->joyy;
#ifdef NATIVE_CAMERA
            motion[0] = nm_camera_axis(controllers[i]->joyx);
            motion[2] = -nm_camera_axis(controllers[i]->joyy);
            motion[1] = ((int)controllers[i]->rtrigger - controllers[i]->ltrigger) / 255.0f;
            reset_camera = controllers[i]->button.B == ENJ_BUTTON_DOWN_THIS_FRAME;
#endif
            break;
        }
    }
#ifdef NATIVE_CAMERA
#ifdef NATIVE_CAMERA_TEST
    /* Synthetic input, not a recorded camera/board-state replay. The engine's
     * exit chord remains active before this updater is called. */
    motion[0] = motion[1] = motion[2] = 0;
    motion[(probe->frames / 200) % 3] = (probe->frames / 100) & 1 ? -1 : 1;
    reset_camera = probe->frames % 200 == 0;
#endif
    const uint64_t now = timer_ns_gettime64();
    const float seconds = probe->camera_time ? (now - probe->camera_time) * 1e-9f : 0;
    probe->camera_time = now;
    if (!nm_camera_update(probe->camera, motion, seconds, reset_camera) ||
        !nm_render_camera_set(probe->camera)) {
        puts("FAIL: invalid native camera input/state");
        probe->failed = 1;
        nm_render_release();
        nm_materials_unload();
        enj_state_flag_shutdown(NULL);
        return;
    }
#endif
#ifdef NATIVE_RENDER
    if (!nm_render_enqueue()) {
        puts("FAIL: native draw stream capacity/material check");
        probe->failed = 1;
        nm_render_release();
        nm_materials_unload();
        enj_state_flag_shutdown(NULL);
        return;
    }
#else
    enj_render_list_add(PVR_LIST_OP_POLY, render_probe, probe);
    enj_render_list_add(PVR_LIST_PT_POLY, render_labels, NULL);
#endif
    ++probe->frames;
    /* Ignore startup frames.
     * No console I/O inside this window: dc-load syscalls mask interrupts and
     * can delay the timestamp taken by KOS's PVR completion handler. */
    if (probe->frames >= 5 && probe->frames <= 600) {
        pvr_stats_t stats;
        const int have_stats = pvr_get_stats(&stats) == 0;
        if (have_stats) {
            probe->register_sum += stats.reg_last_time;
            probe->render_sum += stats.rnd_last_time;
            if (stats.reg_last_time > probe->register_max) {
                probe->register_max = stats.reg_last_time;
                probe->register_max_frame = probe->frames;
            }
            if (stats.rnd_last_time > probe->render_max) {
                probe->render_max = stats.rnd_last_time;
                probe->render_max_frame = probe->frames;
            }
            probe->over_budget += stats.reg_last_time + stats.rnd_last_time >= 16000000;
            ++probe->samples;
        }
        if (probe->frames == 600 && have_stats) {
            printf("Native PVR sample n=%u: register avg/max=%llu/%llu us "
                   "render avg/max=%llu/%llu us; register+render>=16ms=%u "
                   "max observed at updates=%u/%u\n",
                   probe->samples,
                   (unsigned long long)(probe->register_sum / probe->samples / 1000),
                   (unsigned long long)(probe->register_max / 1000),
                   (unsigned long long)(probe->render_sum / probe->samples / 1000),
                   (unsigned long long)(probe->render_max / 1000), probe->over_budget,
                   probe->register_max_frame, probe->render_max_frame);
            const struct mallinfo memory = mallinfo();
            printf("Allocator arena=%lu used=%lu free=%lu bytes; PVR free=%lu bytes\n",
                   (unsigned long)memory.arena, (unsigned long)memory.uordblks,
                   (unsigned long)memory.fordblks, (unsigned long)pvr_mem_available());
            printf("Last PVR sample: ready=%llu us TA=%lu max=%lu bytes\n",
                   (unsigned long long)(stats.frame_last_time / 1000),
                   (unsigned long)stats.vtx_buffer_used,
                   (unsigned long)stats.vtx_buffer_used_max);
#if defined(NATIVE_MODELS) && !defined(NATIVE_RENDER)
            /* Diagnostic builds finish through enDjinn, ready for the next upload. */
            enj_state_flag_shutdown(NULL);
#endif
        }
    }
}

static int nm_probe_run(void) {
    /* Push before resource loading so every later failure can shut down
     * through enDjinn; run() without any mode does not perform shutdown. */
    probe_t probe = {0};
    pvr_poly_cxt_t context;
    pvr_poly_cxt_col(&context, PVR_LIST_OP_POLY);
    context.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&probe.header, &context);
    enj_mode_t mode = {.name = "Native diagnostic", .mode_updater = update,
                       .data = &probe};
    if (!enj_mode_push(&mode)) return 1;
#ifdef NATIVE_TRANSFER_BENCH
    const int transfer_ok=nm_transfer_bench();
    enj_state_flag_shutdown(NULL); enj_state_run();
    return transfer_ok ? 0 : 1;
#endif
#ifdef NATIVE_MODELS
    nm_pack_t models = {0};
    const uint64_t load_started = timer_ns_gettime64();
    /* KOS calls main(0, NULL); use the disc root or kos-tool mapping, not argv. */
    if (!nm_pack_load(&models, NM_ASSET_ROOT "scene.dnm")) {
        puts("FAIL: load " NM_ASSET_ROOT "scene.dnm");
        enj_state_flag_shutdown(NULL); enj_state_run();
        return 1;
    }
    printf("Native model pack: %lu resident bytes, %lu meshes, %lu vertices, %lu faces, "
           "%lu materials, %lu instances; load+validate=%llu us\n",
           (unsigned long)models.header->size, (unsigned long)models.header->meshes,
           (unsigned long)models.header->vertices, (unsigned long)models.header->faces,
           (unsigned long)models.header->materials, (unsigned long)models.header->instances,
           (unsigned long long)((timer_ns_gettime64() - load_started) / 1000));
    if (!nm_pack_check_math(&models) || !nm_pack_check_projection(&models) || !nm_pack_check_lighting(&models)) {
        puts("FAIL: native model math/projection/lighting check");
        nm_pack_unload(&models);
        enj_state_flag_shutdown(NULL); enj_state_run();
        return 1;
    }
#endif
#ifdef NATIVE_MATERIALS
    /* Measure material allocation and release. */
    const unsigned pvr_before_materials = pvr_mem_available();
    const int materials_ok = nm_materials_load(NM_ASSET_ROOT "materials.dnt") && nm_pack_check_materials(&models);
#ifdef NATIVE_RENDER
    if (!materials_ok || !nm_render_init(&models)) {
        nm_render_release();
        nm_materials_unload();
        nm_pack_unload(&models);
        enj_state_flag_shutdown(NULL); enj_state_run();
        return 1;
    }
    (void)pvr_before_materials;
#else
    /* This bring-up verifies residency/bindings, not textured drawing yet.
     * Release while PVR is alive: enDjinn shuts it down before run returns. */
    nm_materials_unload();
    nm_materials_unload();
    const int materials_released = pvr_mem_available() == pvr_before_materials;
    printf("Native materials release: PVR before=%u after=%u %s\n", pvr_before_materials,
           (unsigned)pvr_mem_available(), materials_released ? "PASS" : "FAIL");
    if (!materials_ok || !materials_released) {
        nm_pack_unload(&models);
        enj_state_flag_shutdown(NULL); enj_state_run();
        return 1;
    }
#endif
#endif
    printf("Native startup: SH4ZAM check PASS; 640x480 RGB888P, dithering off\n"
           "PVR free after resource upload: %lu bytes\n",
           (unsigned long)pvr_mem_available());
    enj_state_run();
#ifdef NATIVE_MODELS
    nm_pack_unload(&models);
#endif
    puts("Native foundation: enDjinn returned; exiting to loader");
    return probe.failed ? 1 : 0;
}
#endif
