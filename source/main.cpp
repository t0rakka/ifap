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

namespace
{

    struct LaunchOptions
    {
        bool vulkan = false;
        ifap::CommandLine commands;
    };

    static bool isVulkanFlag(std::string_view arg)
    {
        return arg == "--vk" || arg == "--vulkan";
    }

    static LaunchOptions parseCommandLine(const mango::CommandLine& commands)
    {
        LaunchOptions options;

        if (!commands.empty())
        {
            options.commands.push_back(commands[0]);
        }

        for (size_t i = 1; i < commands.size(); ++i)
        {
            const std::string_view arg = commands[i];

            if (isVulkanFlag(arg))
            {
                options.vulkan = true;
                continue;
            }

            options.commands.push_back(arg);
        }

        return options;
    }

} // namespace

int mangoMain(const mango::CommandLine& commands)
{
    printEnable(Print::Info, true);

    const LaunchOptions options = parseCommandLine(commands);

    if (options.vulkan)
    {
        ifap::runVulkanApp(options.commands);
    }
    else
    {
        ifap::runOpenGLApp(options.commands);
    }

    return 0;
}
