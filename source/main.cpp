/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#define MANGO_IMPLEMENT_MAIN
#include "window.hpp"

//
// NOTE: Load all .so files from the same directory as the binary:
//   patchelf --set-rpath '$ORIGIN' ifap
//

using namespace mango;

int mangoMain(const mango::CommandLine& commands)
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
