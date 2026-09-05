// The ABI's layout, checked against the compiler rather than trusted. Task 2.1.
//
// `native-abi`: "WHEN a struct crosses the ABI THEN its layout SHALL be fixed-width, explicitly
// padded, and asserted with `static_assert(sizeof(...))` on both sides".
//
// cy/abi/cy_abi.h carries the sizes, so a module compiled by another compiler fails at its own
// compile. This file carries the *offsets*, and it carries them for one reason that the header
// cannot: `tools/abi/abi_describe.py` computes every offset itself, from the declarations alone,
// under the layout model the C ABI fixes — and the description it produces is what the Swift
// overlay and the Rust SDK are generated from. If the model and the compiler ever disagree, the
// overlays are generated against a struct that does not exist, and nothing else in the tree would
// notice. So the numbers below are the model's, written out, and the compiler is asked to agree.
//
// A failure here means one of two things and the fix differs: either a struct was edited and the
// description regenerated (in which case update these numbers with it), or the layout model in
// abi_describe.py is wrong on this platform (in which case the description is what must change).

#include <cy/abi/cy_abi.h>
#include <cy/abi/host.h>
#include <cy/abi/var.h>
#include <cy/test/test.h>

#include <cstddef>

CY_TEST_CASE("every ABI struct has the size the header declares") {
    // The header already static_asserts these; repeating them as runtime checks is what puts them
    // in the test report, so a reader of a passing run can see that the layout was checked at all.
    CY_CHECK_EQ(sizeof(CyVarPayload), 16U);
    CY_CHECK_EQ(sizeof(CyVar), 32U);
    CY_CHECK_EQ(sizeof(CyFieldDesc), 24U);
    CY_CHECK_EQ(sizeof(CyComponentTypeDesc), 32U);
    CY_CHECK_EQ(sizeof(CyBehaviourVTable), 56U);
    CY_CHECK_EQ(sizeof(CyBorrow), 16U);
    CY_CHECK_EQ(sizeof(CyInterfaceHeader), 16U);
    CY_CHECK_EQ(sizeof(CyModuleInit), 40U);
}

CY_TEST_CASE("every ABI struct has the offsets the description generator computes") {
    CY_CHECK_EQ(offsetof(CyVar, type), 0U);
    CY_CHECK_EQ(offsetof(CyVar, flags), 4U);
    CY_CHECK_EQ(offsetof(CyVar, length), 8U);
    CY_CHECK_EQ(offsetof(CyVar, payload), 16U);

    CY_CHECK_EQ(offsetof(CyFieldDesc, struct_size), 0U);
    CY_CHECK_EQ(offsetof(CyFieldDesc, type), 4U);
    CY_CHECK_EQ(offsetof(CyFieldDesc, offset), 8U);
    CY_CHECK_EQ(offsetof(CyFieldDesc, size), 12U);
    CY_CHECK_EQ(offsetof(CyFieldDesc, name), 16U);

    CY_CHECK_EQ(offsetof(CyComponentTypeDesc, struct_size), 0U);
    CY_CHECK_EQ(offsetof(CyComponentTypeDesc, field_count), 12U);
    CY_CHECK_EQ(offsetof(CyComponentTypeDesc, name), 16U);
    CY_CHECK_EQ(offsetof(CyComponentTypeDesc, fields), 24U);

    CY_CHECK_EQ(offsetof(CyBehaviourVTable, struct_size), 0U);
    CY_CHECK_EQ(offsetof(CyBehaviourVTable, schema_version), 4U);
    CY_CHECK_EQ(offsetof(CyBehaviourVTable, create), 8U);
    CY_CHECK_EQ(offsetof(CyBehaviourVTable, user_data), 48U);

    CY_CHECK_EQ(offsetof(CyInterfaceHeader, table_size), 12U);
    CY_CHECK_EQ(offsetof(CyModuleInit, initialize), 16U);
    CY_CHECK_EQ(offsetof(CyBorrow, epoch), 8U);
}

CY_TEST_CASE("the interface table starts with its header, which is what makes growth readable") {
    // A module reads `table_size` before it reads anything else, so the header cannot move. If it
    // ever did, an older module would read a function pointer as a version number.
    CY_CHECK_EQ(offsetof(CyInterface, header), 0U);
    CY_CHECK_EQ(sizeof(CyInterface) % sizeof(void*), 0U);
}

CY_TEST_CASE("the ABI's types are the C types they claim to be") {
    // Fixed width, not "whatever the compiler chose". An `int` in this header would be an ABI that
    // changes shape between two targets that both compile it.
    CY_CHECK_EQ(sizeof(CyEntity), 8U);
    CY_CHECK_EQ(sizeof(CyComponentTypeId), 4U);
    CY_CHECK(static_cast<CyEntity>(CY_ENTITY_NULL) == 0U);

    // The handles are pointers to distinct incomplete types, so the compiler rejects passing one
    // where another is expected. That is checked by the fact that this file compiles at all, and
    // stated here so the property is not deleted by accident.
    CY_CHECK_EQ(sizeof(CyEngine), sizeof(void*));
    CY_CHECK_EQ(sizeof(CyWorld), sizeof(void*));
}

CY_TEST_CASE("the var blob header is the size var_release subtracts") {
    CY_CHECK_EQ(cy::abi::kVarBlobHeaderSize, 32U);
}
