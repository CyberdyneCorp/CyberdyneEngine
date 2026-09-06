//! The engine's own words, and the familiar ones kept as search aliases.
//!
//! `editor-visual-language`: "Interface text SHALL use **the engine's own vocabulary**, taken from
//! the specifications, and SHALL NOT adopt another engine's product terms. Where a competing
//! engine's term is widely understood, the engine's term SHALL be used in the interface and the
//! familiar term MAY be offered as a **search alias**, so a user who types the term they know still
//! finds the feature."
//!
//! Two functions, and they are the two halves of that sentence. [`check_label`] refuses borrowed
//! vocabulary in interface text; [`engine_term_for`] answers the palette when somebody types
//! "blueprint". One table serves both, so a term can never be forbidden in labels and unreachable in
//! search — which is the failure mode that makes people stop using the engine's word.
//!
//! --- WHAT IS DELIBERATELY NOT FLAGGED --------------------------------------------------------------
//!
//! The bare word **"level"**. It is in the specification's "Not" column and it is not in the check,
//! because "level of detail", "log level", "zoom level" and "level set" are all correct engine
//! vocabulary and a check that flagged them would be turned off within a week. What is flagged is
//! the phrase that can only mean the borrowed noun: "persistent level", "open level", "sub level",
//! "level editor". A check nobody trusts is worse than no check, and this is the shape of decision
//! that decides which of the two it is.
//!
//! The word **"actor"** *is* flagged, and it collides with this workspace's own
//! `cy_editor_core::Actor` — which means *who made a change*, a person or an agent, and not a thing
//! in a scene. That collision is worth the flag rather than an exemption: interface text about
//! provenance says "author", and `cy-editor-interface` labels it that way for exactly this reason.

use cy_editor_core::problem::{Problem, Result};

/// One row of the vocabulary table: the engine's word, and the words it replaces.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Term {
    /// The engine's own word, as interface text should say it.
    pub engine: &'static str,
    /// The borrowed words this replaces, lower-case. Matched whole, never as a substring.
    pub borrowed: &'static [&'static str],
    /// Why the engine's word is the one to use, for the message a check produces.
    pub reason: &'static str,
}

/// The vocabulary, from `editor-visual-language`'s table.
pub const TERMS: [Term; 6] = [
    Term {
        engine: "node, entity",
        borrowed: &[
            "actor",
            "actors",
            "gameobject",
            "gameobjects",
            "game object",
        ],
        reason: "the engine's scene is nodes and entities, and `ecs-core` names them that way",
    },
    Term {
        engine: "mesh, mesh instance",
        borrowed: &["static mesh actor", "static mesh component"],
        reason: "a mesh is a resource and a mesh instance is what places it",
    },
    Term {
        engine: "graph, script graph",
        borrowed: &[
            "blueprint",
            "blueprints",
            "blueprint class",
            "blueprint log",
        ],
        reason: "`visual-scripting` calls it a graph, and so does every panel that shows one",
    },
    Term {
        engine: "content browser",
        borrowed: &["content drawer"],
        reason: "the browser is a panel, not a drawer that slides",
    },
    Term {
        engine: "prefab",
        borrowed: &["prefab variant asset", "blueprint asset"],
        reason: "`serialization-and-prefabs` calls it a prefab",
    },
    Term {
        engine: "world, scene, cell",
        borrowed: &[
            "persistent level",
            "sub level",
            "sublevel",
            "open level",
            "level editor",
        ],
        reason: "`world-partition-and-streaming` calls them worlds and cells",
    },
];

/// The engine's term for a word somebody typed, whether or not it is one of ours.
///
/// This is the alias half: "WHEN a user searches for 'blueprint' THEN the script graph editor SHALL
/// be offered, labelled with the engine's own term." The palette calls this on every query and adds
/// the engine's word to the search rather than rejecting the user's.
#[must_use]
pub fn engine_term_for(query: &str) -> Option<&'static Term> {
    let query = query.trim().to_lowercase();
    TERMS
        .iter()
        .find(|term| term.borrowed.iter().any(|borrowed| *borrowed == query))
}

/// Every alias and the term it reaches, for a palette building its index.
pub fn aliases() -> impl Iterator<Item = (&'static str, &'static Term)> {
    TERMS
        .iter()
        .flat_map(|term| term.borrowed.iter().map(move |borrowed| (*borrowed, term)))
}

/// Refuse interface text that uses another engine's product vocabulary.
///
/// "WHEN a panel labels a selection count in another engine's terms THEN it SHALL be flagged against
/// this requirement." Whole words and whole phrases only: `distractor` does not contain the word
/// `actor` for this purpose, and a check that thought it did would be one nobody ran.
pub fn check_label(text: &str) -> Result<()> {
    let lowered = text.to_lowercase();
    for term in &TERMS {
        for borrowed in term.borrowed {
            if !contains_word(&lowered, borrowed) {
                continue;
            }
            return Err(Problem::new(
                format!("show the text {text:?}"),
                format!(
                    "{borrowed:?} is another engine's product vocabulary; {}",
                    term.reason
                ),
            )
            .with_remedy(format!(
                "say {:?} instead; {borrowed:?} stays reachable as a search alias, so a user who \
                 types it still finds the feature",
                term.engine
            )));
        }
    }
    Ok(())
}

/// Whether `haystack` contains `needle` bounded by something that is not a letter or a digit.
fn contains_word(haystack: &str, needle: &str) -> bool {
    let mut from = 0;
    while let Some(offset) = haystack[from..].find(needle) {
        let start = from + offset;
        let end = start + needle.len();
        let before_is_word = haystack[..start]
            .chars()
            .next_back()
            .is_some_and(|character| character.is_alphanumeric() || character == '_');
        let after_is_word = haystack[end..]
            .chars()
            .next()
            .is_some_and(|character| character.is_alphanumeric() || character == '_');
        if !before_is_word && !after_is_word {
            return true;
        }
        from = start + 1;
        if from >= haystack.len() {
            break;
        }
    }
    false
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn borrowed_vocabulary_in_a_label_is_caught_with_the_word_to_use_instead() {
        let problem = check_label("3 Actors selected").unwrap_err();
        assert!(problem.because.contains("actor"), "{problem}");
        assert!(
            problem.remedy.as_deref().unwrap().contains("node, entity"),
            "{problem}"
        );
    }

    #[test]
    fn the_engines_own_words_pass() {
        for label in [
            "3 nodes selected",
            "Content Browser",
            "Script Graph",
            "Open World",
            "Prefab",
            "Mesh Instance",
            "Level of detail",
            "Log level: warning",
        ] {
            check_label(label).unwrap_or_else(|problem| panic!("{label:?}: {problem}"));
        }
    }

    #[test]
    fn a_familiar_term_still_finds_the_feature() {
        // "WHEN a user searches for 'blueprint' THEN the script graph editor SHALL be offered,
        // labelled with the engine's own term."
        let term = engine_term_for("Blueprint").expect("an alias every Unreal user types");
        assert_eq!(term.engine, "graph, script graph");
        assert!(
            engine_term_for("node").is_none(),
            "our own words are not aliases"
        );
    }

    #[test]
    fn a_substring_is_not_a_word() {
        // The check has to be trusted to be run. `distractor` contains `actor` and is not a
        // vocabulary violation.
        check_label("distractor density").unwrap();
        check_label("Levelled terrain").unwrap();
    }

    #[test]
    fn every_alias_reaches_a_term_and_no_alias_is_claimed_twice() {
        let mut seen: Vec<&str> = Vec::new();
        for (alias, term) in aliases() {
            assert!(!term.engine.is_empty());
            assert!(
                !seen.contains(&alias),
                "{alias:?} is claimed by two terms, so search would have to pick one"
            );
            seen.push(alias);
        }
        assert!(seen.len() >= TERMS.len());
    }
}
