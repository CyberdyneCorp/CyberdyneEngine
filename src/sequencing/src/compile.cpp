// The sequence compiler. Six passes, each its own function, in the order compile() calls them:
// flatten, validate, build segments, build the interval index, build the event index, derive the
// preload plan.

#include <cy/sequencing/compile.h>

#include <algorithm>
#include <cmath>

namespace cy::sequencing {
namespace {

[[nodiscard]] f32 quantise_component(f32 value, f32 step) noexcept {
    if (step <= 0.0F) {
        return value;
    }
    return std::round(value / step) * step;
}

/// The largest absolute component difference between two values of one type. The compression
/// report's unit: one number, and the same number for a scalar, a colour and a quaternion.
[[nodiscard]] f32 component_error(const ChannelValue& a, const ChannelValue& b) noexcept {
    f32 worst = 0.0F;
    for (u32 index = 0; index < 4; ++index) {
        worst = std::max(worst, std::fabs(a.components[index] - b.components[index]));
    }
    return worst;
}

[[nodiscard]] bool discrete_channel(ChannelType type) noexcept {
    return type == ChannelType::Boolean || type == ChannelType::Enumeration;
}

}  // namespace

const char* diagnostic_code_name(DiagnosticCode code) noexcept {
    switch (code) {
        case DiagnosticCode::None:
            return "None";
        case DiagnosticCode::DomainForbidsAuthority:
            return "DomainForbidsAuthority";
        case DiagnosticCode::AuthoritativeStateWrittenDirectly:
            return "AuthoritativeStateWrittenDirectly";
        case DiagnosticCode::CameraTransformWritten:
            return "CameraTransformWritten";
        case DiagnosticCode::SkippableWithoutOutcomes:
            return "SkippableWithoutOutcomes";
        case DiagnosticCode::RequiredOutcomeUnresolved:
            return "RequiredOutcomeUnresolved";
        case DiagnosticCode::NestedUnresolved:
            return "NestedUnresolved";
        case DiagnosticCode::NestedCycle:
            return "NestedCycle";
        case DiagnosticCode::NestedTooDeep:
            return "NestedTooDeep";
        case DiagnosticCode::NoAdapter:
            return "NoAdapter";
        case DiagnosticCode::UnknownProperty:
            return "UnknownProperty";
        case DiagnosticCode::PropertyTypeMismatch:
            return "PropertyTypeMismatch";
        case DiagnosticCode::AdapterNotDeterministic:
            return "AdapterNotDeterministic";
        case DiagnosticCode::AdapterNotNetworkSafe:
            return "AdapterNotNetworkSafe";
        case DiagnosticCode::AdapterEditorOnly:
            return "AdapterEditorOnly";
        case DiagnosticCode::AdapterCannotRestore:
            return "AdapterCannotRestore";
        case DiagnosticCode::TransformChannelUnsupported:
            return "TransformChannelUnsupported";
        case DiagnosticCode::DiscreteChannelInterpolated:
            return "DiscreteChannelInterpolated";
        case DiagnosticCode::SectionRangeInvalid:
            return "SectionRangeInvalid";
        case DiagnosticCode::SectionOutsideDuration:
            return "SectionOutsideDuration";
        case DiagnosticCode::BindingUnresolved:
            return "BindingUnresolved";
        case DiagnosticCode::DuplicateStableId:
            return "DuplicateStableId";
        case DiagnosticCode::CommandUndeclared:
            return "CommandUndeclared";
        case DiagnosticCode::Count:
            break;
    }
    return "Unknown";
}

bool CompileReport::has(DiagnosticCode code) const noexcept {
    return first(code) != nullptr;
}

const Diagnostic* CompileReport::first(DiagnosticCode code) const noexcept {
    for (const Diagnostic& diagnostic : diagnostics.span()) {
        if (diagnostic.code == code) {
            return &diagnostic;
        }
    }
    return nullptr;
}

SequenceCompiler::SequenceCompiler(Allocator& allocator, const AdapterRegistry& registry) noexcept
    : allocator_(&allocator),
      registry_(&registry),
      flat_(allocator),
      nesting_(allocator),
      pending_events_(allocator),
      pending_markers_(allocator) {}

void SequenceCompiler::diagnose(CompileReport& report, DiagnosticCode code, Severity severity,
                                const Track* track, const Section* section, const char* message,
                                Name detail) noexcept {
    Diagnostic diagnostic;
    diagnostic.code = code;
    diagnostic.severity = severity;
    diagnostic.message = message;
    diagnostic.detail = detail;
    if (track != nullptr) {
        diagnostic.track = track->name;
        diagnostic.track_stable_id = track->stable_id;
    }
    if (section != nullptr) {
        diagnostic.section_stable_id = section->stable_id;
    }
    if (severity == Severity::Error) {
        ++report.errors;
    } else {
        ++report.warnings;
    }
    // A diagnostic that could not be recorded is still counted: the compilation fails either way,
    // and losing the count as well as the message would turn an allocation failure into a success.
    (void)report.diagnostics.push_back(diagnostic);
}

// --- Pass 1: flatten
// ------------------------------------------------------------------------------

Status SequenceCompiler::flatten(const SequenceSource& source, const CompileOptions& options,
                                 i64 offset, u32 depth, CompileReport& report) noexcept {
    if (depth > options.max_nesting_depth) {
        diagnose(report, DiagnosticCode::NestedTooDeep, Severity::Error, nullptr, nullptr,
                 "nested sequences are deeper than the declared limit");
        return ok();
    }
    for (const u64 open : nesting_.span()) {
        if (open == source.stable_id) {
            diagnose(report, DiagnosticCode::NestedCycle, Severity::Error, nullptr, nullptr,
                     "a sequence nests itself", source.name);
            return ok();
        }
    }
    if (Status pushed = nesting_.push_back(source.stable_id); !pushed) {
        return pushed;
    }

    for (const Track& track : source.tracks.span()) {
        if (track.kind == TrackKind::NestedSequence) {
            const SequenceSource* child =
                (options.nested_resolver != nullptr)
                    ? options.nested_resolver(track.nested_sequence, options.nested_user)
                    : nullptr;
            if (child == nullptr) {
                diagnose(report, DiagnosticCode::NestedUnresolved, Severity::Error, &track, nullptr,
                         "a nested sequence could not be resolved");
                continue;
            }
            ++report.nested_flattened;
            if (Status nested = flatten(*child, options, offset + track.nested_offset.ticks(),
                                        depth + 1, report);
                !nested) {
                return nested;
            }
            continue;
        }

        if (Status validated = validate_track(source, track, options, report); !validated) {
            return validated;
        }

        for (const Section& section : track.sections.span()) {
            FlatSection flat;
            flat.section = &section;
            flat.track = &track;
            flat.owner = &source;
            flat.offset = offset;
            flat.nested_sequence = (depth == 0) ? 0 : source.stable_id;
            if (Status pushed = flat_.push_back(flat); !pushed) {
                return pushed;
            }
        }
        for (const EventDeclaration& event : track.events.span()) {
            CompiledEvent compiled;
            compiled.ticks = event.time.ticks() + offset;
            compiled.type = event.type;
            compiled.policy = event.policy;
            compiled.binding = event.binding != 0 ? event.binding : track.binding;
            compiled.track_stable_id = track.stable_id;
            compiled.stable_id = event.stable_id;
            compiled.payload_size = event.payload_size;
            for (u16 index = 0; index < event.payload_size && index < kMaxEventPayload; ++index) {
                compiled.payload[index] = event.payload[index];
            }
            if (Status pushed = pending_events_.push_back(compiled); !pushed) {
                return pushed;
            }
        }
    }

    for (const MarkerDeclaration& marker : source.markers.span()) {
        CompiledMarker compiled;
        compiled.ticks = marker.time.ticks() + offset;
        compiled.name = marker.name;
        if (Status pushed = pending_markers_.push_back(compiled); !pushed) {
            return pushed;
        }
    }

    nesting_.pop_back();
    return ok();
}

// --- Pass 2: validate one track
// -------------------------------------------------------------------
//
// Every refusal the specification names is here, and each one names the track it refuses. The
// function is a sequence of guard clauses rather than nested conditions for exactly that reason: a
// reader checking that a scenario is implemented should find one paragraph per scenario.

Status SequenceCompiler::validate_track(const SequenceSource& source, const Track& track,
                                        const CompileOptions& options,
                                        CompileReport& report) noexcept {
    const bool authoritative = track.authority == AuthorityClass::AuthoritativeGameplay ||
                               track.authority == AuthorityClass::DeterministicSimulation;

    // "An impossible combination is rejected": a presentation-domain sequence containing an
    // authoritative gameplay track SHALL fail naming the track.
    if (authoritative && !domain_permits_authoritative(source.domain)) {
        diagnose(report, DiagnosticCode::DomainForbidsAuthority, Severity::Error, &track, nullptr,
                 "only a simulation-domain sequence may carry an authoritative track",
                 Name::intern(clock_domain_name(source.domain)));
    }

    // "Gameplay goes through gameplay": authoritative change SHALL be expressed as commands and
    // events, "not as property tracks writing authoritative component data".
    if (authoritative && track.kind != TrackKind::GameplayCommand &&
        track.kind != TrackKind::GameplayEvent) {
        diagnose(report, DiagnosticCode::AuthoritativeStateWrittenDirectly, Severity::Error, &track,
                 nullptr, "authoritative change is a command or an event, never a property track",
                 Name::intern(track_kind_name(track.kind)));
    }

    if (track.kind == TrackKind::GameplayCommand && track.command_stable_id == 0) {
        diagnose(report, DiagnosticCode::CommandUndeclared, Severity::Error, &track, nullptr,
                 "a gameplay command track names no command type");
    }

    // The binding, and the rule this milestone's exit criterion is about.
    const BindingDeclaration* binding = nullptr;
    if (track.binding != 0) {
        for (const BindingDeclaration& declared : source.bindings.span()) {
            if (declared.stable_id == track.binding) {
                binding = &declared;
                break;
            }
        }
        if (binding == nullptr) {
            diagnose(report, DiagnosticCode::BindingUnresolved, Severity::Error, &track, nullptr,
                     "the track names a binding the sequence does not declare");
        }
    }

    // A CAMERA IS DRIVEN THROUGH THE CAMERA STACK. Rig, lens, blend and cut — nothing else — so a
    // camera binding accepts only the two camera track kinds. A transform track pointed at a camera
    // is the failure M8.c was told to prevent, and it fails here rather than working.
    if (binding != nullptr && binding->kind == BindingKind::Camera &&
        track.kind != TrackKind::Camera && track.kind != TrackKind::CameraCut) {
        diagnose(report, DiagnosticCode::CameraTransformWritten, Severity::Error, &track, nullptr,
                 "a sequence drives a camera through the camera stack — rig, lens, blend and cut — "
                 "and writes no camera transform",
                 Name::intern(track_kind_name(track.kind)));
    }

    // The adapter, and what its declaration permits.
    //
    // A MARKER AND A TIME SCALE DISPATCH TO NO SUBSYSTEM. A marker is an editorial reference and a
    // time scale is a request the host applies to its own clock, so neither resolves a property
    // against an adapter — and asking the adapter for one is not merely unnecessary, it is wrong:
    // `subsystem_for()` has to answer something for every kind, so a time-scale track would resolve
    // "scale" against whatever adapter happened to hold that subsystem and fail for a reason that
    // has nothing to do with the track.
    const bool dispatches = track.kind != TrackKind::Marker && track.kind != TrackKind::TimeScale;
    const SubsystemId subsystem = track_subsystem(track);
    const u32 adapter_id = dispatches ? registry_->adapter_for(subsystem) : 0;
    const AdapterProperties* adapter = registry_->properties(adapter_id);
    if (adapter == nullptr) {
        if (dispatches) {
            diagnose(report, DiagnosticCode::NoAdapter, Severity::Error, &track, nullptr,
                     "no adapter is registered for this track's subsystem",
                     Name::intern(subsystem_name(subsystem)));
        }
    } else {
        if (source.deterministic_profile && !adapter->deterministic) {
            diagnose(report, DiagnosticCode::AdapterNotDeterministic, Severity::Error, &track,
                     nullptr, "a deterministic sequence may not use a non-deterministic adapter",
                     adapter->name);
        }
        if (source.network != NetworkPolicy::LocalOnly && !adapter->network_safe) {
            diagnose(report, DiagnosticCode::AdapterNotNetworkSafe, Severity::Error, &track,
                     nullptr, "the adapter declares itself unsafe over the network", adapter->name);
        }
        if (options.cooking && adapter->editor_only) {
            diagnose(report, DiagnosticCode::AdapterEditorOnly, Severity::Error, &track, nullptr,
                     "an editor-only adapter cannot be cooked", adapter->name);
        }
    }

    for (const Section& section : track.sections.span()) {
        if (section.end <= section.start) {
            diagnose(report, DiagnosticCode::SectionRangeInvalid, Severity::Error, &track, &section,
                     "a section ends at or before it starts");
        }
        if (source.duration.ticks() > 0 && section.end > source.duration) {
            diagnose(report, DiagnosticCode::SectionOutsideDuration, Severity::Warning, &track,
                     &section, "a section extends beyond the sequence's declared duration");
        }
        if (section.completion == CompletionPolicy::Restore && dispatches &&
            (adapter == nullptr || !adapter->supports_capture_restore)) {
            diagnose(report, DiagnosticCode::AdapterCannotRestore, Severity::Error, &track,
                     &section, "the section declares restoration and its adapter cannot capture");
        }
        for (const Channel& channel : section.channels.span()) {
            if (channel.type == ChannelType::Transform) {
                diagnose(report, DiagnosticCode::TransformChannelUnsupported, Severity::Error,
                         &track, &section,
                         "author a transform as position, rotation and scale channels — see "
                         "add_transform_channels()",
                         channel.property);
                continue;
            }
            if (discrete_channel(channel.type)) {
                for (const Key& key : channel.keys.span()) {
                    if (key.interpolation != Interpolation::Constant) {
                        diagnose(report, DiagnosticCode::DiscreteChannelInterpolated,
                                 Severity::Error, &track, &section,
                                 "a boolean or enumeration channel holds; it does not interpolate",
                                 channel.property);
                        break;
                    }
                }
            }
            if (!dispatches || adapter == nullptr) {
                continue;  // dispatches to nothing, or the absent adapter is already diagnosed
            }
            const Expected<ResolvedProperty, Error> resolved =
                registry_->resolve(subsystem, channel.property, channel.type);
            if (!resolved) {
                const DiagnosticCode code = (resolved.error().code == ErrorCode::InvalidArgument)
                                                ? DiagnosticCode::PropertyTypeMismatch
                                                : DiagnosticCode::UnknownProperty;
                diagnose(report, code, Severity::Error, &track, &section, resolved.error().message,
                         channel.property);
            }
        }
    }
    return ok();
}

// --- Pass 3: segments and their channels
// ----------------------------------------------------------

Status SequenceCompiler::compact_channel(const Channel& channel, const CompressionOptions& options,
                                         Program& out, CompiledChannel& compiled,
                                         CompressionReport& report) noexcept {
    compiled.type = channel.type;
    compiled.key_begin = static_cast<u32>(out.keys_.size());
    compiled.key_count = 0;
    compiled.constant = false;
    report.keys_in += static_cast<u32>(channel.keys.size());

    // Stage one: copy, with the rotation-specific encoding applied on the way in. Normalising and
    // hemisphere-aligning BEFORE quantisation is what makes a quantised quaternion still take the
    // short arc; doing it afterwards would quantise a value that then gets negated.
    const usize first = out.keys_.size();
    for (const Key& key : channel.keys.span()) {
        CompiledKey compiled_key;
        compiled_key.ticks = key.time.ticks();
        compiled_key.integer = key.integer;
        compiled_key.interpolation = key.interpolation;
        for (u32 index = 0; index < 4; ++index) {
            compiled_key.value[index] = key.value[index];
        }
        if (options.rotation_encoding && channel.type == ChannelType::Rotation) {
            f32 length = 0.0F;
            for (const f32 component : compiled_key.value) {
                length += component * component;
            }
            length = std::sqrt(length);
            if (length > 1e-8F) {
                for (f32& component : compiled_key.value) {
                    component /= length;
                }
            }
            if (!out.keys_.empty() && out.keys_.size() > first) {
                const CompiledKey& previous = out.keys_[out.keys_.size() - 1];
                f32 dot = 0.0F;
                for (u32 index = 0; index < 4; ++index) {
                    dot += previous.value[index] * compiled_key.value[index];
                }
                if (dot < 0.0F) {
                    for (f32& component : compiled_key.value) {
                        component = -component;
                    }
                }
            }
            ++report.rotations_encoded;
        }
        if (options.quantise && !discrete_channel(channel.type)) {
            for (f32& component : compiled_key.value) {
                component = quantise_component(component, options.quantisation_step);
            }
        }
        if (Status pushed = out.keys_.push_back(compiled_key); !pushed) {
            return pushed;
        }
        ++compiled.key_count;
    }
    if (compiled.key_count == 0) {
        return ok();
    }

    // Stage two: redundant key removal. A key is redundant when removing it changes the curve by
    // less than the tolerance AT ITS OWN TIME, which is the only place removing it can change
    // anything for a piecewise interpolation.
    if (options.remove_redundant_keys && !discrete_channel(channel.type) &&
        compiled.key_count > 2) {
        u32 write = 1;
        for (u32 read = 1; read + 1 < compiled.key_count; ++read) {
            const CompiledKey& previous = out.keys_[compiled.key_begin + write - 1];
            const CompiledKey& candidate = out.keys_[compiled.key_begin + read];
            const CompiledKey& next = out.keys_[compiled.key_begin + read + 1];
            const i64 span = next.ticks - previous.ticks;
            bool redundant = span > 0 && candidate.interpolation == previous.interpolation;
            if (redundant) {
                const f32 t = static_cast<f32>(static_cast<f64>(candidate.ticks - previous.ticks) /
                                               static_cast<f64>(span));
                for (u32 index = 0; index < 4; ++index) {
                    const f32 interpolated =
                        previous.value[index] + ((next.value[index] - previous.value[index]) * t);
                    if (std::fabs(interpolated - candidate.value[index]) > options.tolerance) {
                        redundant = false;
                        break;
                    }
                }
            }
            if (!redundant) {
                out.keys_[compiled.key_begin + write] = candidate;
                ++write;
            } else {
                ++report.keys_removed;
            }
        }
        out.keys_[compiled.key_begin + write] =
            out.keys_[compiled.key_begin + compiled.key_count - 1];
        ++write;
        // Trim: the removed keys are at the end of the run, which is the end of the array because a
        // channel is compacted in one go.
        while (out.keys_.size() > compiled.key_begin + write) {
            out.keys_.pop_back();
        }
        compiled.key_count = write;
    }

    // Stage three: constant folding.
    if (options.fold_constants && compiled.key_count > 1) {
        bool identical = true;
        const CompiledKey& reference = out.keys_[compiled.key_begin];
        for (u32 index = 1; index < compiled.key_count && identical; ++index) {
            const CompiledKey& candidate = out.keys_[compiled.key_begin + index];
            identical = candidate.integer == reference.integer;
            for (u32 component = 0; component < 4 && identical; ++component) {
                identical = candidate.value[component] == reference.value[component];
            }
        }
        if (identical) {
            while (out.keys_.size() > compiled.key_begin + 1) {
                out.keys_.pop_back();
            }
            compiled.key_count = 1;
            compiled.constant = true;
            ++report.channels_folded;
        }
    }

    // The achieved error, MEASURED against the authored curve at every authored key time and
    // through the function evaluation will use. Not estimated, and not the tolerance that was asked
    // for — see compile.h.
    const Span<const CompiledKey> compacted(out.keys_.data() + compiled.key_begin,
                                            compiled.key_count);
    for (const Key& key : channel.keys.span()) {
        ChannelValue authored;
        authored.type = channel.type;
        authored.integer = key.integer;
        for (u32 index = 0; index < 4; ++index) {
            authored.components[index] = key.value[index];
        }
        ChannelValue produced;
        if (sample_keys(compacted, channel.type, compiled.constant, key.time, produced)) {
            report.max_error = std::max(report.max_error, component_error(authored, produced));
        }
    }
    report.keys_out += compiled.key_count;
    return ok();
}

Status SequenceCompiler::build_segments(const CompileOptions& options, Program& out,
                                        CompileReport& report) noexcept {
    // Authored order first, then time. `std::sort` rather than `std::stable_sort` — the comparator
    // is a total order over identities, so the result does not depend on the sort being stable and
    // `stable_sort` may allocate.
    std::sort(flat_.data(), flat_.data() + flat_.size(),
              [](const FlatSection& a, const FlatSection& b) noexcept {
                  const i64 a_start = a.section->start.ticks() + a.offset;
                  const i64 b_start = b.section->start.ticks() + b.offset;
                  if (a_start != b_start) {
                      return a_start < b_start;
                  }
                  if (a.track->stable_id != b.track->stable_id) {
                      return a.track->stable_id < b.track->stable_id;
                  }
                  return a.section->stable_id < b.section->stable_id;
              });

    for (const FlatSection& flat : flat_.span()) {
        Segment segment;
        segment.start = flat.section->start.ticks() + flat.offset;
        segment.end = flat.section->end.ticks() + flat.offset;
        segment.active_start = segment.start - flat.section->pre_roll.ticks();
        segment.active_end = segment.end + flat.section->post_roll.ticks();
        segment.binding = flat.track->binding;
        segment.kind = flat.track->kind;
        segment.authority = flat.track->authority;
        segment.subsystem = track_subsystem(*flat.track);
        segment.blend = flat.section->blend;
        segment.loop = flat.section->loop;
        segment.completion = flat.section->completion;
        segment.priority = flat.section->priority;
        segment.weight = flat.section->weight;
        segment.blend_group = flat.section->blend_group;
        segment.exclusive_group = flat.section->exclusive_group;
        segment.command_stable_id = flat.track->command_stable_id;
        segment.command_payload_size = flat.section->command_payload_size;
        for (u16 index = 0; index < flat.section->command_payload_size && index < kMaxEventPayload;
             ++index) {
            segment.command_payload[index] = flat.section->command_payload[index];
        }
        segment.blend_in_seconds = flat.section->blend_in_seconds;
        segment.blend_out_seconds = flat.section->blend_out_seconds;
        segment.blend_curve = flat.section->blend_curve;
        segment.blend_position = flat.section->blend_position;
        segment.blend_rotation = flat.section->blend_rotation;
        segment.blend_lens = flat.section->blend_lens;
        segment.framing_binding = flat.section->framing_binding;
        segment.spawn_template = flat.section->spawn_template;
        segment.spawn_lifetime = flat.section->spawn_lifetime;
        segment.time_scale_domain = flat.track->time_scale_domain;
        segment.asset = flat.section->asset;
        segment.channel_begin = static_cast<u32>(out.channels_.size());
        segment.channel_count = 0;
        segment.debug = static_cast<u32>(out.debug_.size());

        DebugEntry debug;
        debug.track_name = flat.track->name;
        debug.section_name = flat.section->name;
        debug.track_stable_id = flat.track->stable_id;
        debug.section_stable_id = flat.section->stable_id;
        debug.nested_sequence = flat.nested_sequence;
        if (Status pushed = out.debug_.push_back(debug); !pushed) {
            return pushed;
        }

        for (const Channel& channel : flat.section->channels.span()) {
            if (channel.type == ChannelType::Transform) {
                continue;  // already diagnosed in validate_track
            }
            CompiledChannel compiled;
            if (flat.track->kind != TrackKind::Marker && flat.track->kind != TrackKind::TimeScale) {
                const Expected<ResolvedProperty, Error> resolved =
                    registry_->resolve(segment.subsystem, channel.property, channel.type);
                if (resolved) {
                    compiled.target = resolved.value();
                }
            }
            if (Status compacted = compact_channel(channel, options.compression, out, compiled,
                                                   report.compression);
                !compacted) {
                return compacted;
            }
            if (Status pushed = out.channels_.push_back(compiled); !pushed) {
                return pushed;
            }
            ++segment.channel_count;
            ++report.channels;
        }

        if (flat.section->asset != 0) {
            PreloadEntry entry;
            entry.asset = flat.section->asset;
            entry.required_at = segment.active_start;
            entry.releasable_at = segment.active_end;
            entry.priority = flat.section->asset_priority;
            entry.segment = static_cast<u32>(out.segments_.size());
            if (Status pushed = out.preload_.push_back(entry); !pushed) {
                return pushed;
            }
        }

        if (Status pushed = out.segments_.push_back(segment); !pushed) {
            return pushed;
        }
        ++report.segments;
    }
    return ok();
}

// --- Pass 4: the interval index
// -------------------------------------------------------------------

Status SequenceCompiler::build_index(Program& out) noexcept {
    i64 begin = 0;
    i64 end = out.end_;
    bool first = true;
    for (const Segment& segment : out.segments_.span()) {
        begin = first ? segment.active_start : std::min(begin, segment.active_start);
        end = first ? segment.active_end : std::max(end, segment.active_end);
        first = false;
    }
    if (first) {
        out.begin_ = 0;
        out.end_ = std::max<i64>(out.end_, 0);
        out.bucket_count_ = 0;
        out.bucket_span_ = 1;
        return ok();
    }
    end = std::max(end, out.end_);
    out.begin_ = begin;
    out.end_ = end;

    const i64 range = std::max<i64>(end - begin, 1);
    // One bucket per frame, capped. A cap rather than a proportion: the index's cost is bounded by
    // the engine's constant and not by how long a cinematic is.
    auto buckets = static_cast<u32>(std::min<i64>((range / kTicksPerFrame) + 1, kMaxBuckets));
    buckets = std::max<u32>(buckets, 1);
    const i64 span = ((range + buckets) / buckets) + 1;

    out.bucket_count_ = buckets;
    out.bucket_span_ = span;
    if (Status resized = out.bucket_offsets_.resize(buckets + 1); !resized) {
        return resized;
    }
    for (u32 index = 0; index <= buckets; ++index) {
        out.bucket_offsets_[index] = 0;
    }
    // Count, prefix-sum, fill: the standard two passes, so the index is one allocation rather than
    // one array per bucket.
    for (const Segment& segment : out.segments_.span()) {
        const auto low = static_cast<u32>((segment.active_start - begin) / span);
        const auto high = static_cast<u32>((segment.active_end - begin) / span);
        for (u32 bucket = low; bucket <= high && bucket < buckets; ++bucket) {
            ++out.bucket_offsets_[bucket + 1];
        }
    }
    for (u32 index = 0; index < buckets; ++index) {
        out.bucket_offsets_[index + 1] += out.bucket_offsets_[index];
    }
    if (Status resized = out.bucket_items_.resize(out.bucket_offsets_[buckets]); !resized) {
        return resized;
    }
    Array<u32> cursor(*allocator_);
    if (Status resized = cursor.resize(buckets); !resized) {
        return resized;
    }
    for (u32 index = 0; index < buckets; ++index) {
        cursor[index] = out.bucket_offsets_[index];
    }
    for (usize index = 0; index < out.segments_.size(); ++index) {
        const Segment& segment = out.segments_[index];
        const auto low = static_cast<u32>((segment.active_start - begin) / span);
        const auto high = static_cast<u32>((segment.active_end - begin) / span);
        for (u32 bucket = low; bucket <= high && bucket < buckets; ++bucket) {
            out.bucket_items_[cursor[bucket]] = static_cast<u32>(index);
            ++cursor[bucket];
        }
    }
    return ok();
}

// --- Pass 5: the event index
// ----------------------------------------------------------------------

Status SequenceCompiler::build_events(const SequenceSource& source, Program& out,
                                      CompileReport& report) noexcept {
    std::sort(pending_events_.data(), pending_events_.data() + pending_events_.size(),
              [](const CompiledEvent& a, const CompiledEvent& b) noexcept {
                  if (a.ticks != b.ticks) {
                      return a.ticks < b.ticks;
                  }
                  if (a.track_stable_id != b.track_stable_id) {
                      return a.track_stable_id < b.track_stable_id;
                  }
                  return a.stable_id < b.stable_id;
              });
    for (const CompiledEvent& event : pending_events_.span()) {
        if (Status pushed = out.events_.push_back(event); !pushed) {
            return pushed;
        }
        ++report.events;
    }
    std::sort(pending_markers_.data(), pending_markers_.data() + pending_markers_.size(),
              [](const CompiledMarker& a, const CompiledMarker& b) noexcept {
                  return a.ticks < b.ticks;
              });
    for (const CompiledMarker& marker : pending_markers_.span()) {
        if (Status pushed = out.markers_.push_back(marker); !pushed) {
            return pushed;
        }
        ++report.markers;
    }

    // The required outcomes, checked against what was compiled. A skip that named an event nobody
    // authored would silently apply nothing, which is the defect "the door still opens" is about.
    for (const RequiredOutcome& outcome : source.required_outcomes.span()) {
        bool found = outcome.event == 0;
        if (!found) {
            for (const CompiledEvent& event : out.events_.span()) {
                if (event.stable_id == outcome.event) {
                    found = true;
                    break;
                }
            }
        }
        bool track_found = outcome.track == 0;
        if (!track_found) {
            for (const DebugEntry& entry : out.debug_.span()) {
                if (entry.track_stable_id == outcome.track) {
                    track_found = true;
                    break;
                }
            }
        }
        if (!found || !track_found) {
            diagnose(report, DiagnosticCode::RequiredOutcomeUnresolved, Severity::Error, nullptr,
                     nullptr, "a required outcome names a track or an event that was not compiled");
            continue;
        }
        if (Status pushed = out.outcomes_.push_back(outcome); !pushed) {
            return pushed;
        }
    }
    return ok();
}

// --- Pass 6: the preload plan
// ---------------------------------------------------------------------

Status SequenceCompiler::build_preload(Program& out, CompileReport& report) noexcept {
    // Merge by asset: one entry per asset, required at the earliest time any section needs it and
    // releasable at the latest time any section is done with it. Two shots using one asset must not
    // produce a release between them.
    for (usize index = 0; index < out.preload_.size();) {
        bool merged = false;
        for (usize other = 0; other < index; ++other) {
            if (out.preload_[other].asset != out.preload_[index].asset) {
                continue;
            }
            out.preload_[other].required_at =
                std::min(out.preload_[other].required_at, out.preload_[index].required_at);
            out.preload_[other].releasable_at =
                std::max(out.preload_[other].releasable_at, out.preload_[index].releasable_at);
            out.preload_[other].priority =
                std::max(out.preload_[other].priority, out.preload_[index].priority);
            out.preload_.remove_unordered(index);
            merged = true;
            break;
        }
        if (!merged) {
            ++index;
        }
    }
    std::sort(out.preload_.data(), out.preload_.data() + out.preload_.size(),
              [](const PreloadEntry& a, const PreloadEntry& b) noexcept {
                  if (a.required_at != b.required_at) {
                      return a.required_at < b.required_at;
                  }
                  return a.asset < b.asset;
              });
    report.preload_entries = static_cast<u32>(out.preload_.size());
    return ok();
}

// --- The driver
// -----------------------------------------------------------------------------------

Status SequenceCompiler::compile(const SequenceSource& source, const CompileOptions& options,
                                 Program& out, CompileReport& report) noexcept {
    flat_.clear();
    nesting_.clear();
    pending_events_.clear();
    pending_markers_.clear();

    out.name_ = source.name;
    out.stable_id_ = source.stable_id;
    out.rate_ = source.rate;
    out.domain_ = source.domain;
    out.skip_ = source.skip;
    out.network_ = source.network;
    out.persistence_ = source.persistence;
    out.deterministic_ = source.deterministic_profile;
    out.accessibility_ = source.accessibility;
    out.end_ = source.duration.ticks();

    if (!source.rate.valid()) {
        diagnose(report, DiagnosticCode::None, Severity::Error, nullptr, nullptr,
                 "the sequence declares a rate outside the representable range");
        return fail(ErrorCode::InvalidArgument, "invalid rate");
    }

    for (const BindingDeclaration& binding : source.bindings.span()) {
        CompiledBinding compiled;
        compiled.stable_id = binding.stable_id;
        compiled.name = binding.name;
        compiled.kind = binding.kind;
        compiled.requirement = binding.requirement;
        compiled.constraint = binding.constraint;
        compiled.fallback_target = binding.fallback_target;
        if (Status pushed = out.bindings_.push_back(compiled); !pushed) {
            return pushed;
        }
    }
    for (const ParameterDeclaration& parameter : source.parameters.span()) {
        CompiledParameter compiled;
        compiled.stable_id = parameter.stable_id;
        compiled.name = parameter.name;
        compiled.default_value = parameter.default_value;
        if (Status pushed = out.parameters_.push_back(compiled); !pushed) {
            return pushed;
        }
    }

    if (Status flattened = flatten(source, options, 0, 0, report); !flattened) {
        return flattened;
    }

    // "Undeclared consequence is caught": a sequence carrying authoritative tracks that does not
    // declare its required outcomes SHALL NOT be marked skippable, and this is where it is caught.
    bool authoritative = false;
    for (const Track& track : source.tracks.span()) {
        authoritative = authoritative || track.authority == AuthorityClass::AuthoritativeGameplay ||
                        track.authority == AuthorityClass::DeterministicSimulation;
    }
    if (authoritative && source.skip != SkipPolicy::NotSkippable &&
        source.required_outcomes.empty()) {
        diagnose(report, DiagnosticCode::SkippableWithoutOutcomes, Severity::Error, nullptr,
                 nullptr,
                 "a skippable sequence with authoritative tracks must declare its required "
                 "outcomes");
    }

    if (Status built = build_segments(options, out, report); !built) {
        return built;
    }
    if (Status built = build_index(out); !built) {
        return built;
    }
    if (Status built = build_events(source, out, report); !built) {
        return built;
    }
    if (Status built = build_preload(out, report); !built) {
        return built;
    }

    if (report.errors != 0) {
        return fail(ErrorCode::InvalidArgument, "the sequence did not compile");
    }
    return ok();
}

}  // namespace cy::sequencing
