/*
 * Test runner for the bgfx uniform upload integration tests.
 *
 * Installs an assert handler that makes bgfx/bx contract violations
 * observable (recorded message + thrown exception), then runs the whole
 * suite inside a headless bgfx session using the noop renderer, so the
 * tests work on machines without a graphics device.
 */

#define CATCH_CONFIG_RUNNER
#include "test.h"

#include <bgfx/bgfx.h>

#include <cstdarg>
#include <cstdio>
#include <exception>

char     g_lastAssertMessage[1024];
uint32_t g_assertCount = 0;

void resetAssertRecord()
{
	g_lastAssertMessage[0] = '\0';
	g_assertCount = 0;
}

bool lastAssertContains(const char* _substring)
{
	return NULL != std::strstr(g_lastAssertMessage, _substring);
}

static bool testAssertHandler(const bx::Location& _location, uint32_t _skip, const char* _format, va_list _argList)
{
	BX_UNUSED(_location, _skip);

	std::vsnprintf(g_lastAssertMessage, sizeof(g_lastAssertMessage), _format, _argList);
	g_lastAssertMessage[sizeof(g_lastAssertMessage) - 1] = '\0';
	++g_assertCount;

	// Throwing lets tests assert on contract violations with
	// REQUIRE_BGFX_ASSERT. bgfx guards its locks with RAII scopes, so
	// unwinding through the call site is safe.
	throw std::exception();
}

int main(int _argc, char* _argv[])
{
	bx::setAssertHandler(testAssertHandler);

	// Headless session: the noop renderer executes the full
	// submit/frame pipeline without creating a graphics device.
	bgfx::Init init;
	init.type = bgfx::RendererType::Noop;

	if (!bgfx::init(init) )
	{
		std::fprintf(stderr, "bgfx::init failed (noop renderer)\n");
		return 1;
	}

	const int result = Catch::Session().run(_argc, _argv);

	bgfx::shutdown();
	return result;
}
