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
# `engine-architecture` names seven servers. M3 opens the layer with the first of them; physics and
# audio arrive at M4 and M7 and are added here in the same shape.
add_subdirectory(render)
