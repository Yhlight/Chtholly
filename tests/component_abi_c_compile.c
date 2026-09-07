#include "chtholly/component_loader_v1.h"

int chtholly_component_c_header_probe(void) {
  return sizeof(chtholly_component_value_v1) == 32 &&
         sizeof(chtholly_component_descriptor_v1) == 208;
}
