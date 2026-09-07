#include "chtholly/Driver/ComponentDeployment.h"
#include <iostream>
#include <string>

int main(int argc, char **argv) {
  if (argc < 3) { std::cerr << "usage: chtholly-component-deploy install <root> <manifest> | activate <root> <generation> | rollback <root> | active <root> | remove <root> <generation>\n"; return 2; }
  const std::string action = argv[1], root = argv[2]; std::string error; bool ok = false;
  chtholly::ComponentGenerationInfo generation;
  if (action == "install" && argc == 4) ok = chtholly::installComponentGeneration(root, argv[3], generation, error);
  else if (action == "activate" && argc == 4) ok = chtholly::activateComponentGeneration(root, argv[3], error);
  else if (action == "rollback" && argc == 3) ok = chtholly::rollbackComponentGeneration(root, error);
  else if (action == "active" && argc == 3) ok = chtholly::activeComponentGeneration(root, generation, error);
  else if (action == "remove" && argc == 4) ok = chtholly::removeComponentGeneration(root, argv[3], error);
  else { std::cerr << "invalid arguments\n"; return 2; }
  if (!ok) { std::cerr << error << '\n'; return 1; }
  if (action == "install" || action == "active") std::cout << "generation\t" << generation.id << "\nversion\t" << generation.manifest.version << "\ndigest\t" << generation.manifest.contract_digest << '\n';
  else std::cout << action << "\tok\n";
  return 0;
}
