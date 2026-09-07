// Included by PlaceState.cpp; this fragment retains the shared place-state
// stores and worklists without introducing a second analysis context.
class PlaceStateAnalyzer {
public:
#include "PlaceStateAnalyzerSetup.inc"
#include "PlaceStateAnalyzerPlace.inc"
#include "PlaceStateAnalyzerCleanup.inc"
#include "PlaceStateAnalyzerCall.inc"

#include "PlaceStateAnalyzerFlow.inc"
#include "PlaceStateAnalyzerMembers.inc"
};
