ENJ_BASENAME := palette-probe
ENJ_CODEDIR := ./code
ENJ_INJECT_QFONT := 1
ifndef PROBE_DATA_DIR
$(error PROBE_DATA_DIR must contain palette_probe_data.h (tools/prepare_palette_probe.py))
endif
ENJ_CFLAGS += -I$(abspath $(PROBE_DATA_DIR))
