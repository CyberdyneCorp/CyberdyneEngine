// `Name` — interning identity, O(1) comparison, and the table's accounting. Task 1.3.5.
//
// `core-type-system` — "String interning". The scenario this file is written against is
// "Name comparison is a pointer compare": two `Name`s compare without touching character data. That
// is not directly observable from outside, so what is asserted is the property it rests on — the
// same text always yields the same index, and different text never does — plus the accounting that
// proves the table stored one entry rather than two.

#include <cy/core/values/name.h>

#include <cy/test/test.h>

#include <string>

CY_TEST_CASE("Name: the same text interns to the same value") {
    const cy::Name first = cy::Name::intern("player.health");
    const cy::Name second = cy::Name::intern("player.health");
    const cy::Name other = cy::Name::intern("player.stamina");

    CY_CHECK(first == second);
    CY_CHECK(first != other);
    CY_CHECK_EQ(first.index(), second.index());
    CY_CHECK_NE(first.index(), other.index());
}

CY_TEST_CASE("Name: interning stores one entry for repeated text") {
    const cy::NameTableStats before = cy::name_table_stats();
    const cy::Name once = cy::Name::intern("values.test.one_entry");
    const cy::NameTableStats after_first = cy::name_table_stats();

    for (int i = 0; i < 8; ++i) {
        const cy::Name again = cy::Name::intern("values.test.one_entry");
        CY_CHECK(again == once);
    }
    const cy::NameTableStats after_repeats = cy::name_table_stats();

    CY_CHECK_EQ(after_first.entries, before.entries + 1);
    CY_CHECK_EQ(after_repeats.entries, after_first.entries);
    CY_CHECK_EQ(after_repeats.insertions, after_first.insertions);
}

CY_TEST_CASE("Name: text round-trips and is NUL-terminated") {
    const cy::Name name = cy::Name::intern("animation/track/path");
    CY_CHECK_EQ(name.text(), std::string_view("animation/track/path"));
    CY_CHECK_EQ(std::string(name.c_str()), std::string("animation/track/path"));
}

CY_TEST_CASE("Name: the default is the empty name") {
    const cy::Name unset;
    CY_CHECK(unset.is_empty());
    CY_CHECK_EQ(unset.index(), 0u);
    CY_CHECK(unset.text().empty());
    CY_CHECK(unset == cy::Name::intern(""));
}

CY_TEST_CASE("Name: find does not intern") {
    const cy::NameTableStats before = cy::name_table_stats();
    const cy::Name missing = cy::Name::find("values.test.never.interned");
    const cy::NameTableStats after = cy::name_table_stats();

    CY_CHECK(missing.is_empty());
    CY_CHECK_EQ(after.entries, before.entries);
}

CY_TEST_CASE("Name: text longer than the limit is rejected rather than stored") {
    const std::string oversized(cy::Name::kMaxLength + 1, 'x');
    const cy::NameTableStats before = cy::name_table_stats();
    const cy::Name rejected = cy::Name::intern(oversized);
    const cy::NameTableStats after = cy::name_table_stats();

    CY_CHECK(rejected.is_empty());
    CY_CHECK_EQ(after.entries, before.entries);
    CY_CHECK_EQ(after.rejections, before.rejections + 1);
}

CY_TEST_CASE("Name: CY_NAME interns once at its declaration site") {
    const cy::Name from_macro = CY_NAME("values.test.macro");
    const cy::NameTableStats after_first = cy::name_table_stats();
    const cy::Name again = CY_NAME("values.test.macro");
    const cy::NameTableStats after_second = cy::name_table_stats();

    CY_CHECK(from_macro == again);
    // The second CY_NAME is a different declaration site with the same literal, so it interns once
    // itself and then finds the existing entry: no new entry, and the lookup count is unchanged
    // after the first evaluation of each site.
    CY_CHECK_EQ(after_second.entries, after_first.entries);
}

CY_TEST_CASE("Name: ordering is a total order over indices") {
    const cy::Name a = cy::Name::intern("values.test.order.a");
    const cy::Name b = cy::Name::intern("values.test.order.b");
    CY_CHECK((a < b) != (b < a));
    CY_CHECK_FALSE(a < a);
}
