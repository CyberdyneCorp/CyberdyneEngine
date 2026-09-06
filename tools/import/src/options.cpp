#include <cy/import/options.h>

#include <algorithm>
#include <cstring>

namespace cy::import {

const char* option_type_name(OptionType type) noexcept {
    switch (type) {
        case OptionType::Bool:
            return "bool";
        case OptionType::Int:
            return "int";
        case OptionType::Float:
            return "float";
        case OptionType::Text:
            return "text";
        case OptionType::Enumeration:
            return "enumeration";
    }
    return "bool";
}

// --- OptionValue ---------------------------------------------------------------------------------

bool OptionValue::as_bool() const noexcept {
    CY_ASSERT_MSG(type_ == OptionType::Bool, "as_bool() on an option that is not a bool");
    return boolean_;
}

i64 OptionValue::as_int() const noexcept {
    CY_ASSERT_MSG(type_ == OptionType::Int, "as_int() on an option that is not an int");
    return integer_;
}

f64 OptionValue::as_float() const noexcept {
    CY_ASSERT_MSG(type_ == OptionType::Float, "as_float() on an option that is not a float");
    return real_;
}

std::string_view OptionValue::as_text() const noexcept {
    CY_ASSERT_MSG(type_ == OptionType::Text || type_ == OptionType::Enumeration,
                  "as_text() on an option that holds no text");
    return text_;
}

bool operator==(const OptionValue& a, const OptionValue& b) noexcept {
    if (a.type_ != b.type_) {
        return false;
    }
    switch (a.type_) {
        case OptionType::Bool:
            return a.boolean_ == b.boolean_;
        case OptionType::Int:
            return a.integer_ == b.integer_;
        // Bit equality rather than numeric: two options are the same option when they produce the
        // same cache key, the key is built from the bits, and two NaNs that compare unequal
        // numerically would make an unchanged option look changed on every import.
        case OptionType::Float: {
            u64 left = 0;
            u64 right = 0;
            std::memcpy(&left, &a.real_, sizeof(f64));
            std::memcpy(&right, &b.real_, sizeof(f64));
            return left == right;
        }
        case OptionType::Text:
        case OptionType::Enumeration:
            return a.text_ == b.text_;
    }
    return false;
}

// --- OptionsSchema -------------------------------------------------------------------------------

const OptionSpec* OptionsSchema::find(std::string_view name) const noexcept {
    for (const OptionSpec& option : options_) {
        if (option.name == name) {
            return &option;
        }
    }
    return nullptr;
}

Status OptionsSchema::validate() const noexcept {
    for (usize index = 0; index < options_.size(); ++index) {
        const OptionSpec& option = options_[index];
        if (option.name.empty()) {
            return fail(ErrorCode::InvalidArgument, "an import option has no name");
        }
        if (option.description.empty()) {
            return fail(ErrorCode::InvalidArgument,
                        "an import option has no description saying what it is for");
        }
        for (usize other = 0; other < index; ++other) {
            if (options_[other].name == option.name) {
                return fail(ErrorCode::AlreadyExists, "two import options share a name");
            }
        }
        if (option.default_value.type() != option.type) {
            return fail(ErrorCode::InvalidArgument,
                        "an import option's default is not of its declared type");
        }
        if (option.type == OptionType::Enumeration) {
            if (option.choices.empty()) {
                return fail(ErrorCode::InvalidArgument,
                            "an enumeration option declares no choices, so nothing can be set");
            }
            bool found = false;
            for (const std::string_view choice : option.choices) {
                found = found || choice == option.default_value.as_text();
            }
            if (!found) {
                return fail(ErrorCode::InvalidArgument,
                            "an enumeration option's default is not one of its choices");
            }
        } else if (!option.choices.empty()) {
            return fail(ErrorCode::InvalidArgument,
                        "only an enumeration option may declare choices");
        }
        if (option.minimum != option.maximum) {
            const f64 value = option.type == OptionType::Int
                                  ? static_cast<f64>(option.default_value.as_int())
                                  : option.default_value.as_float();
            if (option.type != OptionType::Int && option.type != OptionType::Float) {
                return fail(ErrorCode::InvalidArgument,
                            "only a numeric import option may declare bounds");
            }
            if (value < option.minimum || value > option.maximum) {
                return fail(ErrorCode::InvalidArgument,
                            "an import option's default is outside its own bounds");
            }
        }
    }
    return ok();
}

// --- ImportOptions -------------------------------------------------------------------------------

Status ImportOptions::set(const OptionsSchema& schema, std::string_view name,
                          const OptionValue& value) noexcept {
    const OptionSpec* declared = schema.find(name);
    if (declared == nullptr) {
        // The refusal that keeps the cache honest: an option that changed the output and not the
        // derivation key is the one defect content addressing cannot survive.
        return fail(ErrorCode::NotFound, "this importer declares no option of that name");
    }
    if (value.type() != declared->type) {
        return fail(ErrorCode::InvalidArgument, "the value is not of the option's declared type");
    }
    if (declared->type == OptionType::Enumeration) {
        bool found = false;
        for (const std::string_view choice : declared->choices) {
            found = found || choice == value.as_text();
        }
        if (!found) {
            return fail(ErrorCode::InvalidArgument, "not one of the option's declared choices");
        }
    }
    if (declared->minimum != declared->maximum) {
        const f64 numeric =
            declared->type == OptionType::Int ? static_cast<f64>(value.as_int()) : value.as_float();
        if (numeric < declared->minimum || numeric > declared->maximum) {
            return fail(ErrorCode::OutOfRange, "the value is outside the option's bounds");
        }
    }

    for (Entry& entry : values_) {
        if (entry.name == name) {
            // Replacing in place keeps the set order, so a sidecar rewritten after one edit is a
            // one-line diff rather than a reordering.
            entry.value = value;
            return ok();
        }
    }
    // The name recorded is the SCHEMA's, not the caller's: the caller's may be a slice of a buffer
    // it is about to reuse, and the schema's is a literal that outlives everything.
    return values_.push_back(Entry{declared->name, value});
}

Expected<OptionValue, Error> ImportOptions::get(const OptionsSchema& schema,
                                                std::string_view name) const noexcept {
    const OptionSpec* declared = schema.find(name);
    if (declared == nullptr) {
        return fail(ErrorCode::NotFound, "this importer declares no option of that name");
    }
    for (const Entry& entry : values_) {
        if (entry.name == name) {
            return entry.value;
        }
    }
    return declared->default_value;
}

bool ImportOptions::is_set(std::string_view name) const noexcept {
    return std::ranges::any_of(values_,
                               [name](const Entry& entry) noexcept { return entry.name == name; });
}

std::string_view ImportOptions::name_at(usize index) const noexcept {
    CY_ASSERT_MSG(index < values_.size(), "an option index past the end");
    return values_[index].name;
}

const OptionValue& ImportOptions::value_at(usize index) const noexcept {
    CY_ASSERT_MSG(index < values_.size(), "an option index past the end");
    return values_[index].value;
}

void ImportOptions::contribute_to(const OptionsSchema& schema,
                                  assets::DerivationKeyBuilder& builder) const noexcept {
    for (const OptionSpec& declared : schema.options()) {
        OptionValue value = declared.default_value;
        for (const Entry& entry : values_) {
            if (entry.name == declared.name) {
                value = entry.value;
                break;
            }
        }
        switch (value.type()) {
            case OptionType::Bool:
                builder.flag(declared.name, value.as_bool());
                break;
            case OptionType::Int:
                builder.number(declared.name, static_cast<u64>(value.as_int()));
                break;
            case OptionType::Float: {
                // The bits, not a rendering. A float written as text and hashed would make the key
                // depend on the formatter's precision, and two builds with different libc would
                // disagree about whether their caches matched.
                const f64 real = value.as_float();
                builder.bytes(declared.name, &real, sizeof(real));
                break;
            }
            case OptionType::Text:
            case OptionType::Enumeration:
                builder.text(declared.name, value.as_text());
                break;
        }
    }
}

}  // namespace cy::import
