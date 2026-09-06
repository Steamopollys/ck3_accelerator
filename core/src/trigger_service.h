#pragma once
#include <ck3accel/core_api.h>

// Shared trigger-evaluator hook. The core hooks the evaluator once; consumers register handlers that
// run as a chain ending in the original, so several plugins can observe or override triggers at once.
namespace ck3accel {
bool trigger_service_ensure();                                                   // install the hook (idempotent)
void trigger_service_register(ck3accel_trigger_handler h, void* user, int priority);  // higher priority = outer
}
