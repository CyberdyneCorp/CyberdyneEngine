//! The transaction journal, autosave, and crash recovery. Task 3.6.
//!
//! "Each document SHALL maintain a **transaction journal**: committed operations since the last
//! save, persisted incrementally. Autosave SHALL persist the journal rather than rewriting source
//! assets, so an autosave never overwrites a file the user has not saved. After an abnormal
//! termination, the editor SHALL offer recovery from the last saved revision plus its journal,
//! reporting how many transactions are recoverable."
//!
//! And the sentence that makes the whole design pay: "The journal SHALL be the same operation stream
//! used by diff, live editing, and any future collaboration, rather than a separate
//! representation." So a journal record is a [`Transaction`] through
//! [`cy_editor_core::codec`] — the same bytes the live bridge sends.
//!
//! --- WHY EACH RECORD IS LENGTH-PREFIXED AND THE FILE IS APPEND-ONLY ------------------------------------
//!
//! Because the failure this exists for is a process that stopped in the middle of a write. A
//! length-prefixed record can be checked before it is decoded, so a half-written tail is *detected*
//! and dropped, and everything before it recovers. A file rewritten in place would instead lose
//! whatever was being rewritten — which is the work the user most wants back.
//!
//! [`Journal::read`] therefore stops at the first record that does not check out and reports how
//! many were good. That is the number the recovery prompt shows.

use std::fs::{File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};

use cy_editor_core::codec::{Reader, Writer};
use cy_editor_core::ids::DocumentId;
use cy_editor_core::problem::{Problem, Result};

use crate::transaction::Transaction;

/// The journal format's version, written in every file's header.
///
/// A file whose version this build does not know is refused rather than parsed optimistically: a
/// recovery that silently dropped operations it could not read would be worse than one that says it
/// cannot read them.
pub const FORMAT_VERSION: u32 = 1;

/// The eight bytes every journal file begins with, so that a file that is not one is named as such.
const MAGIC: &[u8; 8] = b"CYJRNL\x00\x01";

/// A document's on-disk journal of committed transactions since its last save.
#[derive(Clone, Debug)]
pub struct Journal {
    path: PathBuf,
    records: u64,
}

/// What a recovery found.
#[derive(Clone, Debug)]
pub struct Recovery {
    /// The transactions that decoded cleanly, in order.
    pub transactions: Vec<Transaction>,
    /// Whether the file ended in a record that did not check out — the shape of a crash mid-write.
    pub truncated: bool,
}

impl Recovery {
    /// How many transactions are recoverable. The number the prompt shows.
    #[must_use]
    pub fn count(&self) -> usize {
        self.transactions.len()
    }
}

impl Journal {
    /// The journal file for a document under `directory`, created if it is not there.
    ///
    /// Named by the document's identity rather than by its path, because the identity is what
    /// survives a rename of the asset and a restart of the editor — which are the two situations a
    /// journal has to be findable in.
    pub fn open(directory: &Path, document: DocumentId) -> Result<Self> {
        std::fs::create_dir_all(directory).map_err(|error| {
            Problem::new(
                format!("create the journal directory {}", directory.display()),
                error.to_string(),
            )
            .with_remedy("check that the path is writable")
        })?;
        let path = directory.join(format!("{document}.cyjournal"));
        let journal = Self { path, records: 0 };
        if !journal.path.exists() {
            journal.write_header()?;
        }
        Ok(journal)
    }

    /// Where the journal is.
    #[must_use]
    pub fn path(&self) -> &Path {
        &self.path
    }

    /// How many records this session has appended.
    #[must_use]
    pub const fn records(&self) -> u64 {
        self.records
    }

    /// Append one committed transaction, flushing before returning.
    ///
    /// Flushed rather than buffered, because the whole value of a journal is what survives a process
    /// that did not get to run its shutdown. Buffering would trade the property for throughput the
    /// editor does not need — a committed transaction happens at human speed.
    pub fn append(&mut self, transaction: &Transaction) -> Result<()> {
        let mut payload = Writer::new();
        transaction.encode(&mut payload);
        let payload = payload.finish();

        let mut record = Writer::new();
        record.u32(u32::try_from(payload.len()).unwrap_or(u32::MAX));
        record.u32(checksum(&payload));
        let mut bytes = record.finish();
        bytes.extend_from_slice(&payload);

        let mut file = OpenOptions::new()
            .append(true)
            .open(&self.path)
            .map_err(|error| {
                Problem::new(
                    format!("open the journal {}", self.path.display()),
                    error.to_string(),
                )
            })?;
        file.write_all(&bytes)
            .and_then(|()| file.flush())
            .map_err(|error| {
                Problem::new(
                    format!("write to the journal {}", self.path.display()),
                    error.to_string(),
                )
            })?;
        self.records += 1;
        Ok(())
    }

    /// Read every record that checks out, stopping at the first that does not.
    pub fn read(&self) -> Result<Recovery> {
        let mut bytes = Vec::new();
        File::open(&self.path)
            .and_then(|mut file| file.read_to_end(&mut bytes))
            .map_err(|error| {
                Problem::new(
                    format!("read the journal {}", self.path.display()),
                    error.to_string(),
                )
            })?;
        Self::decode(&bytes)
    }

    /// Discard the journal, which is what a successful save does.
    ///
    /// Called *after* the source assets are written, never before: a journal removed first and a
    /// save that then failed would lose exactly the work the journal existed to hold.
    pub fn reset(&mut self) -> Result<()> {
        self.write_header()?;
        self.records = 0;
        Ok(())
    }

    /// Whether a journal for this document holds anything to recover.
    #[must_use]
    pub fn has_records(&self) -> bool {
        self.read().is_ok_and(|recovery| recovery.count() > 0)
    }

    fn write_header(&self) -> Result<()> {
        let mut header = Vec::from(*MAGIC);
        header.extend_from_slice(&FORMAT_VERSION.to_le_bytes());
        std::fs::write(&self.path, header).map_err(|error| {
            Problem::new(
                format!("create the journal {}", self.path.display()),
                error.to_string(),
            )
            .with_remedy("check that the journal directory is writable")
        })
    }

    /// Decode a journal file's bytes. Split out so that the recovery rules are testable in memory.
    pub fn decode(bytes: &[u8]) -> Result<Recovery> {
        if bytes.len() < MAGIC.len() + 4 || &bytes[..MAGIC.len()] != MAGIC {
            return Err(Problem::new(
                "read a journal",
                "the file does not start with a journal header",
            )
            .with_remedy("it is not a journal, or it was never written to"));
        }
        let version = u32::from_le_bytes(
            bytes[MAGIC.len()..MAGIC.len() + 4]
                .try_into()
                .expect("four bytes"),
        );
        if version != FORMAT_VERSION {
            return Err(Problem::new(
                "read a journal",
                format!("it is format version {version} and this build writes {FORMAT_VERSION}"),
            )
            .with_remedy("open it with the editor that wrote it"));
        }

        let mut at = MAGIC.len() + 4;
        let mut transactions = Vec::new();
        let mut truncated = false;
        while at < bytes.len() {
            // A header that does not fit is a crash between records: everything before it is good.
            if bytes.len() - at < 8 {
                truncated = true;
                break;
            }
            let length =
                u32::from_le_bytes(bytes[at..at + 4].try_into().expect("four bytes")) as usize;
            let expected =
                u32::from_le_bytes(bytes[at + 4..at + 8].try_into().expect("four bytes"));
            at += 8;
            if bytes.len() - at < length {
                truncated = true;
                break;
            }
            let payload = &bytes[at..at + length];
            if checksum(payload) != expected {
                truncated = true;
                break;
            }
            at += length;
            let mut reader = Reader::new(payload);
            let Ok(transaction) = Transaction::decode(&mut reader) else {
                truncated = true;
                break;
            };
            transactions.push(transaction);
        }
        Ok(Recovery {
            transactions,
            truncated,
        })
    }
}

/// FNV-1a over 32 bits, for detecting a torn write.
///
/// Not a security primitive and not asked to be one: what it has to catch is a record that stopped
/// half way or a block of zeroes a filesystem left behind, and a four-byte checksum catches both
/// with four bytes of overhead per transaction.
fn checksum(bytes: &[u8]) -> u32 {
    let mut hash = 0x811c_9dc5_u32;
    for byte in bytes {
        hash ^= u32::from(*byte);
        hash = hash.wrapping_mul(0x0100_0193);
    }
    hash
}

#[cfg(test)]
mod tests {
    use cy_editor_core::Actor;
    use cy_editor_core::ids::{FieldId, NodeId, TypeId};
    use cy_editor_core::value::Value;

    use super::*;
    use crate::operation::Operation;
    use crate::transaction::TransactionId;

    fn document() -> DocumentId {
        DocumentId::of_asset("worlds/city.cyworld")
    }

    fn transaction(sequence: u16) -> Transaction {
        Transaction {
            id: TransactionId::from_raw(u64::from(sequence)),
            document: document(),
            name: format!("Edit {sequence}"),
            actor: Actor::agent("claude", "s-1", "raise the lamps"),
            operations: vec![Operation::SetField {
                node: NodeId::in_document(document(), 1),
                component: TypeId::from_raw(1),
                field: FieldId::from_raw(1),
                before: Value::Float(0.0),
                after: Value::Float(f32::from(sequence)),
            }],
            coalesce_key: None,
        }
    }

    fn scratch(name: &str) -> PathBuf {
        let directory =
            std::env::temp_dir().join(format!("cy-editor-journal-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&directory);
        directory
    }

    #[test]
    fn a_journal_round_trips_every_committed_transaction() {
        let directory = scratch("round-trip");
        let mut journal = Journal::open(&directory, document()).unwrap();
        for sequence in 1..=5 {
            journal.append(&transaction(sequence)).unwrap();
        }

        let recovery = journal.read().unwrap();
        assert_eq!(recovery.count(), 5);
        assert!(!recovery.truncated);
        assert_eq!(recovery.transactions[4].name, "Edit 5");
        assert_eq!(
            recovery.transactions[0].actor.intent(),
            Some("raise the lamps")
        );

        std::fs::remove_dir_all(&directory).unwrap();
    }

    #[test]
    fn a_crash_mid_write_loses_only_the_torn_record() {
        let directory = scratch("torn");
        let mut journal = Journal::open(&directory, document()).unwrap();
        for sequence in 1..=3 {
            journal.append(&transaction(sequence)).unwrap();
        }
        // Simulate a process that stopped part way through the fourth record's payload.
        let mut bytes = std::fs::read(journal.path()).unwrap();
        let mut partial = Writer::new();
        transaction(4).encode(&mut partial);
        let payload = partial.finish();
        bytes.extend_from_slice(&u32::try_from(payload.len()).unwrap().to_le_bytes());
        bytes.extend_from_slice(&checksum(&payload).to_le_bytes());
        bytes.extend_from_slice(&payload[..payload.len() / 2]);
        std::fs::write(journal.path(), &bytes).unwrap();

        let recovery = journal.read().unwrap();
        assert_eq!(
            recovery.count(),
            3,
            "everything before the torn record recovers"
        );
        assert!(
            recovery.truncated,
            "and the caller is told the file ended badly"
        );

        std::fs::remove_dir_all(&directory).unwrap();
    }

    #[test]
    fn a_corrupted_record_is_detected_by_its_checksum() {
        let directory = scratch("corrupt");
        let mut journal = Journal::open(&directory, document()).unwrap();
        journal.append(&transaction(1)).unwrap();
        journal.append(&transaction(2)).unwrap();

        let mut bytes = std::fs::read(journal.path()).unwrap();
        let last = bytes.len() - 1;
        bytes[last] ^= 0xff;
        std::fs::write(journal.path(), &bytes).unwrap();

        let recovery = journal.read().unwrap();
        assert_eq!(recovery.count(), 1);
        assert!(recovery.truncated);

        std::fs::remove_dir_all(&directory).unwrap();
    }

    #[test]
    fn a_reset_journal_holds_nothing_and_is_still_a_journal() {
        let directory = scratch("reset");
        let mut journal = Journal::open(&directory, document()).unwrap();
        journal.append(&transaction(1)).unwrap();
        journal.reset().unwrap();

        let recovery = journal.read().unwrap();
        assert_eq!(recovery.count(), 0);
        assert!(!recovery.truncated);

        std::fs::remove_dir_all(&directory).unwrap();
    }

    #[test]
    fn a_file_that_is_not_a_journal_is_named_rather_than_parsed() {
        let problem = Journal::decode(b"not a journal at all").unwrap_err();
        assert!(problem.remedy.is_some(), "{problem}");
    }
}
