#pragma once

#include <string>

namespace chtholly {

struct ProjectScaffoldRequest {
  std::string root_path;
  std::string package_name;
  bool library = false;
};

bool createProjectScaffold(const ProjectScaffoldRequest &request,
                           std::string &created_root, std::string &error);

} // namespace chtholly
