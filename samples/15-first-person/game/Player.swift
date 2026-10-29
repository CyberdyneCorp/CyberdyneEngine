// SPDX-License-Identifier: MIT
// Player.swift — the first person: a character that walks, sprints, jumps and crouches, a camera
// that looks and kicks, and a weapon whose every shot is a physics raycast. All of it goes through
// the C ABI by way of CyberdyneKit.
//
// WHICH CALLBACK DOES WHAT, AND WHY. Each service says which phases may call it:
//
//   onCreate       (no phase)    the character controller on this node's entity, the first-person
//                                camera and its weapon view, the effects, the mark and the cues
//   onReady        (the pump)    the targets and the props by `@Node`; a `Health` on each target;
//                                where the host offers an interface, the HUD and the pause menu
//   onFixedUpdate  (fixed step)  the move — walk, sprint, jump, and the crouch, which is the
//                                CHARACTER CONTROLLER rebuilt shorter, and taller again only when a
//                                shape test finds the headroom — the magazine and its reload, and
//                                the trigger: a raycast along the aim the last frame drew, an
//                                impulse on a prop, damage on a target; and a fallen target given
//                                its health back once it has lain long enough. Simulation, so it
//                                replays.
//   onUpdate       (frame)       the mouse's counts and the pad's stick into the camera, the
//                                recoil's kick, and what a shot leaves behind: the muzzle flash (a
//                                light, ABI 1.11), the muzzle burst and the sparks (effects, ABI
//                                1.15), the scorch (a decal, ABI 1.15) and the sounds — where the
//                                head is, and the HUD.
//
// PLAYED (samples/play): the host pauses the game — `Time.now.isPaused` — and the menu shows; the
// look the player set there is applied where the look is, once, in the first-person camera's
// sensitivity; the head is drawn at the frame's own time between two ticks, carried forward by the
// character's velocity over `Time.now.interpolation` of a step. A scripted run has no interface
// and frames that fall on their ticks, so none of it changes what it does.
//
// THE AIM CROSSES FROM THE FRAME TO THE STEP. The camera is presentation: a fixed step may not ask
// it where the weapon points. So `onUpdate` RECORDS the eye the last frame was drawn from and the
// weapon's aim, recoil included, and the next `onFixedUpdate` shoots along them — the shot goes
// where the player saw the sights. Likewise a shot's flash, burst, sparks, mark and sounds are
// presentation: the step QUEUES them and the next frame plays them.

import CyberdyneKit

@Behaviour(name: "Player", schema: 1)
final class Player: Behaviour {

    // --- Tunables ------------------------------------------------------------------------------

    /// Where the feet start (host/arena.h's `kPlayerStart`).
    @Export var start: Vec3 = Vec3(x: 5, y: 0, z: 10)
    /// Metres a second: walking, sprinting, crouched; and the jump's take-off speed.
    @Export(range: 0...10) var walkSpeed: Float = 4
    @Export(range: 0...15) var sprintSpeed: Float = 7
    @Export(range: 0...5) var crouchSpeed: Float = 2
    @Export(range: 0...10) var jumpSpeed: Float = 5
    /// The capsule standing and crouched, and how far the eyes are below its top.
    @Export(range: 1...2.5) var standHeight: Float = 1.8
    @Export(range: 0.6...1.5) var crouchHeight: Float = 1.1
    @Export(range: 0...0.5) var radius: Float = 0.3
    @Export(range: 0...0.5) var eyeBelowTop: Float = 0.15
    /// Fixed ticks between shots, what a shot does to a target and a prop, and its reach.
    @Export(range: 1...60) var fireInterval: Float = 6
    @Export(range: 0...100) var damage: Float = 40
    @Export(range: 0...400) var impulse: Float = 70
    @Export(range: 1...500) var range: Float = 120
    /// The kick a shot gives the recoil spring, radians a second up, and to the side.
    @Export(range: 0...10) var kickPitch: Float = 3.2
    @Export(range: 0...5) var kickYaw: Float = 0.6
    /// Rounds a magazine holds, and seconds a reload takes.
    @Export(range: 1...100) var magazine: Float = 30
    @Export(range: 0...5) var reloadSeconds: Float = 1.4
    /// Seconds a fallen target lies before the game gives it its health back.
    @Export(range: 1...120) var respawnSeconds: Float = 10
    /// Radians a second the pad's look stick turns at full deflection.
    @Export(range: 0...10) var stickLookRate: Float = 3.2

    @Node("../Targets/T0") var target0: Entity?
    @Node("../Targets/T1") var target1: Entity?
    @Node("../Targets/T2") var target2: Entity?
    @Node("../Targets/T3") var target3: Entity?
    @Node("../Props/Crate0") var prop0: Entity?
    @Node("../Props/Crate1") var prop1: Entity?
    @Node("../Props/Crate2") var prop2: Entity?
    @Node("../Props/Barrel") var prop3: Entity?

    // --- State ---------------------------------------------------------------------------------

    private var eye: FirstPersonCamera?
    private var camera: Camera?
    private var muzzleEffect = Effect(handle: 0)
    private var sparkEffect = Effect(handle: 0)
    private var scorch = DecalMaterial(handle: 0)
    private var targets: [Entity] = []
    private var props: [Entity] = []
    private var crouched = false
    private var height: Float = 1.8
    /// The eye's height above the feet, eased towards the stance's.
    private var eyeHeight: Float = 1.65
    /// Which way the player faces on the ground, from the look the last frame drew.
    private var heading = Vec3(x: 0, y: 0, z: -1)
    private var cooldown: Float = 0
    /// The aim the last frame drew: where the eye was and where the weapon pointed.
    private var aim: (origin: Vec3, direction: Vec3)?
    /// What the fixed steps since the last frame fired, for the frame to show and sound.
    private var pending: [Shot] = []
    private var shotIndex: Int = 0
    private var probed = false
    /// The magazine: rounds left, and the reload's seconds left (zero when not reloading).
    private var rounds: Float = 30
    private var reloadLeft: Float = 0
    /// Seconds each fallen target has lain.
    private var lying: [Float] = []
    /// The camera's settings as made, and the player's on top of them.
    private var lookSettings = FirstPersonSettings()
    private var menu: PauseMenu?
    private var hud: FirstPersonHud?
    private var tally = PlayerReport()
    private var report = ComponentType(id: 0)
    private var health = ComponentType(id: 0)
    private let fields = ReportFields()
    private let healthFields = HealthFields()

    // --- Lifecycle -----------------------------------------------------------------------------

    override func onCreate() throws {
        guard let world else { throw CyberdyneError.invalidHandle }
        report = try Components.register(PlayerReport.self, in: world)
        health = try Components.register(Health.self, in: world)
        if !world.has(report, on: entity) {
            try world.add(report, to: entity)
        }
        height = standHeight
        eyeHeight = standHeight - eyeBelowTop
        rounds = magazine
        tally.ammo = rounds
        try makeCharacter(centre: start + Vec3(x: 0, y: standHeight * 0.5 + 0.02, z: 0))
        // THE CAMERA the host made active gets first-person behaviours and a weapon view: the arms
        // and the weapon are on `weaponLayers`, drawn with their own lens and never into a wall.
        if let active = try Camera.active() {
            camera = active
            let behaviours = try FirstPersonCamera(camera: active, settings: lookSettings)
            try behaviours.setWeaponView(WeaponView(layers: weaponLayers))
            eye = behaviours
        }
        muzzleEffect = try Effect.named(Content.muzzle)
        sparkEffect = try Effect.named(Content.sparks)
        scorch = try DecalMaterial.named(Content.scorch)
    }

    override func onReady() throws {
        guard let world else { return }
        targets = [target0, target1, target2, target3].compactMap { $0 }
        props = [prop0, prop1, prop2, prop3].compactMap { $0 }
        for target in targets where !world.has(health, on: target) {
            try world.add(Health(), as: health, to: target)
        }
        lying = Array(repeating: 0, count: targets.count)
        // PLAYED ONLY: a host with no interface — the scripted session — refuses both, and the
        // game plays on without them.
        hud = try? FirstPersonHud(owner: entity)
        menu = try? PauseMenu(for: self, title: "Paused") { [unowned self] settings in
            self.adjustLook(settings)
        }
        try publish()
    }

    override func onFixedUpdate(_ delta: Double) throws {
        probeRefusals()
        try move()
        try reload(Float(delta))
        try fire()
        try respawn(Float(delta))
        try publish()
    }

    override func onUpdate(_ delta: Double) throws {
        let now = try Time.now
        try look(Float(delta), now: now)
        try present()
        try showHud(Float(delta), paused: now.isPaused)
        try publish()
    }

    // --- The move ------------------------------------------------------------------------------

    private func makeCharacter(centre: Vec3) throws {
        try CharacterController.create(
            on: entity,
            CharacterController.Description(
                radius: radius, height: height, layer: Layers.player, mask: Layers.solid,
                start: Pose(position: centre)))
    }

    /// Crouching is the character rebuilt shorter, its feet where they were; standing is it
    /// rebuilt taller, and only when a capsule of the standing size fits where it would stand.
    private func stance(crouch: Bool, state: CharacterController.State) throws {
        let feet = state.position - Vec3(x: 0, y: height * 0.5, z: 0)
        if crouch && !crouched {
            try CharacterController(entity).destroy()
            height = crouchHeight
            crouched = true
            try makeCharacter(centre: feet + Vec3(x: 0, y: height * 0.5, z: 0))
        } else if !crouch && crouched {
            // A little above the floor and a little thinner than the capsule, so the floor the
            // feet stand on and the wall the shoulder brushes are not headroom refused.
            let standing = feet + Vec3(x: 0, y: standHeight * 0.5 + 0.05, z: 0)
            let shape = Physics.Shape.capsule(
                radius: radius - 0.03, halfHeight: standHeight * 0.5 - radius)
            let blocking = try Physics.overlap(
                shape, at: Pose(position: standing), filter: Layers.shot.ignoring(entity))
            guard blocking.isEmpty else {
                tally.standRefused += 1
                return
            }
            try CharacterController(entity).destroy()
            height = standHeight
            crouched = false
            try makeCharacter(centre: standing)
        }
    }

    private func move() throws {
        let character = CharacterController(entity)
        let before = try character.state
        try stance(crouch: try Input.action(Content.crouch).pressed, state: before)
        let axes = try Input.action(Content.move).axis2
        let sprinting = try Input.action(Content.sprint).pressed && axes.y > 0 && !crouched
        let speed = crouched ? crouchSpeed : (sprinting ? sprintSpeed : walkSpeed)
        // The move is in the look's frame: forward along the heading, right across it.
        let forward = heading
        let right = Vec3(x: -heading.z, y: 0, z: heading.x)
        var wish = right * axes.x + forward * axes.y
        if wish.length > 1 { wish = wish.normalised }
        let jumping = try Input.action(Content.jump).justPressed && before.isGrounded && !crouched
        try character.move(velocity: wish * speed, jump: jumping ? jumpSpeed : nil)
        if jumping { tally.jumps += 1 }
        let after = try character.state
        let pace = after.velocity.planarLength
        if after.isGrounded && wish.length > 0.5 {
            if crouched {
                tally.crouchSpeed = max(tally.crouchSpeed, pace)
            } else if sprinting {
                tally.sprintSpeed = max(tally.sprintSpeed, pace)
            } else {
                tally.walkSpeed = max(tally.walkSpeed, pace)
            }
        }
        if !after.isGrounded { tally.airborne += 1 }
        tally.peakHeight = max(tally.peakHeight, after.position.y - height * 0.5)
        if crouched {
            tally.crouched += 1
            // The beam's footprint (host/arena.h): under it a standing character does not fit.
            let p = after.position
            if p.x > 3.25 && p.x < 6.75 && p.z > 3.65 && p.z < 4.35 { tally.underBeam += 1 }
        }
    }

    // --- The trigger ---------------------------------------------------------------------------

    private func fire() throws {
        cooldown = max(0, cooldown - 1)
        guard try Input.action(Content.fire).justPressed, cooldown == 0, let aim else { return }
        // AN EMPTY MAGAZINE CLICKS and starts the reload; a reload under way refuses the trigger.
        guard reloadLeft == 0 else { return }
        guard rounds >= 1 else {
            tally.dryFires += 1
            reloadLeft = reloadSeconds
            return
        }
        rounds -= 1
        tally.ammo = rounds
        cooldown = fireInterval
        let ray = Ray(origin: aim.origin, direction: aim.direction, maxDistance: range)
        let hit = try Physics.raycast(ray, filter: Layers.shot.ignoring(entity))
        var shot = Shot(origin: aim.origin, direction: aim.direction, hit: hit, kind: .miss)
        if let hit {
            shot.kind = try land(hit, along: aim.direction)
        } else {
            tally.misses += 1
        }
        tally.shots += 1
        record(shot)
        pending.append(shot)
    }

    /// The reload: asked for, or started by an empty magazine; the magazine full once its seconds
    /// have run in fixed steps.
    private func reload(_ step: Float) throws {
        let asked = try Input.action(Content.reload).justPressed
        if reloadLeft == 0 && rounds < magazine && asked {
            reloadLeft = reloadSeconds
        }
        guard reloadLeft > 0 else { return }
        reloadLeft = max(0, reloadLeft - step)
        if reloadLeft == 0 {
            rounds = magazine
            tally.ammo = rounds
            tally.reloads += 1
        }
    }

    /// A target that has lain `respawnSeconds` gets its health back — the game's decision; the
    /// engine's AI stands it up when it reads the health (host/targets.h).
    private func respawn(_ step: Float) throws {
        guard let world else { return }
        for (index, target) in targets.enumerated() where index < lying.count {
            let points = try world.float(target, health, field: healthFields.points)
            guard points <= 0 else {
                lying[index] = 0
                continue
            }
            lying[index] += step
            if lying[index] >= respawnSeconds {
                lying[index] = 0
                try world.setFloat(Health().points, target, health, field: healthFields.points)
                tally.respawns += 1
            }
        }
    }

    /// What a hit does: damage on a target, an impulse at the point on a prop.
    private func land(_ hit: Physics.Hit, along direction: Vec3) throws -> Shot.Kind {
        guard let world else { return .world }
        if targets.contains(hit.entity) {
            tally.targetHits += 1
            let target = hit.entity
            let points = try world.float(target, health, field: healthFields.points)
            if points > 0 {
                try world.setFloat(points - damage, target, health, field: healthFields.points)
                if points - damage <= 0 { tally.kills += 1 }
            }
            let hits = try world.float(target, health, field: healthFields.hits)
            try world.setFloat(hits + 1, target, health, field: healthFields.hits)
            try world.setVec3(aim?.origin ?? Vec3(), target, health, field: healthFields.source)
            return points > 0 && points - damage <= 0 ? .kill : .target
        }
        if props.contains(hit.entity) {
            tally.propHits += 1
            try RigidBody(hit.entity).applyImpulse(direction * impulse, at: hit.point)
            tally.impulses += 1
            return .prop
        }
        tally.worldHits += 1
        return .world
    }

    private func record(_ shot: Shot) {
        tally.lastOrigin = shot.origin
        tally.lastDirection = shot.direction
        tally.lastHit = shot.hit?.entity ?? .null
        tally.lastPoint = shot.hit?.point ?? Vec3()
        tally.lastNormal = shot.hit?.normal ?? Vec3()
        tally.lastDistance = shot.hit?.distance ?? -1
        tally.lastKind = Float(shot.kind.code)
    }

    /// A fixed step may not show or sound a shot: both are presentation, which a resimulated tick
    /// would repeat. Asked once, refused once each — the number the report says.
    private func probeRefusals() {
        guard !probed else { return }
        probed = true
        do { try muzzleEffect.play(at: start) } catch { tally.refused += 1 }
        do {
            try Decal.spawn(.mark(scorch, at: start, normal: Vec3(x: 0, y: 1, z: 0), radius: 0.1))
        } catch { tally.refused += 1 }
    }

    // --- The frame -----------------------------------------------------------------------------

    private func look(_ seconds: Float, now: GameTime) throws {
        guard let eye, let camera else { return }
        let pointer = try Input.pointer()
        let aiming = try Input.action(Content.aim).pressed
        let state = try CharacterController(entity).state
        // The mouse's counts: +x turns right, and the window's +y is down, so pushing the mouse
        // away looks up. Paused, the cursor is the menu's and turns nothing.
        var counts = now.isPaused ? Vec2() : Vec2(x: pointer.delta.x, y: -pointer.delta.y)
        // THE PAD'S STICK, a rate: radians a second at full deflection, over the frame — turned
        // into the counts the camera's one sensitivity turns back into radians.
        let stick = now.isPaused ? Vec2() : try Input.action(Content.look).axis2
        if stick.x != 0 || stick.y != 0 {
            let scale = stickLookRate * seconds / max(lookSettings.lookSensitivity, 1e-6)
            counts = Vec2(x: counts.x + stick.x * scale, y: counts.y + stick.y * scale)
        }
        try eye.feed(
            look: counts, speed: state.isGrounded ? state.velocity.planarLength : 0,
            aiming: aiming, airborne: !state.isGrounded)
        if aiming { tally.aimed += 1 }
        // THE AIM THE LAST FRAME DREW, for the next fixed step's shot: the eye the host placed and
        // where the weapon pointed, recoil and all.
        let drawn = try camera.view()
        let first = try eye.state()
        let flat = Vec3(x: first.viewAim.x, y: 0, z: first.viewAim.z)
        if flat.length > 1e-3 { heading = flat.normalised }
        aim = (drawn.pose.position, first.weaponAim.normalised)
        // THE HEAD: the top of the capsule less the eyes' depth, eased so a crouch lowers it rather
        // than cutting it.
        let target = height - eyeBelowTop
        eyeHeight += (target - eyeHeight) * 0.25
        var feet = state.position - Vec3(x: 0, y: height * 0.5, z: 0)
        // AT THE FRAME'S OWN TIME: a frame between two ticks carries the head forward by the
        // character's velocity over that much of a step. A frame on its tick moves it by nothing.
        let ahead = Float(now.interpolation * now.fixedDelta)
        if ahead > 0 && !now.isPaused {
            feet = feet + state.velocity * ahead
        }
        try camera.overridePose(Pose(position: feet + Vec3(x: 0, y: eyeHeight, z: 0)))
    }

    /// The menu's look: the camera's own sensitivity scaled — the one place it is applied — and
    /// its Y inverted or not.
    private func adjustLook(_ settings: PlaySettings) {
        let base = FirstPersonSettings()
        lookSettings.lookSensitivity = base.lookSensitivity * settings.lookSensitivity
        lookSettings.invertY = settings.invertY
        try? eye?.configure(lookSettings)
    }

    /// The HUD and the menu, where the host offers an interface.
    private func showHud(_ seconds: Float, paused: Bool) throws {
        try menu?.update(paused: paused)
        try hud?.update(
            FirstPersonHud.Reading(
                ammo: rounds, magazine: magazine, reloading: reloadLeft > 0,
                targetHits: tally.targetHits, kills: tally.kills),
            seconds: seconds, paused: paused)
    }

    /// Everything the fixed steps since the last frame fired, shown and sounded.
    private func present() throws {
        guard !pending.isEmpty, let eye, let camera else { return }
        let drawn = try camera.view()
        let state = try eye.state()
        let weapon = state.weapon
        // THE BARREL'S TIP, where the frame DRAWS it. The weapon is drawn through its own, narrower
        // lens, so the tip's place in the eye's space is pulled out across the view by the ratio of
        // the two lenses' tangents: the point the world's lens puts where the weapon's lens put
        // the tip. A flash at the tip's true place would hang in the air beside the barrel.
        let tip = weapon.position + weapon.rotation.rotate(Vec3(x: 0, y: 0.012, z: -0.56))
        let spread = tangent(state.fieldOfView * 0.5) / tangent(state.weaponFieldOfView * 0.5)
        let seen = Vec3(x: tip.x * spread, y: tip.y * spread, z: tip.z)
        let muzzle = drawn.pose.position + drawn.pose.rotation.rotate(seen)
        for shot in pending {
            try kick(eye)
            try Light.spawn(
                .point(
                    at: muzzle, colour: Vec3(x: 1, y: 0.72, z: 0.38), candela: 14000, range: 7,
                    lifetime: 0.06, fade: .quadratic))
            tally.flashes += 1
            let d = shot.direction
            try muzzleEffect.play(at: muzzle, parameters: [d.x, d.y, d.z, 0])
            tally.bursts += 1
            try Audio.play(Content.shot, at: muzzle)
            tally.sounds += 1
            if let hit = shot.hit {
                try mark(hit, kind: shot.kind)
            }
            if shot.kind == .target || shot.kind == .kill {
                try hud?.hit(kill: shot.kind == .kill)
            }
        }
        pending.removeAll()
    }

    private func kick(_ eye: FirstPersonCamera) throws {
        // A climbing kick, alternately to each side, so a burst walks up and wanders.
        shotIndex += 1
        let side: Float = shotIndex % 2 == 0 ? 1 : -1
        try eye.kick(pitch: kickPitch, yaw: kickYaw * side)
        tally.kicks += 1
    }

    /// What a hit leaves: sparks off the surface — bouncing on the floor, whose height in the
    /// effect's own space is a parameter — a sound, and on the arena itself a scorch mark.
    private func mark(_ hit: Physics.Hit, kind: Shot.Kind) throws {
        let n = hit.normal
        let floor = -hit.point.y
        try sparkEffect.play(at: hit.point, parameters: [n.x, n.y, n.z, 0, floor, 0, 0, 0])
        tally.sparks += 1
        try Audio.play(Content.impact, at: hit.point)
        tally.sounds += 1
        switch kind {
        case .world:
            if try Decal.spawn(.mark(scorch, at: hit.point, normal: n, radius: 0.14)) != nil {
                tally.marks += 1
            }
        case .target, .kill:
            try Audio.play(Content.hit, at: hit.point)
            tally.sounds += 1
            if kind == .kill {
                try Audio.play(Content.down, at: hit.point)
                tally.sounds += 1
            }
        case .prop, .miss:
            break
        }
    }

    private func publish() throws {
        guard let world else { return }
        try fields.write(tally, to: entity, report, in: world)
    }
}

/// One shot, from the step that fired it to the frame that shows it.
private struct Shot {
    enum Kind {
        case miss, world, prop, target, kill

        /// `PlayerReport.lastKind`'s spelling: a kill is a target hit.
        var code: Int {
            switch self {
            case .miss: return 0
            case .world: return 1
            case .prop: return 2
            case .target, .kill: return 3
            }
        }
    }
    var origin: Vec3
    var direction: Vec3
    var hit: Physics.Hit?
    var kind: Kind
}

/// `Health`'s field indices, resolved by name once.
private struct HealthFields {
    let points = Health.field("points")
    let hits = Health.field("hits")
    let source = Health.field("source")
}

/// `PlayerReport`'s fields, by name, and the one write of all of them.
private struct ReportFields {
    private let floats: [(UInt32, KeyPath<PlayerReport, Float>)] = [
        ("walkSpeed", \.walkSpeed), ("sprintSpeed", \.sprintSpeed),
        ("crouchSpeed", \.crouchSpeed), ("jumps", \.jumps), ("airborne", \.airborne),
        ("peakHeight", \.peakHeight), ("crouched", \.crouched),
        ("standRefused", \.standRefused), ("underBeam", \.underBeam), ("shots", \.shots),
        ("targetHits", \.targetHits), ("propHits", \.propHits), ("worldHits", \.worldHits),
        ("misses", \.misses), ("kills", \.kills), ("impulses", \.impulses), ("kicks", \.kicks),
        ("aimed", \.aimed), ("flashes", \.flashes), ("bursts", \.bursts), ("sparks", \.sparks),
        ("marks", \.marks), ("sounds", \.sounds), ("refused", \.refused),
        ("ammo", \.ammo), ("reloads", \.reloads), ("dryFires", \.dryFires),
        ("respawns", \.respawns),
        ("lastDistance", \.lastDistance), ("lastKind", \.lastKind),
    ].map { (PlayerReport.field($0.0), $0.1) }
    private let vectors: [(UInt32, KeyPath<PlayerReport, Vec3>)] = [
        ("lastOrigin", \.lastOrigin), ("lastDirection", \.lastDirection),
        ("lastPoint", \.lastPoint), ("lastNormal", \.lastNormal),
    ].map { (PlayerReport.field($0.0), $0.1) }
    private let lastHit = PlayerReport.field("lastHit")

    func write(
        _ tally: PlayerReport, to entity: Entity, _ report: ComponentType, in world: World
    )
        throws
    {
        for (field, path) in floats {
            try world.setFloat(tally[keyPath: path], entity, report, field: field)
        }
        for (field, path) in vectors {
            try world.setVec3(tally[keyPath: path], entity, report, field: field)
        }
        try world.setValue(.entity(tally.lastHit), entity, report, field: lastHit)
    }
}
