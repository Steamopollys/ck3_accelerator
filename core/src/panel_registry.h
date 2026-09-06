#pragma once
#include <ck3accel/core_api.h>

// Control-panel registry. Plugins publish a CK3AccelPanel at load; an overlay reads them back.
// Registration happens on the loader thread before rendering, so reads afterward are stable.
namespace ck3accel {
void panel_register(const CK3AccelPanel* p);   // shallow-copies p into the registry
int  panel_count();
const CK3AccelPanel* panel_get(int index);     // null if out of range
}
