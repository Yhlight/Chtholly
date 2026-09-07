#include "chtholly/Driver/SemVer.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>

namespace chtholly {

namespace {

enum class ComparatorOp {
  Equal,
  GreaterEqual,
  LessEqual,
  Greater,
  Less,
};

struct Comparator {
  ComparatorOp op = ComparatorOp::Equal;
  SemVer version;
};

bool isIdentifierChar(char ch) {
  return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '-';
}

bool parseNumber(std::string_view text, std::size_t &cursor, int &out) {
  if (cursor >= text.size() || !std::isdigit(static_cast<unsigned char>(text[cursor]))) {
    return false;
  }
  if (text[cursor] == '0' && cursor + 1 < text.size() &&
      std::isdigit(static_cast<unsigned char>(text[cursor + 1]))) {
    return false;
  }
  int value = 0;
  while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor]))) {
    value = value * 10 + static_cast<int>(text[cursor] - '0');
    ++cursor;
  }
  out = value;
  return true;
}

bool parseIdentifiers(std::string_view text, std::size_t &cursor, std::string &out) {
  const auto start = cursor;
  bool saw_char = false;
  bool segment_has_char = false;
  while (cursor < text.size()) {
    const char ch = text[cursor];
    if (ch == '.') {
      if (!segment_has_char) {
        return false;
      }
      segment_has_char = false;
      ++cursor;
      continue;
    }
    if (!isIdentifierChar(ch)) {
      break;
    }
    saw_char = true;
    segment_has_char = true;
    ++cursor;
  }
  if (!saw_char || !segment_has_char) {
    return false;
  }
  out = std::string(text.substr(start, cursor - start));
  return true;
}

bool isNumericIdentifier(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char ch) {
           return std::isdigit(static_cast<unsigned char>(ch)) != 0;
         });
}

bool prereleaseIdentifiersAreValid(std::string_view text) {
  std::size_t cursor = 0;
  while (cursor <= text.size()) {
    const auto dot = text.find('.', cursor);
    const auto part = text.substr(cursor, dot == std::string_view::npos
                                             ? std::string_view::npos
                                             : dot - cursor);
    if (isNumericIdentifier(part) && part.size() > 1 && part.front() == '0') {
      return false;
    }
    if (dot == std::string_view::npos) {
      break;
    }
    cursor = dot + 1;
  }
  return true;
}

std::vector<std::string_view> splitPrerelease(std::string_view text) {
  std::vector<std::string_view> parts;
  std::size_t cursor = 0;
  while (cursor <= text.size()) {
    const auto dot = text.find('.', cursor);
    parts.push_back(text.substr(cursor, dot == std::string_view::npos
                                            ? std::string_view::npos
                                            : dot - cursor));
    if (dot == std::string_view::npos) {
      break;
    }
    cursor = dot + 1;
  }
  return parts;
}

int comparePrerelease(std::string_view lhs, std::string_view rhs) {
  if (lhs.empty() && rhs.empty()) {
    return 0;
  }
  if (lhs.empty()) {
    return 1;
  }
  if (rhs.empty()) {
    return -1;
  }
  const auto lhs_parts = splitPrerelease(lhs);
  const auto rhs_parts = splitPrerelease(rhs);
  const auto count = std::min(lhs_parts.size(), rhs_parts.size());
  for (std::size_t i = 0; i < count; ++i) {
    const auto lnum = isNumericIdentifier(lhs_parts[i]);
    const auto rnum = isNumericIdentifier(rhs_parts[i]);
    if (lnum && rnum) {
      if (lhs_parts[i].size() != rhs_parts[i].size()) {
        return lhs_parts[i].size() < rhs_parts[i].size() ? -1 : 1;
      }
      if (lhs_parts[i] != rhs_parts[i]) {
        return lhs_parts[i] < rhs_parts[i] ? -1 : 1;
      }
      continue;
    }
    if (lnum != rnum) {
      return lnum ? -1 : 1;
    }
    if (lhs_parts[i] != rhs_parts[i]) {
      return lhs_parts[i] < rhs_parts[i] ? -1 : 1;
    }
  }
  if (lhs_parts.size() == rhs_parts.size()) {
    return 0;
  }
  return lhs_parts.size() < rhs_parts.size() ? -1 : 1;
}

bool containsPrereleaseVersion(std::string_view text) {
  auto cursor = text.find_first_of("0123456789");
  while (cursor != std::string_view::npos) {
    int ignored = 0;
    auto number_cursor = cursor;
    if (parseNumber(text, number_cursor, ignored) && number_cursor < text.size() &&
        text[number_cursor] == '.') {
      ++number_cursor;
      if (parseNumber(text, number_cursor, ignored) && number_cursor < text.size() &&
          text[number_cursor] == '.') {
        ++number_cursor;
        if (parseNumber(text, number_cursor, ignored) && number_cursor < text.size() &&
            text[number_cursor] == '-') {
          return true;
        }
      }
    }
    cursor = text.find_first_of("0123456789", cursor + 1);
  }
  return false;
}

std::optional<Comparator> parseComparator(std::string_view token, std::string &error) {
  Comparator comparator;
  if (token.starts_with(">=")) {
    comparator.op = ComparatorOp::GreaterEqual;
    token.remove_prefix(2);
  } else if (token.starts_with("<=")) {
    comparator.op = ComparatorOp::LessEqual;
    token.remove_prefix(2);
  } else if (token.starts_with(">")) {
    comparator.op = ComparatorOp::Greater;
    token.remove_prefix(1);
  } else if (token.starts_with("<")) {
    comparator.op = ComparatorOp::Less;
    token.remove_prefix(1);
  } else {
    comparator.op = ComparatorOp::Equal;
  }
  auto version = parseSemVer(token, error);
  if (!version) {
    return std::nullopt;
  }
  comparator.version = *version;
  return comparator;
}

std::vector<Comparator> expandRequirement(const VersionRequirement &requirement,
                                          std::string &error) {
  std::vector<Comparator> comparators;
  std::istringstream input(requirement.text);
  std::string token;
  while (input >> token) {
    if (token.starts_with('^')) {
      auto base = parseSemVer(std::string_view(token).substr(1), error);
      if (!base) {
        return {};
      }
      comparators.push_back({ComparatorOp::GreaterEqual, *base});
      SemVer upper = *base;
      if (base->major > 0) {
        ++upper.major;
        upper.minor = 0;
        upper.patch = 0;
      } else if (base->minor > 0) {
        ++upper.minor;
        upper.patch = 0;
      } else {
        ++upper.patch;
      }
      upper.prerelease.clear();
      upper.build.clear();
      comparators.push_back({ComparatorOp::Less, upper});
    } else if (token.starts_with('~')) {
      auto base = parseSemVer(std::string_view(token).substr(1), error);
      if (!base) {
        return {};
      }
      comparators.push_back({ComparatorOp::GreaterEqual, *base});
      SemVer upper = *base;
      ++upper.minor;
      upper.patch = 0;
      upper.prerelease.clear();
      upper.build.clear();
      comparators.push_back({ComparatorOp::Less, upper});
    } else {
      auto comparator = parseComparator(token, error);
      if (!comparator) {
        return {};
      }
      comparators.push_back(*comparator);
    }
  }
  if (comparators.empty()) {
    error = "version requirement cannot be empty";
  }
  return comparators;
}

bool comparatorMatches(const SemVer &version, const Comparator &comparator) {
  const auto cmp = compareSemVer(version, comparator.version);
  switch (comparator.op) {
  case ComparatorOp::Equal:
    return cmp == 0;
  case ComparatorOp::GreaterEqual:
    return cmp >= 0;
  case ComparatorOp::LessEqual:
    return cmp <= 0;
  case ComparatorOp::Greater:
    return cmp > 0;
  case ComparatorOp::Less:
    return cmp < 0;
  }
  return false;
}

} // namespace

std::optional<SemVer> parseSemVer(std::string_view text, std::string &error) {
  SemVer version;
  std::size_t cursor = 0;
  if (!parseNumber(text, cursor, version.major) || cursor >= text.size() ||
      text[cursor++] != '.' || !parseNumber(text, cursor, version.minor) ||
      cursor >= text.size() || text[cursor++] != '.' ||
      !parseNumber(text, cursor, version.patch)) {
    error = "invalid semantic version '" + std::string(text) + "'";
    return std::nullopt;
  }
  if (cursor < text.size() && text[cursor] == '-') {
    ++cursor;
    if (!parseIdentifiers(text, cursor, version.prerelease)) {
      error = "invalid semantic version prerelease '" + std::string(text) + "'";
      return std::nullopt;
    }
    if (!prereleaseIdentifiersAreValid(version.prerelease)) {
      error = "invalid semantic version prerelease '" + std::string(text) + "'";
      return std::nullopt;
    }
  }
  if (cursor < text.size() && text[cursor] == '+') {
    ++cursor;
    if (!parseIdentifiers(text, cursor, version.build)) {
      error = "invalid semantic version build metadata '" + std::string(text) + "'";
      return std::nullopt;
    }
  }
  if (cursor != text.size()) {
    error = "invalid semantic version '" + std::string(text) + "'";
    return std::nullopt;
  }
  return version;
}

int compareSemVer(const SemVer &lhs, const SemVer &rhs) {
  if (lhs.major != rhs.major) {
    return lhs.major < rhs.major ? -1 : 1;
  }
  if (lhs.minor != rhs.minor) {
    return lhs.minor < rhs.minor ? -1 : 1;
  }
  if (lhs.patch != rhs.patch) {
    return lhs.patch < rhs.patch ? -1 : 1;
  }
  return comparePrerelease(lhs.prerelease, rhs.prerelease);
}

std::optional<VersionRequirement> parseVersionRequirement(std::string_view text,
                                                          std::string &error) {
  VersionRequirement requirement{std::string(text)};
  auto comparators = expandRequirement(requirement, error);
  if (comparators.empty()) {
    return std::nullopt;
  }
  return requirement;
}

bool versionRequirementAllowsPrerelease(const VersionRequirement &requirement) {
  return containsPrereleaseVersion(requirement.text);
}

bool versionSatisfiesRequirement(const SemVer &version,
                                 const VersionRequirement &requirement) {
  if (!version.prerelease.empty() && !versionRequirementAllowsPrerelease(requirement)) {
    return false;
  }
  std::string error;
  const auto comparators = expandRequirement(requirement, error);
  if (comparators.empty()) {
    return false;
  }
  return std::all_of(comparators.begin(), comparators.end(), [&](const Comparator &comparator) {
    return comparatorMatches(version, comparator);
  });
}

std::optional<std::string> selectHighestVersion(
    const std::vector<std::string> &versions, const VersionRequirement &requirement,
    std::string &error) {
  std::optional<std::pair<SemVer, std::string>> best;
  for (const auto &text : versions) {
    auto version = parseSemVer(text, error);
    if (!version) {
      return std::nullopt;
    }
    if (!versionSatisfiesRequirement(*version, requirement)) {
      continue;
    }
    if (!best || compareSemVer(*version, best->first) > 0) {
      best = std::make_pair(*version, text);
    }
  }
  if (!best) {
    error = "no version satisfies requirement '" + requirement.text + "'";
    return std::nullopt;
  }
  return best->second;
}

} // namespace chtholly
