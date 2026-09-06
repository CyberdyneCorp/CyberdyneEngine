//! Problems surface at the object that has them, and every one says what would fix it.
//!
//! `editor-ui-ux`: "Problems SHALL be surfaced **at the object that has them** — in the hierarchy,
//! the inspector row, the asset entry, the viewport — not only in a console ... Every problem SHALL
//! state what is wrong, where, and what would fix it; **a message with no actionable content SHALL
//! be treated as a defect**."
//!
//! The last clause is the one this module makes real. [`Problems::report`] **refuses** a problem
//! with no remedy, so "treated as a defect" is a returned error at the point the message is written
//! rather than a code review six weeks later. The refusal itself names what to do, because a
//! refusal with no remedy would be the same defect one level up.
//!
//! --- WHY A SITE AND NOT A PANEL --------------------------------------------------------------------
//!
//! A report names *the thing that has the problem* — a node, a field, an asset — and never the panel
//! that shows it. That is what makes one report appear in the hierarchy row, the inspector row and
//! the problems view at once: three panels ask "what is wrong with this node", and the answer does
//! not depend on which of them asked. A report that named a panel would have to be posted three
//! times, and the third one would be the one somebody forgot.
//!
//! --- WHY IT IS BOUNDED -----------------------------------------------------------------------------
//!
//! "The console SHALL support filtering ... and SHALL cope with **high message rates** without
//! stalling the editor." A cook that fails on ten thousand assets posts ten thousand reports. The
//! store keeps the most recent [`CAPACITY`] and counts what it dropped, which is the same rule the
//! notification log and the undo history follow: a user who missed something and cannot tell is
//! worse off than one who knows they did.

use std::collections::VecDeque;

use cy_editor_core::ids::{DocumentId, FieldId, NodeId, TypeId};
use cy_editor_core::observe::{Revision, Versioned};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_services::Severity;

/// What has the problem.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Site {
    /// A node in a document.
    Node(NodeId),
    /// One field of one component of one node — the row an inspector marks.
    Field {
        /// The node.
        node: NodeId,
        /// The component.
        component: TypeId,
        /// The field.
        field: FieldId,
    },
    /// An asset, by project-relative path.
    Asset(String),
    /// A document as a whole.
    Document(DocumentId),
    /// Something visible in the viewport that is not one object.
    Viewport,
}

impl Site {
    /// The node this site is on, if it is on one.
    ///
    /// A field's problem is also the node's problem, which is why the hierarchy shows a marker for
    /// a node whose *property* is broken.
    #[must_use]
    pub const fn node(&self) -> Option<NodeId> {
        match self {
            Site::Node(node) | Site::Field { node, .. } => Some(*node),
            _ => None,
        }
    }
}

/// One thing that is wrong.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Report {
    /// How serious.
    pub severity: Severity,
    /// What is wrong, why, and what would fix it.
    pub problem: Problem,
    /// What has it.
    pub site: Site,
    /// What produced it: `import`, `cook`, `validation`, a plugin's name.
    pub source: String,
}

impl Report {
    /// A report. Use [`Problems::report`] to file it, which is where the remedy is required.
    pub fn new(
        severity: Severity,
        source: impl Into<String>,
        site: Site,
        problem: Problem,
    ) -> Self {
        Self {
            severity,
            problem,
            site,
            source: source.into(),
        }
    }
}

/// What the problems view is showing.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct Filter {
    /// Only this severity and above.
    pub severity: Option<Severity>,
    /// Only this source.
    pub source: Option<String>,
    /// Only reports whose text contains this, case-insensitively.
    pub text: Option<String>,
}

impl Filter {
    /// Whether a report passes.
    #[must_use]
    pub fn admits(&self, report: &Report) -> bool {
        if self.severity.is_some_and(|least| report.severity < least) {
            return false;
        }
        if self
            .source
            .as_ref()
            .is_some_and(|source| &report.source != source)
        {
            return false;
        }
        if let Some(text) = &self.text {
            let text = text.to_lowercase();
            let haystack = format!(
                "{} {} {}",
                report.problem.what,
                report.problem.because,
                report.problem.remedy.as_deref().unwrap_or_default()
            )
            .to_lowercase();
            if !haystack.contains(&text) {
                return false;
            }
        }
        true
    }
}

/// How many reports are held before the oldest is dropped.
pub const CAPACITY: usize = 4_096;

/// Everything that is wrong, and where.
#[derive(Debug)]
pub struct Problems {
    reports: VecDeque<Report>,
    dropped: u64,
    revision: Versioned<()>,
}

impl Default for Problems {
    fn default() -> Self {
        Self::new()
    }
}

impl Problems {
    /// Nothing wrong.
    #[must_use]
    pub fn new() -> Self {
        Self {
            reports: VecDeque::new(),
            dropped: 0,
            revision: Versioned::new(()),
        }
    }

    /// File a report, **refusing** one a user could do nothing about.
    pub fn report(&mut self, report: Report) -> Result<()> {
        if report.problem.remedy.is_none() {
            return Err(Problem::new(
                format!("report {:?}", report.problem.what),
                "the problem states no remedy, and a message with no actionable content is a defect",
            )
            .with_remedy(
                "say what would fix it — the operation, the setting, or the value that would make \
                 it work — or do not surface it at all",
            ));
        }
        if report.problem.because.trim().is_empty() {
            return Err(Problem::new(
                format!("report {:?}", report.problem.what),
                "the problem states no reason",
            )
            .with_remedy("say why it is wrong, in a clause that completes \"because ...\""));
        }
        if self.reports.len() == CAPACITY {
            self.reports.pop_front();
            self.dropped += 1;
        }
        self.reports.push_back(report);
        self.revision.update(|()| {});
        Ok(())
    }

    /// Every report, oldest first.
    pub fn all(&self) -> impl Iterator<Item = &Report> {
        self.reports.iter()
    }

    /// How many reports are held.
    #[must_use]
    pub fn len(&self) -> usize {
        self.reports.len()
    }

    /// Whether nothing is wrong.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.reports.is_empty()
    }

    /// How many were dropped because the store was full.
    #[must_use]
    pub const fn dropped(&self) -> u64 {
        self.dropped
    }

    /// The revision, so a panel rebuilds only when something was filed.
    #[must_use]
    pub fn revision(&self) -> Revision {
        self.revision.revision()
    }

    /// Everything wrong with a node, **including its fields** — what a hierarchy row marks.
    pub fn at_node(&self, node: NodeId) -> impl Iterator<Item = &Report> {
        self.reports
            .iter()
            .filter(move |report| report.site.node() == Some(node))
    }

    /// Everything wrong with one field — what an inspector row marks.
    pub fn at_field(
        &self,
        node: NodeId,
        component: TypeId,
        field: FieldId,
    ) -> impl Iterator<Item = &Report> {
        self.reports.iter().filter(move |report| {
            report.site
                == Site::Field {
                    node,
                    component,
                    field,
                }
        })
    }

    /// Everything wrong with an asset — what a content browser entry marks.
    pub fn at_asset<'problems>(
        &'problems self,
        path: &'problems str,
    ) -> impl Iterator<Item = &'problems Report> {
        self.reports
            .iter()
            .filter(move |report| report.site == Site::Asset(path.to_string()))
    }

    /// The reports a filter admits, most recent first.
    #[must_use]
    pub fn filtered(&self, filter: &Filter) -> Vec<&Report> {
        self.reports
            .iter()
            .rev()
            .filter(|report| filter.admits(report))
            .collect()
    }

    /// How many reports are at least this severe.
    #[must_use]
    pub fn count(&self, least: Severity) -> usize {
        self.reports
            .iter()
            .filter(|report| report.severity >= least)
            .count()
    }

    /// Forget everything one source filed: what a re-import does before it files again.
    pub fn clear_from(&mut self, source: &str) {
        self.reports.retain(|report| report.source != source);
        self.revision.update(|()| {});
    }

    /// The line a footer shows: how many problems, in words as well as by colour.
    ///
    /// "Status indicators SHALL state the condition in text as well as colour, and SHALL be readable
    /// without hovering."
    #[must_use]
    pub fn summary(&self) -> String {
        let errors = self.count(Severity::Error);
        let warnings = self.count(Severity::Warning) - errors;
        match (errors, warnings) {
            (0, 0) => "No problems".to_string(),
            (0, warnings) => format!("{warnings} warning(s)"),
            (errors, 0) => format!("{errors} error(s)"),
            (errors, warnings) => format!("{errors} error(s), {warnings} warning(s)"),
        }
    }
}

#[cfg(test)]
mod tests {
    use cy_editor_core::ids::DocumentId;

    use super::*;

    fn node() -> NodeId {
        NodeId::in_document(DocumentId::of_asset("worlds/city.cyworld"), 1)
    }

    fn missing_reference() -> Report {
        Report::new(
            Severity::Error,
            "validation",
            Site::Field {
                node: node(),
                component: TypeId::from_raw(3),
                field: FieldId::from_raw(9),
            },
            Problem::new(
                "resolve the material on this node",
                "materials/brick.cymat is not in the project",
            )
            .with_remedy("import it, or point the field at a material that is"),
        )
    }

    #[test]
    fn one_report_reaches_the_row_the_hierarchy_entry_and_the_problems_view() {
        // "WHEN an asset reference cannot be resolved THEN the inspector row, the hierarchy entry,
        // and the problems view SHALL show it, with an offered resolution." One report, three
        // panels, because a report names the object and not the panel.
        let mut problems = Problems::new();
        problems.report(missing_reference()).unwrap();

        assert_eq!(problems.at_node(node()).count(), 1, "the hierarchy entry");
        assert_eq!(
            problems
                .at_field(node(), TypeId::from_raw(3), FieldId::from_raw(9))
                .count(),
            1,
            "the inspector row"
        );
        assert_eq!(
            problems.filtered(&Filter::default()).len(),
            1,
            "the problems view"
        );
        assert!(
            problems
                .all()
                .next()
                .unwrap()
                .problem
                .remedy
                .as_deref()
                .unwrap()
                .contains("import it")
        );
    }

    #[test]
    fn a_message_with_no_remedy_is_refused_as_the_defect_it_is() {
        let mut problems = Problems::new();
        let problem = problems
            .report(Report::new(
                Severity::Error,
                "import",
                Site::Asset("meshes/rock.gltf".into()),
                Problem::new("import meshes/rock.gltf", "it failed"),
            ))
            .unwrap_err();
        assert!(problem.remedy.is_some(), "{problem}");
        assert!(problems.is_empty(), "and nothing was filed");
    }

    #[test]
    fn a_high_message_rate_bounds_itself_and_says_what_it_dropped() {
        let mut problems = Problems::new();
        for index in 0..(CAPACITY + 500) {
            problems
                .report(Report::new(
                    Severity::Warning,
                    "cook",
                    Site::Asset(format!("meshes/rock{index}.gltf")),
                    Problem::new("cook a mesh", "it has no material")
                        .with_remedy("assign a material, or accept the default"),
                ))
                .unwrap();
        }
        assert_eq!(problems.len(), CAPACITY);
        assert_eq!(problems.dropped(), 500);
    }

    #[test]
    fn the_console_filters_by_severity_source_and_text() {
        let mut problems = Problems::new();
        problems.report(missing_reference()).unwrap();
        problems
            .report(Report::new(
                Severity::Warning,
                "cook",
                Site::Asset("meshes/rock.gltf".into()),
                Problem::new("cook meshes/rock.gltf", "it has no lightmap coordinates")
                    .with_remedy("generate them on import, or disable lightmapping for it"),
            ))
            .unwrap();

        assert_eq!(problems.filtered(&Filter::default()).len(), 2);
        assert_eq!(
            problems
                .filtered(&Filter {
                    severity: Some(Severity::Error),
                    ..Filter::default()
                })
                .len(),
            1
        );
        assert_eq!(
            problems
                .filtered(&Filter {
                    source: Some("cook".into()),
                    ..Filter::default()
                })
                .len(),
            1
        );
        assert_eq!(
            problems
                .filtered(&Filter {
                    text: Some("lightmap".into()),
                    ..Filter::default()
                })
                .len(),
            1
        );
    }

    #[test]
    fn a_re_import_replaces_what_it_filed_before() {
        let mut problems = Problems::new();
        problems.report(missing_reference()).unwrap();
        problems.clear_from("validation");
        assert!(problems.is_empty());
    }

    #[test]
    fn the_footer_says_what_is_wrong_in_words() {
        let mut problems = Problems::new();
        assert_eq!(problems.summary(), "No problems");
        problems.report(missing_reference()).unwrap();
        assert_eq!(problems.summary(), "1 error(s)");
    }
}
