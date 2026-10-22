// SPDX-License-Identifier: MIT
// cook_main.cpp — `cy_rts_map_cook`: the RTS test map's source in, its cooked package out.
//
//   cy_rts_map_cook --source content/rts/frontier.cymap --content-root . --out frontier.cypak
//
// The build runs it (samples/13-rts-api/CMakeLists.txt) so the sample and its tests find the
// package beside them; it prints one line of what the cook made and how long each step took.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "map_cook.h"

int main(int argc, char** argv) {
    sample::rts::map::CookRequest request;
    for (int index = 1; index + 1 < argc; index += 2) {
        const char* flag = argv[index];
        const char* value = argv[index + 1];
        if (std::strcmp(flag, "--source") == 0) {
            request.source = value;
        } else if (std::strcmp(flag, "--content-root") == 0) {
            request.content_root = value;
        } else if (std::strcmp(flag, "--out") == 0) {
            request.package = value;
        } else if (std::strcmp(flag, "--albedo-edge") == 0) {
            request.albedo_edge = static_cast<cy::u32>(std::strtoul(value, nullptr, 10));
        } else {
            std::fprintf(stderr, "cy_rts_map_cook: unknown argument '%s'\n", flag);
            return 2;
        }
    }
    if (request.source.empty() || request.package.empty()) {
        std::fprintf(stderr,
                     "usage: cy_rts_map_cook --source <map.cymap> --content-root <dir> "
                     "--out <map.cypak> [--albedo-edge <texels>]\n");
        return 2;
    }
    if (request.content_root.empty()) {
        request.content_root = ".";
    }
    sample::rts::map::MapContent content;
    sample::rts::map::CookReport report;
    std::string detail;
    if (const cy::Status cooked = sample::rts::map::cook_map(request, content, report, detail);
        !cooked) {
        std::fprintf(stderr, "cy_rts_map_cook: %s (%s)\n", cooked.error().message, detail.c_str());
        return 1;
    }
    std::printf(
        "rts map cook modifiers=%u terrain_edge=%u ground_triangles=%u painted_texels=%u "
        "albedo=%u meshes=%u placements=%u foliage=%u clusters=%u foliage_bytes=%llu "
        "candidates=%u rejected_weight=%u rejected_spacing=%u rejected_exclusion=%u on_roads=%u "
        "worker_vertices=%u package_bytes=%llu digest=%016llx terrain_ms=%.1f bake_ms=%.1f "
        "foliage_ms=%.1f total_ms=%.1f\n",
        report.modifiers, report.terrain_edge, report.ground_triangles, report.painted_texels,
        report.albedo_edge, report.meshes, report.placements, report.foliage,
        report.foliage_clusters, static_cast<unsigned long long>(report.foliage_bytes),
        report.foliage_candidates, report.foliage_rejected_weight, report.foliage_rejected_spacing,
        report.foliage_rejected_exclusion, report.foliage_on_roads, report.worker_vertices,
        static_cast<unsigned long long>(report.package_bytes),
        static_cast<unsigned long long>(sample::rts::map::content_digest(content)),
        report.terrain_ms, report.bake_ms, report.foliage_ms, report.total_ms);
    return 0;
}
