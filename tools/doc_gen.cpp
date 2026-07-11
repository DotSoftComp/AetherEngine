// Aether Engine — reference-doc generator CLI.
//
//   AetherDocGen <projectDir>          writes <projectDir>/Docs/reference/*.md
//   AetherDocGen <projectDir> --with-module   also loads the project's
//                                      GameModule + plugins first, so their
//                                      script components appear in the docs
//
// Headless (no window/GL). See engine/doc_gen.h.
#include "engine/component_registry.h"
#include "engine/doc_gen.h"
#include "engine/game_module.h"
#include "engine/plugin_manager.h"
#include "engine/project.h"
#include "core/paths.h"
#include <cstdio>
#include <cstring>

using namespace ae;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("Usage: AetherDocGen <projectDir> [--with-module]\n");
        return 1;
    }
    bool withModule = argc > 2 && !std::strcmp(argv[2], "--with-module");

    Project project;
    if (!project.load(argv[1])) return 1;

    registerBuiltinComponents();
    GameModule gameModule;
    PluginManager plugins;
    if (withModule) {
        if (project.hasModule()) gameModule.load(project, /*hotCopy=*/false);
        plugins.loadEnabled(project, /*hotCopy=*/false);
    }

    return generateReferenceDocs(joinPath(project.root, "Docs")) ? 0 : 1;
}
