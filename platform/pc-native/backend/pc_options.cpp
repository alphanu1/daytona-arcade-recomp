/* Daytona USA PC: presentation options read once from the environment. */
#include <algorithm>
#include <cstdlib>

extern "C" {

/* DAYTONA_DRAW_DISTANCE: scenery cells drawn around the camera, as the desktop
 * recomp's draw distance slider (-2 shortest .. 0 the game's own .. +2 7x7),
 * and +3, every cell of the course (the recomp stops at 7x7: the original's
 * cell list holds 63; the native renderer has no such list). Default +3. */
int pc_draw_distance(void)
{
    static const int level = [] {
        const char *v = std::getenv("DAYTONA_DRAW_DISTANCE");
        return v != nullptr && *v != '\0' ? std::clamp(std::atoi(v), -2, 3) : 3;
    }();
    return level;
}

}  // extern "C"

extern "C" {

/* Revision A's rolling-start camera around the player's car (the generated
 * reva_start_camera.h: build_pc_assets.sh with DAYTONA_REVA_ROM), the default
 * when it was made; DAYTONA_ROLLING_START=93 (or "deluxe") keeps Deluxe '93's
 * own. */
int pc_rolling_start_reva(void)
{
    static const int on = [] {
        const char *v = std::getenv("DAYTONA_ROLLING_START");
        return v == nullptr || (v[0] != '9' && v[0] != 'd' && v[0] != 'D');
    }();
    return on;
}

/* DAYTONA_CAR_DETAIL: the opponents' model. Default: every drawn car with its
 * nearest, full model (the arcade's level 4, animated wheels), at any distance;
 * "arcade": the game's own level by distance (fewer polygons further away). */
int pc_car_full_detail(void)
{
    static const int on = [] {
        const char *v = std::getenv("DAYTONA_CAR_DETAIL");
        return v == nullptr || (v[0] != 'a' && v[0] != 'A' && v[0] != '0');
    }();
    return on;
}

}  // extern "C"
