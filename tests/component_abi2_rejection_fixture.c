#include "chtholly/component_abi_v1.h"

static const chtholly_component_descriptor_v1 descriptor = {
    sizeof(descriptor),
    2u,
    0u,
    0u,
    {1, 0},
    {1, 0},
    {1, 0},
    {1, 0},
    "abi2-rejection-fixture",
    22,
    0,
    0,
    {0, 0, 0, 0}};

CHTHOLLY_COMPONENT_EXPORT const chtholly_component_descriptor_v1 *
chtholly_component_query_v1(void) {
  return &descriptor;
}
