//! The runtime's side of the ring: which image the next frame may be drawn into, and what happens
//! when the answer is "none of them".
//!
//! This is pure decision-making with no Vulkan in it, so that the two properties that cost the most
//! to get wrong are testable on a machine with no GPU at all.
//!
//! # How many images, decided by measurement
//!
//! | images | what the spike measured |
//! |---|---|
//! | 1 | wedges: the runtime and the editor want the same image at the same time |
//! | 2 | throttles the runtime to the editor's refresh rate, and costs a whole editor frame — 16 ms — of latency |
//! | 3 | where pipelining starts: 1,727 runtime fps, 0.42 ms latency, no corruption |
//! | 4 | removes the last stalls, for 8.4 MB |
//!
//! Three is the minimum, four is preferred, and [`Ring::advisory`] says so out loud for anything
//! smaller rather than letting a two-image ring look like a working one.
//!
//! # The editor must never throttle the runtime
//!
//! So a full ring is the **runtime's** frame to drop, not the editor's frame to wait for
//! ([`FullRingPolicy::Drop`], the default). Blocking exists only as the measurement's control: it
//! is how the spike showed what two images cost, and it is what a runtime must not do in an editor
//! session.
//!
//! # `held` is what makes a released frame safe to re-read
//!
//! A monotonic release counter alone is not enough, and this is reproduced corruption rather than
//! caution. An editor that has released frame *N* and then keeps **re-sampling** it — which is
//! exactly what it does whenever no newer frame has arrived — is reading a slot the runtime
//! believes it may write. [`Ring::observe_held`] takes the editor's `(frame_id, slot)` claim out of
//! the shared page and excludes that slot until the claim moves.

use cy_editor_core::problem::{Problem, Result};

use crate::announce::held_unpack;
use crate::wire::MAX_BUFFERS;

/// The fewest images that pipeline. Below this the runtime and the editor contend for one image.
pub const MINIMUM_BUFFERS: usize = 3;

/// What the ring should be built with when nothing argues otherwise.
pub const PREFERRED_BUFFERS: usize = 4;

/// What a runtime does when every image is spoken for.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum FullRingPolicy {
    /// Drop this frame and carry on. **The default, and the only one an editor session may use.**
    #[default]
    Drop,
    /// Wait for the editor to release an image.
    ///
    /// Present for the measurement that showed what a small ring costs, and for a headless
    /// publisher that wants every frame delivered. In an editor session it makes the editor's
    /// refresh rate the runtime's frame rate, which is the thing the whole design refuses.
    Block,
}

/// One image of the ring, as the runtime sees it.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RingSlot {
    /// The identity of the last frame written here, or zero.
    pub last_frame: u64,
    /// When non-zero, the editor was observed holding this slot for that frame, and the runtime
    /// must wait for the release timeline to reach it before writing here again.
    pub needs_release: u64,
    /// Whether anything has ever been drawn here. Decides `UNDEFINED` against
    /// `SHADER_READ_ONLY_OPTIMAL` as the barrier's source layout.
    pub initialised: bool,
}

/// Whether an editor's claim names this slot.
///
/// The runtime asks this **after** declaring the slot it is about to write into, and drops the
/// frame when the answer is yes. See [`crate::announce`]'s `writing` field for why that second look
/// is not redundant with [`Ring::observe_held`]: the first look happens before the slot is chosen,
/// and the editor is entitled to claim it in between.
#[must_use]
pub fn claim_names(held: u64, slot: usize) -> bool {
    held != 0 && held_unpack(held).1 as usize == slot
}

/// The ring, and the rule for choosing the next image.
#[derive(Clone, Debug)]
pub struct Ring {
    slots: Vec<RingSlot>,
    policy: FullRingPolicy,
    published: Option<usize>,
    held_slot: Option<u32>,
    dropped: u64,
    vetoed: u64,
    generation: u32,
}

impl Ring {
    /// A ring of `count` images.
    ///
    /// Refuses zero and more than [`MAX_BUFFERS`]; accepts one and two, which are measurable and
    /// bad, and reports what they cost through [`Ring::advisory`].
    pub fn new(count: usize, policy: FullRingPolicy) -> Result<Self> {
        if count == 0 || count > MAX_BUFFERS {
            return Err(Problem::new(
                "build the viewport's ring",
                format!("{count} images; a ring holds 1 to {MAX_BUFFERS}"),
            )
            .with_remedy(format!(
                "{MINIMUM_BUFFERS} is the minimum that pipelines and {PREFERRED_BUFFERS} is preferred"
            )));
        }
        Ok(Self {
            slots: vec![RingSlot::default(); count],
            policy,
            published: None,
            held_slot: None,
            dropped: 0,
            vetoed: 0,
            generation: 1,
        })
    }

    /// What is wrong with this ring's size, or `None` when there is nothing to say.
    #[must_use]
    pub fn advisory(&self) -> Option<&'static str> {
        match self.slots.len() {
            1 => Some(
                "a one-image ring wedges: the runtime and the editor want the same image at the \
                 same time",
            ),
            2 => Some(
                "a two-image ring throttles the runtime to the editor's refresh rate and costs a \
                 whole editor frame of latency",
            ),
            _ => None,
        }
    }

    /// How many images the ring holds.
    #[must_use]
    pub fn len(&self) -> usize {
        self.slots.len()
    }

    /// Whether the ring holds no images. It never does; the lint asks for the method.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.slots.is_empty()
    }

    /// The ring's generation, which the handshake carries and every announcement repeats.
    #[must_use]
    pub fn generation(&self) -> u32 {
        self.generation
    }

    /// One image, by slot.
    #[must_use]
    pub fn slot(&self, index: usize) -> Option<&RingSlot> {
        self.slots.get(index)
    }

    /// How many frames were dropped because every image was spoken for.
    ///
    /// A number rather than a log line: dropping is normal when the runtime outruns the editor, and
    /// what matters is the rate, which only a counter can show.
    #[must_use]
    pub fn dropped(&self) -> u64 {
        self.dropped
    }

    /// What the runtime does when the ring is full.
    #[must_use]
    pub fn policy(&self) -> FullRingPolicy {
        self.policy
    }

    /// Take the editor's claim out of the shared page.
    ///
    /// A claim only pins a slot when it names the frame that slot actually holds: a stale claim —
    /// the editor holding a frame that has since been overwritten — must not pin the ring forever.
    pub fn observe_held(&mut self, held: u64) {
        if held == 0 {
            self.held_slot = None;
            return;
        }
        let (frame_id, slot) = held_unpack(held);
        self.held_slot = Some(slot);
        let index = slot as usize;
        if let Some(entry) = self.slots.get_mut(index)
            && entry.last_frame == frame_id
        {
            // From here on, this slot may not be rewritten until the editor's release timeline
            // reaches this value. This assignment is what closes the corruption that a release
            // counter alone leaves open.
            entry.needs_release = frame_id;
        }
    }

    /// The image the next frame may be drawn into, given how far the editor's release timeline has
    /// advanced, or `None` when every image is spoken for.
    ///
    /// A slot may be written when it is not the one just published (the editor may be about to
    /// latch it), not the one the editor says it is holding, and either never published or already
    /// released. The oldest eligible slot wins, so the editor's chance of latching a frame before
    /// it is overwritten is as large as the ring allows.
    #[must_use]
    pub fn pick(&self, release_value: u64) -> Option<usize> {
        let mut best: Option<(u64, usize)> = None;
        for (index, slot) in self.slots.iter().enumerate() {
            if self.published == Some(index) {
                continue;
            }
            if self.held_slot.is_some_and(|held| held as usize == index) {
                continue;
            }
            if slot.needs_release != 0 && release_value < slot.needs_release {
                continue;
            }
            if best.is_none_or(|(age, _)| slot.last_frame < age) {
                best = Some((slot.last_frame, index));
            }
        }
        best.map(|(_, index)| index)
    }

    /// Count a frame the runtime gave up on because the ring was full.
    pub fn drop_frame(&mut self) {
        self.dropped += 1;
    }

    /// Count a frame the runtime gave up on because the editor claimed the slot it had chosen.
    ///
    /// Separate from [`Ring::drop_frame`] because they say different things: a full ring means the
    /// editor is slower than the runtime, which is normal; a vetoed frame means the two chose the
    /// same slot at the same instant, which should be rare and whose rate is worth watching.
    pub fn veto_frame(&mut self) {
        self.vetoed += 1;
    }

    /// How many frames the editor's claim took away at the last instant.
    #[must_use]
    pub fn vetoed(&self) -> u64 {
        self.vetoed
    }

    /// Record that a frame has been submitted into a slot and announced.
    ///
    /// Called **after** `vkQueueSubmit` and immediately before the announcement, which is the order
    /// [`crate::announce`] explains and the reason a killed runtime is survivable.
    pub fn record_published(&mut self, index: usize, frame_id: u64) {
        if let Some(slot) = self.slots.get_mut(index) {
            slot.last_frame = frame_id;
            slot.initialised = true;
        }
        self.published = Some(index);
    }

    /// The release value this slot's next write must wait for, or zero for none.
    #[must_use]
    pub fn release_requirement(&self, index: usize) -> u64 {
        self.slots.get(index).map_or(0, |slot| slot.needs_release)
    }

    /// The editor is gone: reclaim the whole ring.
    ///
    /// Without this a runtime whose editor died keeps every slot the editor was holding, and a
    /// three-image ring is exhausted within three frames — a runtime that stops rendering because
    /// something *else* stopped watching, which is the inversion the whole design is against.
    pub fn reclaim(&mut self) {
        for slot in &mut self.slots {
            slot.needs_release = 0;
        }
        self.held_slot = None;
        self.published = None;
    }

    /// Rebuild the ring for a new size or a new device, bumping the generation.
    ///
    /// The generation is what stops a resize being sampled against a destroyed image: the editor
    /// refuses an announcement whose generation is not the one it hand-shook.
    pub fn regenerate(&mut self) {
        self.generation += 1;
        for slot in &mut self.slots {
            *slot = RingSlot::default();
        }
        self.published = None;
        self.held_slot = None;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::announce::held_pack;

    #[test]
    fn a_full_ring_costs_the_runtime_a_frame_and_never_a_stall() {
        // The requirement in one test: the editor must never throttle the runtime.
        let mut ring = Ring::new(MINIMUM_BUFFERS, FullRingPolicy::Drop).expect("three images");
        // Fill the ring: every slot written, every one released at 0 (nothing released yet), and
        // the editor holding the newest.
        for frame_id in 1..=3_u64 {
            let index = ring.pick(0).expect("a free slot");
            ring.record_published(index, frame_id);
            ring.observe_held(held_pack(frame_id, u32::try_from(index).expect("a slot")));
        }
        // Two slots are pinned by the editor's claims and one is the published one.
        assert_eq!(ring.pick(0), None, "every image is spoken for");
        assert_eq!(ring.policy(), FullRingPolicy::Drop, "the default");
        ring.drop_frame();
        assert_eq!(ring.dropped(), 1, "the runtime dropped rather than waited");

        // The editor releases up to frame 2 and lets go of its claim: the ring is usable again.
        ring.observe_held(0);
        assert!(ring.pick(2).is_some(), "released images come back");
    }

    #[test]
    fn a_slot_the_editor_says_it_is_holding_is_never_chosen() {
        // The corruption a release counter alone leaves open: the editor keeps re-sampling a frame
        // it already released, because nothing newer has arrived.
        let mut ring = Ring::new(MINIMUM_BUFFERS, FullRingPolicy::Drop).expect("three images");
        ring.record_published(0, 1);
        ring.observe_held(held_pack(1, 0));
        // The release timeline has advanced past everything, and the slot is STILL not eligible:
        // the editor is reading it right now.
        assert_ne!(ring.pick(u64::MAX), Some(0));
        assert_eq!(ring.release_requirement(0), 1, "and it must be released");

        // The editor moves on, and the two other slots are written. Slot 0 now holds the oldest
        // frame of the three, so it is the next one chosen — the claim held it back and nothing
        // more.
        ring.observe_held(0);
        ring.record_published(1, 2);
        ring.record_published(2, 3);
        assert_eq!(
            ring.pick(u64::MAX),
            Some(0),
            "once the editor moves on, the oldest slot is the next one written"
        );
    }

    #[test]
    fn a_stale_claim_does_not_pin_the_ring_forever() {
        // The editor claims a frame that has since been overwritten. Honouring that claim would
        // remove a slot from the ring permanently.
        let mut ring = Ring::new(MINIMUM_BUFFERS, FullRingPolicy::Drop).expect("three images");
        ring.record_published(0, 1);
        ring.record_published(1, 2);
        ring.observe_held(held_pack(1, 1));
        assert_eq!(
            ring.release_requirement(1),
            0,
            "slot 1 holds frame 2, not frame 1: the claim names a frame that is gone"
        );
    }

    #[test]
    fn one_and_two_image_rings_are_allowed_and_say_what_they_cost() {
        assert!(Ring::new(0, FullRingPolicy::Drop).is_err());
        assert!(Ring::new(MAX_BUFFERS + 1, FullRingPolicy::Drop).is_err());
        assert!(
            Ring::new(1, FullRingPolicy::Drop)
                .expect("allowed")
                .advisory()
                .is_some_and(|note| note.contains("wedges"))
        );
        assert!(
            Ring::new(2, FullRingPolicy::Drop)
                .expect("allowed")
                .advisory()
                .is_some_and(|note| note.contains("throttles"))
        );
        assert_eq!(
            Ring::new(PREFERRED_BUFFERS, FullRingPolicy::Drop)
                .expect("allowed")
                .advisory(),
            None
        );
    }

    #[test]
    fn a_claim_on_the_chosen_slot_is_a_veto() {
        let mut ring = Ring::new(MINIMUM_BUFFERS, FullRingPolicy::Drop).expect("three images");
        assert!(!claim_names(0, 1), "no claim names nothing");
        assert!(claim_names(held_pack(9, 1), 1));
        assert!(!claim_names(held_pack(9, 1), 2));
        ring.veto_frame();
        assert_eq!(ring.vetoed(), 1);
        assert_eq!(ring.dropped(), 0, "a veto is not a full ring, and says so");
    }

    #[test]
    fn a_dead_editor_gives_the_whole_ring_back() {
        let mut ring = Ring::new(MINIMUM_BUFFERS, FullRingPolicy::Drop).expect("three images");
        for frame_id in 1..=3_u64 {
            let index = ring.pick(0).expect("a free slot");
            ring.record_published(index, frame_id);
            ring.observe_held(held_pack(frame_id, u32::try_from(index).expect("a slot")));
        }
        assert_eq!(ring.pick(0), None);
        ring.reclaim();
        assert!(
            ring.pick(0).is_some(),
            "a runtime whose editor died keeps rendering at full rate"
        );
    }

    #[test]
    fn a_rebuilt_ring_is_a_new_generation() {
        let mut ring = Ring::new(PREFERRED_BUFFERS, FullRingPolicy::Drop).expect("four images");
        assert_eq!(ring.generation(), 1);
        ring.record_published(0, 5);
        ring.regenerate();
        assert_eq!(
            ring.generation(),
            2,
            "a resize cannot be sampled as the old one"
        );
        assert_eq!(ring.slot(0).expect("a slot").last_frame, 0);
    }
}
