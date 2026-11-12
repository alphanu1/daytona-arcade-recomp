/* Daytona USA PC: entry point.
 *
 * platform/dreamcast-native/code/main.c for the PC build: the same enDjinn
 * and PVR setup (640x480, pre-sorted translucent list, ARGB1555 palette RAM,
 * punch-through alpha reference 0x80), without the Dreamcast-only register
 * tuning and diagnostics, and with the asset set found at run time instead
 * of dcload's /pc/:
 *
 *   daytona [ASSET_DIR]      ASSET_DIR, else $DAYTONA_ASSETS, else
 *                            <executable dir>/assets/, else ./assets/
 */
#include <enDjinn/enj_enDjinn.h>
#include <sh4zam/shz_sh4zam.h>

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "practice.h"
#include "render/materials/palette_ram.h"
#include "save_store.h"
#include "game_save.h"

/* The VMU on PC: each save file (DAYTONA93NAT, save_data.h) is a file of the
 * same bytes in the user's data folder (SDL_GetPrefPath). */
static char save_dir[4096];
static int save_card;

static void save_path(char *out, size_t size, const char *name) { snprintf(out, size, "%s%s.vms", save_dir, name); }
static void *pc_card_find(void *context) { (void)context; return save_dir[0] ? &save_card : NULL; }
static int pc_card_free_blocks(void *context, void *card) { (void)context; (void)card; return 200; }
static int pc_card_file_blocks(void *context, void *card, const char *name) {
    (void)context; (void)card;
    char path[4200];
    struct stat st;
    save_path(path, sizeof path, name);
    if (stat(path, &st) != 0) return 0;
    return (int)((st.st_size + 511) / 512);
}
static int pc_card_read(void *context, void *card, const char *name, uint8_t **data, size_t *size) {
    (void)context; (void)card;
    char path[4200];
    save_path(path, sizeof path, name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buffer = n > 0 ? malloc((size_t)n) : NULL;
    if (!buffer || fread(buffer, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buffer); return -1; }
    fclose(f);
    *data = buffer; *size = (size_t)n;
    return 0;
}
static void pc_card_release(void *context, uint8_t *data) { (void)context; free(data); }
static int pc_card_write(void *context, void *card, const char *name, const uint8_t *data, size_t size) {
    (void)context; (void)card;
    char path[4200], temp[4210];
    save_path(path, sizeof path, name);
    snprintf(temp, sizeof temp, "%s.tmp", path);
    FILE *f = fopen(temp, "wb");
    if (!f) return -1;
    const int ok = fwrite(data, 1, size, f) == size;
    if (fclose(f) != 0 || !ok) { remove(temp); return -1; }
    remove(path); /* rename does not replace on Windows */
    return rename(temp, path) == 0 ? 0 : -1;
}
static const nm_save_device_t pc_card = {NULL, pc_card_find, pc_card_free_blocks, pc_card_file_blocks,
                                         pc_card_read, pc_card_release, pc_card_write};

static void save_init(void) {
    char *pref = SDL_GetPrefPath("daytona", "Daytona USA");
    if (pref) {
        snprintf(save_dir, sizeof save_dir, "%s", pref);
        SDL_free(pref);
        printf("daytona: saves in %s\n", save_dir);
        nm_game_save_device_set(&pc_card);
    }
}

#ifndef NM_PT_ALPHA_REF
#define NM_PT_ALPHA_REF 0x80
#endif
#ifndef PVR_PT_ALPHA_REF
#define PVR_PT_ALPHA_REF 0x011c
#endif

static int check_math(void) {
    shz_xmtrx_init_scale(2.0f, -3.0f, 4.0f);
    const shz_vec4_t v = shz_xmtrx_transform_vec4(shz_vec4_init(3.0f, -4.0f, 5.0f, 1.0f));
    return v.x == 6.0f && v.y == 12.0f && v.z == 20.0f && v.w == 1.0f;
}

/* An asset set is a directory holding the race's drive data. */
static int is_asset_dir(const char *dir) {
    char probe[4096];
    struct stat st;
    snprintf(probe, sizeof probe, "%sdrive.dnd", dir);
    return stat(probe, &st) == 0;
}

static void with_slash(char *out, size_t size, const char *dir) {
    const size_t n = strlen(dir);
    snprintf(out, size, "%s%s", dir, n && (dir[n - 1] == '/' || dir[n - 1] == '\\') ? "" : "/");
}

static const char *find_assets(int argc, char **argv) {
    static char root[4096];
    if (argc > 1) {
        with_slash(root, sizeof root, argv[1]);
        return root;
    }
    const char *env = getenv("DAYTONA_ASSETS");
    if (env && *env) {
        with_slash(root, sizeof root, env);
        return root;
    }
    const char *base = SDL_GetBasePath(); /* ends in a separator; owned by SDL */
    if (base) {
        snprintf(root, sizeof root, "%sassets/", base);
        if (is_asset_dir(root)) return root;
    }
    snprintf(root, sizeof root, "assets/");
    return root;
}

int main(int argc, char **argv) {
    if (!check_math()) {
        puts("FAIL: SH4ZAM scale check");
        return 1;
    }
    const char *root = find_assets(argc, argv);
    if (!is_asset_dir(root)) {
        fprintf(stderr,
                "daytona: no asset set at %s (no drive.dnd).\n"
                "Build one from your ROM set with platform/pc-native/recipes/build_pc_assets.sh,\n"
                "then run: daytona ASSET_DIR (or set DAYTONA_ASSETS).\n", root);
        return 1;
    }
    printf("daytona: assets %s\n", root);
    save_init();

    enj_state_init_defaults();
    enj_state_get()->video.pvr_params.vertex_buf_size = 16 * 1024 * 1024;
    for (unsigned list = 0; list < 5; ++list)
        enj_state_get()->video.pvr_params.opb_sizes[list] = PVR_BINSIZE_16;
    /* Translucent list in submission order: the renderer pre-sorts the smoke
     * back to front (main.c, NM_TR_AUTOSORT). */
    enj_state_get()->video.pvr_params.autosort_disabled = 1;
    enj_state_get()->video.display_mode = DM_640x480;
    enj_state_get()->video.pixel_mode = PM_RGB888P;
    if (enj_state_startup() != 0) return 1;
    if (!nm_palette_ram_init()) return 1;
    PVR_SET(PVR_PT_ALPHA_REF, (uint32_t)NM_PT_ALPHA_REF);
    return nm_practice_run(root);
}
