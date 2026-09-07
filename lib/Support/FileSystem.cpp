#include "chtholly/Support/FileSystem.h"

#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace chtholly {
namespace {

std::filesystem::path pathForFileSystemImpl(std::string_view path,
                                            bool prepare_for_descendants) {
#ifdef _WIN32
  if (path.empty()) {
    return {};
  }
  const int wide_size =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                          static_cast<int>(path.size()), nullptr, 0);
  if (wide_size <= 0) {
    return std::filesystem::path(std::string(path));
  }
  std::wstring wide(static_cast<std::size_t>(wide_size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                      static_cast<int>(path.size()), wide.data(), wide_size);
  std::replace(wide.begin(), wide.end(), L'/', L'\\');
  if (wide.rfind(L"\\\\?\\", 0) == 0) {
    auto native = std::filesystem::path(std::move(wide));
    native.make_preferred();
    return native;
  }

  const DWORD required = GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);
  if (required == 0) {
    return std::filesystem::path(std::move(wide));
  }
  std::wstring absolute(static_cast<std::size_t>(required), L'\0');
  const DWORD written =
      GetFullPathNameW(wide.c_str(), required, absolute.data(), nullptr);
  if (written == 0 || written >= required) {
    return std::filesystem::path(std::move(wide));
  }
  absolute.resize(written);

  // Directory creation starts failing before MAX_PATH once a filename is
  // appended, so use the conservative Win32 directory threshold. Directory
  // tree roots opt in early because their descendants may cross it later.
  if (!prepare_for_descendants && absolute.size() < 248) {
    return std::filesystem::path(std::move(wide));
  }
  if (absolute.rfind(L"\\\\", 0) == 0) {
    return std::filesystem::path(L"\\\\?\\UNC\\" + absolute.substr(2));
  }
  return std::filesystem::path(L"\\\\?\\" + absolute);
#else
  (void)prepare_for_descendants;
  return std::filesystem::path(std::u8string(path.begin(), path.end()));
#endif
}

} // namespace

std::filesystem::path pathForFileSystem(std::string_view path) {
  return pathForFileSystemImpl(path, false);
}

std::filesystem::path pathForFileSystemTreeRoot(std::string_view path) {
  return pathForFileSystemImpl(path, true);
}

std::string pathForExternalTool(std::string_view path) {
#ifdef _WIN32
  const auto wide = pathForFileSystem(path).native();
  if (wide.empty()) {
    return {};
  }
  const int utf8_size = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                            static_cast<int>(wide.size()),
                                            nullptr, 0, nullptr, nullptr);
  if (utf8_size <= 0) {
    return std::string(path);
  }
  std::string utf8(static_cast<std::size_t>(utf8_size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                      utf8.data(), utf8_size, nullptr, nullptr);
  return utf8;
#else
  return std::string(path);
#endif
}

std::optional<std::string> readTextFile(const std::string &path,
                                        std::string &error) {
  std::ifstream input(pathForFileSystem(path), std::ios::binary);
  if (!input) {
    error = "failed to open input file: " + path;
    return std::nullopt;
  }

  std::ostringstream buffer;
  buffer << input.rdbuf();
  if (!input.good() && !input.eof()) {
    error = "failed to read input file: " + path;
    return std::nullopt;
  }
  return buffer.str();
}

bool writeTextFile(const std::string &path, const std::string &text,
                   std::string &error) {
  std::ofstream output(pathForFileSystem(path),
                       std::ios::binary | std::ios::trunc);
  if (!output) {
    error = "failed to open output file: " + path;
    return false;
  }
  output << text;
  if (!output) {
    error = "failed to write output file: " + path;
    return false;
  }
  return true;
}

bool replaceFile(const std::string &source, const std::string &destination,
                 std::error_code &error) {
#ifdef _WIN32
  const auto source_path = pathForFileSystem(source);
  const auto destination_path = pathForFileSystem(destination);
  if (MoveFileExW(source_path.c_str(), destination_path.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH |
                      MOVEFILE_COPY_ALLOWED) == 0) {
    error = std::error_code(static_cast<int>(GetLastError()),
                            std::system_category());
    return false;
  }
  error.clear();
  return true;
#else
  const auto source_path = pathForFileSystem(source);
  const auto destination_path = pathForFileSystem(destination);
  std::filesystem::rename(source_path, destination_path, error);
  if (!error) {
    return true;
  }
  if (error != std::errc::cross_device_link) {
    return false;
  }
  error.clear();
  std::filesystem::copy_file(source_path, destination_path,
                             std::filesystem::copy_options::overwrite_existing,
                             error);
  if (error) {
    return false;
  }
  return std::filesystem::remove(source_path, error) && !error;
#endif
}

bool removeFile(const std::string &path, std::error_code &error) {
  return std::filesystem::remove(pathForFileSystem(path), error);
}

} // namespace chtholly
