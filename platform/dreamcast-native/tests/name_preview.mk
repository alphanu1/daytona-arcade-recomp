# From platform/dreamcast-native: make -f tests/name_preview.mk ENDJINN_ROOT=...
# NAME_DATA_DIR=<prepare_models --name-header output dir> ENJ_BASENAME=name-preview
ifndef NAME_DATA_DIR
$(error NAME_DATA_DIR must contain name_data.h and name.dnm)
endif
override ENJ_CODEDIR := ./tests/name_preview
OBJS = $(ENJ_BUILDDIR)/code/meshpack.o $(ENJ_BUILDDIR)/code/geometry.o $(ENJ_BUILDDIR)/code/render/ui/name_render.o $(ENJ_BUILDDIR)/code/render/ui/name_labels.o
OBJS += $(ENJ_BUILDDIR)/code/gameplay/name_scene.o
ENJ_CFLAGS += -I$(abspath $(NAME_DATA_DIR))
include Makefile
$(ENJ_BUILDDIR)/tests/name_preview/main.o: $(abspath $(NAME_DATA_DIR))/name_data.h $(abspath $(NAME_DATA_DIR))/labels/name_label_data.h
