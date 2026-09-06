// The control for the `thirdparty` check: the one directory Jolt may be named from.
//
// A rule that fires everywhere is as broken as one that fires nowhere, and this is the file that
// catches the first case — it is what src/backends/physics-jolt/ does in the real tree.
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>

void make_body() {}
