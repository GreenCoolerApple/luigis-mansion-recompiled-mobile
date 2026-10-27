#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace port::mobile {

void init();

std::filesystem::path storageRoot();

std::filesystem::path importDir();

std::filesystem::path bundleDir();

const char* platformName();

std::vector<std::filesystem::path> imports();

bool importFile(const std::string& source, const std::string& name, const std::function<bool(float)>& progress,
                std::filesystem::path& out, std::string& error);

void requestSetup();
bool takeSetupRequest();

}
