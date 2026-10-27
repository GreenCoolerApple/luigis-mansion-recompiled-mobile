#pragma once
// Recompiles a disc's main.dol and compiles the result into the game module DLL with the
// bundled clang + lld (docs/SETUP.md).
#include "setup/disc.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

namespace setup {

struct Progress {
    std::function<void(const std::string& stage, float fraction)> report;
    std::atomic<bool>* cancel = nullptr;
};

enum class ModuleTarget { Host, Android, Ios };

ModuleTarget hostTarget();
const char* targetLabel(ModuleTarget t);

// Where a version's module lives: <dataDir>/game/<GAMEID>/module.dll
std::filesystem::path moduleDir(port::GameVersion v);
std::filesystem::path modulePath(port::GameVersion v);

// Cache key of the module for this disc and this executable (inputs, sources, flags).
std::string moduleKey(const Disc& disc, ModuleTarget target = ModuleTarget::Host);
bool moduleUpToDate(const Disc& disc);

std::string embeddedModuleKey(const std::filesystem::path& file);
std::string embeddedModuleGame(const std::filesystem::path& file);

// Locates clang.exe and lld-link.exe; returns false with a message if missing.
bool findToolchain(std::filesystem::path& clang, std::filesystem::path& lld, std::string& error);

bool buildModule(const Disc& disc, const Progress& progress, std::string& error);

std::string mobileModuleName(port::GameVersion v, ModuleTarget t);
std::filesystem::path exportDir(ModuleTarget t);
bool exportModule(const Disc& disc, ModuleTarget target, const Progress& progress, std::filesystem::path& out,
                  std::string& error);

}  // namespace setup
