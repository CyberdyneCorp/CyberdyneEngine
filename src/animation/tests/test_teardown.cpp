// Teardown under load: pose worlds, batches and their instances created and destroyed while the
// machine is busy. M8.b section 5, and the milestone brief's hard rule 4.
//
// WHY THIS IS NOT A UNIT TEST. Sixty-four rounds of building a rig, filling a batch and tearing
// both down, with four spinner threads holding the cores, is the shape that reproduces a lifetime
// bug — M5.5's gate found one in a job bridge freeing its pool underneath a worker, and it did not
// reproduce until the machine was busy. A one-millisecond budget would be met by trimming the
// rounds until the race stopped reproducing, which is the opposite of the point.

#include <cy/animation/evaluate.h>
#include <cy/animation/pose_world.h>
#include <cy/graph/cybergraph.h>
#include <cy/graph/lower_pose.h>
#include <cy/test/test.h>

#include "fixture.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace cy;
using namespace cy::animation;
using namespace cy::animation::testing;

namespace {

[[nodiscard]] graph::Literal text(const char* value) noexcept {
    graph::Literal literal;
    literal.type = Name::intern("name");
    literal.text = Name::intern(value);
    return literal;
}

/// The smallest graph that samples a clip: one clip node into one state.
[[nodiscard]] Status build_one_state(graph::Graph& graph) {
    if (Status added = graph.add_node(1, Name::intern("pose.clip")); !added) {
        return added;
    }
    if (Status set = graph.set_property(1, Name::intern("clip"), text("walk")); !set) {
        return set;
    }
    if (Status set = graph.set_property(1, Name::intern("time_parameter"), text("walk_time"));
        !set) {
        return set;
    }
    if (Status added = graph.add_node(2, Name::intern("pose.state")); !added) {
        return added;
    }
    return graph.connect(1, Name::intern("pose"), 2, Name::intern("pose"));
}

}  // namespace

CY_TEST_CASE("animation teardown: rigs, batches and pose worlds are torn down under load") {
    std::atomic<bool> stop{false};
    std::atomic<u64> spun{0};
    std::vector<std::thread> spinners;
    spinners.reserve(4);
    for (u32 worker = 0; worker < 4; ++worker) {
        spinners.emplace_back([&stop, &spun]() {
            u64 local = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                for (u32 step = 0; step < 4096; ++step) {
                    local += step;
                }
            }
            spun.fetch_add(local, std::memory_order_relaxed);
        });
    }

    u32 rounds = 0;
    u64 evaluated = 0;
    for (u32 round = 0; round < 64; ++round) {
        Skeleton skeleton(allocator());
        CY_REQUIRE(build_biped(skeleton).has_value());
        Clip walk(allocator());
        CY_REQUIRE(build_walk(walk).has_value());

        graph::NodeRegistry registry(allocator());
        CY_REQUIRE(graph::pose::register_pose_nodes(registry).has_value());
        graph::Graph graph(allocator(), Name::intern("teardown"));
        CY_REQUIRE(build_one_state(graph).has_value());
        graph.resolve(registry);
        graph::DiagnosticSink sink(allocator());
        auto program = graph::pose::compile_pose(graph, registry, kJointCount, sink);
        CY_REQUIRE(program.has_value());

        Array<const Clip*> clips(allocator());
        CY_REQUIRE(clips.resize(program.value().clips().size()).has_value());
        for (const Clip*& entry : clips) {
            entry = &walk;
        }

        AnimationRig rig(allocator());
        CY_REQUIRE(rig.bind(skeleton, program.value(), clips.span()).has_value());

        PoseWorld world(allocator());
        AnimationBatch batch(allocator(), rig);
        Array<PoseHandle> handles(allocator());
        for (u32 instance = 0; instance < 24; ++instance) {
            CY_REQUIRE(batch.add().has_value());
            Expected<PoseHandle, Error> handle = world.add(kJointCount);
            CY_REQUIRE(handle.has_value());
            CY_REQUIRE(handles.push_back(*handle).has_value());
        }

        PoseScratch scratch(allocator());
        CY_REQUIRE(scratch.prepare(rig).has_value());
        Array<Transform> poses(allocator());
        Array<Transform> model(allocator());
        Array<Mat4> matrices(allocator());
        CY_REQUIRE(poses.resize(static_cast<usize>(24) * kJointCount).has_value());
        CY_REQUIRE(model.resize(kJointCount).has_value());
        CY_REQUIRE(matrices.resize(kJointCount).has_value());

        EventBuffer events(allocator());
        EvaluationStats stats;
        for (u32 tick = 0; tick < 4; ++tick) {
            CY_REQUIRE(batch.advance_all(1.0F / 60.0F, &events).has_value());
            CY_REQUIRE(batch.evaluate_range(0, 24, 0, scratch, poses.span(), stats).has_value());
            for (u32 instance = 0; instance < 24; ++instance) {
                CY_REQUIRE(
                    publish_pose(skeleton,
                                 poses.span().subspan(static_cast<usize>(instance) * kJointCount,
                                                      kJointCount),
                                 0, world, handles[instance], model.span(), matrices.span())
                        .has_value());
            }
        }
        evaluated += stats.clips_sampled;

        // Half the instances leave while the rest are still publishing — the case a streaming world
        // hits every frame.
        for (u32 instance = 0; instance < 24; instance += 2) {
            CY_REQUIRE(world.remove(handles[instance]).has_value());
        }
        for (u32 instance = 1; instance < 24; instance += 2) {
            CY_REQUIRE(publish_pose(skeleton,
                                    poses.span().subspan(static_cast<usize>(instance) * kJointCount,
                                                         kJointCount),
                                    0, world, handles[instance], model.span(), matrices.span())
                           .has_value());
        }
        CY_CHECK_EQ(world.stats().instances, 12U);
        ++rounds;
        // Everything above goes out of scope here, in the order it was declared: the world before
        // the batch, the batch before the rig, the rig before the clip and the skeleton it points
        // at. Nothing outlives what it names.
    }

    stop.store(true, std::memory_order_relaxed);
    for (std::thread& spinner : spinners) {
        spinner.join();
    }
    CY_CHECK_EQ(rounds, 64U);
    CY_CHECK_GT(evaluated, 0U);
    CY_CHECK_GT(spun.load(std::memory_order_relaxed), 0U);
}
