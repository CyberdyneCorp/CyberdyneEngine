//! Stable panel identity, separated from the title a person reads. Task 1.0.5.
//!
//! --- WHERE THE IDEA COMES FROM, AND WHY WE TOOK IT ------------------------------------------------
//!
//! M5.5 chose egui over Dear ImGui, and the M5.5 design note names the one thing Dear ImGui's
//! docking does better and says to steal it: its `WindowKey` separates a window's **stable
//! identity** from its **display title**. egui_dock, like most immediate-mode docking, tends to key
//! a tab by the string it shows.
//!
//! That is a defect a persisted workspace discovers late and expensively. `editor-ui-ux` requires
//! named workspaces that survive a restart, and a layout keyed by display title breaks on the two
//! things a shipping editor certainly does:
//!
//!   * **Localisation.** The same layout file opened in another language keys on "Hierarchie" and
//!     finds nothing. Every saved workspace resets to its default, and the user's arrangement is
//!     gone with no error to read.
//!   * **Renaming.** "Content Drawer" becomes "Content browser" — which this project's vocabulary
//!     rules actually require — and every layout saved before the rename loses that panel.
//!
//! Both are silent. The layout loads, the panel is simply missing, and the editor is not broken
//! enough to be reported.
//!
//! --- THE SEPARATION, CONCRETELY -------------------------------------------------------------------
//!
//! A [`PanelKey`] is what a layout stores: a **kind** (`hierarchy`, `viewport`, `content-browser`)
//! and an **instance** number, because two viewports are two panels of one kind rather than two
//! kinds. It converts to and from the [`PanelId`] that [`crate::docking::Layout`] already holds, so
//! this adds structure to identity without changing what a layout is or how it is encoded.
//!
//! A [`PanelTitles`] is what an interface reads: kind → default title, key → override, and a
//! localisation that replaces the default without touching either. **No title ever enters a
//! layout**, which is the property that makes the two failures above impossible rather than
//! unlikely — and [`PanelTitles::is_layout_safe`] is a check a test can make of a candidate string.

use std::collections::BTreeMap;

use cy_editor_core::problem::{Problem, Result};

use crate::docking::PanelId;

/// The separator between a panel's kind and its instance number.
///
/// `#` rather than a space or a dash: [`PanelId`] refuses whitespace, and a dash appears inside
/// every kind this editor has (`content-browser`), so parsing on one would be ambiguous.
const INSTANCE_SEPARATOR: char = '#';

/// A panel's stable identity: what it is, and which one of those it is.
///
/// This is the thing a workspace persists and the thing a command names. It contains no display
/// text of any kind, in any language, by construction.
#[derive(Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub struct PanelKey {
    kind: String,
    instance: u32,
}

impl PanelKey {
    /// The first panel of a kind: `hierarchy`, `inspector`, `content-browser`.
    ///
    /// Refuses a kind that would not round-trip through a layout — whitespace, or the separator
    /// this encoding reserves.
    pub fn new(kind: impl Into<String>) -> Result<Self> {
        Self::instanced(kind, 0)
    }

    /// The `instance`-th panel of a kind. Two viewports are `viewport#0` and `viewport#1`.
    pub fn instanced(kind: impl Into<String>, instance: u32) -> Result<Self> {
        let kind = kind.into();
        if kind.is_empty()
            || kind.chars().any(char::is_whitespace)
            || kind.contains(INSTANCE_SEPARATOR)
        {
            return Err(Problem::new(
                format!("name a panel kind {kind:?}"),
                format!(
                    "a kind is one word with no whitespace and no {INSTANCE_SEPARATOR:?}, which \
                     this encoding reserves for the instance number"
                ),
            )
            .with_remedy("use a hyphenated name such as content-browser"));
        }
        Ok(Self { kind, instance })
    }

    /// What the panel is.
    #[must_use]
    pub fn kind(&self) -> &str {
        &self.kind
    }

    /// Which panel of that kind this is. Zero for the first.
    #[must_use]
    pub fn instance(&self) -> u32 {
        self.instance
    }

    /// The identifier a layout stores.
    ///
    /// `hierarchy` for the first of a kind and `viewport#1` for the second, so an existing layout
    /// written before instances existed still names the panel it named.
    pub fn to_panel_id(&self) -> Result<PanelId> {
        if self.instance == 0 {
            PanelId::new(self.kind.clone())
        } else {
            PanelId::new(format!(
                "{}{INSTANCE_SEPARATOR}{}",
                self.kind, self.instance
            ))
        }
    }

    /// Read a key back out of a layout.
    ///
    /// An identifier with no instance suffix is instance zero, which is what makes every layout
    /// saved before this type existed still load.
    pub fn from_panel_id(id: &PanelId) -> Result<Self> {
        let text = id.as_str();
        match text.split_once(INSTANCE_SEPARATOR) {
            None => Self::new(text),
            Some((kind, instance)) => {
                let instance = instance.parse::<u32>().map_err(|_| {
                    Problem::new(
                        format!("read the panel identifier {text:?}"),
                        format!("{instance:?} after {INSTANCE_SEPARATOR:?} is not a number"),
                    )
                    .with_remedy("an instanced panel is named kind#N, as in viewport#1")
                })?;
                Self::instanced(kind, instance)
            }
        }
    }
}

impl std::fmt::Display for PanelKey {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if self.instance == 0 {
            formatter.write_str(&self.kind)
        } else {
            write!(
                formatter,
                "{}{INSTANCE_SEPARATOR}{}",
                self.kind, self.instance
            )
        }
    }
}

/// What each panel is called, and in what language — the half a layout must never contain.
#[derive(Clone, Debug, Default)]
pub struct PanelTitles {
    defaults: BTreeMap<String, String>,
    localised: BTreeMap<String, String>,
    overrides: BTreeMap<PanelKey, String>,
}

impl PanelTitles {
    /// An empty registry.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Give a kind its default English title: `hierarchy` → "Hierarchy".
    pub fn define(&mut self, kind: impl Into<String>, title: impl Into<String>) {
        self.defaults.insert(kind.into(), title.into());
    }

    /// Replace a kind's title for the current language.
    ///
    /// Separate from [`PanelTitles::define`] rather than overwriting it, so that switching language
    /// back is dropping a map rather than reconstructing every default — and so that a missing
    /// translation falls through to the default instead of showing an identifier.
    pub fn localise(&mut self, kind: impl Into<String>, title: impl Into<String>) {
        self.localised.insert(kind.into(), title.into());
    }

    /// Forget every translation, returning to the defined titles.
    pub fn clear_localisation(&mut self) {
        self.localised.clear();
    }

    /// Give one particular panel its own title: the second viewport called "Top", say.
    pub fn rename(&mut self, key: &PanelKey, title: impl Into<String>) {
        self.overrides.insert(key.clone(), title.into());
    }

    /// Take back a rename, so the panel is called whatever its kind is called.
    pub fn clear_rename(&mut self, key: &PanelKey) {
        self.overrides.remove(key);
    }

    /// What to draw on the tab.
    ///
    /// In order: this panel's own name, the kind's translated title, the kind's defined title, and
    /// finally the identifier itself — which is not a good title but is a legible one, and is
    /// better than an empty tab a user cannot click with confidence.
    #[must_use]
    pub fn title(&self, key: &PanelKey) -> String {
        if let Some(title) = self.overrides.get(key) {
            return title.clone();
        }
        let base = self
            .localised
            .get(&key.kind)
            .or_else(|| self.defaults.get(&key.kind));
        match (base, key.instance) {
            (Some(title), 0) => title.clone(),
            (Some(title), instance) => format!("{title} {}", instance + 1),
            (None, _) => key.to_string(),
        }
    }

    /// Whether a string is safe to put in a layout. It is not, and never is.
    ///
    /// A method that always answers `false` looks like a joke until it is the thing a test asserts.
    /// It exists so that "layouts key on identity, never on display text" is a check somebody can
    /// call rather than a paragraph somebody can skip, and so that a future `PanelId::new(title)`
    /// has an obvious thing to fail against.
    #[must_use]
    pub fn is_layout_safe(_title: &str) -> bool {
        false
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn titles() -> PanelTitles {
        let mut titles = PanelTitles::new();
        titles.define("hierarchy", "Hierarchy");
        titles.define("viewport", "Viewport");
        titles.define("content-browser", "Content browser");
        titles
    }

    #[test]
    fn a_layout_survives_a_panel_being_renamed() {
        // The failure this type exists to prevent, in the shape it actually arrives in: the product
        // vocabulary changes, and every workspace saved before the change loses the panel.
        let key = PanelKey::new("content-browser").expect("a kind");
        let stored = key.to_panel_id().expect("an identifier");

        let mut titles = titles();
        titles.rename(&key, "Content Drawer");
        assert_eq!(titles.title(&key), "Content Drawer");
        titles.clear_rename(&key);
        titles.define("content-browser", "Content browser");

        // The layout never saw either string.
        assert_eq!(stored.as_str(), "content-browser");
        assert_eq!(
            PanelKey::from_panel_id(&stored).expect("read back"),
            key,
            "the identifier resolves to the same panel whatever it is called today"
        );
    }

    #[test]
    fn a_layout_survives_being_opened_in_another_language() {
        let key = PanelKey::new("hierarchy").expect("a kind");
        let stored = key.to_panel_id().expect("an identifier");
        let mut titles = titles();
        assert_eq!(titles.title(&key), "Hierarchy");

        titles.localise("hierarchy", "Hierarchie");
        assert_eq!(titles.title(&key), "Hierarchie");
        assert_eq!(
            stored.as_str(),
            "hierarchy",
            "the saved layout is the same bytes in every language"
        );

        titles.clear_localisation();
        assert_eq!(titles.title(&key), "Hierarchy");
    }

    #[test]
    fn two_panels_of_one_kind_have_two_identities_and_two_titles() {
        let first = PanelKey::new("viewport").expect("a kind");
        let second = PanelKey::instanced("viewport", 1).expect("a kind");
        assert_ne!(first, second);
        assert_eq!(first.to_panel_id().expect("an id").as_str(), "viewport");
        assert_eq!(second.to_panel_id().expect("an id").as_str(), "viewport#1");

        let mut titles = titles();
        assert_eq!(titles.title(&first), "Viewport");
        assert_eq!(titles.title(&second), "Viewport 2");
        titles.rename(&second, "Top");
        assert_eq!(titles.title(&second), "Top");
        assert_eq!(titles.title(&first), "Viewport", "one rename, one panel");
    }

    #[test]
    fn an_identifier_written_before_instances_existed_still_names_its_panel() {
        let old = PanelId::new("inspector").expect("an identifier");
        let key = PanelKey::from_panel_id(&old).expect("read back");
        assert_eq!(key.kind(), "inspector");
        assert_eq!(key.instance(), 0);
        assert_eq!(key.to_panel_id().expect("an id"), old);
    }

    #[test]
    fn a_kind_that_could_not_round_trip_is_refused_at_construction() {
        assert!(PanelKey::new("").is_err());
        assert!(PanelKey::new("content browser").is_err());
        let problem = PanelKey::new("viewport#2").expect_err("refused");
        assert!(problem.remedy.is_some(), "{problem}");

        let malformed = PanelId::new("viewport#top").expect("a valid identifier");
        assert!(
            PanelKey::from_panel_id(&malformed).is_err(),
            "an instance is a number, and a layout that says otherwise says so out loud"
        );
    }

    #[test]
    fn a_title_is_never_an_identity() {
        // The rule, as a callable check. A panel whose kind happens to read like a title is still
        // keyed by the kind, and no code path turns a displayed string into a stored one.
        assert!(!PanelTitles::is_layout_safe("Hierarchy"));
        assert!(!PanelTitles::is_layout_safe("Hierarchie"));
        let key = PanelKey::new("hierarchy").expect("a kind");
        let titles = titles();
        assert_ne!(
            titles.title(&key),
            key.to_panel_id().expect("an id").as_str(),
            "the two are deliberately different strings"
        );
    }

    #[test]
    fn an_unknown_kind_shows_its_identifier_rather_than_nothing() {
        let key = PanelKey::instanced("some-plugins-panel", 2).expect("a kind");
        assert_eq!(PanelTitles::new().title(&key), "some-plugins-panel#2");
    }
}
