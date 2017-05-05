// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>
#include <string.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "identifier_table.h"
#include "module.h"
#include "source_manager.h"

namespace fidl {
namespace {

enum struct Behavior {
    None,
    ModuleDump,
};

bool TestParser(int file_count, char** file_names, Behavior behavior) {
    Module module;

    for (int idx = 0; idx < file_count; ++idx) {
        StringView source;
        if (!module.CreateSource(file_names[idx], &source)) {
            fprintf(stderr, "Couldn't read in source data from %s\n", file_names[idx]);
            return false;
        }
        if (!module.Parse(source)) {
            fprintf(stderr, "Parse failed!\n");
            return false;
        }
    }

    if (behavior != Behavior::ModuleDump)
        return true;

    return module.Dump();
}

} // namespace
} // namespace fidl

int main(int argc, char* argv[]) {
    if (argc < 3)
        return 1;

    // Parse the program name.
    --argc;
    ++argv;

    // Parse the behavior.
    fidl::Behavior behavior;
    if (!strncmp(argv[0], "none", 4))
        behavior = fidl::Behavior::None;
    else if (!strncmp(argv[0], "module-dump", 11))
        behavior = fidl::Behavior::ModuleDump;
    else
        return 1;
    --argc;
    ++argv;

    return TestParser(argc, argv, behavior) ? 0 : 1;
}
