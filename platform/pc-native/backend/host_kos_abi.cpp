#include <kos.h>
#include <pc_endjinn/kos_abi_contract.generated.h>

#include <cstddef>
#include <cstdint>

#define ABI_ASSERT_VALUE(name, expression)                                      \
  static_assert((expression) == PC_ENDJINN_KOS_ABI_##name,                     \
                "pc-enDjinn KOS ABI mismatch: " #name)

ABI_ASSERT_VALUE(SIZEOF_PVR_INIT_PARAMS, sizeof(pvr_init_params_t));
ABI_ASSERT_VALUE(ALIGNOF_PVR_INIT_PARAMS, alignof(pvr_init_params_t));
ABI_ASSERT_VALUE(OFFSETOF_PVR_INIT_PARAMS_VERTEX_BUF_SIZE,
                 offsetof(pvr_init_params_t, vertex_buf_size));
ABI_ASSERT_VALUE(OFFSETOF_PVR_INIT_PARAMS_OPB_SIZES,
                 offsetof(pvr_init_params_t, opb_sizes));

ABI_ASSERT_VALUE(SIZEOF_PVR_VERTEX, sizeof(pvr_vertex_t));
ABI_ASSERT_VALUE(ALIGNOF_PVR_VERTEX, alignof(pvr_vertex_t));
ABI_ASSERT_VALUE(OFFSETOF_PVR_VERTEX_X, offsetof(pvr_vertex_t, x));
ABI_ASSERT_VALUE(OFFSETOF_PVR_VERTEX_U, offsetof(pvr_vertex_t, u));
ABI_ASSERT_VALUE(OFFSETOF_PVR_VERTEX_ARGB, offsetof(pvr_vertex_t, argb));

ABI_ASSERT_VALUE(SIZEOF_PVR_SPRITE_HDR, sizeof(pvr_sprite_hdr_t));
ABI_ASSERT_VALUE(ALIGNOF_PVR_SPRITE_HDR, alignof(pvr_sprite_hdr_t));
ABI_ASSERT_VALUE(OFFSETOF_PVR_SPRITE_HDR_ARGB,
                 offsetof(pvr_sprite_hdr_t, argb));

ABI_ASSERT_VALUE(SIZEOF_PVR_SPRITE_COL, sizeof(pvr_sprite_col_t));
ABI_ASSERT_VALUE(ALIGNOF_PVR_SPRITE_COL, alignof(pvr_sprite_col_t));
ABI_ASSERT_VALUE(OFFSETOF_PVR_SPRITE_COL_DX,
                 offsetof(pvr_sprite_col_t, dx));
ABI_ASSERT_VALUE(SIZEOF_PVR_SPRITE_TXR, sizeof(pvr_sprite_txr_t));
ABI_ASSERT_VALUE(ALIGNOF_PVR_SPRITE_TXR, alignof(pvr_sprite_txr_t));
ABI_ASSERT_VALUE(OFFSETOF_PVR_SPRITE_TXR_AUV,
                 offsetof(pvr_sprite_txr_t, auv));

ABI_ASSERT_VALUE(SIZEOF_CONT_STATE, sizeof(cont_state_t));
ABI_ASSERT_VALUE(ALIGNOF_CONT_STATE, alignof(cont_state_t));
ABI_ASSERT_VALUE(OFFSETOF_CONT_STATE_BUTTONS, offsetof(cont_state_t, buttons));
ABI_ASSERT_VALUE(OFFSETOF_CONT_STATE_LTRIG, offsetof(cont_state_t, ltrig));
ABI_ASSERT_VALUE(OFFSETOF_CONT_STATE_JOYX, offsetof(cont_state_t, joyx));
ABI_ASSERT_VALUE(SIZEOF_PURUPURU_EFFECT, sizeof(purupuru_effect_t));
ABI_ASSERT_VALUE(ALIGNOF_PURUPURU_EFFECT, alignof(purupuru_effect_t));

ABI_ASSERT_VALUE(PVR_LIST_OP_POLY, PVR_LIST_OP_POLY);
ABI_ASSERT_VALUE(PVR_LIST_TR_POLY, PVR_LIST_TR_POLY);
ABI_ASSERT_VALUE(PVR_LIST_PT_POLY, PVR_LIST_PT_POLY);
ABI_ASSERT_VALUE(PVR_CMD_VERTEX, static_cast<int32_t>(PVR_CMD_VERTEX));
ABI_ASSERT_VALUE(PVR_CMD_VERTEX_EOL, static_cast<int32_t>(PVR_CMD_VERTEX_EOL));
ABI_ASSERT_VALUE(MAPLE_PORT_COUNT, MAPLE_PORT_COUNT);
ABI_ASSERT_VALUE(MAPLE_UNIT_COUNT, MAPLE_UNIT_COUNT);
ABI_ASSERT_VALUE(MAPLE_FUNC_CONTROLLER, MAPLE_FUNC_CONTROLLER);
ABI_ASSERT_VALUE(MAPLE_FUNC_LCD, MAPLE_FUNC_LCD);
ABI_ASSERT_VALUE(MAPLE_FUNC_PURUPURU, MAPLE_FUNC_PURUPURU);
ABI_ASSERT_VALUE(CONT_A, CONT_A);
ABI_ASSERT_VALUE(CONT_B, CONT_B);
ABI_ASSERT_VALUE(CONT_X, CONT_X);
ABI_ASSERT_VALUE(CONT_Y, CONT_Y);
ABI_ASSERT_VALUE(CONT_START, CONT_START);
