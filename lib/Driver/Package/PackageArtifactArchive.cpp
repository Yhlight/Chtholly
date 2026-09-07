#include "chtholly/Driver/PackageArtifactArchive.h"

#include "chtholly/Driver/PackageArtifactClosure.h"
#include "chtholly/Support/Digest.h"
#include "chtholly/Support/FileSystem.h"

#include "miniz.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>

namespace chtholly {
namespace {

constexpr std::size_t kMaximumIndexSize = 16u * 1024u * 1024u;

struct ZipReader {
  mz_zip_archive archive{};
  bool open = false;
  ~ZipReader() {
    if (open) {
      mz_zip_reader_end(&archive);
    }
  }
};

struct ZipWriter {
  mz_zip_archive archive{};
  bool open = false;
  ~ZipWriter() {
    if (open) {
      mz_zip_writer_end(&archive);
    }
  }
};

bool isHexDigest(std::string_view value) {
  return value.size() == 64 &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

bool isValidUtf8(std::string_view value) {
  std::size_t index = 0;
  while (index < value.size()) {
    const auto first = static_cast<unsigned char>(value[index]);
    if (first < 0x80) {
      ++index;
      continue;
    }
    std::size_t count = 0;
    std::uint32_t codepoint = 0;
    if ((first & 0xe0) == 0xc0) {
      count = 2;
      codepoint = first & 0x1f;
    } else if ((first & 0xf0) == 0xe0) {
      count = 3;
      codepoint = first & 0x0f;
    } else if ((first & 0xf8) == 0xf0) {
      count = 4;
      codepoint = first & 0x07;
    } else {
      return false;
    }
    if (index + count > value.size()) {
      return false;
    }
    for (std::size_t offset = 1; offset < count; ++offset) {
      const auto continuation =
          static_cast<unsigned char>(value[index + offset]);
      if ((continuation & 0xc0) != 0x80) {
        return false;
      }
      codepoint = (codepoint << 6) | (continuation & 0x3f);
    }
    if ((count == 2 && codepoint < 0x80) ||
        (count == 3 && codepoint < 0x800) ||
        (count == 4 && codepoint < 0x10000) || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
      return false;
    }
    index += count;
  }
  return true;
}

bool safeArchivePath(std::string_view path) {
  if (path.empty() || !isValidUtf8(path) || path.front() == '/' ||
      path.find('\\') != std::string_view::npos ||
      path.find(':') != std::string_view::npos || path.back() == '/') {
    return false;
  }
  std::size_t start = 0;
  while (start <= path.size()) {
    const auto slash = path.find('/', start);
    const auto component = path.substr(
        start, slash == std::string_view::npos ? path.size() - start
                                               : slash - start);
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
    if (slash == std::string_view::npos) {
      break;
    }
    start = slash + 1;
  }
  return true;
}

std::vector<std::string_view> splitTabs(std::string_view line) {
  std::vector<std::string_view> fields;
  while (true) {
    const auto tab = line.find('\t');
    fields.push_back(line.substr(0, tab));
    if (tab == std::string_view::npos) {
      break;
    }
    line.remove_prefix(tab + 1);
  }
  return fields;
}

std::filesystem::path utf8Path(std::string_view value) {
  std::u8string converted;
  converted.reserve(value.size());
  for (const unsigned char ch : value) {
    converted.push_back(static_cast<char8_t>(ch));
  }
  return std::filesystem::path(converted);
}

std::optional<std::vector<unsigned char>> readBinary(const std::string &path,
                                                     std::string &error) {
  std::ifstream input(pathForFileSystem(path), std::ios::binary);
  if (!input) {
    error = "failed to open file for reading: '" + path + "'";
    return std::nullopt;
  }
  input.seekg(0, std::ios::end);
  const auto end = input.tellg();
  if (end < 0 || static_cast<std::uintmax_t>(end) >
                     std::numeric_limits<std::size_t>::max()) {
    error = "file is too large to read: '" + path + "'";
    return std::nullopt;
  }
  std::vector<unsigned char> data(static_cast<std::size_t>(end));
  input.seekg(0, std::ios::beg);
  if (!data.empty()) {
    input.read(reinterpret_cast<char *>(data.data()),
               static_cast<std::streamsize>(data.size()));
  }
  if (!input) {
    error = "failed to read file: '" + path + "'";
    return std::nullopt;
  }
  return data;
}

bool writeBinary(const std::filesystem::path &path, const void *data,
                 std::size_t size, std::string &error) {
  std::error_code ec;
  const auto filesystem_path = pathForFileSystem(path.string());
  std::filesystem::create_directories(filesystem_path.parent_path(), ec);
  if (ec) {
    error = "failed to create archive output directory: " + ec.message();
    return false;
  }
  std::ofstream output(filesystem_path, std::ios::binary | std::ios::trunc);
  if (!output) {
    error = "failed to open file for writing: '" + path.string() + "'";
    return false;
  }
  if (size != 0) {
    output.write(static_cast<const char *>(data),
                 static_cast<std::streamsize>(size));
  }
  if (!output) {
    error = "failed to write file: '" + path.string() + "'";
    return false;
  }
  return true;
}

std::string zipError(mz_zip_archive &archive) {
  return mz_zip_get_error_string(mz_zip_get_last_error(&archive));
}

std::optional<std::string> zipFilename(mz_zip_archive &archive,
                                       mz_uint index, std::string &error) {
  const auto size = mz_zip_reader_get_filename(&archive, index, nullptr, 0);
  if (size == 0) {
    error = "artifact archive contains an unreadable entry name";
    return std::nullopt;
  }
  std::string name(size, '\0');
  if (mz_zip_reader_get_filename(&archive, index, name.data(), size) != size) {
    error = "artifact archive contains an unreadable entry name";
    return std::nullopt;
  }
  name.resize(size - 1);
  if (name.find('\0') != std::string::npos) {
    error = "artifact archive entry name contains NUL";
    return std::nullopt;
  }
  return name;
}

std::optional<std::vector<unsigned char>> extractEntry(mz_zip_archive &archive,
                                                        mz_uint index,
                                                        std::uint64_t size,
                                                        std::string &error) {
  if (size > std::numeric_limits<std::size_t>::max()) {
    error = "artifact archive entry is too large";
    return std::nullopt;
  }
  std::vector<unsigned char> data(static_cast<std::size_t>(size));
  if (!mz_zip_reader_extract_to_mem(&archive, index, data.data(), data.size(),
                                    0)) {
    error = "failed to extract artifact archive entry: " + zipError(archive);
    return std::nullopt;
  }
  return data;
}

std::string bytesDigest(const std::vector<unsigned char> &data) {
  return sha256Hex(std::string_view(
      reinterpret_cast<const char *>(data.data()), data.size()));
}

std::string renderIndex(const PackageArtifactClosure &closure) {
  return canonicalPackageArtifactClosureIndex(closure);
}

bool isSymlink(const mz_zip_archive_file_stat &stat) {
  constexpr mz_uint32 unixSymlink = 0120000u;
  constexpr mz_uint32 unixTypeMask = 0170000u;
  const auto unix_mode = (stat.m_external_attr >> 16) & 0xffffu;
  return (unix_mode & unixTypeMask) == unixSymlink;
}

} // namespace

std::optional<PackageArtifactArchiveInfo>
parsePackageArtifactArchiveIndex(std::string_view index, std::string &error) {
  if (index.size() > kMaximumIndexSize) {
    error = "artifact archive index exceeds the size limit";
    return std::nullopt;
  }
  PackageArtifactArchiveInfo info;
  std::istringstream input{std::string(index)};
  std::string line;
  if (!std::getline(input, line) || line != PackageArchiveIndexHeader) {
    error = "invalid Chtholly artifact archive index header";
    return std::nullopt;
  }
  bool root_seen = false;
  bool end_seen = false;
  std::string previous_path;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      error = "artifact archive index is not canonical";
      return std::nullopt;
    }
    if (line == "end") {
      end_seen = true;
      break;
    }
    const auto fields = splitTabs(line);
    if (fields.size() == 3 && fields[0] == "root" && !root_seen) {
      if (!safeArchivePath(fields[1]) || !isHexDigest(fields[2])) {
        error = "artifact archive index contains an invalid root record";
        return std::nullopt;
      }
      root_seen = true;
      info.root_manifest_relative_path = std::string(fields[1]);
      info.artifact_identity = std::string(fields[2]);
      continue;
    }
    if (fields.size() == 4 && fields[0] == "file" && root_seen) {
      std::uint64_t size = 0;
      const auto conversion = std::from_chars(fields[3].data(),
                                               fields[3].data() + fields[3].size(),
                                               size);
      if (!safeArchivePath(fields[1]) || !isHexDigest(fields[2]) ||
          conversion.ec != std::errc{} ||
          conversion.ptr != fields[3].data() + fields[3].size() ||
          (!previous_path.empty() && previous_path >= fields[1])) {
        error = "artifact archive index contains an invalid or unsorted file record";
        return std::nullopt;
      }
      previous_path = std::string(fields[1]);
      info.files.push_back({previous_path, std::string(fields[2]), size});
      continue;
    }
    error = "artifact archive index contains an unknown or duplicate record";
    return std::nullopt;
  }
  std::string trailing;
  if (!root_seen || !end_seen || info.files.empty() ||
      static_cast<bool>(std::getline(input, trailing))) {
    error = "artifact archive index is incomplete or contains trailing data";
    return std::nullopt;
  }
  const auto root = std::find_if(info.files.begin(), info.files.end(),
                                 [&](const auto &file) {
                                   return file.relative_path ==
                                          info.root_manifest_relative_path;
                                 });
  if (root == info.files.end()) {
    error = "artifact archive index root manifest is not in the file set";
    return std::nullopt;
  }
  info.canonical_index = std::string(index);
  info.closure_digest = sha256Hex(index);
  return info;
}

std::optional<PackageArtifactArchiveInfo>
packPackageArtifactArchive(const std::string &root_manifest_path,
                           const std::string &archive_path,
                           std::string &error) {
  auto closure = loadPackageArtifactClosure(root_manifest_path, error);
  if (!closure) {
    return std::nullopt;
  }
  const auto index = renderIndex(*closure);
  auto info = parsePackageArtifactArchiveIndex(index, error);
  if (!info) {
    return std::nullopt;
  }

  ZipWriter writer;
  if (!mz_zip_writer_init_heap(&writer.archive, 0, 0)) {
    error = "failed to initialize artifact archive writer";
    return std::nullopt;
  }
  writer.open = true;
  if (!mz_zip_writer_add_mem_ex_v2(
          &writer.archive, "closure.index", index.data(), index.size(), nullptr,
          0, MZ_NO_COMPRESSION, 0, 0, nullptr, nullptr, 0, nullptr, 0)) {
    error = "failed to add artifact archive index: " + zipError(writer.archive);
    return std::nullopt;
  }
  for (const auto &file : closure->files) {
    auto data = readBinary(file.absolute_path, error);
    if (!data || bytesDigest(*data) != file.sha256) {
      if (error.empty()) {
        error = "package artifact closure changed while packing: '" +
                file.absolute_path + "'";
      }
      return std::nullopt;
    }
    const auto name = "tree/" + file.relative_path;
    if (!mz_zip_writer_add_mem_ex_v2(
            &writer.archive, name.c_str(), data->data(), data->size(), nullptr,
            0, MZ_NO_COMPRESSION, 0, 0, nullptr, nullptr, 0, nullptr, 0)) {
      error = "failed to add artifact archive entry '" + name + "': " +
              zipError(writer.archive);
      return std::nullopt;
    }
  }
  void *archive_data = nullptr;
  std::size_t archive_size = 0;
  if (!mz_zip_writer_finalize_heap_archive(&writer.archive, &archive_data,
                                           &archive_size)) {
    error = "failed to finalize artifact archive: " + zipError(writer.archive);
    return std::nullopt;
  }
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto temporary = archive_path + ".tmp-" + std::to_string(nonce);
  const bool wrote = writeBinary(temporary, archive_data, archive_size, error);
  if (!mz_zip_writer_end(&writer.archive)) {
    if (error.empty()) {
      error = "failed to release artifact archive writer";
    }
    writer.open = false;
    mz_free(archive_data);
    return std::nullopt;
  }
  writer.open = false;
  mz_free(archive_data);
  if (!wrote) {
    return std::nullopt;
  }
  std::error_code ec;
  if (!replaceFile(temporary, archive_path, ec)) {
    error = "failed to replace artifact archive: " + ec.message();
    removeFile(temporary, ec);
    return std::nullopt;
  }
  info->archive_path = archive_path;
  info->archive_sha256 = sha256File(archive_path).value_or(std::string{});
  return info;
}

std::optional<PackageArtifactArchiveInfo>
inspectPackageArtifactArchive(const std::string &archive_path,
                              std::string &error) {
  auto archive_data = readBinary(archive_path, error);
  if (!archive_data) {
    return std::nullopt;
  }
  ZipReader reader;
  if (!mz_zip_reader_init_mem(&reader.archive, archive_data->data(),
                              archive_data->size(), 0)) {
    error = "invalid artifact archive: " + zipError(reader.archive);
    return std::nullopt;
  }
  reader.open = true;
  const auto count = mz_zip_reader_get_num_files(&reader.archive);
  if (count < 2) {
    error = "artifact archive does not contain a closure";
    return std::nullopt;
  }
  std::vector<std::string> names;
  names.reserve(count);
  std::set<std::string> unique_names;
  std::vector<mz_zip_archive_file_stat> stats(count);
  for (mz_uint index = 0; index < count; ++index) {
    auto name = zipFilename(reader.archive, index, error);
    if (!name || !safeArchivePath(*name) ||
        !unique_names.insert(*name).second ||
        !mz_zip_reader_file_stat(&reader.archive, index, &stats[index]) ||
        stats[index].m_is_directory || stats[index].m_is_encrypted ||
        stats[index].m_method != 0 || isSymlink(stats[index])) {
      if (error.empty()) {
        error = "artifact archive contains an unsafe, duplicate, encrypted, "
                "compressed, or non-file entry";
      }
      return std::nullopt;
    }
    names.push_back(std::move(*name));
  }
  if (names.front() != "closure.index" ||
      stats.front().m_uncomp_size > kMaximumIndexSize) {
    error = "artifact archive canonical index entry is missing or too large";
    return std::nullopt;
  }
  auto index_data = extractEntry(reader.archive, 0, stats.front().m_uncomp_size,
                                 error);
  if (!index_data) {
    return std::nullopt;
  }
  const std::string index(reinterpret_cast<const char *>(index_data->data()),
                          index_data->size());
  auto info = parsePackageArtifactArchiveIndex(index, error);
  if (!info) {
    return std::nullopt;
  }
  if (count != info->files.size() + 1) {
    error = "artifact archive entries do not match its canonical index";
    return std::nullopt;
  }
  std::optional<std::string> root_manifest_text;
  for (std::size_t index_pos = 0; index_pos < info->files.size(); ++index_pos) {
    const auto archive_index = static_cast<mz_uint>(index_pos + 1);
    const auto expected_name = "tree/" + info->files[index_pos].relative_path;
    if (names[archive_index] != expected_name ||
        stats[archive_index].m_uncomp_size != info->files[index_pos].size) {
      error = "artifact archive entry order, path, or size does not match its index";
      return std::nullopt;
    }
    auto data = extractEntry(reader.archive, archive_index,
                             stats[archive_index].m_uncomp_size, error);
    if (!data || bytesDigest(*data) != info->files[index_pos].sha256) {
      if (error.empty()) {
        error = "artifact archive entry SHA-256 mismatch: '" + expected_name +
                "'";
      }
      return std::nullopt;
    }
    if (info->files[index_pos].relative_path ==
        info->root_manifest_relative_path) {
      root_manifest_text = std::string(
          reinterpret_cast<const char *>(data->data()), data->size());
    }
  }
  if (!root_manifest_text) {
    error = "artifact archive root manifest payload is missing";
    return std::nullopt;
  }
  auto root_manifest = parsePackageArtifactManifest(
      *root_manifest_text,
      archive_path + "!/tree/" + info->root_manifest_relative_path, error);
  if (!root_manifest ||
      root_manifest->artifact_identity != info->artifact_identity) {
    if (error.empty()) {
      error = "artifact archive root manifest identity does not match its index";
      if (root_manifest) {
        error += ": manifest=" + root_manifest->artifact_identity +
                 " index=" + info->artifact_identity;
      }
    }
    return std::nullopt;
  }
  info->package_name = root_manifest->package_name;
  info->target = root_manifest->target;
  info->abi_version = root_manifest->abi_version;
  info->runtime_abi = root_manifest->runtime_abi;
  info->default_features = root_manifest->default_features;
  info->resolved_features = root_manifest->resolved_features;
  info->archive_path = archive_path;
  info->archive_sha256 = bytesDigest(*archive_data);
  return info;
}

bool extractPackageArtifactArchive(const std::string &archive_path,
                                   const std::string &destination,
                                   PackageArtifactArchiveInfo &info,
                                   std::string &error) {
  auto inspected = inspectPackageArtifactArchive(archive_path, error);
  if (!inspected) {
    return false;
  }
  auto archive_data = readBinary(archive_path, error);
  if (!archive_data) {
    return false;
  }
  if (bytesDigest(*archive_data) != inspected->archive_sha256) {
    error = "artifact archive changed between inspection and extraction";
    return false;
  }
  ZipReader reader;
  if (!mz_zip_reader_init_mem(&reader.archive, archive_data->data(),
                              archive_data->size(), 0)) {
    error = "failed to reopen artifact archive: " + zipError(reader.archive);
    return false;
  }
  reader.open = true;
  const auto destination_path = std::filesystem::path(destination);
  if (!writeBinary(destination_path / "closure.index",
                   inspected->canonical_index.data(),
                   inspected->canonical_index.size(), error)) {
    return false;
  }
  for (std::size_t index = 0; index < inspected->files.size(); ++index) {
    auto data = extractEntry(reader.archive, static_cast<mz_uint>(index + 1),
                             inspected->files[index].size, error);
    if (!data || bytesDigest(*data) != inspected->files[index].sha256 ||
        !writeBinary(destination_path / "tree" /
                         utf8Path(inspected->files[index].relative_path),
                     data->data(), data->size(), error)) {
      if (data && error.empty()) {
        error = "artifact archive entry changed during extraction: '" +
                inspected->files[index].relative_path + "'";
      }
      return false;
    }
  }
  info = std::move(*inspected);
  return true;
}

} // namespace chtholly
