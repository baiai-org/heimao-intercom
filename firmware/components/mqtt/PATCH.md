# Project MQTT override

Copied from the ESP-IDF v5.4.1 esp-mqtt component used by this project.
The upstream license is preserved in LICENSE.

Includes the existing nonblocking partial-message read fix (tools/patches/0001)
and PSRAM task-stack allocation (0002). This override additionally pairs all
task creation paths with vTaskDeleteWithCaps, and publishes STOPPED_BIT only
after the last client access. Repeated relay switches therefore reclaim stacks
and cannot destroy a client while its worker still accesses its state.

This component takes priority over the SDK mqtt component. No edits to the
shared .idf checkout are required. Both CPU-affinity and fallback allocations
use the WithCaps family; the internal fallback retains the same deletion API.
