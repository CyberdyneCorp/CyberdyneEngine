//! The command palette: one input that reaches everything, and never blocks the interface.
//!
//! `editor-ui-ux`: "The editor SHALL provide a **command palette** that searches commands, assets,
//! entities, settings, documentation, and recent items from one input. Search SHALL be **fuzzy,
//! ranked, and incremental**, and SHALL show what a result is and where it comes from ... Search
//! SHALL never block the interface thread, and partial results SHALL be shown while indexing
//! continues."
//!
//! Four properties, and each is a decision here rather than a note:
//!
//! * **One input reaches everything** — [`Origin`] has a variant per source and [`Index`] holds them
//!   in one list, so a result from the asset database and a result from the command registry are
//!   ranked against each other rather than in separate lists a user has to choose between.
//! * **Ranked** — [`score`] is deterministic and total. Two runs of the same query over the same
//!   index produce the same order, which is what makes a palette feel like a tool rather than a
//!   lottery.
//! * **Never blocks** — [`Index::search`] takes `&self` and allocates only its results. Building the
//!   index is somebody else's thread: [`Index::ingest`] is fed in batches from a background
//!   operation, and [`Index::is_indexing`] is what the palette shows a user so that partial results
//!   are honestly labelled partial.
//! * **Shows where it comes from** — every [`Match`] carries its [`Entry`], and every entry carries
//!   its origin. A result with no provenance is the failure this requirement names.
//!
//! --- ALIASES, AND WHY THEY ARE THE VOCABULARY TABLE ------------------------------------------------
//!
//! `editor-visual-language` requires that a user who types another engine's word still finds the
//! feature, "labelled with the engine's own term". So a query is also looked up in
//! `cy_editor_visual::vocabulary`, and when it is an alias the engine's term is searched as well and
//! the match records which alias found it. That is one table serving the search and the label check,
//! rather than two lists that drift.

use cy_editor_commands::Registry;
use cy_editor_core::ids::NodeId;
use cy_editor_visual::vocabulary;

/// Where a result came from.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub enum Origin {
    /// A registered command.
    Command,
    /// An asset in the project.
    Asset,
    /// A node in the open document.
    Entity,
    /// A setting.
    Setting,
    /// A documentation page.
    Documentation,
    /// Something opened recently.
    Recent,
}

impl Origin {
    /// The word shown beside a result, so a user can see what a result *is*.
    #[must_use]
    pub const fn label(self) -> &'static str {
        match self {
            Origin::Command => "Command",
            Origin::Asset => "Asset",
            Origin::Entity => "Node",
            Origin::Setting => "Setting",
            Origin::Documentation => "Documentation",
            Origin::Recent => "Recent",
        }
    }
}

/// What choosing a result does.
///
/// "Results SHALL be actionable directly — opening an asset, invoking a command, focusing an entity,
/// or navigating to a setting." Every variant is something the shell already knows how to do, and
/// the command variant is the only one that mutates anything — which is the point of commands being
/// the single action surface.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Action {
    /// Invoke a registered command by identifier.
    Invoke(String),
    /// Open an asset by project-relative path.
    OpenAsset(String),
    /// Select and frame a node.
    FocusNode(NodeId),
    /// Navigate to a setting.
    OpenSetting(String),
    /// Open a documentation page.
    OpenDocumentation(String),
}

/// One searchable thing.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Entry {
    /// What a user reads: `Create Entity`, `materials/brick.cymat`.
    pub label: String,
    /// What it is, in one line — a command's description, an asset's type.
    pub detail: String,
    /// Where it came from.
    pub origin: Origin,
    /// What choosing it does.
    pub action: Action,
    /// The label, lower-cased once at ingest so that searching allocates nothing per entry.
    folded: String,
}

impl Entry {
    /// An entry.
    pub fn new(
        label: impl Into<String>,
        detail: impl Into<String>,
        origin: Origin,
        action: Action,
    ) -> Self {
        let label = label.into();
        let folded = label.to_lowercase();
        Self {
            label,
            detail: detail.into(),
            origin,
            action,
            folded,
        }
    }
}

/// One result.
#[derive(Clone, PartialEq, Debug)]
pub struct Match<'index> {
    /// The entry that matched.
    pub entry: &'index Entry,
    /// How well, higher first. Comparable only against other matches of the same query.
    pub score: i32,
    /// The alias that found it, when the user typed another engine's word.
    ///
    /// Shown as "blueprint → Script Graph", which is how the specification's scenario is satisfied:
    /// the feature is found and it is labelled with the engine's own term.
    pub via_alias: Option<&'static str>,
}

/// Everything the palette can find.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct Index {
    entries: Vec<Entry>,
    indexing: bool,
}

impl Index {
    /// An empty index.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Add entries. Called in batches from a background operation, never from a search.
    pub fn ingest(&mut self, entries: impl IntoIterator<Item = Entry>) {
        self.entries.extend(entries);
    }

    /// Add every registered command.
    ///
    /// The whole registry, unfiltered: "Every user-invokable action SHALL be registered in a command
    /// registry ... the command palette ... SHALL all invoke commands", so a command the palette
    /// could not find would be an action reachable only from a widget.
    pub fn ingest_commands(&mut self, registry: &Registry) {
        let commands: Vec<Entry> = registry
            .all()
            .map(|metadata| {
                Entry::new(
                    metadata.label.clone(),
                    metadata.description.clone(),
                    Origin::Command,
                    Action::Invoke(metadata.id.clone()),
                )
            })
            .collect();
        self.ingest(commands);
    }

    /// Say that a background pass is still adding to the index.
    pub fn set_indexing(&mut self, indexing: bool) {
        self.indexing = indexing;
    }

    /// Whether results are still incomplete, so the palette can say so.
    #[must_use]
    pub const fn is_indexing(&self) -> bool {
        self.indexing
    }

    /// How many entries are indexed.
    #[must_use]
    pub fn len(&self) -> usize {
        self.entries.len()
    }

    /// Whether nothing is indexed.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    /// The best `limit` results for a query, best first.
    ///
    /// An empty query returns the first `limit` entries rather than nothing, because a palette that
    /// is blank until you type cannot be browsed — and browsing it is how a user finds out what the
    /// editor can do.
    #[must_use]
    pub fn search(&self, query: &str, limit: usize) -> Vec<Match<'_>> {
        let folded = query.trim().to_lowercase();
        if folded.is_empty() {
            return self
                .entries
                .iter()
                .take(limit)
                .map(|entry| Match {
                    entry,
                    score: 0,
                    via_alias: None,
                })
                .collect();
        }

        let alias = vocabulary::engine_term_for(&folded);
        let alias_query = alias.map(|term| term.engine.to_lowercase());
        // The alias reported back is the word the user actually typed rather than the term's first
        // spelling, so that a result reads "blueprints → Script Graph" when that is what was typed.
        let typed = alias.and_then(|term| {
            term.borrowed
                .iter()
                .copied()
                .find(|borrowed| *borrowed == folded)
        });

        let mut matches: Vec<Match<'_>> = self
            .entries
            .iter()
            .filter_map(|entry| {
                let direct = score(&folded, &entry.folded);
                let through_alias = alias_query
                    .as_deref()
                    .and_then(|engine| score_any_word(engine, &entry.folded));
                match (direct, through_alias) {
                    (Some(direct), Some(through)) if through > direct => Some(Match {
                        entry,
                        score: through,
                        via_alias: typed,
                    }),
                    (Some(direct), _) => Some(Match {
                        entry,
                        score: direct,
                        via_alias: None,
                    }),
                    (None, Some(through)) => Some(Match {
                        entry,
                        score: through,
                        via_alias: typed,
                    }),
                    (None, None) => None,
                }
            })
            .collect();

        // Deterministic to the last tie: score, then the shorter label, then the label itself.
        // Ranking that depended on insertion order would reorder itself when an unrelated asset
        // finished importing, which is the palette equivalent of a panel moving under the pointer.
        matches.sort_unstable_by(|first, second| {
            second
                .score
                .cmp(&first.score)
                .then(first.entry.label.len().cmp(&second.entry.label.len()))
                .then(first.entry.label.cmp(&second.entry.label))
        });
        matches.truncate(limit);
        matches
    }
}

/// The score of `query` against `candidate`, or `None` when it is not a subsequence.
///
/// Deliberately simple and deliberately total: a fuzzy matcher with a tuning table is a thing nobody
/// can predict the behaviour of, and a palette whose order surprises people is one they stop
/// trusting to be at the top of the list. The four bonuses are the four things users actually mean.
///
/// Matching is byte-wise over the folded strings, which is exact for ASCII and treats a multi-byte
/// character as its bytes. That is a subsequence match either way and it cannot panic on a character
/// boundary, which is the property that matters for a function every keystroke runs a hundred
/// thousand times.
#[must_use]
pub fn score(query: &str, candidate: &str) -> Option<i32> {
    /// Matching the first character of the candidate.
    const AT_START: i32 = 40;
    /// Matching just after a space or a hyphen: the start of a word.
    const AT_WORD: i32 = 18;
    /// Matching immediately after the previous match.
    const CONTIGUOUS: i32 = 10;
    /// Every matched character is worth something on its own.
    const MATCHED: i32 = 4;

    if query.is_empty() {
        return Some(0);
    }
    let candidate = candidate.as_bytes();
    let mut score = 0;
    let mut position = 0;
    let mut previous: Option<usize> = None;

    for wanted in query.bytes() {
        let found = candidate[position..]
            .iter()
            .position(|byte| *byte == wanted)
            .map(|offset| position + offset)?;
        score += MATCHED;
        if found == 0 {
            score += AT_START;
        } else if matches!(candidate[found - 1], b' ' | b'-' | b'_' | b'/' | b'.') {
            score += AT_WORD;
        }
        if previous == Some(found.wrapping_sub(1)) {
            score += CONTIGUOUS;
        }
        previous = Some(found);
        position = found + 1;
    }

    // A short label that matched everything beats a long one that also did: `Undo` above
    // `Undo History Settings` for the query `undo`.
    let excess = candidate.len().saturating_sub(query.len());
    Some(score - i32::try_from(excess.min(64)).unwrap_or(64))
}

/// The best score of any single word of `query` against `candidate`.
///
/// Used for the alias path, where the engine's term is a phrase — "graph, script graph" — and the
/// user typed one foreign word. Scoring the whole phrase would find nothing.
fn score_any_word(query: &str, candidate: &str) -> Option<i32> {
    query
        .split(|character: char| !character.is_alphanumeric())
        .filter(|word| !word.is_empty())
        .filter_map(|word| score(word, candidate))
        .max()
}

#[cfg(test)]
mod tests {
    use cy_editor_core::Actor;
    use cy_editor_services::builtin;

    use super::*;

    fn index() -> Index {
        let mut index = Index::new();
        let mut registry = Registry::new();
        builtin::register(&mut registry).unwrap();
        index.ingest_commands(&registry);
        index.ingest([
            Entry::new(
                "materials/brick.cymat",
                "Material",
                Origin::Asset,
                Action::OpenAsset("materials/brick.cymat".into()),
            ),
            Entry::new(
                "Script Graph",
                "Open the script graph editor",
                Origin::Command,
                Action::Invoke("window.script-graph".into()),
            ),
            Entry::new(
                "Interface Scale",
                "How large the editor's interface is drawn",
                Origin::Setting,
                Action::OpenSetting("interface.scale".into()),
            ),
            Entry::new(
                "Streetlight",
                "Node in worlds/city.cyworld",
                Origin::Entity,
                Action::FocusNode(NodeId::in_document(
                    cy_editor_core::ids::DocumentId::of_asset("worlds/city.cyworld"),
                    7,
                )),
            ),
        ]);
        let _ = Actor::human("designer");
        index
    }

    #[test]
    fn one_input_reaches_commands_assets_entities_and_settings() {
        // "WHEN a user types a term THEN matching commands, assets, entities, and settings SHALL be
        // offered with their origin shown."
        let index = index();
        let found = |query: &str| {
            index
                .search(query, 8)
                .into_iter()
                .map(|matched| matched.entry.origin)
                .collect::<Vec<_>>()
        };
        assert!(found("create").contains(&Origin::Command));
        assert!(found("brick").contains(&Origin::Asset));
        assert!(found("streetlight").contains(&Origin::Entity));
        assert!(found("scale").contains(&Origin::Setting));

        for matched in index.search("s", 8) {
            assert!(
                !matched.entry.origin.label().is_empty(),
                "every result says what it is"
            );
        }
    }

    #[test]
    fn a_familiar_term_finds_the_feature_and_it_is_labelled_with_ours() {
        // `editor-visual-language`: "WHEN a user searches for 'blueprint' THEN the script graph
        // editor SHALL be offered, labelled with the engine's own term."
        let index = index();
        let results = index.search("blueprint", 4);
        let first = results.first().expect("an alias reaches the feature");
        assert_eq!(first.entry.label, "Script Graph");
        assert_eq!(first.via_alias, Some("blueprint"));

        // And the word reported back is the one that was typed, not the term's first spelling.
        let plural = index.search("blueprints", 4);
        assert_eq!(plural[0].entry.label, "Script Graph");
        assert_eq!(plural[0].via_alias, Some("blueprints"));
    }

    #[test]
    fn ranking_puts_the_thing_the_user_meant_first() {
        let mut index = Index::new();
        index.ingest([
            Entry::new(
                "Undo History Settings",
                "",
                Origin::Setting,
                Action::OpenSetting("x".into()),
            ),
            Entry::new(
                "Undo",
                "",
                Origin::Command,
                Action::Invoke("edit.undo".into()),
            ),
            Entry::new(
                "Bundle Options",
                "",
                Origin::Setting,
                Action::OpenSetting("y".into()),
            ),
        ]);
        let results = index.search("undo", 3);
        assert_eq!(results[0].entry.label, "Undo");
        assert_eq!(results[1].entry.label, "Undo History Settings");
    }

    #[test]
    fn the_same_query_over_the_same_index_ranks_the_same_way_every_time() {
        let index = index();
        let first: Vec<String> = index
            .search("e", 10)
            .into_iter()
            .map(|matched| matched.entry.label.clone())
            .collect();
        for _ in 0..8 {
            let again: Vec<String> = index
                .search("e", 10)
                .into_iter()
                .map(|matched| matched.entry.label.clone())
                .collect();
            assert_eq!(first, again, "ranking is not deterministic");
        }
    }

    #[test]
    fn partial_results_are_shown_while_indexing_continues() {
        let mut index = Index::new();
        index.set_indexing(true);
        index.ingest([Entry::new(
            "materials/brick.cymat",
            "Material",
            Origin::Asset,
            Action::OpenAsset("materials/brick.cymat".into()),
        )]);

        assert!(index.is_indexing(), "the palette says results are partial");
        assert_eq!(index.search("brick", 4).len(), 1, "and shows what it has");

        index.set_indexing(false);
        assert!(!index.is_indexing());
    }

    #[test]
    fn an_empty_query_offers_something_to_browse() {
        let index = index();
        assert_eq!(index.search("", 3).len(), 3);
        assert_eq!(index.search("   ", 3).len(), 3);
    }

    #[test]
    fn a_query_that_matches_nothing_returns_nothing_rather_than_everything() {
        assert!(index().search("qqqqzz", 8).is_empty());
    }

    #[test]
    fn every_registered_command_is_reachable_from_the_palette() {
        // Keyboard-first, and the forbidden pattern "an action reachable only from one widget with
        // no command registration" read from the other end: every command is findable by typing.
        let mut registry = Registry::new();
        builtin::register(&mut registry).unwrap();
        let mut index = Index::new();
        index.ingest_commands(&registry);

        for metadata in registry.all() {
            let found = index
                .search(&metadata.label, 8)
                .into_iter()
                .any(|matched| matched.entry.action == Action::Invoke(metadata.id.clone()));
            assert!(found, "{} cannot be found by typing its label", metadata.id);
        }
    }
}
