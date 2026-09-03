#pragma once
// A reader for the capture format, used by the tests.
//
// Deliberately a second implementation: the writer, this, and tools/trace/trace_inspect.py are
// three readings of format.h, and a change that breaks one breaks the others loudly. A capture is
// meant to be readable by something that is not the engine, and this is the cheapest proof of it.

#include <cy/core/diagnostics/format.h>
#include <cy/core/diagnostics/trace.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace cy_test {

using namespace cy::diag;

struct FieldMeta {
    std::string name;
    cy::diag::u8 type = 0;
    cy::diag::u8 privacy = 0;
};

struct ReadField {
    cy::diag::u32 field = 0;
    cy::diag::u16 flags = 0;
    cy::diag::u64 bits = 0;
    std::string text;
    [[nodiscard]] bool redacted() const { return (flags & cy::diag::format::kFieldRedacted) != 0; }
};

struct ReadRecord {
    cy::diag::u8 kind = 0;
    cy::diag::u8 channel = 0;
    cy::diag::u32 name = 0;
    cy::diag::u32 category = 0;
    cy::diag::u64 timestamp = 0;
    cy::diag::u64 a = 0;
    cy::diag::u64 b = 0;
    cy::diag::u32 thread = 0;
    std::vector<ReadField> fields;
};

struct LossEntry {
    cy::diag::u32 thread = 0;
    cy::diag::u8 channel = 0;
    cy::diag::u8 reason = 0;
    cy::diag::u64 count = 0;
};

struct Capture {
    bool valid = false;
    cy::diag::format::FileHeader header{};
    std::map<cy::diag::u32, std::string> names;
    std::map<cy::diag::u32, std::string> categories;
    std::map<cy::diag::u32, FieldMeta> fields;
    std::map<std::string, std::string> identity;
    std::vector<ReadRecord> records;
    std::vector<LossEntry> losses;
    std::vector<cy::diag::u8> bytes;
    cy::diag::u32 chunk_count = 0;

    [[nodiscard]] const std::string& name_of(cy::diag::u32 id) const {
        static const std::string empty;
        const auto found = names.find(id);
        return (found == names.end()) ? empty : found->second;
    }

    /// True when `needle` appears anywhere in the file, redaction markers included. The bluntest
    /// possible check that a classified value did not reach the artefact.
    [[nodiscard]] bool contains_bytes(const char* needle) const {
        const std::size_t length = std::strlen(needle);
        if (length == 0 || bytes.size() < length) {
            return false;
        }
        for (std::size_t offset = 0; offset + length <= bytes.size(); ++offset) {
            if (std::memcmp(bytes.data() + offset, needle, length) == 0) {
                return true;
            }
        }
        return false;
    }
};

class Cursor {
public:
    Cursor(const cy::diag::u8* data, std::size_t size) : data_(data), size_(size) {}

    template <class T>
    T read() {
        T value{};
        if (offset_ + sizeof(T) <= size_) {
            std::memcpy(&value, data_ + offset_, sizeof(T));
            offset_ += sizeof(T);
        }
        return value;
    }

    std::string read_string() {
        const auto length = read<cy::diag::u16>();
        std::string text;
        if (offset_ + length <= size_) {
            text.assign(reinterpret_cast<const char*>(data_ + offset_), length);
            offset_ += length;
        }
        return text;
    }

    [[nodiscard]] bool done() const { return offset_ >= size_; }

private:
    const cy::diag::u8* data_;
    std::size_t size_;
    std::size_t offset_ = 0;
};

inline void read_metadata(Capture& capture, const cy::diag::u8* payload, std::size_t bytes) {
    Cursor cursor(payload, bytes);
    const auto name_count = cursor.read<cy::diag::u32>();
    for (cy::diag::u32 index = 0; index < name_count; ++index) {
        const auto id = cursor.read<cy::diag::u32>();
        capture.names[id] = cursor.read_string();
    }
    const auto category_count = cursor.read<cy::diag::u32>();
    for (cy::diag::u32 index = 0; index < category_count; ++index) {
        const auto id = cursor.read<cy::diag::u32>();
        capture.categories[id] = cursor.read_string();
    }
    const auto field_count = cursor.read<cy::diag::u32>();
    for (cy::diag::u32 index = 0; index < field_count; ++index) {
        const auto id = cursor.read<cy::diag::u32>();
        FieldMeta meta;
        meta.type = cursor.read<cy::diag::u8>();
        meta.privacy = cursor.read<cy::diag::u8>();
        meta.name = cursor.read_string();
        capture.fields[id] = meta;
    }
    const auto identity_count = cursor.read<cy::diag::u32>();
    for (cy::diag::u32 index = 0; index < identity_count; ++index) {
        const std::string key = cursor.read_string();
        capture.identity[key] = cursor.read_string();
    }
}

inline void read_events(Capture& capture, const cy::diag::u8* payload, std::size_t bytes,
                        cy::diag::u32 thread) {
    std::size_t offset = 0;
    while (offset + cy::diag::format::kRecordFixedBytes <= bytes) {
        cy::diag::format::RecordHeader header{};
        cy::diag::format::RecordBody body{};
        std::memcpy(&header, payload + offset, sizeof(header));
        std::memcpy(&body, payload + offset + sizeof(header), sizeof(body));
        if (header.size == 0 || offset + header.size > bytes) {
            break;
        }
        ReadRecord record;
        record.kind = header.kind;
        record.channel = header.channel;
        record.name = header.name;
        record.category = body.category;
        record.timestamp = body.timestamp_ns;
        record.a = body.a;
        record.b = body.b;
        record.thread = thread;

        const cy::diag::u8* fields = payload + offset + cy::diag::format::kRecordFixedBytes;
        const cy::diag::u8* text =
            fields + (body.field_count * sizeof(cy::diag::format::FieldRecord));
        for (cy::diag::u16 index = 0; index < body.field_count; ++index) {
            cy::diag::format::FieldRecord entry{};
            std::memcpy(&entry, fields + (index * sizeof(entry)), sizeof(entry));
            ReadField value;
            value.field = entry.field;
            value.flags = entry.flags;
            value.bits = entry.bits;
            const auto meta = capture.fields.find(entry.field);
            const bool is_text = meta != capture.fields.end() && meta->second.type == 4;
            if (is_text && !value.redacted() && entry.text_offset + entry.bits <= body.text_bytes) {
                value.text.assign(reinterpret_cast<const char*>(text) + entry.text_offset,
                                  static_cast<std::size_t>(entry.bits));
            }
            record.fields.push_back(value);
        }
        capture.records.push_back(record);
        offset += header.size;
    }
}

inline void read_losses(Capture& capture, const cy::diag::u8* payload, std::size_t bytes) {
    Cursor cursor(payload, bytes);
    const auto count = cursor.read<cy::diag::u32>();
    for (cy::diag::u32 index = 0; index < count; ++index) {
        LossEntry entry;
        entry.thread = cursor.read<cy::diag::u32>();
        entry.channel = cursor.read<cy::diag::u8>();
        entry.reason = cursor.read<cy::diag::u8>();
        (void)cursor.read<cy::diag::u16>();
        entry.count = cursor.read<cy::diag::u64>();
        capture.losses.push_back(entry);
    }
}

/// Read a whole capture. Two passes, because the metadata chunk that classifies a field is written
/// at close as well as at open, and a record's text can only be read once its field is known.
inline Capture read_capture(const char* path) {
    Capture capture;
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return capture;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    capture.bytes.resize(static_cast<std::size_t>(size));
    const std::size_t read = std::fread(capture.bytes.data(), 1, capture.bytes.size(), file);
    std::fclose(file);
    if (read != capture.bytes.size() || read < sizeof(cy::diag::format::FileHeader)) {
        return capture;
    }
    std::memcpy(&capture.header, capture.bytes.data(), sizeof(capture.header));
    if (std::memcmp(capture.header.magic, cy::diag::format::kMagic, 8) != 0) {
        return capture;
    }

    for (int pass = 0; pass < 2; ++pass) {
        std::size_t offset = sizeof(cy::diag::format::FileHeader);
        while (offset + sizeof(cy::diag::format::ChunkHeader) + 8 <= capture.bytes.size()) {
            cy::diag::format::ChunkHeader chunk{};
            std::memcpy(&chunk, capture.bytes.data() + offset, sizeof(chunk));
            const cy::diag::u8* payload =
                capture.bytes.data() + offset + sizeof(cy::diag::format::ChunkHeader);
            const auto bytes = static_cast<std::size_t>(chunk.payload_bytes);
            if (offset + sizeof(chunk) + bytes > capture.bytes.size()) {
                break;
            }
            if (pass == 0 && chunk.tag == cy::diag::format::kChunkMeta) {
                read_metadata(capture, payload, bytes);
            } else if (pass == 1 && chunk.tag == cy::diag::format::kChunkEvents) {
                read_events(capture, payload, bytes, chunk.flags);
            } else if (pass == 1 && chunk.tag == cy::diag::format::kChunkLoss) {
                read_losses(capture, payload, bytes);
            }
            if (pass == 1) {
                ++capture.chunk_count;
            }
            offset += sizeof(chunk) + bytes;
        }
    }
    capture.valid = true;
    return capture;
}

}  // namespace cy_test
