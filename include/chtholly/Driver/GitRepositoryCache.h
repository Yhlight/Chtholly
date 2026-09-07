#pragma once

#include <optional>
#include <string>

namespace chtholly {

struct GitRepositoryCacheRequest {
  std::string subject;
  std::string url;
  std::string source_identity;
  std::string selector_ref;
  std::string marker_selector_kind;
  std::string marker_selector_value;
  std::string pinned_commit;
  std::string cache_root;
  std::string required_relative_path;
  bool offline = false;
  bool locked = false;
  bool update = false;
  bool allow_unpinned_offline = false;
};

struct GitRepositoryCheckout {
  std::string commit;
  std::string checkout_root;
};

std::optional<GitRepositoryCheckout>
resolveGitRepositoryCheckout(const GitRepositoryCacheRequest &request,
                             std::string &error);

} // namespace chtholly
