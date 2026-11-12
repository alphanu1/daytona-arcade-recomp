#include <kos.h>

#include <cstdio>
#include <cstring>

extern "C" {

void gdb_init(void) {}
void perf_monitor_init(int, int) {}
void perf_monitor_print(FILE *) {}
void arch_set_exit_path(int) {}
void vmufb_paint_area(vmufb_t *, unsigned int, unsigned int, unsigned int,
                      unsigned int, const uint8_t *) {}
void vmufb_clear(vmufb_t *fb)
{
    if (fb != nullptr) {
        std::memset(fb, 0, sizeof(*fb));
    }
}
void vmufb_present(const vmufb_t *, maple_device_t *) {}
void vmufb_print_string_into(vmufb_t *, const vmufb_font_t *, unsigned int,
                             unsigned int, unsigned int, unsigned int,
                             unsigned int, const char *) {}

}  // extern "C"
