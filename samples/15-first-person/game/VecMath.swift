// SPDX-License-Identifier: MIT
// VecMath.swift — the little vector arithmetic the game needs. CyberdyneKit's values are plain
// storage; a module that does arithmetic on them brings its own.

import CyberdyneKit

extension Vec3 {
    static func + (a: Vec3, b: Vec3) -> Vec3 { Vec3(x: a.x + b.x, y: a.y + b.y, z: a.z + b.z) }
    static func - (a: Vec3, b: Vec3) -> Vec3 { Vec3(x: a.x - b.x, y: a.y - b.y, z: a.z - b.z) }
    static func * (a: Vec3, s: Float) -> Vec3 { Vec3(x: a.x * s, y: a.y * s, z: a.z * s) }
    var length: Float { (x * x + y * y + z * z).squareRoot() }
    var planarLength: Float { (x * x + z * z).squareRoot() }
    var normalised: Vec3 {
        let l = length
        return l > 1e-6 ? self * (1 / l) : Vec3(x: 0, y: 0, z: -1)
    }
}

extension Quat {
    /// `v` turned by this rotation: v + 2w(u × v) + 2u × (u × v).
    func rotate(_ v: Vec3) -> Vec3 {
        let ux = x
        let uy = y
        let uz = z
        let cx = uy * v.z - uz * v.y
        let cy = uz * v.x - ux * v.z
        let cz = ux * v.y - uy * v.x
        let dx = uy * cz - uz * cy
        let dy = uz * cx - ux * cz
        let dz = ux * cy - uy * cx
        return Vec3(
            x: v.x + 2 * (w * cx + dx), y: v.y + 2 * (w * cy + dy), z: v.z + 2 * (w * cz + dz))
    }
}

/// tan(x) for a half field of view, |x| under 0.8 radians: sine and cosine as their series to the
/// ninth and eighth order, under a millionth of error there. The game needs one tangent a shot and
/// no math library for it.
func tangent(_ x: Float) -> Float {
    let x2 = x * x
    let sine = x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72))))
    let cosine = 1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56)))
    return sine / cosine
}
