#!/bin/sh
set -eu

symbols=$(sh-elf-nm -C "$2")
instructions=$(sh-elf-objdump -d "$1")
# NATIVE_LTO builds (fat objects carry .gnu.lto_ sections): the link internalises, renames (.constprop.N, .part.N,
# .isra.N, .lto_priv.N) or wholly inlines functions. elf_symbol prints the linked name of a function under any
# binding and suffix; linked asks whether the link defines a global function, in the ELF or, under LTO when it was
# inlined away, in one of the link's objects. Without LTO both reduce to the plain ' T name' / ' t name' tests.
lto=0
if sh-elf-objdump -h "$1" | rg -q 'gnu\.lto_'; then lto=1; fi
link_objects=$(dirname "$1")/..
elf_symbol() {
    if [ "$lto" = 1 ]; then
        printf '%s\n' "$symbols" | rg " [tTW] $1(\.(constprop|part|isra|lto_priv)\.[0-9]+)*$" | awk 'NR==1 {print $3}'
    else
        printf '%s\n' "$symbols" | rg " [$2] $1$" | awk 'NR==1 {print $3}'
    fi
}
linked() {
    if [ -n "$(elf_symbol "$1" T)" ]; then return 0; fi
    [ "$lto" = 1 ] && find "$link_objects" -name '*.o' -exec sh-elf-nm -C --defined-only {} + 2>/dev/null | rg -q " T $1$"
}
if [ -f "$(dirname "$1")/geometry.o" ]; then
geometry_instructions=$(sh-elf-objdump -d "$(dirname "$1")/geometry.o")
if [ -f "$(dirname "$1")/geometry.s" ]; then
    geometry_instructions=$(sed '/^[[:space:]]*\./d' "$(dirname "$1")/geometry.s")
fi
if printf '%s\n' "$geometry_instructions" | rg -q 'fcnvsd|fcnvds|fpscr'; then
    echo 'FAIL: native geometry must remain single precision, without FPSCR mode switches' >&2
    exit 1
fi
echo 'PASS: geometry uses single precision without FPSCR mode switches'
else
    case "$1" in */tests/transfer/main.o) ;; *) exit 1 ;; esac
    if [ -n "$(elf_symbol nm_practice_run T)" ]; then
        echo 'FAIL: ROM-free transfer target links gameplay' >&2; exit 1
    fi
    echo 'PASS: separate ROM-free transfer entrypoint, no gameplay linked'
fi
for presentation in finish result; do
if linked "nm_${presentation}_render_load"; then
    finish_instructions=$(sh-elf-objdump -d "$(dirname "$1")/render/ui/${presentation}_render.o")
    if printf '%s\n' "$finish_instructions" | rg -q 'fcnvsd|fcnvds|fpscr'; then
        echo "FAIL: $presentation presentation must remain single precision" >&2
        exit 1
    fi
    printf '%s\n' "$finish_instructions" | rg -q '[[:space:]]ftrv[[:space:]]'
    test "$(sh-elf-objdump -d --disassemble=_render "$(dirname "$1")/render/ui/${presentation}_render.o" | rg -c '[[:space:]]pref[[:space:]]')" -ge 2
    echo "PASS: $presentation FTRV, single precision and direct header/vertex SQ commits"
fi
done
if linked nm_result_labels_load; then
    label_instructions=$(sh-elf-objdump -d "$(dirname "$1")/render/ui/result_labels.o")
    if printf '%s\n' "$label_instructions" | rg -q 'fcnvsd|fcnvds|fpscr'; then
        echo 'FAIL: result labels must remain single precision' >&2
        exit 1
    fi
    test "$(sh-elf-objdump -d --disassemble=_render "$(dirname "$1")/render/ui/result_labels.o" | rg -c '[[:space:]]pref[[:space:]]')" -ge 2
    echo 'PASS: result labels single precision and direct header/vertex SQ commits'
fi
for entry in enj_state_run enj_ctrl_get_states enj_render_list_add; do
    linked "$entry"
done
printf '%s\n' "$instructions" | rg -qi '[[:space:]]ftrv[[:space:]]'
if linked nm_render_init; then
    sq_symbol=_submit_draw
    if [ -n "$(elf_symbol sq_emit t)" ]; then sq_symbol=_$(elf_symbol sq_emit t); fi
    if [ -n "$(elf_symbol sq_submit_object t)" ]; then sq_symbol=_$(elf_symbol sq_submit_object t); fi
    # The production emitter's face loop (model_submission.h), one instance per list.
    if [ -n "$(elf_symbol sq_emit_faces_op t)" ]; then sq_symbol=_$(elf_symbol sq_emit_faces_op t); fi
    sq_commits=$(sh-elf-objdump -d --disassemble="$sq_symbol" "$2" | rg -c '[[:space:]]pref[[:space:]]')
    test "$sq_commits" -ge 2 # Header and vertex pvr_dr_commit both flush SQ.
    echo 'PASS: native 3D header/vertex submission contains SH4 SQ flushes'
fi
if linked nm_render_world_attach; then
    linked nm_world_materials_upload
    linked nm_scenery_load
    linked nm_sky_load
    if linked nm_sky_enqueue; then
    sky_symbol=$(elf_symbol sky_render t); test -n "$sky_symbol"
    sky_instructions=$(sh-elf-objdump -d --disassemble="_$sky_symbol" "$2")
    printf '%s\n' "$sky_instructions" | rg -q '[[:space:]]ftrv[[:space:]]'
    test "$(printf '%s\n' "$sky_instructions" | rg -c '[[:space:]]pref[[:space:]]')" -ge 2
    else
        linked nm_track_surface_select
        if [ -n "$(elf_symbol sky_render tT)" ]; then
            echo 'FAIL: reduced track view still links a sky callback' >&2; exit 1
        fi
        echo 'PASS: reduced track view has surface selection and no sky callback'
    fi
    # Vertex transforms of the model renderer: sq_prepare_batch, inlined into the object callback or split into parts.
    ftrv_symbol=_$(elf_symbol sq_submit_object t)
    if [ -n "$(elf_symbol sq_prepare_batch t)" ]; then ftrv_symbol=_$(elf_symbol sq_prepare_batch t); fi
    # sq_prepare_batch in parts: the wide-view vertex loop is a function of its own.
    if [ -n "$(elf_symbol sq_cache_points_wide t)" ]; then ftrv_symbol=_$(elf_symbol sq_cache_points_wide t); fi
    test "$ftrv_symbol" != _
    sh-elf-objdump -d --disassemble="$ftrv_symbol" "$2" | rg -q '[[:space:]]ftrv[[:space:]]'
    if [ -n "$(elf_symbol practice_render tT)" ]; then
        echo 'FAIL: world build must not substitute the debug road ribbon' >&2
        exit 1
    fi
    echo 'PASS: shared SQ/FTRV track/car renderer'
elif linked nm_practice_run; then
    sh-elf-objdump -d --disassemble=_practice_render "$2" | rg -q '[[:space:]]pref[[:space:]]'
    if printf '%s\n' "$symbols" | rg -q ' [tT] practice_triangle$'; then
        sh-elf-objdump -d --disassemble=_practice_triangle "$2" | rg -q '[[:space:]]pref[[:space:]]'
        if printf '%s\n' "$symbols" | rg -q ' [tT] practice_point$'; then
            sh-elf-objdump -d --disassemble=_practice_point "$2" | rg -q '[[:space:]]ftrv[[:space:]]'
        else # GCC inlines the single remaining road-point call in textured mode.
            sh-elf-objdump -d --disassemble=_practice_render "$2" | rg -q '[[:space:]]ftrv[[:space:]]'
        fi
    else
        # Passing scene data lets GCC specialize the helper (.isra.N).
        # Inspect that actual function; never treat an empty disassembly as a pass.
        vertex_symbol=$(printf '%s\n' "$symbols" | rg ' [tT] practice_vertex(\.isra\.[0-9]+)?$' | awk 'NR==1 {print $3}')
        test -n "$vertex_symbol"
        practice_vertex=$(sh-elf-objdump -d --disassemble="_$vertex_symbol" "$2")
        printf '%s\n' "$practice_vertex" | rg -q '[[:space:]]pref[[:space:]]'
        printf '%s\n' "$practice_vertex" | rg -q '[[:space:]]ftrv[[:space:]]'
    fi
    echo 'PASS: practice OP header/vertex SQ transport and presentation FTRV'
fi
if printf '%s\n' "$symbols" | rg -q 'rt::|i960::|tgp::|GameLoop|SoundBoard'; then
    echo 'FAIL: native target links the reference machine runtime' >&2
    exit 1
fi
# Sound is AICAflow's alone (code/audio/sound.c): no enDjinn sound helper and no KOS sound call anywhere; the only
# KOS sound names are the two wrappers there that keep KOS's driver from starting (local.cfg.mk: --wrap).
if rg -n 'enj_sound_|snd_|no_soft_reset|soft_reset_enabled|enj_state_soft_reset_set|PAL8' code \
        | rg -v '^code/audio/sound\.c:[0-9]+:(int __wrap_snd_init\(void\) \{ return 0; \}|void __wrap_snd_shutdown\(void\) \{\})$'; then
    echo 'FAIL: sound only through AICAflow, PAL8-free, and the default exit chord preserved' >&2
    exit 1
fi
# AICAflow (firmware ABI 8) owns the AICA's SH4 interrupt for its status reports: nothing of the port hooks it.
if rg -n 'ASIC_EVT_SPU|asic_evt_' code include; then
    echo 'FAIL: the AICA interrupt (ASIC_EVT_SPU_IRQ) is AICAflow'"'"'s alone' >&2
    exit 1
fi
if linked nm_practice_run; then
    for symbol in __wrap_snd_init __wrap_snd_shutdown afx_init afx_set_end_callback nm_sound_command afx_instance_patch afx_bank_flow_upload; do
        if [ -z "$(elf_symbol "$symbol" T)" ]; then
            echo "FAIL: game ELF lacks $symbol (AICAflow sound, KOS sound wrapped)" >&2
            exit 1
        fi
    done
    echo 'PASS: AICAflow linked (music and effects), KOS sound startup wrapped'
fi
rg -q 'pixel_mode = PM_RGB888P;' code/main.c
rg -q 'vid_set_dithering\(false\)' code/main.c
case "$2" in
    *-disc.elf)
        asset_root="/cd/$(basename "$2" .elf)/"
        main_strings=$(sh-elf-strings "$1")
        case "$2" in
            *-driving-disc.elf|*-driving-3d-disc.elf|*-driving-3d-textured-disc.elf|*-driving-3d-textured-world-disc.elf)
                printf '%s\n' "$main_strings" | rg -Fxq "$asset_root"
                for asset in '%s%sdrive.dnd' '%s%scourse%u.dnc' '%s%scourse%u.dnr' '%s%scourse%u-route%u.dnp'; do
                    sh-elf-strings "$2" | rg -Fxq "$asset"
                done
                ;;
            *)
                for asset in scene.dnm materials.dnt; do
                    printf '%s\n' "$main_strings" | rg -Fxq "$asset_root$asset"
                done
                ;;
        esac
        case "$2" in
            *-driving-3d-disc.elf|*-driving-3d-textured-disc.elf|*-driving-3d-textured-world-disc.elf) sh-elf-strings "$2" | rg -Fxq '%scar.dnm' ;;
        esac
        case "$2" in
            *-driving-3d-textured-disc.elf|*-driving-3d-textured-world-disc.elf) sh-elf-strings "$2" | rg -Fxq '%scar.dnt' ;;
        esac
        case "$2" in
            *-driving-3d-textured-world-disc.elf)
                for asset in world.dnm world.dnt sky.dns scenery.dnv; do
                    sh-elf-strings "$2" | rg -Fxq "%s%s$asset"
                done ;;
        esac
        if printf '%s\n' "$main_strings" | rg -q '/pc/'; then
            echo 'FAIL: standalone disc main still depends on /pc' >&2
            exit 1
        fi
        echo 'PASS: standalone disc asset paths, no /pc dependency in main'
        ;;
esac
echo 'PASS: independent enDjinn ELF, SH4 FTRV, sound only through AICAflow, 24bpp configuration'
echo 'Hardware still required: math self-check, display, input and physical exit chord.'
