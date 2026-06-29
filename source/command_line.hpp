/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#pragma once

#include "window.hpp"

namespace ifap
{

    struct CommandLineOptions
    {
        bool debug = false;
    };

    struct ParsedCommandLine
    {
        CommandLineOptions options;
        CommandLine commands;
    };

    ParsedCommandLine parseCommandLine(const CommandLine& input);

} // namespace ifap
