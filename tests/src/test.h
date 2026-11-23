/*
 * Shared helpers for the bgfx uniform upload integration tests.
 *
 * The tests exercise the path a material system uses to hand groups of
 * transform matrices and vectors to the graphics backends as uniforms:
 *
 *   bgfx::createUniform / bgfx::setUniform / bgfx::setViewUniform (public API)
 *       -> EncoderImpl::setUniform            (src/bgfx_p.h)
 *       -> UniformBuffer opcode stream        (src/bgfx.cpp)
 *       -> rendererUpdateUniforms             (src/bgfx.cpp)
 *       -> backend commit + layout conversion (src/renderer_gl.cpp,
 *                                              src/renderer_vk.cpp)
 *
 * Everything runs against the noop renderer / the device-independent
 * UniformBuffer machinery, so no graphics device is required.
 */

#ifndef BGFX_TESTS_TEST_H_HEADER_GUARD
#define BGFX_TESTS_TEST_H_HEADER_GUARD

#include <bx/bx.h>

#include <catch/catch_amalgamated.hpp>

#include <cstring>

// Records the most recent assert message routed through bx::assertFunction.
// The handler is installed by test_main.cpp via bx::setAssertHandler and
// turns contract-violation asserts into thrown std::exception so tests can
// observe them.
extern char     g_lastAssertMessage[1024];
extern uint32_t g_assertCount;

void resetAssertRecord();
bool lastAssertContains(const char* _substring);

// In debug builds, bgfx upload-contract violations are reported through
// BX_ASSERT (which the test handler converts into a thrown std::exception).
// In release builds the asserts are compiled out, so the check degenerates
// to a no-op (same pattern as bx's own REQUIRE_ASSERTS).
#if BX_CONFIG_DEBUG
#	define REQUIRE_BGFX_ASSERT(_expr, _message)              \
		do {                                                 \
			resetAssertRecord();                             \
			REQUIRE_THROWS_AS(_expr, std::exception);        \
			REQUIRE(lastAssertContains(_message) );          \
		} while (false)
#else
#	define REQUIRE_BGFX_ASSERT(_expr, _message) BX_UNUSED(_message)
#endif // BX_CONFIG_DEBUG

namespace test
{
	// Deterministic, position-dependent value pattern: any element landing
	// at the wrong offset is detected by a plain float comparison.
	inline float pattern(uint32_t _base, uint32_t _index)
	{
		return float(_base * 4096 + _index) + 0.25f;
	}

	// Fills _num consecutive mat4 elements (16 floats each, column-major).
	inline void fillMat4Array(float* _out, uint16_t _num, uint32_t _base)
	{
		for (uint32_t ii = 0; ii < uint32_t(_num) * 16; ++ii)
		{
			_out[ii] = pattern(_base, ii);
		}
	}

	// Fills _num consecutive vec4 elements (4 floats each).
	inline void fillVec4Array(float* _out, uint16_t _num, uint32_t _base)
	{
		for (uint32_t ii = 0; ii < uint32_t(_num) * 4; ++ii)
		{
			_out[ii] = pattern(_base, ii);
		}
	}

	// Fills _num consecutive mat3 elements (9 floats each, column-major).
	inline void fillMat3Array(float* _out, uint16_t _num, uint32_t _base)
	{
		for (uint32_t ii = 0; ii < uint32_t(_num) * 9; ++ii)
		{
			_out[ii] = pattern(_base, ii);
		}
	}
} // namespace test

#endif // BGFX_TESTS_TEST_H_HEADER_GUARD
