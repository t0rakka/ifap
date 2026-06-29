/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#include "command_line.hpp"

namespace ifap
{

    ParsedCommandLine parseCommandLine(const CommandLine& input)
    {
        ParsedCommandLine result;

        if (!input.empty())
        {
            result.commands.push_back(input[0]);
        }

        for (size_t i = 1; i < input.size(); ++i)
        {
            const std::string_view arg = input[i];

            if (arg == "--debug")
            {
                result.options.debug = true;
                continue;
            }

            if (arg == "--info")
            {
                result.options.info = true;
                continue;
            }

            result.commands.push_back(arg);
        }

        return result;
    }

} // namespace ifap
