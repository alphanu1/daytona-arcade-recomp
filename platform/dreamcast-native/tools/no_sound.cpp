// What m2sndrecomp would generate for the sound board's 68000. The recipe's
// tools never build a sound board (m2segment passes sound_enabled=false) and no
// 68000 code is generated for them, but the shared GameLoop links against it.
#include "runtime/snd_gen_support.h"

namespace sndgen {
bool has_code(uint32_t) { return false; }
void run(Env &) {}
} // namespace sndgen
