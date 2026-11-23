# Uniform upload integration tests

Integration tests for the path a material system uses to hand groups of
transform matrices and vectors to the graphics backends as uniforms:

```
bgfx::createUniform / bgfx::setUniform / bgfx::setViewUniform / bgfx::setFrameUniform
    -> EncoderImpl::setUniform              (src/bgfx_p.h)
    -> UniformBuffer opcode stream          (src/bgfx.cpp)
    -> rendererUpdateUniforms               (src/bgfx.cpp)
    -> backend commit + layout conversion   (src/renderer_gl.cpp, src/renderer_vk.cpp)
```

Everything runs against the noop renderer and the device-independent
`bgfx::UniformBuffer` machinery, so **no graphics device is required**.

## Running

```sh
make -C tests            # build (debug) and run
make -C tests run        # run an already built binary
CONFIG=release make -C tests
```

bx and bimg are expected as siblings of this repository (the standard
bgfx checkout layout); point `BX_DIR`/`BIMG_DIR` elsewhere if not.

The makefile compiles the bgfx amalgamated source with every real
renderer disabled (`BGFX_CONFIG_RENDERER_*=0`, noop stays enabled), so
the test binary links no windowing/graphics frameworks. On macOS the
binary is ad-hoc signed after linking because unsigned x86_64 binaries
can hang at exec while Gatekeeper assesses them.

The out-of-range/frequency rejection checks rely on `BX_ASSERT` and are
only active in the debug configuration (`BX_CONFIG_DEBUG=1`); in release
they compile out (same pattern as bx's own `REQUIRE_ASSERTS`).

## What is covered

### `src/test_uniform_api.cpp` — upload interface contract

Driven entirely through the public API on the noop renderer. The
material model is a skinned-material style parameter set: a matrix group
(`mat4[8]`), a vector group (`vec4[4]`) and a `mat3[2]` group.

- Declaration (name, type, array length) is reflected verbatim by
  `bgfx::getUniformInfo`; empty arrays clamp to one element; uniform
  names are unique and reference counted; redeclaring with a different
  type is rejected.
- Full updates, `UINT16_MAX` (use creation count) updates, partial
  updates and single-element updates are accepted, and a partial update
  does not change the declared array length.
- Updates beyond the declared array length are rejected
  ("Truncated uniform update"), at draw, view and frame frequency
  alike, and the uniform remains usable afterwards.
- Updates submitted at the wrong `bgfx::UniformFreq` are rejected.
- End-to-end smoke test: full material commits with varying valid
  prefixes per frame travel through the encoder into the frame uniform
  stream without tripping any contract check.

### `src/test_uniform_layout.cpp` — backend layout conversion

Replays the real `bgfx::UniformBuffer` opcode stream (the same
`writeUniform`/`writeUniformHandle` calls the encoder and the shader
creation paths emit) and applies each backend's layout contract as
documented in `RendererContextGL::commit` (tightly packed elements fed
to `glUniform*fv`) and `RendererContextVK::commit` (16-byte-register
constant buffer, mat3 expanded to 3 vec4 registers with zeroed w).

- `g_uniformTypeSize` matches the shader declaration contract
  (vec4 = 16, mat3 = 36, mat4 = 64 bytes per element).
- Opcode encode/decode round-trips, including field limits and the
  fragment-stage bit.
- A committed material block (mat4 group + vec4 group + mat3 group)
  survives the stream byte for byte, in declaration order.
- **Cross-backend consistency:** the same material parameters, after
  the OpenGL and the Vulkan layout conversions, produce identical
  values for every element of every uniform array the shader reads.
- Vulkan mat3 expansion zero-pads the fourth component of each row.
- Per-backend shader declaration streams (GL: handle + trailing
  location; Vulkan: register offset/count in the opcode) round-trip.
- A partial update commits only the updated elements: the stream
  carries exactly the valid prefix, and applying it leaves the
  remaining declared elements untouched.
- `setUniformRef` pointer entries resolve to the referenced data.
- Writes beyond the stream capacity are rejected instead of
  overrunning the buffer.

## Files

- `makefile` — builds and runs the suite (debug and release).
- `src/test_main.cpp` — Catch2 runner; installs an assert handler that
  turns bgfx/bx contract violations into catchable exceptions; runs the
  suite inside a headless (noop renderer) bgfx session.
- `src/test.h` — shared helpers (assert recording, deterministic
  material data patterns).
- `src/test_uniform_api.cpp` / `src/test_uniform_layout.cpp` — the
  suites described above.
