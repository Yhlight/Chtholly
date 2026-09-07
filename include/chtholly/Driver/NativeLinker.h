#pragma once

#include "chtholly/Driver/TargetConfig.h"

#include <string>
#include <span>
#include <vector>

namespace chtholly {

void appendUniqueLinkValue(std::vector<std::string> &values,
                           const std::string &value);
void appendHostedRuntimeSystemLibraries(std::vector<std::string> &libraries,
                                        const TargetConfig &target);
std::string defaultNativeLinkerForTarget(const TargetConfig &target);
std::string temporaryObjectPathForExecutable(const std::string &output_path,
                                             const TargetConfig &target);
bool linkNativeExecutable(const std::string &object_path,
                          const std::string &output_path,
                          const std::vector<std::string> &library_search_paths,
                          const std::vector<std::string> &libraries,
                          const TargetConfig &target, std::string &error);
bool linkNativeExecutable(const std::vector<std::string> &object_paths,
                          const std::string &output_path,
                          const std::vector<std::string> &library_search_paths,
                          const std::vector<std::string> &libraries,
                          const TargetConfig &target, std::string &error);
bool linkNativeSharedLibrary(
    const std::vector<std::string> &object_paths,
    const std::string &output_path,
    const std::vector<std::string> &library_search_paths,
    const std::vector<std::string> &libraries, const TargetConfig &target,
    std::string &error,
    std::span<const std::string> exported_symbols = {});

} // namespace chtholly
