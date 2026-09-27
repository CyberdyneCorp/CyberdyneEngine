// SPDX-License-Identifier: MIT
// A look from disk. See the header.

#include <cy/rendering/grading/look_file.h>

#include <cy/core/assets/file.h>

#include <cstdio>
#include <cstring>
#include <string_view>

namespace cy::rendering::grading {
namespace {

/// A whole file's text, read into `out`.
[[nodiscard]] Status read_text(const char* path, Array<char>& out) noexcept {
    Expected<assets::File, Error> file = assets::File::open(path, assets::FileMode::Read);
    if (!file.has_value()) {
        return make_unexpected(file.error());
    }
    const Expected<u64, Error> size = file->size();
    if (!size.has_value()) {
        return make_unexpected(size.error());
    }
    if (Status sized = out.resize(static_cast<usize>(*size)); !sized) {
        return sized;
    }
    const Expected<usize, Error> read = file->read(out.data(), out.size());
    if (!read.has_value()) {
        return make_unexpected(read.error());
    }
    if (*read != out.size()) {
        return fail(ErrorCode::Io, "grading: a look or cube file was read short");
    }
    return ok();
}

/// `relative` resolved against the directory `sibling` is in.
[[nodiscard]] bool beside(const char* sibling, const char* relative, char (&out)[1024]) noexcept {
    const std::string_view path(sibling);
    const usize slash = path.find_last_of('/');
    const int written = slash == std::string_view::npos
                            ? std::snprintf(out, sizeof(out), "%s", relative)
                            : std::snprintf(out, sizeof(out), "%.*s/%s", static_cast<int>(slash),
                                            sibling, relative);
    return written > 0 && static_cast<usize>(written) < sizeof(out);
}

}  // namespace

Status load_cube(const char* path, CubeLut& out) noexcept {
    Array<char> text(out.entries.allocator());
    if (Status read = read_text(path, text); !read) {
        return read;
    }
    return parse_cube_lut(std::string_view(text.data(), text.size()), out);
}

Status load_look(const char* path, Allocator& allocator, Look& look, Array<Vec3>& table) noexcept {
    Array<char> text(allocator);
    if (Status read = read_text(path, text); !read) {
        return read;
    }
    if (Status parsed = parse_look(std::string_view(text.data(), text.size()), look); !parsed) {
        return parsed;
    }
    CubeLut cube(allocator);
    DisplayGrade grade;
    grade.settings = look.settings;
    grade.cube_encoding = look.cube_encoding;
    if (look.cube[0] != '\0') {
        char cube_path[1024];
        if (!beside(path, look.cube, cube_path)) {
            return fail(ErrorCode::InvalidArgument, "grading: the cube's path is too long");
        }
        if (Status loaded = load_cube(cube_path, cube); !loaded) {
            return loaded;
        }
        grade.cube = &cube;
    }
    const usize count = static_cast<usize>(look.lut_size) * look.lut_size * look.lut_size;
    if (Status sized = table.resize(count); !sized) {
        return sized;
    }
    if (!bake_display_lut(grade, look.lut_size, table.data(), table.size())) {
        return fail(ErrorCode::InvalidArgument, "grading: the look's table could not be baked");
    }
    return ok();
}

}  // namespace cy::rendering::grading
