// The carried impulses and the ledger, tested through the same calls the
// Director makes.
//
// This is the one seam in the mod worth an offline test: it is pure state with
// no game, no SkyrimNet and no LLM behind it, its arguments are integers and
// strings, and it can regress two already-shipped lenses with no visible
// symptom — a companion simply stops raising things, days later, for a reason
// nothing in the log connects to a slot that was evicted by another lens.
//
// The same is true of multi-carry: what a companion is carrying reaches exactly
// one prompt, through a decorator, so a regression there is invisible from
// everywhere except her own next line.
//
// Everything else about the Activity lens is verified in game (the prompts have
// no offline renderer, and whether a proposal reads well is judgement); see the
// issue for what was deliberately left there.

#include "PendingImpulse.h"

#include <cstdio>

namespace
{
    using AgencyEngine::PendingImpulses::Disposition;
    namespace Ledger = AgencyEngine::PendingImpulses;

    int g_checks = 0;
    int g_failures = 0;

    void Check(bool condition, const char* what, const char* file, int line)
    {
        g_checks += 1;
        if (!condition) {
            g_failures += 1;
            std::printf("  FAILED: %s\n    at %s:%d\n", what, file, line);
        }
    }

#define CHECK(cond) Check((cond), #cond, __FILE__, __LINE__)

    constexpr std::uint32_t kSerana = 0x0001A2B3;
    constexpr std::uint32_t kLydia = 0x000A2C8E;
    constexpr auto          kAspiration = "Aspiration";
    constexpr auto          kActivity = "Activity";

    // Each case starts from an empty ledger, a known global cap, and the lens
    // list the Director republishes on every pass. Reset is what the plugin calls
    // on a new game or a load, and the two publishing calls are what
    // Director::PumpPendingImpulses does — so none of this is a test-only door.
    //
    // Publishing matters to the outcome, not just the setup: a lens name only
    // names a ring while it is still configured, which is what keeps a renamed
    // row's slots working instead of stranding them.
    void Begin(const char* name, std::size_t globalCap = 6,
               std::vector<Ledger::LensRing> rings = { { "Aspiration", 0 }, { "Relationship", 0 },
                                                       { "Activity", 3 } })
    {
        std::printf("- %s\n", name);
        Ledger::Reset();
        Ledger::SetLedgerCap(globalCap);
        Ledger::SetLensRings(std::move(rings));
    }

    // A busy lens must not evict another lens's subjects.
    //
    // This is the case the whole per-lens ring exists for, and it fails against
    // the shared-ring code: an Activity lens with a ten-subject vocabulary
    // cycles fast, and every proposal it records used to push out the oldest
    // slot the character had — which is an Aspiration or Relationship subject
    // that was genuinely settled and is now raisable again.
    void CrossLensEvictionLeavesTheOtherLensAlone()
    {
        Begin("a busy lens does not evict another lens's subjects");

        const char* settled[] = { "her father",   "the coin split", "the Markarth road",
                                  "his sword",    "the moth priest", "the order of this" };
        for (const auto* topic : settled) {
            Ledger::LedgerRecord(kSerana, "Serana", topic, kAspiration, 6);
            Ledger::LedgerDecide(kSerana, topic, Disposition::Confirm);
        }

        // More proposals than the Activity ring holds, so it recycles its own
        // slots several times over.
        for (const auto* topic : { "a drink", "sparring", "a game by the fire", "a story", "something to fight" }) {
            Ledger::LedgerRecord(kSerana, "Serana", topic, kActivity, 3);
            Ledger::LedgerDecide(kSerana, topic, Disposition::Confirm);
        }

        for (const auto* topic : settled) {
            CHECK(Ledger::LedgerSuppresses(kSerana, topic, kAspiration));
        }
    }

    // A ring at its slot count recycles its own oldest and keeps the rest.
    void ALensRingRecyclesItsOldest()
    {
        Begin("a lens ring recycles its oldest subject and keeps the rest");

        for (const auto* topic : { "a drink", "sparring", "a game by the fire", "a story" }) {
            Ledger::LedgerRecord(kSerana, "Serana", topic, kActivity, 3);
            Ledger::LedgerDecide(kSerana, topic, Disposition::Confirm);
        }

        // Four proposals through three slots: the first is available again,
        // which is the recurrence the closed activity vocabulary needs.
        CHECK(!Ledger::LedgerSuppresses(kSerana, "a drink", kActivity));
        CHECK(Ledger::LedgerSuppresses(kSerana, "sparring", kActivity));
        CHECK(Ledger::LedgerSuppresses(kSerana, "a game by the fire", kActivity));
        CHECK(Ledger::LedgerSuppresses(kSerana, "a story", kActivity));
    }

    // 0 on a lens row means "whatever the global setting says", so that
    // changing one number does not mean editing every row.
    void ZeroSlotsFallsBackToTheGlobalCount()
    {
        // Aspiration is configured at 0, i.e. shared, against a global of 2.
        Begin("a per-lens slot count of 0 falls back to the global count", 2);

        for (const auto* topic : { "her father", "the coin split", "the Markarth road" }) {
            Ledger::LedgerRecord(kSerana, "Serana", topic, kAspiration, 0);
            Ledger::LedgerDecide(kSerana, topic, Disposition::Confirm);
        }

        CHECK(!Ledger::LedgerSuppresses(kSerana, "her father", kAspiration));
        CHECK(Ledger::LedgerSuppresses(kSerana, "the coin split", kAspiration));
        CHECK(Ledger::LedgerSuppresses(kSerana, "the Markarth road", kAspiration));
    }

    // The other half of it: a lens that *does* declare a count uses that count
    // rather than the global one, without the caller having to pass it. This is
    // the path Clear() takes — it knows the lens an entry came from and nothing
    // about the settings.
    void ADeclaredSlotCountBeatsTheGlobalOne()
    {
        // Activity declares 3 in Begin's lens list, against a global of 6.
        Begin("a lens's own slot count is used when the caller passes none", 6);

        for (const auto* topic : { "a drink", "sparring", "a game by the fire", "a story" }) {
            Ledger::LedgerRecord(kSerana, "Serana", topic, kActivity, 0);
            Ledger::LedgerDecide(kSerana, topic, Disposition::Confirm);
        }

        // Three, not six: the drink has already come round again.
        CHECK(!Ledger::LedgerSuppresses(kSerana, "a drink", kActivity));
        CHECK(Ledger::LedgerSuppresses(kSerana, "a story", kActivity));
    }

    // A lens renamed on the Settings page. Its name stops being a ring, so its
    // slots fall back to the shared one: they go on suppressing and go on being
    // evicted. The failure this guards against is that they suppress nothing and
    // nothing can ever evict them, which is a silent, permanent leak.
    void RenamingALensDoesNotStrandItsSlots()
    {
        Begin("renaming a lens leaves its slots working");

        Ledger::LedgerRecord(kSerana, "Serana", "a drink", kActivity, 0);
        Ledger::LedgerDecide(kSerana, "a drink", Disposition::Confirm);

        // The user renames the row from "Activity" to "Downtime".
        Ledger::SetLensRings({ { "Aspiration", 0 }, { "Relationship", 0 }, { "Downtime", 3 } });

        // Still spent, for the renamed lens and for every other one.
        CHECK(Ledger::LedgerSuppresses(kSerana, "a drink", "Downtime"));
        CHECK(Ledger::LedgerSuppresses(kSerana, "a drink", kAspiration));
    }

    // A slot is provisional until the entry that owns it dies. Confirmed it
    // stays; withdrawn the subject comes straight back, because withdrawal is
    // what "nobody ever answered it" means.
    void AProvisionalSlotIsDecidedByItsVerdict()
    {
        Begin("a provisional slot suppresses once confirmed and stops once withdrawn");

        Ledger::LedgerRecord(kSerana, "Serana", "a drink", kActivity, 3);
        CHECK(Ledger::LedgerSuppresses(kSerana, "a drink", kActivity));

        Ledger::LedgerDecide(kSerana, "a drink", Disposition::Confirm);
        CHECK(Ledger::LedgerSuppresses(kSerana, "a drink", kActivity));

        Ledger::LedgerRecord(kLydia, "Lydia", "sparring", kActivity, 3);
        Ledger::LedgerDecide(kLydia, "sparring", Disposition::Withdraw);
        CHECK(!Ledger::LedgerSuppresses(kLydia, "sparring", kActivity));

        // One character's ledger is not the other's.
        CHECK(!Ledger::LedgerSuppresses(kLydia, "a drink", kActivity));
    }

    // The same subject raised twice is one subject. Case and punctuation are
    // ignored, because the model writes the slug fresh each time.
    void ARepeatMovesToNewestRatherThanTakingASecondSlot()
    {
        Begin("a repeated subject moves to newest instead of taking a second slot");

        for (const auto* topic : { "a drink", "sparring", "A Drink." }) {
            Ledger::LedgerRecord(kSerana, "Serana", topic, kActivity, 2);
            Ledger::LedgerDecide(kSerana, topic, Disposition::Confirm);
        }

        // Three records through two slots, but only two subjects — so nothing
        // has been evicted yet.
        CHECK(Ledger::LedgerSuppresses(kSerana, "a drink", kActivity));
        CHECK(Ledger::LedgerSuppresses(kSerana, "sparring", kActivity));

        Ledger::LedgerRecord(kSerana, "Serana", "a game by the fire", kActivity, 2);
        Ledger::LedgerDecide(kSerana, "a game by the fire", Disposition::Confirm);

        // The drink was refreshed by the repeat, so sparring is the oldest and
        // the one that drops off.
        CHECK(!Ledger::LedgerSuppresses(kSerana, "sparring", kActivity));
        CHECK(Ledger::LedgerSuppresses(kSerana, "a drink", kActivity));
        CHECK(Ledger::LedgerSuppresses(kSerana, "a game by the fire", kActivity));
    }

    // Slots written by a version that had no lens key. They are not migrated
    // and not discarded: they keep the shared ring they were written under, so
    // they suppress whichever lens asks and are evicted by whatever comes next.
    void ALegacySlotKeepsSharedRingBehaviour()
    {
        Begin("legacy slots survive a small-ring lens and suppress every lens");

        // A full shared ring, as an upgrading save arrives with.
        const char* legacy[] = { "her father",   "the coin split",  "the Markarth road",
                                 "his sword",    "the moth priest", "the order of this" };
        for (const auto* topic : legacy) {
            Ledger::LedgerRecord(kSerana, "Serana", topic, "", 6);
            Ledger::LedgerDecide(kSerana, topic, Disposition::Confirm);
        }

        // The first proposals after the upgrade, from a lens holding three
        // slots. If the shared ring counted towards its ring, the very first
        // record would push four settled subjects out — losing on the tick
        // after an upgrade exactly what the per-lens ring exists to protect.
        for (const auto* topic : { "a drink", "sparring", "a game by the fire", "a story" }) {
            Ledger::LedgerRecord(kSerana, "Serana", topic, kActivity, 3);
            Ledger::LedgerDecide(kSerana, topic, Disposition::Confirm);
        }

        for (const auto* topic : legacy) {
            CHECK(Ledger::LedgerSuppresses(kSerana, topic, kActivity));
            CHECK(Ledger::LedgerSuppresses(kSerana, topic, kAspiration));
        }

        // Six legacy plus the activity lens's three, all rendered into the
        // prompt together: the "already raised" list is combined so that no lens
        // repeats another's subject.
        CHECK(Ledger::LedgerTopics(kSerana).size() == 9);
    }

    // ---- what she is carrying ---------------------------------------------

    Ledger::Entry Carried(std::uint32_t formID, const char* speaker, const char* lens, const char* topic,
                          const char* text, double gameDays = 10.0)
    {
        Ledger::Entry entry;
        entry.formID = formID;
        entry.speakerName = speaker;
        entry.targetName = "the Dragonborn";
        entry.lens = lens;
        entry.topic = topic;
        entry.text = text;
        entry.createdGameDays = gameDays;
        return entry;
    }

    bool Holds(const std::string& haystack, const char* needle)
    {
        return haystack.find(needle) != std::string::npos;
    }

    // The point of multi-carry: two lenses coming due together used to mean one
    // of them threw the other's question away, silently, for no reason anyone
    // could see from the game.
    void TwoLensesCoexistAndRenderNewestFirst()
    {
        Begin("two lenses' impulses coexist in one bio, newest first");

        Ledger::Set(Carried(kSerana, "Serana", kAspiration, "her father", "She has been meaning to say ..."));
        Ledger::Set(Carried(kSerana, "Serana", kActivity, "a drink", "She would like a drink with him."));

        const auto rendered = Ledger::Get(kSerana);
        CHECK(Holds(rendered, "She has been meaning to say ..."));
        CHECK(Holds(rendered, "She would like a drink with him."));
        // Newest first, so the drink — set second — leads.
        CHECK(rendered.find("She would like a drink") < rendered.find("She has been meaning"));
        // One line each, so a line break is unambiguously a separator between
        // two subjects rather than something inside one.
        CHECK(std::ranges::count(rendered, '\n') == 1);
        CHECK(Ledger::Count() == 2);
        // Somebody else's bio is untouched by either.
        CHECK(Ledger::Get(kLydia).empty());
    }

    // Within a lens the old rule stands: a second impulse supersedes, because a
    // lens returning to the same register twice is the nag this avoids.
    void ASecondFromTheSameLensSupersedes()
    {
        Begin("a second impulse from the same lens supersedes rather than stacks");

        Ledger::Set(Carried(kSerana, "Serana", kAspiration, "her father", "The first one."));
        Ledger::Set(Carried(kSerana, "Serana", kActivity, "a drink", "A drink."));
        Ledger::Set(Carried(kSerana, "Serana", kAspiration, "the moth priest", "The second one."));

        CHECK(Ledger::Count() == 2);
        const auto rendered = Ledger::Get(kSerana);
        CHECK(!Holds(rendered, "The first one."));
        CHECK(Holds(rendered, "The second one."));
        CHECK(Holds(rendered, "A drink."));
    }

    // Resolution is per impulse. One being met says nothing about the others,
    // and the ledger slot it confirms is its own.
    void ImpulsesResolveIndependently()
    {
        Begin("each carried impulse resolves on its own, and takes only its own slot");

        Ledger::Set(Carried(kSerana, "Serana", kAspiration, "her father", "The unsaid thing."));
        Ledger::Set(Carried(kSerana, "Serana", kActivity, "a drink", "The drink."));

        // The check comes due per entry, oldest check first, and marking one
        // does not mark the other.
        auto due = Ledger::NextDueForCheck(11.0, 60.0f);
        CHECK(due.has_value() && due->lens == kAspiration);
        Ledger::MarkChecked(kSerana, kAspiration, 11.0);
        due = Ledger::NextDueForCheck(11.0, 60.0f);
        CHECK(due.has_value() && due->lens == kActivity);

        // The aspiration is judged met; the drink is untouched, and so is the
        // ring the drink would have used.
        Ledger::Clear(kSerana, kAspiration, "resolved", Disposition::Confirm);
        CHECK(Ledger::Count() == 1);
        CHECK(Holds(Ledger::Get(kSerana), "The drink."));
        CHECK(Ledger::LedgerSuppresses(kSerana, "her father", kAspiration));
        CHECK(!Ledger::LedgerSuppresses(kSerana, "a drink", kActivity));
    }

    // Said and unsaid are separate blocks in the bio, and a companion can be in
    // both states at once — telling her she has not said a thing she just said
    // is how she comes to say it twice.
    void SaidAndUnsaidRenderSeparately()
    {
        Begin("what she has said and what she has not are rendered apart");

        Ledger::Set(Carried(kSerana, "Serana", kAspiration, "her father", "The unsaid thing."));
        auto spoken = Carried(kSerana, "Serana", kActivity, "a drink", "The one she asked for.");
        spoken.spoken = true;
        spoken.spokenGameDays = 10.0;
        Ledger::Set(spoken);

        CHECK(Ledger::Get(kSerana) == "- The unsaid thing.");
        CHECK(Ledger::GetSpoken(kSerana) == "- The one she asked for.");
        // The compatibility decorator prefers the unsaid one, which is the
        // wording an older prompt file renders by default.
        CHECK(Ledger::State(kSerana) == "carried");
    }

    // A slot is written the moment a subject is carried, so an entry that dies
    // without ever being answered has to hand its slot back. The failure this
    // guards against is silent and long-lived: the subject goes on being
    // suppressed with nothing left that could ever release it, and she simply
    // stops raising something she never got to raise.
    void AnUnansweredCarryReleasesItsSlot()
    {
        Begin("a carried subject that is superseded or forgotten releases its slot");

        // What Director::CarryImpulse does, in order: the entry, then its
        // provisional slot.
        Ledger::Set(Carried(kSerana, "Serana", kAspiration, "her father", "One."));
        Ledger::LedgerRecord(kSerana, "Serana", "her father", kAspiration, 6);
        CHECK(Ledger::LedgerSuppresses(kSerana, "her father", kAspiration));

        // The same lens comes back with a different subject. Nobody ever
        // answered the first, so it goes back into circulation.
        Ledger::Set(Carried(kSerana, "Serana", kAspiration, "the moth priest", "Two."));
        Ledger::LedgerRecord(kSerana, "Serana", "the moth priest", kAspiration, 6);
        CHECK(!Ledger::LedgerSuppresses(kSerana, "her father", kAspiration));
        CHECK(Ledger::LedgerSuppresses(kSerana, "the moth priest", kAspiration));

        // And the TTL is the same verdict by another route.
        Ledger::Clear(kSerana, kAspiration, "ttl");
        CHECK(!Ledger::LedgerSuppresses(kSerana, "the moth priest", kAspiration));

        // Only an explicit resolved verdict keeps one.
        Ledger::Set(Carried(kSerana, "Serana", kActivity, "a drink", "Three."));
        Ledger::LedgerRecord(kSerana, "Serana", "a drink", kActivity, 3);
        Ledger::Clear(kSerana, kActivity, "resolved", Disposition::Confirm);
        CHECK(Ledger::LedgerSuppresses(kSerana, "a drink", kActivity));
    }

    // Clearing by actor is for the two callers that mean the actor rather than
    // one of her subjects — the load-order check and the UI's Forget button.
    void ClearAllTakesEverythingSheIsCarrying()
    {
        Begin("clearing by actor drops every lens's impulse and nobody else's");

        Ledger::Set(Carried(kSerana, "Serana", kAspiration, "her father", "One."));
        Ledger::Set(Carried(kSerana, "Serana", kActivity, "a drink", "Two."));
        Ledger::Set(Carried(kLydia, "Lydia", kActivity, "sparring", "Three."));

        CHECK(Ledger::ClearAll(kSerana, "stale") == 2);
        CHECK(Ledger::Get(kSerana).empty());
        CHECK(Holds(Ledger::Get(kLydia), "Three."));
        // Withdrawn, not confirmed: nothing about a stale entry says the subject
        // was ever answered.
        CHECK(!Ledger::LedgerSuppresses(kSerana, "her father", kAspiration));
    }

    // The bio's two closings turn on this one string, and getting it wrong is
    // silent in both directions: stuck on, she interrupts every conversation
    // she is in; stuck off, the cue fires and her prompt still tells her not to
    // go looking, so the speaking turn is spent on nothing.
    //
    // The window's *expiry* is not covered here — it is 30 real seconds and
    // there is no clock to move — so what is tested is the grant, its scope,
    // and that a load does not carry a turn across into another save.
    void TheFloorIsGrantedToOneCompanionAtATime()
    {
        Begin("a speaking turn is granted to the companion it was cued for, and to nobody else");

        CHECK(Ledger::HasTheFloor(kSerana).empty());

        Ledger::GrantFloor(kSerana);
        CHECK(Ledger::HasTheFloor(kSerana) == "1");
        // Standing next to somebody who was given a turn is not being given one.
        CHECK(Ledger::HasTheFloor(kLydia).empty());

        Ledger::GrantFloor(kLydia);
        CHECK(Ledger::HasTheFloor(kSerana) == "1");
        CHECK(Ledger::HasTheFloor(kLydia) == "1");

        // A turn granted in the save we just left is not one she holds in this
        // one; Reset runs on kNewGame and kPostLoadGame.
        Ledger::Reset();
        CHECK(Ledger::HasTheFloor(kSerana).empty());
        CHECK(Ledger::HasTheFloor(kLydia).empty());
    }
}

int main()
{
    std::printf("AgencyEngine ledger tests\n");

    CrossLensEvictionLeavesTheOtherLensAlone();
    ALensRingRecyclesItsOldest();
    ZeroSlotsFallsBackToTheGlobalCount();
    ADeclaredSlotCountBeatsTheGlobalOne();
    RenamingALensDoesNotStrandItsSlots();
    AProvisionalSlotIsDecidedByItsVerdict();
    ARepeatMovesToNewestRatherThanTakingASecondSlot();
    ALegacySlotKeepsSharedRingBehaviour();

    TwoLensesCoexistAndRenderNewestFirst();
    ASecondFromTheSameLensSupersedes();
    ImpulsesResolveIndependently();
    SaidAndUnsaidRenderSeparately();
    AnUnansweredCarryReleasesItsSlot();
    ClearAllTakesEverythingSheIsCarrying();
    TheFloorIsGrantedToOneCompanionAtATime();

    std::printf("%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
