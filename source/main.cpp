/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#include "window.hpp"

//
// NOTE: Load all .so files from the same directory as the binary:
//
// > patchelf --set-rpath '$ORIGIN' ifap
//

namespace ifap
{

    // -----------------------------------------------------------------------
    // main()
    // -----------------------------------------------------------------------

    int main(const CommandLine& commands)
    {
        ifap::OpenGLContext::Config config;
        config.red   = 16;
        config.green = 16;
        config.blue  = 16;
        config.alpha = 16;

        printEnable(Print::Info, true);

        ifap::AppWindow window(config, commands);
        window.enterEventLoop();

        return 0;
    }

} // namespace ifap

#ifdef MANGO_PLATFORM_WINDOWS

using namespace mango;
using namespace mango::filesystem;
using namespace mango::image;

// -----------------------------------------------------------------------
// WinMain()
// -----------------------------------------------------------------------

//#define CREATE_CONSOLE

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPTSTR cmdline, int)
{
#ifdef CREATE_CONSOLE
	// create output console
	AllocConsole();
	FILE* fp;
	freopen_s(&fp, "CON", "w", stdout);
#endif

    // Get the executable path (including binary filename)
    WCHAR executable[_MAX_PATH];
    ::GetModuleFileNameW(NULL, executable, _MAX_PATH);

    ifap::CommandLine commands;
    commands.push_back(mango::u16_toBytes(executable));

    int status = ifap::main(commands);

#ifdef CREATE_CONSOLE
    // close output console
	fclose(fp);
#endif

	return status;
}

#else

int main(int argc, const char* argv[])
{
    ifap::CommandLine commands(argv + 0, argv + argc);
    if (argc > 2)
    {
        // This is for XCode littering command line with it's debugging junk
        commands.clear();
    }

    int status = ifap::main(commands);

    return status;
}

#endif

