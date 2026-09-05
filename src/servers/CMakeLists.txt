# src/servers/ — layer 2: the singleton facades `engine-architecture` calls servers.
#
# A server owns all of its subsystem's state, addresses every object through opaque generational
# handles, and **has no knowledge of the ECS world, the scene graph, or scripting**. That last
# clause is why this layer sits below the backends rather than above them: a server is an interface
# the runtime wires a backend to, not a thing that reaches down to one.
#
# WHAT THAT COSTS, STATED ONCE HERE SO IT IS NOT REDISCOVERED PER MODULE. Layer 2 is *below*
# src/backends/ (3) and src/rendering/ (4), so a server may not include `cy/backends/rhi/**` or
# `cy/rendering/**`. The render server therefore holds no device handle, no image format the RHI
# named, and no render graph. It holds the model — scenes, views, instances, meshes, materials — in
# engine-owned types, and the modules above it turn that model into GPU work.
#
# `engine-architecture` names seven servers. M3 opened the layer with the first of them. M4 adds the
# camera and the audio server; the rest arrive with their milestones, in the same shape.
add_subdirectory(render)

# CyberCamera. Added after render/ because it consumes `cy::render::ViewDescription` — a sideways
# dependency within layer 2, which the layer checker permits and which camera/CMakeLists.txt
# explains at length.
add_subdirectory(camera)

# The audio server: the driver interface, the bus graph, voices and spatialisation. The miniaudio
# backend beneath it is src/backends/audio-miniaudio/, at layer 3, because no miniaudio type may
# appear above src/backends/.
add_subdirectory(audio)

# CyberInput. Appended last to minimise the conflict with the other M4 servers being added to this
# file at the same time; the order of these lines does not matter, because every one of them is a
# layer-2 module and none depends on another.
#
# The one worth knowing about this server: it is the one whose *absence* is a supported
# configuration. `input-and-actions` requires the action and command path to work with no device
# backend at all, so this module opens no device, the headless case is the default rather than a
# mode, and every test in it runs with no platform layer. See src/servers/input/CMakeLists.txt.
add_subdirectory(input)

# CyberPhysics: `PhysicsServer`, the reference backend that was written before Jolt, the character
# controller, the fixed-step integration on the simulation clock and the determinism probe.
# Appended last for the same reason CyberInput was — several M4 servers land in this file at once
# and the order of these lines does not matter.
#
# It is NOT gated by CY_PHYSICS. That option gates the Jolt backend in src/backends/physics-jolt/
# and the dependency fetch behind it; the interface and its reference implementation compile in
# every build, so the suites that exercise them run in every configuration rather than in the ones
# somebody remembered to turn an option on for.
add_subdirectory(physics)
