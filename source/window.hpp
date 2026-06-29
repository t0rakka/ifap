/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#pragma once

#include <vector>
#include <string_view>

namespace ifap
{

    using CommandLine = std::vector<std::string_view>;

    void runApp(const CommandLine& commands);

} // namespace ifap
