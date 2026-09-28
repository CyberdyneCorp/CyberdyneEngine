// Live scene VFX parameters through the generated ABI 1.3 table.

import CyberdyneABI
import CyberdyneCore

/// Read or change an exposed parameter on one playing scene effect entity.
public enum VfxEffects {
    public static func set(
        _ value: Value, on entity: Entity, parameter: String, emitter: String = ""
    ) throws {
        guard let engine = Runtime.engine else { throw CyberdyneError.invalidHandle }
        try emitter.withCString { owner in
            try parameter.withCString { name in
                try value.withCyVar { variable in
                    var variable = variable
                    try engine.vfxEffectParameterSet(
                        entity: entity.bits, emitter: owner, parameter: name, value: &variable)
                }
            }
        }
    }

    public static func get(
        from entity: Entity, parameter: String, emitter: String = ""
    ) throws -> Value {
        guard let engine = Runtime.engine else { throw CyberdyneError.invalidHandle }
        var result = CyVar()
        try emitter.withCString { owner in
            try parameter.withCString { name in
                try engine.vfxEffectParameterGet(
                    entity: entity.bits, emitter: owner, parameter: name, into: &result)
            }
        }
        guard let value = Value(reading: result) else {
            throw CyberdyneError.notRepresentable("VFX parameter type \(result.type)")
        }
        return value
    }
}
