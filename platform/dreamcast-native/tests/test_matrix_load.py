"""Exercise the actual render loaders against independent affine expansion."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
library = root.parents[2] / "sh4zam"
functions = []
for path, declaration in (
    ("include/render/models/model_math.h", "static inline void load_matrix("),
    ("code/render/scene/practice.c", "void nm_practice_render_matrix("),
):
    source = (root / path).read_text()
    start = source.index(declaration)
    functions.append(source[start:source.index("\n}", start) + 2])

program = r'''
#include <sh4zam/shz_sh4zam.h>
#include <assert.h>
#include <string.h>
''' + "\n".join(functions) + r'''
int main(void) {
    alignas(32) float storage[16];
    float *input = storage + 1; /* Callers only guarantee float alignment. */
    for (unsigned sample = 0; sample < 4096; ++sample) {
        shz_mat4x4_t expected = {0}, actual;
        for (unsigned c = 0; c < 4; ++c)
            for (unsigned r = 0; r < 3; ++r) {
                float value = ((int)((sample*37+c*19+r*11)%257)-128)*0.125f;
                if (!sample) value = -0.0f;
                input[c*3+r] = expected.elem[c*4+r] = value;
            }
        expected.elem[15] = 1.0f;
        load_matrix(input);
        shz_xmtrx_store_4x4(&actual);
        assert(!memcmp(&actual, &expected, sizeof(actual)));
        nm_practice_render_matrix(input);
        shz_xmtrx_store_4x4(&actual);
        assert(!memcmp(&actual, &expected, sizeof(actual)));
    }
}
'''
with tempfile.TemporaryDirectory(prefix="daytona-matrix-") as directory:
    source = Path(directory) / "check.c"
    binary = Path(directory) / "check"
    source.write_text(program)
    subprocess.run(["cc", "-std=gnu11", "-O1", "-fsanitize=address,undefined",
                    "-I", str(library / "include"), str(source),
                    str(library / "source/sw/shz_xmtrx_sw.c"), "-lm",
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("PASS: both SH4ZAM affine loaders, 4096 matrices, float-aligned input")
