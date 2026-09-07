#include <cy/build/description.h>

#include "text.h"

namespace cy::build {
namespace {

[[nodiscard]] Error invalid(const char* message) noexcept {
    return Error{ErrorCode::InvalidArgument, message, 0};
}

[[nodiscard]] Expected<u32, Error> parse_u32(std::string_view word) {
    if (word.empty()) {
        return make_unexpected(invalid("expected a version number"));
    }
    u64 value = 0;
    for (const char character : word) {
        if (character < '0' || character > '9') {
            return make_unexpected(invalid("expected a version number"));
        }
        value = (value * 10) + static_cast<u64>(character - '0');
    }
    return static_cast<u32>(value);
}

/// One node's head line: `node "<name>" <kind> "<producer>" [<version>]`.
[[nodiscard]] Status begin_node(const text::Line& line, const ProducerRegistry* producers,
                                NodeDesc& out) {
    if (line.words.size() < 4) {
        return make_unexpected(invalid("a node needs a name, a kind and a producer"));
    }
    const Expected<NodeKind, Error> kind = node_kind_from_name(line.word(2));
    if (!kind) {
        return make_unexpected(kind.error());
    }
    out = NodeDesc{};
    out.name = std::string(line.word(1));
    out.kind = *kind;
    out.producer = std::string(line.word(3));

    if (line.words.size() >= 5) {
        const Expected<u32, Error> version = parse_u32(line.word(4));
        if (!version) {
            return make_unexpected(version.error());
        }
        out.producer_version = *version;
        return ok();
    }
    if (producers != nullptr) {
        const Producer* producer = producers->find(out.producer);
        if (producer == nullptr) {
            return make_unexpected(Error{ErrorCode::NotFound, "no such producer", 0});
        }
        out.producer_version = producer->version;
        out.distributable = producer->distributable;
    }
    return ok();
}

/// One indented field of the node being read.
[[nodiscard]] Status read_field(const text::Line& line, NodeDesc& node) {
    const std::string_view keyword = line.word(0);
    const std::string value(line.word(1));
    if (keyword == "platform") {
        node.platform = value;
    } else if (keyword == "profile") {
        node.profile = value;
    } else if (keyword == "bundle") {
        node.bundle = value;
    } else if (keyword == "source") {
        node.sources.push_back(value);
    } else if (keyword == "upstream") {
        node.upstreams.push_back(value);
    } else if (keyword == "output") {
        node.outputs.push_back(value);
    } else if (keyword == "distributable") {
        node.distributable = value != "false";
    } else if (keyword == "option") {
        if (line.words.size() < 3) {
            return make_unexpected(invalid("an option needs a name and a value"));
        }
        node.options.push_back(NodeOption{value, std::string(line.word(2))});
    } else {
        return make_unexpected(invalid("unknown node field"));
    }
    return ok();
}

}  // namespace

Status read_description(std::string_view document, BuildGraph& out,
                        const ProducerRegistry* producers) {
    const Expected<std::vector<text::Line>, Error> lines = text::read(document);
    if (!lines) {
        return make_unexpected(lines.error());
    }
    if (lines->empty() || (*lines)[0].word(0) != "cybuild" || (*lines)[0].word(1) != "1") {
        return make_unexpected(invalid("not a cybuild 1 document"));
    }

    NodeDesc node;
    bool have_node = false;
    const auto flush = [&]() -> Status {
        if (!have_node) {
            return ok();
        }
        const Expected<NodeId, Error> added = out.add(std::move(node));
        have_node = false;
        return added ? ok() : make_unexpected(added.error());
    };

    for (usize index = 1; index < lines->size(); ++index) {
        const text::Line& line = (*lines)[index];
        if (line.depth == 0) {
            if (line.word(0) != "node") {
                return make_unexpected(invalid("expected a node record"));
            }
            if (Status flushed = flush(); !flushed) {
                return flushed;
            }
            if (Status started = begin_node(line, producers, node); !started) {
                return started;
            }
            have_node = true;
            continue;
        }
        if (!have_node) {
            return make_unexpected(invalid("a field outside a node"));
        }
        if (Status read = read_field(line, node); !read) {
            return read;
        }
    }
    if (Status flushed = flush(); !flushed) {
        return flushed;
    }
    return out.finalize();
}

std::string write_description(const BuildGraph& graph) {
    std::string out = "cybuild 1\n";
    for (u32 index = 0; index < graph.size(); ++index) {
        const NodeDesc& node = graph.node(static_cast<NodeId>(index));
        out += "node ";
        out += text::quote(node.name);
        out += ' ';
        out += node_kind_name(node.kind);
        out += ' ';
        out += text::quote(node.producer);
        out += ' ';
        char version[16] = {};
        usize length = 0;
        u32 remaining = node.producer_version;
        do {
            version[length++] = static_cast<char>('0' + (remaining % 10));
            remaining /= 10;
        } while (remaining != 0);
        for (usize position = length; position > 0; --position) {
            out += version[position - 1];
        }
        out += '\n';

        const auto field = [&out](const char* keyword, std::string_view value) {
            out += "  ";
            out += keyword;
            out += ' ';
            out += text::quote(value);
            out += '\n';
        };
        field("platform", node.platform);
        field("profile", node.profile);
        if (!node.bundle.empty()) {
            field("bundle", node.bundle);
        }
        for (const std::string& source : node.sources) {
            field("source", source);
        }
        for (const std::string& upstream : node.upstreams) {
            field("upstream", upstream);
        }
        for (const std::string& output : node.outputs) {
            field("output", output);
        }
        for (const NodeOption& option : node.options) {
            out += "  option ";
            out += text::quote(option.name);
            out += ' ';
            out += text::quote(option.value);
            out += '\n';
        }
        if (!node.distributable) {
            out += "  distributable false\n";
        }
    }
    return out;
}

}  // namespace cy::build
