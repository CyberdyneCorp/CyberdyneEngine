## ADDED Requirements

### Requirement: Deterministic lockstep combat
A lockstep session SHALL decide combat — targets, chases, melee blows, projectile flights, hits,
splash, armour, deaths and structures falling — in integers and fixed point only, so that two peers
executing one command log agree on every tick and two architectures agree on the digest. Damage SHALL
be the weapon's points less the target's armour, at least one. The combat state SHALL be part of
the state hash and the world hash whenever anything is armed or fortified, and a session in which
nothing is armed SHALL hash and digest exactly as it did before combat existed.

#### Scenario: The numbers are the specs'
- **WHEN** two armed units with stated health, armour, damage and cooldowns stand within reach
- **THEN** every blow SHALL land on the tick its cooldown gives and deal the points its spec and the target's armour give, and the unit whose health reaches zero SHALL die on that tick

#### Scenario: A follower armed differently
- **WHEN** a follower peer arms a unit with one point more damage than the issuer did
- **THEN** the peers' world hashes SHALL differ, and their state hashes from the first blow

#### Scenario: The battle across legs
- **WHEN** the sample's battle runs from its C++ twin on every CI leg
- **THEN** every leg SHALL end in the committed digest, and the Swift-driven battle SHALL end in the same one
