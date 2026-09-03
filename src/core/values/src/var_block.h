#pragma once
// The reference-counted blocks behind `Var`'s heap kinds. Private to the module.
//
// A block is a header — an atomic reference count and the kind it holds — followed by exactly one
// payload. There is no virtual function and no type-erased deleter: the kind in the header selects
// the derived type in one switch, in var.cpp, which is also the only place a block is created or
// destroyed. That keeps the header two words and keeps `Var` free of anything that would make it
// non-trivially-relocatable in the containers it will be stored in.

#include <cy/core/values/callable.h>
#include <cy/core/values/var.h>

#include <atomic>
#include <string>
#include <vector>

namespace cy::detail {

struct VarBlock {
    /// Mutable so that copying a `const Var` can retain. The count is the only mutable state a
    /// shared block has; the payload is never written while the count is above one, because
    /// mutation detaches first.
    mutable std::atomic<u32> refs{1};
    VarType type = VarType::Nil;
};

struct StringBlock : VarBlock {
    std::string text;
};
struct BytesBlock : VarBlock {
    std::vector<u8> data;
};
struct ArrayBlock : VarBlock {
    VarArray items;
};
struct DictBlock : VarBlock {
    VarDict dict;
};
struct Mat3Block : VarBlock {
    VarMat3 value;
};
struct Mat4Block : VarBlock {
    VarMat4 value;
};
struct TransformBlock : VarBlock {
    VarTransform value;
};
struct AabbBlock : VarBlock {
    VarAabb value;
};
struct CallableBlock : VarBlock {
    Callable value;
};

}  // namespace cy::detail
