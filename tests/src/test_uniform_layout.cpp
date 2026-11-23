/*
 * Integration tests for the uniform layout conversion shared by the
 * graphics backends.
 *
 * The upload path serializes committed material parameters into a
 * bgfx::UniformBuffer opcode stream (EncoderImpl::setUniform ->
 * UniformBuffer::writeUniform, src/bgfx_p.h / src/bgfx.cpp). At draw time
 * each backend replays that stream and converts the payload into the
 * layout its shader consumes:
 *
 *   - OpenGL (RendererContextGL::commit, src/renderer_gl.cpp): tightly
 *     packed elements handed to glUniform*fv - 16 bytes per vec4,
 *     64 bytes per mat4, 36 bytes per mat3.
 *   - Vulkan (RendererContextVK::commit, src/renderer_vk.cpp): payload
 *     copied into a 16-byte-register constant buffer at the declared
 *     byte offset; each mat3 expands to 3 vec4 registers with zeroed w.
 *
 * These tests replay the real UniformBuffer stream through both layout
 * contracts and verify the shader reads identical array elements on both
 * backends. They also pin down the partial-update semantics: a commit
 * only carries the elements it updates, and applying it leaves the rest
 * of the declared array untouched.
 *
 * Only the device-independent machinery is used, so no graphics device
 * is required.
 */

#include "bgfx_p.h" // must precede test.h: it overrides the BX_* macros

#include "test.h"

#include <vector>

namespace
{
	using bgfx::UniformBuffer;
	using bgfx::UniformType;

	// The material parameter set: a group of transform matrices, a group
	// of vectors, and a mat3 group to cover the mat3 layout conversion.
	constexpr uint16_t kNumBones   = 8; // u_boneMatrices : mat4[8]
	constexpr uint16_t kNumLights  = 4; // u_lightParams  : vec4[4]
	constexpr uint16_t kNumNormals = 2; // u_normalMatrix : mat3[2]

	// Uniform handles as the renderer would see them (indices into the
	// renderer-side uniform store, RendererContextI::m_uniforms).
	constexpr uint16_t kBonesHandle   = 0;
	constexpr uint16_t kLightsHandle  = 1;
	constexpr uint16_t kNormalsHandle = 2;
	constexpr uint16_t kNumUniforms   = 3;

	// One shader's uniform declarations. The GL location and the Vulkan
	// constant-buffer byte offset are what shader reflection produces
	// for each backend (renderer_gl.cpp / renderer_vk.cpp shader create).
	struct UniformDecl
	{
		const char*           name;
		UniformType::Enum     type;
		uint16_t              num;       // declared array elements
		uint16_t              handle;    // renderer store index
		uint32_t              glLocation;
		uint32_t              vkOffset;  // byte offset in constant buffer
	};

	// Register cost of one array element in the Vulkan constant buffer
	// (16-byte registers): vec4 = 1, mat4 = 4, mat3 = 3.
	uint32_t vkRegsPerElement(UniformType::Enum _type)
	{
		return UniformType::Mat4 == _type ? 4
			:  UniformType::Mat3 == _type ? 3
			:  1
			;
	}

	// Renderer-side uniform store: models RendererContextI::m_uniforms
	// (src/bgfx_p.h), one zero-initialized, 16-byte-aligned block per
	// declared uniform, sized by the shared g_uniformTypeSize table.
	struct UniformStore
	{
		void create(const UniformDecl& _decl)
		{
			const uint32_t size = bx::alignUp(bgfx::g_uniformTypeSize[_decl.type] * _decl.num, 16);
			data[_decl.handle].assign(size, 0);
			decl[_decl.handle] = &_decl;
		}

		void*       at(uint16_t _handle)       { return data[_handle].data(); }
		const void* at(uint16_t _handle) const { return data[_handle].data(); }

		std::vector<uint8_t> data[kNumUniforms];
		const UniformDecl*   decl[kNumUniforms] = {};
	};

	// Replays an encoder data stream into the uniform store, following
	// Context::rendererUpdateUniforms (src/bgfx.cpp): each commit copies
	// exactly g_uniformTypeSize[type]*num bytes - the elements of this
	// commit and nothing more.
	void applyDataStream(UniformBuffer& _stream, UniformStore& _store)
	{
		_stream.reset();

		for (;;)
		{
			const uint32_t opcode = _stream.read();

			if (UniformType::End == opcode)
			{
				break;
			}

			uint8_t  type;
			uint16_t loc;
			uint16_t num;
			uint16_t copy;
			UniformBuffer::decodeOpcode(opcode, type, loc, num, copy);
			REQUIRE(0 != copy);

			const uint32_t size = bgfx::g_uniformTypeSize[type] * num;
			const char*    data = _stream.read(size);

			// RendererContextI::updateUniform (src/bgfx_p.h).
			bx::memCopy(_store.at(loc), data, size);
		}
	}

	// Converts the store into what the OpenGL backend hands to the
	// shader (RendererContextGL::commit, src/renderer_gl.cpp): tightly
	// packed elements, g_uniformTypeSize[type] bytes each.
	void glReadShaderView(const UniformStore& _store, const UniformDecl& _decl, float* _out)
	{
		const uint32_t numFloats = bgfx::g_uniformTypeSize[_decl.type] * _decl.num / sizeof(float);
		bx::memCopy(_out, _store.at(_decl.handle), numFloats * sizeof(float) );
	}

	// Converts the store into what the Vulkan backend's shader reads
	// (RendererContextVK::commit + setShaderUniform, src/renderer_vk.cpp):
	// numRegs*16 bytes copied to the declared byte offset; mat3 elements
	// expand to 3 vec4 registers with zeroed w.
	void vkWriteConstantBuffer(const UniformStore& _store, const UniformDecl& _decl, uint8_t* _scratch)
	{
		const float* data = reinterpret_cast<const float*>(_store.at(_decl.handle) );

		if (UniformType::Mat3 == _decl.type)
		{
			for (uint32_t elem = 0; elem < _decl.num; ++elem)
			{
				float* dst = reinterpret_cast<float*>(_scratch + _decl.vkOffset + elem * 3 * 16);

				for (uint32_t row = 0; row < 3; ++row)
				{
					dst[row * 4 + 0] = data[elem * 9 + row * 3 + 0];
					dst[row * 4 + 1] = data[elem * 9 + row * 3 + 1];
					dst[row * 4 + 2] = data[elem * 9 + row * 3 + 2];
					dst[row * 4 + 3] = 0.0f;
				}
			}
		}
		else
		{
			const uint32_t numRegs = _decl.num * vkRegsPerElement(_decl.type);
			bx::memCopy(_scratch + _decl.vkOffset, data, numRegs * 16);
		}
	}

	// Reads one array element back from the Vulkan constant buffer the
	// way the shader sees it: vec4 = 4 floats, mat4 = 16 floats, mat3 =
	// 3 rows of vec4 of which only xyz are meaningful.
	void vkReadShaderElement(const uint8_t* _scratch, const UniformDecl& _decl, uint32_t _elem, float* _out)
	{
		const uint32_t elemBytes = vkRegsPerElement(_decl.type) * 16;
		const float*   src       = reinterpret_cast<const float*>(_scratch + _decl.vkOffset + _elem * elemBytes);

		if (UniformType::Mat3 == _decl.type)
		{
			for (uint32_t row = 0; row < 3; ++row)
			{
				_out[row * 3 + 0] = src[row * 4 + 0];
				_out[row * 3 + 1] = src[row * 4 + 1];
				_out[row * 3 + 2] = src[row * 4 + 2];
			}
		}
		else
		{
			const uint32_t numFloats = bgfx::g_uniformTypeSize[_decl.type] / sizeof(float);
			bx::memCopy(_out, src, numFloats * sizeof(float) );
		}
	}

	uint32_t floatsPerElement(UniformType::Enum _type)
	{
		return bgfx::g_uniformTypeSize[_type] / sizeof(float);
	}
}

TEST_CASE("uniform type sizes match the shader declaration contract", "[uniform][layout]")
{
	// Both backends index the payload with this table, so it defines how
	// many bytes one array element of each declared type occupies.
	REQUIRE(bgfx::g_uniformTypeSize[UniformType::Vec4] == 16);
	REQUIRE(bgfx::g_uniformTypeSize[UniformType::Mat3] == 36);
	REQUIRE(bgfx::g_uniformTypeSize[UniformType::Mat4] == 64);
}

TEST_CASE("uniform opcode encoding round-trips", "[uniform][layout]")
{
	struct
	{
		uint8_t  type;
		uint16_t loc;
		uint16_t num;
		uint16_t copy;
	} cases[] =
	{
		{ uint8_t(UniformType::Vec4),                          0,    1, 1 },
		{ uint8_t(UniformType::Mat4),                         11,    8, 1 },
		{ uint8_t(UniformType::Mat3),                       1024,    2, 1 },
		{ uint8_t(UniformType::Mat4),                      65535, 1023, 1 }, // field limits
		{ uint8_t(UniformType::Vec4 | bgfx::kUniformFragmentBit), 7, 4, 0 },
	};

	for (const auto& expected : cases)
	{
		const uint32_t opcode = UniformBuffer::encodeOpcode(expected.type, expected.loc, expected.num, expected.copy);

		uint8_t  type;
		uint16_t loc;
		uint16_t num;
		uint16_t copy;
		UniformBuffer::decodeOpcode(opcode, type, loc, num, copy);

		REQUIRE(type  == expected.type);
		REQUIRE(loc   == expected.loc);
		REQUIRE(num   == expected.num);
		REQUIRE(copy  == expected.copy);
	}
}

TEST_CASE("uniform data stream preserves the committed material block", "[uniform][layout]")
{
	float bones[kNumBones * 16];
	float lights[kNumLights * 4];
	float normals[kNumNormals * 9];
	test::fillMat4Array(bones,   kNumBones,   1);
	test::fillVec4Array(lights,  kNumLights,  2);
	test::fillMat3Array(normals, kNumNormals, 3);

	// Encode exactly what EncoderImpl::setUniform writes for one commit
	// of the whole material (src/bgfx_p.h: uniformBuffer->writeUniform).
	UniformBuffer* stream = UniformBuffer::create(1 << 16);
	stream->writeUniform(UniformType::Mat4, kBonesHandle,   bones,   kNumBones);
	stream->writeUniform(UniformType::Vec4, kLightsHandle,  lights,  kNumLights);
	stream->writeUniform(UniformType::Mat3, kNormalsHandle, normals, kNumNormals);
	stream->finish();

	// Replay the stream the way the render side does and verify every
	// array element arrives in order, byte for byte.
	struct Expected
	{
		UniformType::Enum type;
		uint16_t          loc;
		uint16_t          num;
		const float*      data;
	} expected[] =
	{
		{ UniformType::Mat4, kBonesHandle,   kNumBones,   bones   },
		{ UniformType::Vec4, kLightsHandle,  kNumLights,  lights  },
		{ UniformType::Mat3, kNormalsHandle, kNumNormals, normals },
	};

	stream->reset();

	for (const auto& entry : expected)
	{
		const uint32_t opcode = stream->read();
		REQUIRE(UniformType::End != opcode);

		uint8_t  type;
		uint16_t loc;
		uint16_t num;
		uint16_t copy;
		UniformBuffer::decodeOpcode(opcode, type, loc, num, copy);

		REQUIRE(type == uint8_t(entry.type) );
		REQUIRE(loc  == entry.loc);
		REQUIRE(num  == entry.num);
		REQUIRE(copy == 1);

		const uint32_t size = bgfx::g_uniformTypeSize[entry.type] * entry.num;
		const char*    data = stream->read(size);
		REQUIRE(0 == bx::memCmp(data, entry.data, size) );
	}

	// The stream is terminated exactly once.
	REQUIRE(UniformType::End == stream->read() );

	UniformBuffer::destroy(stream);
}

TEST_CASE("GL and Vulkan layout conversions read identical array elements", "[uniform][layout]")
{
	// The shader's uniform declarations. Vulkan constant-buffer offsets
	// are assigned sequentially in 16-byte registers, as shader
	// reflection would emit them.
	UniformDecl decls[kNumUniforms] =
	{
		{ "u_boneMatrices", UniformType::Mat4, kNumBones,   kBonesHandle,   0, 0 },
		{ "u_lightParams",  UniformType::Vec4, kNumLights,  kLightsHandle,  8, 0 },
		{ "u_normalMatrix", UniformType::Mat3, kNumNormals, kNormalsHandle, 9, 0 },
	};

	uint32_t vkOffset = 0;
	for (UniformDecl& decl : decls)
	{
		decl.vkOffset = vkOffset;
		vkOffset += decl.num * vkRegsPerElement(decl.type) * 16;
	}

	// The material's parameters for this commit.
	float bones[kNumBones * 16];
	float lights[kNumLights * 4];
	float normals[kNumNormals * 9];
	test::fillMat4Array(bones,   kNumBones,   4);
	test::fillVec4Array(lights,  kNumLights,  5);
	test::fillMat3Array(normals, kNumNormals, 6);

	// Upload the commit through the real encoder-side machinery and
	// apply it to the renderer-side store.
	UniformStore store;
	for (const UniformDecl& decl : decls)
	{
		store.create(decl);
	}

	UniformBuffer* stream = UniformBuffer::create(1 << 16);
	stream->writeUniform(UniformType::Mat4, kBonesHandle,   bones,   kNumBones);
	stream->writeUniform(UniformType::Vec4, kLightsHandle,  lights,  kNumLights);
	stream->writeUniform(UniformType::Mat3, kNormalsHandle, normals, kNumNormals);
	stream->finish();

	applyDataStream(*stream, store);
	UniformBuffer::destroy(stream);

	// Vulkan constant buffer image, built by the backend's layout
	// conversion from the same store.
	std::vector<uint8_t> vkScratch(vkOffset, 0);
	for (const UniformDecl& decl : decls)
	{
		vkWriteConstantBuffer(store, decl, vkScratch.data() );
	}

	// Same material parameters, both layout conversions: the shader
	// must read identical array elements.
	for (const UniformDecl& decl : decls)
	{
		const uint32_t numFloats = floatsPerElement(decl.type);

		std::vector<float> glView(numFloats * decl.num);
		glReadShaderView(store, decl, glView.data() );

		for (uint32_t elem = 0; elem < decl.num; ++elem)
		{
			float vkElement[16];
			vkReadShaderElement(vkScratch.data(), decl, elem, vkElement);

			for (uint32_t comp = 0; comp < numFloats; ++comp)
			{
				INFO(decl.name << " element " << elem << " component " << comp);
				REQUIRE(glView[elem * numFloats + comp] == vkElement[comp]);
			}
		}
	}
}

TEST_CASE("Vulkan mat3 expansion zero-pads the fourth component", "[uniform][layout]")
{
	constexpr uint16_t kNum = 2;

	UniformDecl decl = { "u_normalMatrix", UniformType::Mat3, kNum, kNormalsHandle, 0, 0 };

	float normals[kNum * 9];
	test::fillMat3Array(normals, kNum, 7);

	UniformStore store;
	store.create(decl);

	UniformBuffer* stream = UniformBuffer::create(1 << 16);
	stream->writeUniform(UniformType::Mat3, kNormalsHandle, normals, kNum);
	stream->finish();
	applyDataStream(*stream, store);
	UniformBuffer::destroy(stream);

	std::vector<uint8_t> vkScratch(kNum * 3 * 16, 0);
	vkWriteConstantBuffer(store, decl, vkScratch.data() );

	const float* regs = reinterpret_cast<const float*>(vkScratch.data() );
	for (uint32_t elem = 0; elem < kNum; ++elem)
	{
		for (uint32_t row = 0; row < 3; ++row)
		{
			// xyz come from the tightly packed mat3, w is padding.
			REQUIRE(regs[elem * 12 + row * 4 + 0] == normals[elem * 9 + row * 3 + 0]);
			REQUIRE(regs[elem * 12 + row * 4 + 1] == normals[elem * 9 + row * 3 + 1]);
			REQUIRE(regs[elem * 12 + row * 4 + 2] == normals[elem * 9 + row * 3 + 2]);
			REQUIRE(regs[elem * 12 + row * 4 + 3] == 0.0f);
		}
	}
}

TEST_CASE("shader declaration streams round-trip per backend", "[uniform][layout]")
{
	// At shader creation each backend records the uniform's handle and
	// shader-visible location into a per-program UniformBuffer
	// (renderer_gl.cpp / renderer_vk.cpp shader create).
	const bgfx::UniformHandle handle = { kLightsHandle };

	// OpenGL: opcode carries the handle, the 4-byte location follows.
	UniformBuffer* glDecl = UniformBuffer::create(1024);
	glDecl->writeUniformHandle(uint8_t(UniformType::Vec4), 0, handle, kNumLights);
	glDecl->write(7); // glGetUniformLocation result
	glDecl->finish();

	glDecl->reset();
	{
		const uint32_t opcode = glDecl->read();
		REQUIRE(UniformType::End != opcode);

		uint8_t  type;
		uint16_t loc;
		uint16_t num;
		uint16_t copy;
		UniformBuffer::decodeOpcode(opcode, type, loc, num, copy);
		REQUIRE(type == uint8_t(UniformType::Vec4) );
		REQUIRE(num  == kNumLights);
		REQUIRE(copy == 0);

		bgfx::UniformHandle decoded;
		bx::memCopy(&decoded, glDecl->read(sizeof(bgfx::UniformHandle) ), sizeof(bgfx::UniformHandle) );
		REQUIRE(decoded.idx == kLightsHandle);

		REQUIRE(7 == glDecl->read() ); // trailing GL location
		REQUIRE(UniformType::End == glDecl->read() );
	}
	UniformBuffer::destroy(glDecl);

	// Vulkan: opcode carries the constant-buffer byte offset and the
	// register count; the fragment bit marks the shader stage.
	UniformBuffer* vkDecl = UniformBuffer::create(1024);
	vkDecl->writeUniformHandle(uint8_t(UniformType::Vec4) | bgfx::kUniformFragmentBit, 1024, handle, kNumLights);
	vkDecl->finish();

	vkDecl->reset();
	{
		const uint32_t opcode = vkDecl->read();
		REQUIRE(UniformType::End != opcode);

		uint8_t  type;
		uint16_t loc;
		uint16_t num;
		uint16_t copy;
		UniformBuffer::decodeOpcode(opcode, type, loc, num, copy);
		REQUIRE((type & ~bgfx::kUniformMask) == uint8_t(UniformType::Vec4) );
		REQUIRE(0 != (type & bgfx::kUniformFragmentBit) );
		REQUIRE(loc  == 1024);
		REQUIRE(num  == kNumLights);
		REQUIRE(copy == 0);

		bgfx::UniformHandle decoded;
		bx::memCopy(&decoded, vkDecl->read(sizeof(bgfx::UniformHandle) ), sizeof(bgfx::UniformHandle) );
		REQUIRE(decoded.idx == kLightsHandle);
		REQUIRE(UniformType::End == vkDecl->read() );
	}
	UniformBuffer::destroy(vkDecl);
}

TEST_CASE("partial update commits only the updated elements", "[uniform][layout]")
{
	UniformDecl decl = { "u_boneMatrices", UniformType::Mat4, kNumBones, kBonesHandle, 0, 0 };

	// The store starts from a known state, as if earlier commits had
	// already uploaded a full set of matrices.
	UniformStore store;
	store.create(decl);

	float initial[kNumBones * 16];
	test::fillMat4Array(initial, kNumBones, 8);

	UniformBuffer* full = UniformBuffer::create(1 << 16);
	full->writeUniform(UniformType::Mat4, kBonesHandle, initial, kNumBones);
	full->finish();
	applyDataStream(*full, store);
	UniformBuffer::destroy(full);

	// A partial commit updates only the first 3 elements.
	constexpr uint16_t kPartial = 3;

	float updated[kPartial * 16];
	test::fillMat4Array(updated, kPartial, 9);

	UniformBuffer* partial = UniformBuffer::create(1 << 16);
	partial->writeUniform(UniformType::Mat4, kBonesHandle, updated, kPartial);
	partial->finish();

	// The stream carries exactly this commit's valid parameters: 3
	// elements, 3*64 payload bytes, nothing for the remaining 5.
	partial->reset();
	{
		const uint32_t opcode = partial->read();
		uint8_t  type;
		uint16_t loc;
		uint16_t num;
		uint16_t copy;
		UniformBuffer::decodeOpcode(opcode, type, loc, num, copy);
		REQUIRE(type == uint8_t(UniformType::Mat4) );
		REQUIRE(num  == kPartial);
		REQUIRE(bgfx::g_uniformTypeSize[type] * num == kPartial * 64);
	}
	applyDataStream(*partial, store);
	UniformBuffer::destroy(partial);

	// The first 3 elements hold the new values...
	const float* storeData = reinterpret_cast<const float*>(store.at(kBonesHandle) );
	for (uint32_t ii = 0; ii < kPartial * 16; ++ii)
	{
		REQUIRE(storeData[ii] == updated[ii]);
	}

	// ...and the remaining 5 elements are untouched by this commit.
	for (uint32_t elem = kPartial; elem < kNumBones; ++elem)
	{
		for (uint32_t comp = 0; comp < 16; ++comp)
		{
			const uint32_t ii = elem * 16 + comp;
			INFO("element " << elem << " component " << comp);
			REQUIRE(storeData[ii] == initial[ii]);
		}
	}
}

TEST_CASE("uniform reference stream entries resolve to the referenced data", "[uniform][layout]")
{
	// bgfx::setUniformRef commits a pointer instead of a copy; the
	// render side dereferences it when applying the update
	// (Context::rendererUpdateUniforms, src/bgfx.cpp).
	float lights[kNumLights * 4];
	test::fillVec4Array(lights, kNumLights, 10);

	UniformBuffer* stream = UniformBuffer::create(1024);
	stream->writeUniformRef(UniformType::Vec4, kLightsHandle, lights, kNumLights);
	stream->finish();

	stream->reset();

	const uint32_t opcode = stream->read();
	REQUIRE(UniformType::End != opcode);

	uint8_t  type;
	uint16_t loc;
	uint16_t num;
	uint16_t copy;
	UniformBuffer::decodeOpcode(opcode, type, loc, num, copy);
	REQUIRE(0 != (type & bgfx::kUniformRefBit) );
	REQUIRE((type & ~bgfx::kUniformRefBit) == uint8_t(UniformType::Vec4) );
	REQUIRE(loc == kLightsHandle);
	REQUIRE(num == kNumLights);

	const void* ptr = bx::loadUnaligned<const void*>(stream->read(sizeof(ptr) ) );
	REQUIRE(ptr == lights);
	REQUIRE(0 == bx::memCmp(ptr, lights, sizeof(lights) ) );

	REQUIRE(UniformType::End == stream->read() );

	UniformBuffer::destroy(stream);
}

TEST_CASE("uniform stream rejects writes beyond its capacity", "[uniform][layout]")
{
	// UniformBuffer::write asserts before overrunning the buffer
	// (src/bgfx_p.h), so a malformed commit cannot corrupt the stream.
	UniformBuffer* stream = UniformBuffer::create(64);

	float mtx[16];
	test::fillMat4Array(mtx, 1, 11);

	// 4-byte opcode + 64-byte mat4 payload does not fit in 64 bytes.
	REQUIRE_BGFX_ASSERT(
		stream->writeUniform(UniformType::Mat4, kBonesHandle, mtx, 1)
		, "out of bounds"
		);

	UniformBuffer::destroy(stream);
}
