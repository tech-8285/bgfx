/*
 * Integration tests for the uniform upload interface contract.
 *
 * A material declares groups of transform matrices and vectors as uniform
 * arrays (bgfx::createUniform) and uploads them per draw/view/frame
 * (bgfx::setUniform, bgfx::setViewUniform, bgfx::setFrameUniform). These
 * tests pin down the contract of that interface:
 *
 *   - the declaration (name, type, array length) is reflected verbatim;
 *   - full, default (UINT16_MAX) and partial updates are accepted;
 *   - a partial update does not change the declared array length;
 *   - updates beyond the declared array length are rejected
 *     ("Truncated uniform update", src/bgfx.cpp, Encoder::setUniform);
 *   - updates submitted at the wrong frequency are rejected.
 *
 * Everything runs on the noop renderer, so no graphics device is needed.
 */

#include "test.h"

#include <bgfx/bgfx.h>

namespace
{
	// The material parameter set used across the suite: a group of
	// transform matrices plus a group of vectors, mirroring a skinned
	// material with lighting parameters.
	constexpr uint16_t kNumBones   = 8; // u_boneMatrices : mat4[8]
	constexpr uint16_t kNumLights  = 4; // u_lightParams  : vec4[4]
	constexpr uint16_t kNumNormals = 2; // u_normalMatrix : mat3[2]
}

TEST_CASE("uniform declaration is reflected verbatim", "[uniform][api]")
{
	const bgfx::UniformHandle bones   = bgfx::createUniform("api_boneMatrices", bgfx::UniformType::Mat4, kNumBones);
	const bgfx::UniformHandle lights  = bgfx::createUniform("api_lightParams",  bgfx::UniformType::Vec4, kNumLights);
	const bgfx::UniformHandle normals = bgfx::createUniform("api_normalMatrix", bgfx::UniformType::Mat3, kNumNormals);

	REQUIRE(bgfx::isValid(bones) );
	REQUIRE(bgfx::isValid(lights) );
	REQUIRE(bgfx::isValid(normals) );

	bgfx::UniformInfo info;

	bgfx::getUniformInfo(bones, info);
	REQUIRE(0 == std::strcmp(info.name, "api_boneMatrices") );
	REQUIRE(info.type == bgfx::UniformType::Mat4);
	REQUIRE(info.num  == kNumBones);

	bgfx::getUniformInfo(lights, info);
	REQUIRE(0 == std::strcmp(info.name, "api_lightParams") );
	REQUIRE(info.type == bgfx::UniformType::Vec4);
	REQUIRE(info.num  == kNumLights);

	bgfx::getUniformInfo(normals, info);
	REQUIRE(0 == std::strcmp(info.name, "api_normalMatrix") );
	REQUIRE(info.type == bgfx::UniformType::Mat3);
	REQUIRE(info.num  == kNumNormals);

	bgfx::destroy(bones);
	bgfx::destroy(lights);
	bgfx::destroy(normals);
}

TEST_CASE("uniform declaration clamps empty arrays to one element", "[uniform][api]")
{
	// Context::createUniform clamps _num to >= 1 (src/bgfx_p.h).
	const bgfx::UniformHandle handle = bgfx::createUniform("api_clamped", bgfx::UniformType::Vec4, 0);
	REQUIRE(bgfx::isValid(handle) );

	bgfx::UniformInfo info;
	bgfx::getUniformInfo(handle, info);
	REQUIRE(info.num == 1);

	bgfx::destroy(handle);
}

TEST_CASE("uniform names are unique and reference counted", "[uniform][api]")
{
	// Contract from the bgfx::createUniform remarks: creating a uniform
	// with the same name returns the same handle and bumps the refcount.
	const bgfx::UniformHandle first  = bgfx::createUniform("api_shared", bgfx::UniformType::Vec4, kNumLights);
	const bgfx::UniformHandle second = bgfx::createUniform("api_shared", bgfx::UniformType::Vec4, kNumLights);

	REQUIRE(bgfx::isValid(first) );
	REQUIRE(first.idx == second.idx);

	bgfx::UniformInfo info;
	bgfx::getUniformInfo(first, info);
	REQUIRE(info.num == kNumLights);

	bgfx::destroy(first);
	bgfx::destroy(second);
}

TEST_CASE("redeclaring a uniform with a different type is rejected", "[uniform][api]")
{
	const bgfx::UniformHandle handle = bgfx::createUniform("api_typed", bgfx::UniformType::Vec4, kNumLights);
	REQUIRE(bgfx::isValid(handle) );

	REQUIRE_BGFX_ASSERT(
		bgfx::createUniform("api_typed", bgfx::UniformType::Mat4, kNumLights)
		, "Uniform type mismatch"
		);

	// The original declaration is untouched by the rejected redeclaration.
	bgfx::UniformInfo info;
	bgfx::getUniformInfo(handle, info);
	REQUIRE(info.type == bgfx::UniformType::Vec4);
	REQUIRE(info.num  == kNumLights);

	bgfx::destroy(handle);
}

TEST_CASE("full and partial uniform updates are accepted", "[uniform][api]")
{
	const bgfx::UniformHandle bones  = bgfx::createUniform("api_updateBones",  bgfx::UniformType::Mat4, kNumBones);
	const bgfx::UniformHandle lights = bgfx::createUniform("api_updateLights", bgfx::UniformType::Vec4, kNumLights);
	REQUIRE(bgfx::isValid(bones) );
	REQUIRE(bgfx::isValid(lights) );

	float boneData[kNumBones * 16];
	float lightData[kNumLights * 4];
	test::fillMat4Array(boneData,  kNumBones,  1);
	test::fillVec4Array(lightData, kNumLights, 2);

	resetAssertRecord();

	// Full update: every declared element in one commit.
	bgfx::setUniform(bones, boneData, kNumBones);
	bgfx::frame();

	// UINT16_MAX maps to the count passed at creation (contract of
	// bgfx::setUniform's _num argument).
	bgfx::setUniform(bones, boneData, UINT16_MAX);
	bgfx::frame();

	// Partial update: only the first 3 elements are committed.
	bgfx::setUniform(bones, boneData, 3);
	bgfx::frame();

	// Single-element update (the public API default of _num = 1).
	bgfx::setUniform(lights, lightData);
	bgfx::frame();

	// Several material parameters in one commit.
	bgfx::setUniform(bones,  boneData,  kNumBones);
	bgfx::setUniform(lights, lightData, kNumLights);
	bgfx::frame();

	REQUIRE(0 == g_assertCount);

	// Partial updates must not change the declared array length.
	bgfx::UniformInfo info;
	bgfx::getUniformInfo(bones, info);
	REQUIRE(info.num == kNumBones);
	bgfx::getUniformInfo(lights, info);
	REQUIRE(info.num == kNumLights);

	bgfx::destroy(bones);
	bgfx::destroy(lights);
}

TEST_CASE("uniform update beyond the declared range is rejected", "[uniform][api]")
{
	const bgfx::UniformHandle bones  = bgfx::createUniform("api_rangeBones",  bgfx::UniformType::Mat4, kNumBones);
	const bgfx::UniformHandle lights = bgfx::createUniform("api_rangeLights", bgfx::UniformType::Vec4, kNumLights);
	REQUIRE(bgfx::isValid(bones) );
	REQUIRE(bgfx::isValid(lights) );

	// One element past the end of the declared arrays.
	float boneData[(kNumBones + 1) * 16];
	float lightData[(kNumLights + 1) * 4];
	test::fillMat4Array(boneData,  kNumBones + 1,  3);
	test::fillVec4Array(lightData, kNumLights + 1, 4);

	// Encoder::setUniform asserts "Truncated uniform update" when
	// _num exceeds the declared count (src/bgfx.cpp).
	REQUIRE_BGFX_ASSERT(
		bgfx::setUniform(bones, boneData, kNumBones + 1)
		, "Truncated uniform update"
		);

	REQUIRE_BGFX_ASSERT(
		bgfx::setUniform(lights, lightData, kNumLights + 1)
		, "Truncated uniform update"
		);

	// The uniform remains fully usable after the rejected updates:
	// the rejection happened before anything was committed.
	resetAssertRecord();
	bgfx::setUniform(bones,  boneData,  kNumBones);
	bgfx::setUniform(lights, lightData, kNumLights);
	bgfx::frame();
	REQUIRE(0 == g_assertCount);

	bgfx::destroy(bones);
	bgfx::destroy(lights);
}

TEST_CASE("view and frame uniform updates follow the same range contract", "[uniform][api]")
{
	const bgfx::UniformHandle viewUniform  = bgfx::createUniform("api_viewParams",  bgfx::UniformFreq::View,  bgfx::UniformType::Mat4, kNumBones);
	const bgfx::UniformHandle frameUniform = bgfx::createUniform("api_frameParams", bgfx::UniformFreq::Frame, bgfx::UniformType::Vec4, kNumLights);
	REQUIRE(bgfx::isValid(viewUniform) );
	REQUIRE(bgfx::isValid(frameUniform) );

	float boneData[kNumBones * 16];
	float lightData[kNumLights * 4];
	test::fillMat4Array(boneData,  kNumBones,  5);
	test::fillVec4Array(lightData, kNumLights, 6);

	resetAssertRecord();

	// Full and partial updates at view and frame frequency.
	bgfx::setViewUniform(0, viewUniform, boneData, kNumBones);
	bgfx::setViewUniform(0, viewUniform, boneData, 3);
	bgfx::setFrameUniform(frameUniform, lightData, kNumLights);
	bgfx::setFrameUniform(frameUniform, lightData, 2);
	bgfx::frame();

	REQUIRE(0 == g_assertCount);

	// Out-of-range updates are rejected at these frequencies too
	// (UniformCache::setViewUniform, src/bgfx_p.h).
	REQUIRE_BGFX_ASSERT(
		bgfx::setViewUniform(0, viewUniform, boneData, kNumBones + 1)
		, "Truncated uniform update"
		);

	REQUIRE_BGFX_ASSERT(
		bgfx::setFrameUniform(frameUniform, lightData, kNumLights + 1)
		, "Truncated uniform update"
		);

	bgfx::destroy(viewUniform);
	bgfx::destroy(frameUniform);
}

TEST_CASE("uniform updates at the wrong frequency are rejected", "[uniform][api]")
{
	const bgfx::UniformHandle drawUniform  = bgfx::createUniform("api_freqDraw",  bgfx::UniformFreq::Draw,  bgfx::UniformType::Vec4, kNumLights);
	const bgfx::UniformHandle viewUniform  = bgfx::createUniform("api_freqView",  bgfx::UniformFreq::View,  bgfx::UniformType::Vec4, kNumLights);
	const bgfx::UniformHandle frameUniform = bgfx::createUniform("api_freqFrame", bgfx::UniformFreq::Frame, bgfx::UniformType::Vec4, kNumLights);
	REQUIRE(bgfx::isValid(drawUniform) );
	REQUIRE(bgfx::isValid(viewUniform) );
	REQUIRE(bgfx::isValid(frameUniform) );

	float lightData[kNumLights * 4];
	test::fillVec4Array(lightData, kNumLights, 7);

	// A view-frequency uniform cannot be committed per draw.
	REQUIRE_BGFX_ASSERT(
		bgfx::setUniform(viewUniform, lightData, kNumLights)
		, "different bgfx::UniformFreq::Enum"
		);

	// A draw-frequency uniform cannot be committed per view or per frame.
	REQUIRE_BGFX_ASSERT(
		bgfx::setViewUniform(0, drawUniform, lightData, kNumLights)
		, "different bgfx::UniformFreq::Enum"
		);

	REQUIRE_BGFX_ASSERT(
		bgfx::setFrameUniform(drawUniform, lightData, kNumLights)
		, "different bgfx::UniformFreq::Enum"
		);

	// Each uniform still accepts updates at its declared frequency.
	resetAssertRecord();
	bgfx::setUniform(drawUniform, lightData, kNumLights);
	bgfx::setViewUniform(0, viewUniform, lightData, kNumLights);
	bgfx::setFrameUniform(frameUniform, lightData, kNumLights);
	bgfx::frame();
	REQUIRE(0 == g_assertCount);

	bgfx::destroy(drawUniform);
	bgfx::destroy(viewUniform);
	bgfx::destroy(frameUniform);
}

TEST_CASE("material parameter block uploads end to end", "[uniform][api]")
{
	// Smoke test: a full material commit (matrix group + vector group +
	// mat3 group, full and partial updates) travels through the encoder
	// into the frame uniform stream and is consumed by the render side
	// without tripping any contract check.
	const bgfx::UniformHandle bones   = bgfx::createUniform("api_e2eBones",   bgfx::UniformType::Mat4, kNumBones);
	const bgfx::UniformHandle lights  = bgfx::createUniform("api_e2eLights",  bgfx::UniformType::Vec4, kNumLights);
	const bgfx::UniformHandle normals = bgfx::createUniform("api_e2eNormals", bgfx::UniformType::Mat3, kNumNormals);
	REQUIRE(bgfx::isValid(bones) );
	REQUIRE(bgfx::isValid(lights) );
	REQUIRE(bgfx::isValid(normals) );

	float boneData[kNumBones * 16];
	float lightData[kNumLights * 4];
	float normalData[kNumNormals * 9];
	test::fillMat4Array(boneData,  kNumBones,   8);
	test::fillVec4Array(lightData, kNumLights,  9);
	test::fillMat3Array(normalData, kNumNormals, 10);

	resetAssertRecord();

	for (uint32_t frame = 0; frame < 4; ++frame)
	{
		// Vary the committed prefix of each array per frame, as a
		// material would when only some of its parameters animate.
		const uint16_t numBones   = uint16_t(1 + (frame % kNumBones) );
		const uint16_t numLights  = uint16_t(1 + (frame % kNumLights) );
		const uint16_t numNormals = uint16_t(1 + (frame % kNumNormals) );

		bgfx::setUniform(bones,   boneData,   numBones);
		bgfx::setUniform(lights,  lightData,  numLights);
		bgfx::setUniform(normals, normalData, numNormals);

		bgfx::touch(0);
		bgfx::frame();
	}

	REQUIRE(0 == g_assertCount);

	bgfx::destroy(bones);
	bgfx::destroy(lights);
	bgfx::destroy(normals);
}
