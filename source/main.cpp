/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#define MANGO_IMPLEMENT_MAIN
#include <mango/core/system.hpp>
#include "window.hpp"

//
// NOTE: Load all .so files from the same directory as the binary:
//   patchelf --set-rpath '$ORIGIN' ifap
//

using namespace mango;

int mangoMain(const mango::CommandLine& commands)
{
    printEnable(Print::Info, false);
    ifap::runApp(commands);
    return 0;
}
