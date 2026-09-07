#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace chtholly {

std::filesystem::path pathForFileSystem(std::string_view path);
std::filesystem::path pathForFileSystemTreeRoot(std::string_view path);
std::string pathForExternalTool(std::string_view path);
std::optional<std::string> readTextFile(const std::string &path,
                                        std::string &error);
bool writeTextFile(const std::string &path, const std::string &text,
                   std::string &error);
bool replaceFile(const std::string &source, const std::string &destination,
                 std::error_code &error);
bool removeFile(const std::string &path, std::error_code &error);

} // namespace chtholly
