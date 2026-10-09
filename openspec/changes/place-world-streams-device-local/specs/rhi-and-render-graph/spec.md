## ADDED Requirements

### Requirement: Host-visible device-local memory is reported from the device's memory types
A backend SHALL report `Capability::HostVisibleDeviceLocalMemory` from the memory types the device
lists, and SHALL report it whenever one of them is device-local, host-visible and host-coherent.
A backend that observed no memory types SHALL NOT report it. A caller that writes a buffer through
a mapping and wants the device to read it locally SHALL ask this capability before it requests
`MemoryUse::HostVisibleDeviceLocal`, and SHALL use `MemoryUse::Upload` when the capability is
absent.

#### Scenario: A discrete GPU with a mappable device-local type
- **WHEN** a Vulkan device lists a memory type that is device-local, host-visible and host-coherent
- **THEN** the device SHALL report `Capability::HostVisibleDeviceLocalMemory`

#### Scenario: Streams read more than once a frame
- **WHEN** `samples/10-world` creates the vertex, colour and index streams its frame draws three
  times, on a device that reports the capability
- **THEN** it SHALL create them in `MemoryUse::HostVisibleDeviceLocal`, and three device reads of a
  world-sized stream there SHALL take at most half the device time they take from `Upload` memory
  where the device has system memory apart from its own
