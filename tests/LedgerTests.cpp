#include "PendingImpulse.h"
#include "ResolutionScheduler.h"
#include "SkyrimNetEvent.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>

namespace
{
    namespace Pending = AgencyEngine::PendingImpulses;
    namespace Scheduling = AgencyEngine::ResolutionScheduling;

    int g_checks = 0;
    int g_failures = 0;

    void Check(bool condition, const char* what, const char* file, int line)
    {
        ++g_checks;
        if (!condition) {
            ++g_failures;
            std::printf("  FAILED: %s\n    at %s:%d\n", what, file, line);
        }
    }

#define CHECK(condition) Check((condition), #condition, __FILE__, __LINE__)

    constexpr std::uint32_t kSerana = 0x0001A2B3;
    constexpr std::uint32_t kLydia = 0x000A2C8E;

    Pending::Entry Carried(std::uint32_t formID, std::string speaker, std::string lens, std::string topic,
                           double gameDays = 10.0, std::uint64_t target = 1)
    {
        Pending::Entry entry;
        entry.formID = formID;
        entry.speakerName = std::move(speaker);
        entry.target = { target, "Dragonborn" };
        entry.text = "Raise " + topic;
        entry.topic = std::move(topic);
        entry.lens = std::move(lens);
        entry.createdGameDays = gameDays;
        return entry;
    }

    void Begin(const char* name, std::size_t cap = 6)
    {
        std::printf("- %s\n", name);
        Pending::Reset();
        Pending::SetLedgerCap(cap);
        Pending::SetLensRings({ { "Aspiration", 0 }, { "Relationship", 0 }, { "Activity", 3 } });
        Pending::SetPartyEchoGameDays(7.0f);
    }

    void StableIdsRejectStaleCommands()
    {
        Begin("stable IDs reject stale commands against a replacement");
        const auto oldId = Pending::Carry(Carried(kSerana, "Serana", "Activity", "a drink"), 3);
        const auto newId = Pending::Carry(Carried(kSerana, "Serana", "Activity", "sparring"), 3);
        CHECK(oldId != 0 && newId > oldId);
        CHECK(!Pending::MarkRaised(oldId, 11.0));
        CHECK(!Pending::MarkMet(oldId, 11.0));
        CHECK(!Pending::StopCarrying(oldId));
        CHECK(!Pending::ForgetSubject(oldId));
        const auto current = Pending::Find(newId);
        CHECK(current && current->topic == "sparring");
        CHECK(current && current->state == Pending::LifecycleState::Untouched);
    }

    void LifecycleAndMemoryTransitionAtomically()
    {
        Begin("raise and meet update lifecycle and memory atomically");
        const auto id = Pending::Carry(Carried(kSerana, "Serana", "Activity", "a drink"), 3);
        auto personal = Pending::LedgerSnapshot();
        CHECK(personal.size() == 1 && personal[0].originEntryId == id && personal[0].provisional);
        CHECK(Pending::PartySnapshot().empty());
        CHECK(Pending::MarkRaised(id, 10.5));
        const auto raised = Pending::Find(id);
        CHECK(raised && raised->state == Pending::LifecycleState::RaisedUnmet);
        personal = Pending::LedgerSnapshot();
        CHECK(personal.size() == 1 && !personal[0].provisional);
        const auto party = Pending::PartySnapshot();
        CHECK(party.size() == 1 && party[0].originEntryId == id && party[0].raisedGameDays == 10.5);
        CHECK(Pending::MarkRaised(id, 10.6));
        CHECK(Pending::PartySnapshot().size() == 1);
        CHECK(Pending::MarkMet(id, 11.0));
        CHECK(!Pending::Find(id));
        CHECK(Pending::LedgerSuppresses(kSerana, "A DRINK!"));
        CHECK(Pending::PartySuppresses("a drink", { 1, "Dragonborn" }));
        CHECK(!Pending::MarkRaised(id, 12.0));
    }

    void StateAwareExpiryPreservesOnlyConfirmedMemory()
    {
        Begin("expiry withdraws untouched memory and retains raised memory");
        const auto untouched = Pending::Carry(Carried(kSerana, "Serana", "Aspiration", "her father", 10.0));
        const auto raised = Pending::Carry(Carried(kLydia, "Lydia", "Activity", "sparring", 10.0), 3);
        CHECK(Pending::MarkRaised(raised, 10.1));
        Pending::Expire(9.0, 60.0f);
        CHECK(Pending::Find(untouched) && Pending::Find(raised));
        Pending::Expire(11.0, 60.0f);
        CHECK(!Pending::Find(untouched));
        CHECK(!Pending::LedgerSuppresses(kSerana, "her father"));
        CHECK(!Pending::Find(raised));
        CHECK(Pending::LedgerSuppresses(kLydia, "sparring"));
        CHECK(Pending::PartySuppresses("sparring", { 1, "Dragonborn" }));
    }

    void UnicodeNormalizationIsStrictAndDeterministic()
    {
        Begin("Unicode topic keys preserve letters and fold equivalent forms");
        const auto latin = Pending::NormalizeTopic("  CAFÉ—déjà  vu! ");
        const auto decomposed = Pending::NormalizeTopic("cafe\xCC\x81 déjà vu");
        CHECK(latin && decomposed && *latin == *decomposed);
        CHECK(Pending::NormalizeTopic("Ａ ＢＣ") == Pending::NormalizeTopic("a bc"));
        CHECK(Pending::NormalizeTopic("доля монет").value_or("") == "доля монет");
        CHECK(!Pending::NormalizeTopic("父親の剣").value_or("").empty());
        CHECK(Pending::NormalizeTopic("coin---split") == Pending::NormalizeTopic("coin split"));
        CHECK(!Pending::NormalizeTopic("!!!"));
        const std::string invalid{ static_cast<char>(0xC3), static_cast<char>(0x28) };
        CHECK(!Pending::NormalizeTopic(invalid));
    }

    void PersonalSuppressionSpansLensesWithoutCrossRingEviction()
    {
        Begin("personal veto spans lenses while eviction stays local");
        const auto confirmed = Pending::Carry(Carried(kSerana, "Serana", "Aspiration", "a bed"), 6);
        CHECK(Pending::MarkRaised(confirmed, 10.1));
        CHECK(Pending::LedgerSuppresses(kSerana, "A BED."));
        const auto duplicate = Pending::Carry(Carried(kSerana, "Serana", "Activity", "a bed"), 3);
        const auto slots = Pending::LedgerSnapshot();
        CHECK(std::ranges::count_if(slots, [&](const Pending::LedgerSlot& slot) {
                  return slot.formID == kSerana && slot.normalizedTopic == "a bed";
              }) == 1);
        CHECK(std::ranges::any_of(slots, [&](const Pending::LedgerSlot& slot) {
            return slot.originEntryId == confirmed && !slot.provisional;
        }));
        CHECK(Pending::StopCarrying(duplicate));
        CHECK(Pending::LedgerSuppresses(kSerana, "a bed"));
        for (const auto* topic : { "a drink", "sparring", "a story", "a game" }) {
            const auto id = Pending::Carry(Carried(kSerana, "Serana", "Activity", topic), 3);
            Pending::MarkRaised(id, 10.2);
            Pending::StopCarrying(id);
        }
        CHECK(Pending::LedgerSuppresses(kSerana, "a bed"));
    }

    void PartyMemoryScopesEchoesAndRetiresUntouchedCarries()
    {
        Begin("party memory retires compatible untouched echoes only");
        const auto lydia = Pending::Carry(Carried(kLydia, "Lydia", "Activity", "a drink", 10.0, 1), 3);
        const auto seranaEcho = Pending::Carry(Carried(kSerana, "Serana", "Aspiration", "A Drink!", 10.0, 1), 6);
        const auto seranaOtherTarget = Pending::Carry(
            Carried(kSerana, "Serana", "Relationship", "a drink", 10.0, 2), 6);
        CHECK(Pending::MarkRaised(lydia, 10.2));
        CHECK(!Pending::Find(seranaEcho));
        CHECK(Pending::Find(seranaOtherTarget).has_value());
        CHECK(!Pending::PartySuppresses("a drink", { 2, "Other" }));
        CHECK(Pending::PartySuppresses("a drink", { 1, "Dragonborn" }));
        const auto raisedEcho = Pending::Carry(Carried(kSerana, "Serana", "Activity", "a drink", 10.3, 1), 3);
        CHECK(Pending::MarkRaised(raisedEcho, 10.4));
        CHECK(Pending::Find(raisedEcho).has_value());
    }

    void PartyRetentionIsBoundedAndExpires()
    {
        Begin("party memory retains 32 records for seven game days");
        for (std::uint32_t index = 0; index < 33; ++index) {
            auto entry = Carried(0x1000 + index, "Follower", "Activity", "topic " + std::to_string(index),
                                 10.0 + index / 100.0, index + 1);
            const auto id = Pending::Carry(std::move(entry), 3);
            Pending::MarkRaised(id, 10.0 + index / 100.0);
            Pending::StopCarrying(id);
        }
        CHECK(Pending::PartySnapshot().size() == Pending::kPartyLedgerCap);
        CHECK(Pending::PartyPromptSnapshot().size() == Pending::kPartyPromptCap);
        CHECK(!Pending::PartySuppresses("topic 0", { 1, "target" }));
        CHECK(Pending::PartySuppresses("topic 32", { 33, "target" }));
        Pending::Expire(18.0, 0.0f);
        CHECK(Pending::PartySnapshot().empty());
    }

    void ForgetActionsRespectOriginOwnership()
    {
        Begin("stop and forget distinguish open state from owned memory");
        const auto first = Pending::Carry(Carried(kLydia, "Lydia", "Activity", "a drink", 10.0, 1), 3);
        CHECK(Pending::MarkRaised(first, 10.1));
        CHECK(Pending::StopCarrying(first));
        CHECK(Pending::LedgerSuppresses(kLydia, "a drink"));
        const auto second = Pending::Carry(Carried(kSerana, "Serana", "Activity", "a drink", 10.2, 2), 3);
        CHECK(Pending::MarkRaised(second, 10.3));
        CHECK(Pending::ForgetSubject(first));
        CHECK(!Pending::LedgerSuppresses(kLydia, "a drink"));
        CHECK(Pending::LedgerSuppresses(kSerana, "a drink"));
        CHECK(Pending::PartySuppresses("a drink", { 2, "Other" }));
        CHECK(Pending::ForgetAll() == 1);
        CHECK(Pending::Snapshot().empty());
        CHECK(Pending::LedgerSnapshot().empty());
        CHECK(Pending::PartySnapshot().empty());
    }

    void CoalescedCueUsesNewestPreEmissionOwner()
    {
        Begin("coalesced cues use newest ownership before emission");
        const auto aspiration = Pending::Carry(
            Carried(kSerana, "Serana", "Aspiration", "her father"), 6);
        const auto activity = Pending::Carry(
            Carried(kSerana, "Serana", "Activity", "a drink"), 3);
        Pending::CueOwnership cue;
        cue.Coalesce(aspiration, { 1, "Dragonborn" });
        cue.Coalesce(activity, { 2, "Kaidan" });
        CHECK(cue.carries == 2);
        CHECK(cue.entryId == activity);
        CHECK(cue.target.id == 2);
        CHECK(!Pending::FloorOwner(kSerana));  // failed dispatch grants nothing
        Pending::GrantFloor(cue.entryId, kSerana, cue.target);
        const auto owner = Pending::FloorOwner(kSerana);
        CHECK(owner && owner->entryId == activity && owner->target.id == 2);
        CHECK(Pending::Get(kSerana) == "- Raise a drink");
        CHECK(Pending::GetBackground(kSerana) == "- Raise her father");
        CHECK(!Pending::FloorOwner(kSerana, owner->deadline + std::chrono::milliseconds{ 1 }));
    }

    void FloorOwnershipIsStableUntilClosure()
    {
        Begin("floor ownership is entry-specific and immutable");
        const auto first = Pending::Carry(Carried(kSerana, "Serana", "Aspiration", "her father"), 6);
        Pending::GrantFloor(first, kSerana, { 1, "Dragonborn" });
        const auto replacement = Pending::Carry(Carried(kSerana, "Serana", "Aspiration", "the moth priest"), 6);
        const auto owner = Pending::FloorOwner(kSerana);
        CHECK(owner && owner->entryId == first);
        Pending::GrantFloor(replacement, kSerana, { 2, "Kaidan" });
        const auto stillFirst = Pending::FloorOwner(kSerana);
        CHECK(stillFirst && stillFirst->entryId == first && stillFirst->target.id == 1);
        CHECK(!Pending::MarkFloorOwnerRaised(kSerana, 10.2));
        CHECK(Pending::Find(replacement)->state == Pending::LifecycleState::Untouched);
        Pending::CloseFloor(kSerana);
        CHECK(!Pending::FloorOwner(kSerana));
        Pending::GrantFloor(replacement, kSerana, { 1, "Dragonborn" }, 9001);
        const auto active = Pending::FloorOwner(kSerana);
        const auto grantedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   active->grantedAt.time_since_epoch())
                                   .count();
        CHECK(!Pending::MarkFloorOwnerRaisedByIdentity(9002, grantedMs, 10.3));
        CHECK(!Pending::MarkFloorOwnerRaisedByIdentity(9001, grantedMs - 1, 10.3));
        CHECK(Pending::Find(replacement)->state == Pending::LifecycleState::Untouched);
        CHECK(Pending::MarkFloorOwnerRaisedByIdentity(9001, grantedMs, 10.3).has_value());
        CHECK(!Pending::FloorOwner(kSerana));
        CHECK(!Pending::MarkFloorOwnerRaisedByIdentity(9001, grantedMs + 1, 10.3).has_value());
        CHECK(Pending::Find(replacement)->state == Pending::LifecycleState::RaisedUnmet);
        Scheduling::Scheduler resolver;
        resolver.BeginSave("floor-speech");
        auto raised = *Pending::Find(replacement);
        raised.speakerId = 9001;
        std::uint64_t sequence = 0;
        resolver.Enqueue({ "owner-line", 9001, 1, "dialogue", "the moth priest", grantedMs,
                           replacement });
        CHECK(!resolver.TryDispatch(std::span{ &raised, 1 }, 2, 240000, 1000000,
                                    [&] { return ++sequence; }));
        CHECK(resolver.Snapshot().paidBatches == 0);
        resolver.Enqueue({ "target-reply", 1, 9001, "dialogue_player", "I heard you about the moth priest",
                           grantedMs + 1 });
        const auto responseBatch = resolver.TryDispatch(std::span{ &raised, 1 }, 2, 240000, 1000000,
                                                        [&] { return ++sequence; });
        CHECK(responseBatch && responseBatch->entries.size() == 1);
        Pending::Reset();
        CHECK(!Pending::FloorOwner(kSerana));
    }

    void LegacySidecarMigratesAndReloads()
    {
        Begin("legacy sidecar migrates once and preserves live state");
        const auto path = Pending::FilePath();
        auto backup = path;
        backup += ".bak";
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        std::filesystem::remove(path, error);
        std::filesystem::remove(backup, error);
        nlohmann::json legacy = {
            { "seq", 4 },
            { "saves", { { "legacy", {
                { "seq", 4 },
                { "impulses", nlohmann::json::array({ {
                    { "formID", kSerana }, { "speakerName", "Serana" }, { "targetName", "Dragonborn" },
                    { "text", "Raise her father" }, { "topic", "her father" }, { "lens", "Aspiration" },
                    { "createdGameDays", 9.0 }, { "spoken", false }
                } }) },
                { "ledger", nlohmann::json::array({ {
                    { "formID", kSerana }, { "speakerName", "Serana" }, { "topic", "her father" },
                    { "lens", "Aspiration" }, { "provisional", false }
                } }) }
            } } } }
        };
        { std::ofstream file{ path }; file << legacy.dump(2); }
        Pending::SyncPersistence("legacy", 10.0, 10000.0f);
        Pending::TakeUnverified();
        const auto migrated = Pending::Snapshot();
        CHECK(migrated.size() == 1 && migrated[0].id != 0);
        CHECK(migrated[0].state == Pending::LifecycleState::Untouched);
        CHECK(Pending::LedgerSuppresses(kSerana, "her father"));
        CHECK(Pending::PartySnapshot().empty());
        CHECK(std::filesystem::exists(backup));
        nlohmann::json rewritten;
        { std::ifstream file{ path }; file >> rewritten; }
        CHECK(rewritten.value("formatVersion", 0u) == 2);
        CHECK(rewritten["saves"]["legacy"].contains("nextEntryId"));
        CHECK(rewritten["saves"]["legacy"].contains("nextEvidenceSequence"));
        const auto stableId = migrated[0].id;
        Pending::Reset();
        Pending::SyncPersistence("legacy", 10.0, 10000.0f);
        const auto reloaded = Pending::Snapshot();
        CHECK(reloaded.size() == 1 && reloaded[0].id == stableId);
        CHECK(Pending::LedgerSuppresses(kSerana, "her father"));
    }

    void PartyMemoryPersistsAcrossReload()
    {
        Begin("party-heard records persist across reload");
        const auto path = Pending::FilePath();
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        std::filesystem::remove(path, error);
        Pending::SyncPersistence("party", 10.0, 10000.0f);
        const auto id = Pending::Carry(
            Carried(kLydia, "Lydia", "Activity", "a drink", 10.0, 1), 3);
        CHECK(Pending::MarkRaised(id, 10.1));
        Pending::FlushPersistence();
        Pending::Reset();
        Pending::SyncPersistence("party", 10.2, 10000.0f);
        const auto party = Pending::PartySnapshot();
        CHECK(party.size() == 1);
        CHECK(party[0].originEntryId == id);
        CHECK(party[0].speakerFormID == kLydia);
        CHECK(party[0].target.id == 1);
        CHECK(party[0].topic == "a drink");
        CHECK(party[0].normalizedTopic == "a drink");
    }

    void EvidenceSequenceContinuesAbovePersistedWatermark()
    {
        Begin("post-reload evidence sequences exceed persisted watermarks");
        const auto path = Pending::FilePath();
        nlohmann::json root = {
            { "formatVersion", 2 },
            { "saves", { { "watermark", {
                { "version", 2 }, { "nextEntryId", 105 }, { "nextEvidenceSequence", 221 },
                { "entries", nlohmann::json::array({ {
                    { "id", 104 }, { "formID", kSerana }, { "speakerName", "Serana" },
                    { "target", { { "id", 1 }, { "name", "Dragonborn" } } },
                    { "text", "Raise a drink" }, { "topic", "a drink" },
                    { "normalizedTopic", "a drink" }, { "lens", "Activity" },
                    { "state", "raised_unmet" }, { "createdGameDays", 9.0 },
                    { "raisedGameDays", 9.1 }, { "lastAttemptedEvidenceSequence", 220 }
                } }) },
                { "personalLedger", nlohmann::json::array() }, { "partyLedger", nlohmann::json::array() }
            } } } }
        };
        { std::ofstream file{ path }; file << root.dump(2); }
        Pending::SyncPersistence("watermark", 10.0, 10000.0f);
        CHECK(Pending::NextEvidenceSequence() == 221);
        CHECK(Pending::SetLastAttemptedEvidenceSequence(104, 222));
        Pending::FlushPersistence();
        Pending::Reset();
        Pending::SyncPersistence("watermark", 10.0, 10000.0f);
        CHECK(Pending::NextEvidenceSequence() > 222);
    }
    Pending::EntryId RecordConfirmed(std::uint32_t formID, const char* speaker, const char* lens,
                                     const char* topic, std::size_t cap)
    {
        const auto id = Pending::Carry(Carried(formID, speaker, lens, topic), cap);
        Pending::MarkRaised(id, 10.1);
        Pending::StopCarrying(id);
        return id;
    }

    void ExistingPerLensRingContractsRemainCovered()
    {
        Begin("existing per-lens insertion and eviction contracts remain intact");
        for (const auto* topic : { "her father", "the coin split", "the Markarth road",
                                  "his sword", "the moth priest", "the order of this" }) {
            RecordConfirmed(kSerana, "Serana", "Aspiration", topic, 6);
        }
        for (const auto* topic : { "a drink", "sparring", "a game by the fire", "a story" }) {
            RecordConfirmed(kSerana, "Serana", "Activity", topic, 3);
        }
        CHECK(Pending::LedgerSuppresses(kSerana, "her father"));
        CHECK(!Pending::LedgerSuppresses(kSerana, "a drink"));
        CHECK(Pending::LedgerSuppresses(kSerana, "sparring"));
        CHECK(Pending::LedgerSuppresses(kSerana, "a game by the fire"));
        CHECK(Pending::LedgerSuppresses(kSerana, "a story"));

        Begin("zero per-lens slots fall back to the global count", 2);
        for (const auto* topic : { "her father", "the coin split", "the Markarth road" }) {
            RecordConfirmed(kSerana, "Serana", "Aspiration", topic, 0);
        }
        CHECK(!Pending::LedgerSuppresses(kSerana, "her father"));
        CHECK(Pending::LedgerSuppresses(kSerana, "the coin split"));
        CHECK(Pending::LedgerSuppresses(kSerana, "the Markarth road"));

        Begin("declared per-lens slots beat the global count", 6);
        for (const auto* topic : { "a drink", "sparring", "a game by the fire", "a story" }) {
            RecordConfirmed(kSerana, "Serana", "Activity", topic, 0);
        }
        CHECK(!Pending::LedgerSuppresses(kSerana, "a drink"));
        CHECK(Pending::LedgerSuppresses(kSerana, "a story"));
    }

    void ExistingCarryAndRenderContractsRemainCovered()
    {
        Begin("multiple lenses coexist and render newest first");
        const auto aspiration = Pending::Carry(
            Carried(kSerana, "Serana", "Aspiration", "her father"));
        const auto activity = Pending::Carry(
            Carried(kSerana, "Serana", "Activity", "a drink"), 3);
        CHECK(Pending::Count() == 2);
        const auto rendered = Pending::GetBackground(kSerana);
        CHECK(rendered.find("a drink") < rendered.find("her father"));
        CHECK(std::ranges::count(rendered, '\n') == 1);
        CHECK(Pending::GetBackground(kLydia).empty());

        const auto replacement = Pending::Carry(
            Carried(kSerana, "Serana", "Aspiration", "the moth priest"));
        CHECK(Pending::Count() == 2);
        CHECK(!Pending::Find(aspiration));
        CHECK(Pending::Find(activity).has_value());
        CHECK(Pending::Find(replacement).has_value());
        CHECK(Pending::GetBackground(kSerana).find("her father") == std::string::npos);

        CHECK(Pending::MarkRaised(activity, 10.2));
        CHECK(Pending::GetSpoken(kSerana).find("a drink") != std::string::npos);
        CHECK(Pending::GetBackground(kSerana).find("the moth priest") != std::string::npos);
        CHECK(Pending::State(kSerana) == "carried");
        CHECK(Pending::MarkMet(activity, 10.3));
        CHECK(Pending::Find(replacement).has_value());
    }

    void ExistingProvisionalAndActorClearContractsRemainCovered()
    {
        Begin("provisional memory withdraws and confirmed memory remains");
        const auto provisional = Pending::Carry(
            Carried(kSerana, "Serana", "Activity", "a drink"), 3);
        CHECK(Pending::LedgerSuppresses(kSerana, "a drink"));
        CHECK(Pending::StopCarrying(provisional));
        CHECK(!Pending::LedgerSuppresses(kSerana, "a drink"));

        const auto confirmed = Pending::Carry(
            Carried(kSerana, "Serana", "Activity", "sparring"), 3);
        CHECK(Pending::MarkRaised(confirmed, 10.2));
        CHECK(Pending::StopCarrying(confirmed));
        CHECK(Pending::LedgerSuppresses(kSerana, "sparring"));

        const auto seranaA = Pending::Carry(
            Carried(kSerana, "Serana", "Aspiration", "her father"));
        const auto seranaB = Pending::Carry(
            Carried(kSerana, "Serana", "Relationship", "the coin split"));
        const auto lydia = Pending::Carry(
            Carried(kLydia, "Lydia", "Activity", "a story"), 3);
        CHECK(Pending::StopCarryingActor(kSerana, "stale actor") == 2);
        CHECK(!Pending::Find(seranaA) && !Pending::Find(seranaB));
        CHECK(Pending::Find(lydia).has_value());
    }

    void ConfirmedDuplicatesNeverMoveOrDowngrade()
    {
        Begin("confirmed duplicates remain in their original lens ring");
        const auto original = RecordConfirmed(kSerana, "Serana", "Aspiration", "a drink", 6);
        const auto duplicate = Pending::Carry(
            Carried(kSerana, "Serana", "Activity", "A Drink."), 3);
        const auto slots = Pending::LedgerSnapshot();
        CHECK(std::ranges::count_if(slots, [](const Pending::LedgerSlot& slot) {
                  return slot.normalizedTopic == "a drink";
              }) == 1);
        CHECK(std::ranges::any_of(slots, [&](const Pending::LedgerSlot& slot) {
            return slot.originEntryId == original && slot.lens == "Aspiration" && !slot.provisional;
        }));
        CHECK(Pending::StopCarrying(duplicate));
        CHECK(Pending::LedgerSuppresses(kSerana, "a drink"));
    }

    void RenamedAndLegacyRingsRemainUsable()
    {
        Begin("renaming a lens does not strand its confirmed slots");
        RecordConfirmed(kSerana, "Serana", "Activity", "a drink", 3);
        Pending::SetLensRings({ { "Aspiration", 0 }, { "Relationship", 0 }, { "Downtime", 3 } });
        CHECK(Pending::LedgerSuppresses(kSerana, "a drink"));
        RecordConfirmed(kSerana, "Serana", "Downtime", "sparring", 3);
        CHECK(Pending::LedgerSuppresses(kSerana, "a drink"));
        CHECK(Pending::LedgerSuppresses(kSerana, "sparring"));
    }

    void SchedulerBoundsAndAssociatesEvidence()
    {
        Begin("resolution evidence is bounded, sequenced, deduplicated, and entry-specific");
        Scheduling::Scheduler scheduler{ 2 };
        scheduler.BeginSave("A");
        auto entry = Carried(kSerana, "Serana", "Activity", "dragon hunt");
        entry.id = 41;
        entry.speakerId = 500;
        std::uint64_t sequence = 10;
        scheduler.Enqueue({ "old", 500, 1, "dialogue", "old dragon story", 1 });
        scheduler.Enqueue({ "kept", 500, 1, "dialogue", "the dragon is dead", 2 });
        scheduler.Enqueue({ "new", 500, 1, "dialogue", "another dragon", 3 });
        const auto batch = scheduler.TryDispatch(std::span{ &entry, 1 }, 1, 240000, 1000000, [&] { return ++sequence; });
        CHECK(batch && batch->trigger == Scheduling::Trigger::EventCheckpoint);
        CHECK(batch->entries.size() == 1 && batch->entries[0].entry.id == 41);
        CHECK(batch->events.size() == 1 && batch->events[0].sourceId == "new");
        CHECK(batch->events[0].sequence > 10);
        CHECK(scheduler.Snapshot().queueOverflow == 1);
    }

    void SchedulerSaveTokensRejectABAResults()
    {
        Begin("save generations reject stale callbacks across A-B-A");
        Scheduling::Scheduler scheduler;
        scheduler.BeginSave("A");
        auto entry = Carried(kSerana, "Serana", "Activity", "dragon");
        entry.id = 51;
        entry.speakerId = 500;
        std::uint64_t sequence = 0;
        const Pending::EntryId id = entry.id;
        scheduler.QueueManual(std::span{ &id, 1 });
        const auto batch = scheduler.TryDispatch(std::span{ &entry, 1 }, 30, 240000, 1000000, [&] { return ++sequence; });
        CHECK(batch.has_value());
        scheduler.BeginSave("B");
        scheduler.QueueManual(std::span{ &id, 1 });
        const auto batchB = scheduler.TryDispatch(std::span{ &entry, 1 }, 30, 240000, 1000000, [&] { return ++sequence; });
        CHECK(batchB.has_value());
        scheduler.SubmitResult(batch->token, R"({"verdicts":[]})", true);
        CHECK(!scheduler.TakeResult());
        CHECK(scheduler.InFlight() && scheduler.InFlight()->token == batchB->token);
        scheduler.SubmitResult(batchB->token, R"({"verdicts":[]})", true);
        CHECK(scheduler.TakeResult().has_value());
        const auto returnedA = scheduler.BeginSave("A");
        CHECK(returnedA.generation != batch->token.save.generation);
        scheduler.SubmitResult(batch->token, R"({"verdicts":[]})", true);
        CHECK(!scheduler.TakeResult());
        CHECK(!scheduler.InFlight());
        CHECK(scheduler.Snapshot().staleResults == 2);
    }

    void SchedulerTracksIndependentEntryCheckpoints()
    {
        Begin("entries become due from their own persisted event watermarks");
        Scheduling::Scheduler scheduler;
        scheduler.BeginSave("A");
        auto serana = Carried(kSerana, "Serana", "Activity", "dragon hunt");
        serana.id = 61;
        serana.speakerId = 500;
        auto lydia = Carried(kLydia, "Lydia", "Activity", "dragon hunt");
        lydia.id = 62;
        lydia.speakerId = 600;
        lydia.lastAttemptedEvidenceSequence = 1;
        const Pending::Entry entries[]{ serana, lydia };
        std::uint64_t sequence = 0;
        scheduler.Enqueue({ "serana-line", 500, 1, "dialogue", "we finished the dragon hunt", 1 });
        const auto batch = scheduler.TryDispatch(entries, 1, 240000, 1000000, [&] { return ++sequence; });
        CHECK(batch && batch->entries.size() == 1);
        CHECK(batch->entries[0].entry.id == serana.id);
        CHECK(batch->entries[0].relevantEventIds.size() == 1);
        scheduler.SubmitResult(batch->token, "failed", false);
        CHECK(scheduler.TakeResult().has_value());

        auto attempted = serana;
        attempted.lastAttemptedEvidenceSequence = batch->upperSequence;
        const Pending::Entry later[]{ attempted, lydia };
        scheduler.Enqueue({ "lydia-line", 600, 1, "dialogue", "the weather is changing", 2 });
        const auto next = scheduler.TryDispatch(later, 1, 240000, 1240000, [&] { return ++sequence; });
        CHECK(next && next->entries.size() == 2);
    }

    void SchedulerTriggersBatchOnceAndRetainsInflightArrivals()
    {
        Begin("manual, checkpoint, and in-flight rules produce one paid batch");
        Scheduling::Scheduler scheduler;
        scheduler.BeginSave("A");
        auto entry = Carried(kSerana, "Serana", "Activity", "dragon");
        entry.id = 71;
        entry.speakerId = 500;
        std::uint64_t sequence = 0;
        const Pending::EntryId id = entry.id;
        scheduler.QueueManual(std::span{ &id, 1 });
        const auto first = scheduler.TryDispatch(std::span{ &entry, 1 }, 30, 240000, 1000000,
                                                 [&] { return ++sequence; });
        CHECK(first && first->trigger == Scheduling::Trigger::Manual);
        const auto active = scheduler.Snapshot();
        CHECK(active.batchInFlight && active.lastTrigger == Scheduling::Trigger::Manual &&
              active.eligibleEntries == 1 && active.paidBatches == 1);
        scheduler.QueueManual(std::span{ &id, 1 });
        scheduler.Enqueue({ "during", 500, 1, "dialogue", "dragon again", 2 });
        CHECK(!scheduler.TryDispatch(std::span{ &entry, 1 }, 1, 240000, 1000000,
                                     [&] { return ++sequence; }));
        CHECK(scheduler.InFlight() && scheduler.InFlight()->token == first->token);
        CHECK(scheduler.Snapshot().queuedRaw == 0);
        scheduler.SubmitResult(first->token, R"({"verdicts":[]})", true);
        CHECK(scheduler.TakeResult().has_value());
        const auto second = scheduler.TryDispatch(std::span{ &entry, 1 }, 1, 240000, 1000000,
                                                  [&] { return ++sequence; });
        CHECK(second && second->trigger == Scheduling::Trigger::EventCheckpoint);
        scheduler.CancelInFlight();
        CHECK(!scheduler.InFlight());
        CHECK(scheduler.Snapshot().paidBatches == 2);
    }

    void SchedulerFallbacksCoalesceAndReleaseAfterFailure()
    {
        Begin("due proposal fallbacks coalesce once and release expiry after failure");
        Scheduling::Scheduler scheduler;
        scheduler.BeginSave("A");
        std::vector<Pending::Entry> entries;
        for (Pending::EntryId id = 91; id <= 93; ++id) {
            auto entry = Carried(kSerana + static_cast<std::uint32_t>(id), "Follower", "Activity", "a drink");
            entry.id = id;
            entry.speakerId = 500 + id;

            entry.state = Pending::LifecycleState::RaisedUnmet;
            entry.proposal = true;
            entries.push_back(std::move(entry));
        }
        const Pending::EntryId ids[]{ 91, 92, 93 };
        scheduler.QueueFallback(ids);
        std::uint64_t sequence = 0;
        const auto batch = scheduler.TryDispatch(entries, 30, 240000, 1000000, [&] { return ++sequence; });
        CHECK(batch && batch->trigger == Scheduling::Trigger::PreExpiry && batch->entries.size() == 3);
        scheduler.SubmitResult(batch->token, "request failed", false);
        const auto failed = scheduler.TakeResult();
        CHECK(failed && !failed->success);
        scheduler.QueueFallback(ids);
        CHECK(!scheduler.TryDispatch(entries, 30, 240000, 1000000, [&] { return ++sequence; }));
        CHECK(scheduler.QueuedFallbacks().size() == 3);

        Pending::Reset();
        auto proposal = Carried(kSerana, "Serana", "Activity", "a drink", 10.0);
        proposal.proposal = true;
        const auto persisted = Pending::Carry(std::move(proposal), 6);
        CHECK(Pending::MarkRaised(persisted, 10.1));
        CHECK(Pending::MarkFallbackConsumed(persisted));
        Pending::Expire(20.0, 30.0f, std::span{ &persisted, 1 });
        CHECK(Pending::Find(persisted).has_value());
        Pending::Expire(20.0, 30.0f);
        CHECK(!Pending::Find(persisted));
    }
    void SchedulerCheckpointCooldownRetainsDueEvidence()
    {
        Begin("automatic checkpoints require thirty events and a four-minute cooldown");
        Scheduling::Scheduler scheduler;
        scheduler.BeginSave("A");
        auto entry = Carried(kSerana, "Serana", "Activity", "dragon");
        entry.id = 89;
        entry.speakerId = 500;
        std::uint64_t sequence = 0;
        for (int i = 0; i < 29; ++i) {
            scheduler.Enqueue({ std::format("first-{}", i), 600, 1, "dialogue", "weather", i });
        }
        CHECK(!scheduler.TryDispatch(std::span{ &entry, 1 }, 30, 240000, 1000000,
                                     [&] { return ++sequence; }));
        scheduler.Enqueue({ "first-29", 600, 1, "dialogue", "weather", 29 });
        const auto first = scheduler.TryDispatch(std::span{ &entry, 1 }, 30, 240000, 1000000,
                                                 [&] { return ++sequence; });
        CHECK(first && first->trigger == Scheduling::Trigger::EventCheckpoint);
        scheduler.SubmitResult(first->token, R"({"verdicts":[]})", true);
        CHECK(scheduler.TakeResult().has_value());

        entry.lastAttemptedEvidenceSequence = first->upperSequence;
        for (int i = 0; i < 30; ++i) {
            scheduler.Enqueue({ std::format("second-{}", i), 600, 1, "dialogue", "weather", 30 + i });
        }
        CHECK(!scheduler.TryDispatch(std::span{ &entry, 1 }, 30, 240000, 1239999,
                                     [&] { return ++sequence; }));
        const auto second = scheduler.TryDispatch(std::span{ &entry, 1 }, 30, 240000, 1240000,
                                                  [&] { return ++sequence; });
        CHECK(second && second->trigger == Scheduling::Trigger::EventCheckpoint);
    }
    void SchedulerPollsEveryThirtyAcceptedEvents()
    {
        Begin("thirty accepted events poll every open entry even when unrelated");
        Scheduling::Scheduler scheduler;
        scheduler.BeginSave("checkpoint");
        auto untouched = Carried(kSerana, "Serana", "Activity", "dragon");
        untouched.id = 90;
        untouched.speakerId = 500;
        auto raised = Carried(kLydia, "Lydia", "Relationship", "her housecarl");
        raised.id = 91;
        raised.speakerId = 700;
        raised.state = Pending::LifecycleState::RaisedUnmet;
        const Pending::Entry entries[]{ untouched, raised };
        std::uint64_t sequence = 0;
        for (int i = 0; i < 30; ++i) {
            scheduler.Enqueue({ std::format("weather-checkpoint-{}", i), 600, 1, "dialogue", "weather", i });
        }
        const auto batch = scheduler.TryDispatch(entries, 30, 240000, 1000000,
                                                 [&] { return ++sequence; });
        CHECK(batch && batch->entries.size() == 2);
        CHECK(batch && batch->trigger == Scheduling::Trigger::EventCheckpoint);
    }


    void SchedulerParserRejectsUnknownDuplicateAndRegressiveVerdicts()
    {
        Begin("batch parser accepts only unique requested monotonic verdicts");
        Scheduling::Batch batch;
        auto untouched = Carried(kSerana, "Serana", "Activity", "dragon");
        untouched.id = 81;
        auto raised = Carried(kLydia, "Lydia", "Activity", "sparring");
        raised.id = 82;
        raised.state = Pending::LifecycleState::RaisedUnmet;
        batch.entries = { { untouched, {} }, { raised, {} } };
        const auto parsed = Scheduling::Scheduler::ParseVerdicts(
            R"({"verdicts":[{"id":81,"status":"raised_unmet"},{"id":82,"status":"untouched"},{"id":999,"status":"met"},{"id":81,"status":"met"}]})",
            batch);
        CHECK(parsed.empty());
        const auto partial = Scheduling::Scheduler::ParseVerdicts(
            R"({"verdicts":[{"id":81,"status":"met"},{"id":"bad","status":"met"}]})", batch);
        CHECK(partial.size() == 1 && partial[0].id == 81 &&
              partial[0].state == Pending::LifecycleState::Met);
        CHECK(Scheduling::Scheduler::ParseVerdicts("not json", batch).empty());
    }
    void ScriptedResolutionScenarioKeepsStateAndMemoryConsistent()
    {
        Begin("scripted multi-follower resolution preserves pending state and both ledgers");
        Pending::Reset();
        auto ignoredEntry = Carried(kSerana, "Serana", "Relationship", "her father");
        ignoredEntry.speakerId = 500;
        const auto ignored = Pending::Carry(std::move(ignoredEntry), 6);
        auto deferredEntry = Carried(kLydia, "Lydia", "Activity", "a drink");
        deferredEntry.speakerId = 600;
        deferredEntry.proposal = true;
        const auto deferred = Pending::Carry(std::move(deferredEntry), 3);
        auto metEntry = Carried(kSerana + 1, "Kaidan", "Aspiration", "the road north");
        metEntry.speakerId = 700;
        const auto met = Pending::Carry(std::move(metEntry), 6);
        CHECK(Pending::MarkRaised(ignored, 10.1));
        CHECK(Pending::MarkRaised(deferred, 10.1));

        Scheduling::Scheduler scheduler;
        scheduler.BeginSave("scripted");
        const Pending::EntryId ids[]{ ignored, deferred, met };
        scheduler.QueueManual(ids);
        std::uint64_t sequence = 100;
        auto snapshot = Pending::Snapshot();
        const auto first = scheduler.TryDispatch(snapshot, 30, 240000, 1000000, [&] { return ++sequence; });
        CHECK(first && first->entries.size() == 3);
        for (const auto& item : first->entries) {
            Pending::SetLastAttemptedEvidenceSequence(item.entry.id, first->upperSequence);
        }
        scheduler.SubmitResult(
            first->token,
            std::format(R"({{"verdicts":[{{"id":{},"status":"raised_unmet"}},{{"id":{},"status":"raised_unmet"}},{{"id":{},"status":"met"}}]}})",
                        ignored, deferred, met),
            true);
        const auto completed = scheduler.TakeResult();
        CHECK(completed.has_value());
        for (const auto& verdict : Scheduling::Scheduler::ParseVerdicts(completed->response, completed->batch)) {
            if (verdict.state == Pending::LifecycleState::RaisedUnmet) {
                Pending::MarkRaised(verdict.id, 10.2);
            } else if (verdict.state == Pending::LifecycleState::Met) {
                Pending::MarkMet(verdict.id, 10.2);
            }
        }

        scheduler.Enqueue({ "failed-evidence", 500, 1, "dialogue", "what about your father", 1 });
        scheduler.Enqueue({ "failed-evidence", 500, 1, "dialogue", "what about your father", 1 });
        snapshot = Pending::Snapshot();
        const auto failed = scheduler.TryDispatch(snapshot, 1, 240000, 1000000, [&] { return ++sequence; });
        CHECK(failed && failed->events.size() == 1);
        for (const auto& item : failed->entries) {
            Pending::SetLastAttemptedEvidenceSequence(item.entry.id, failed->upperSequence);
        }
        scheduler.SubmitResult(failed->token, "network failure", false);
        CHECK(scheduler.TakeResult() && !scheduler.InFlight());

        CHECK(Pending::Count() == 2);
        CHECK(Pending::Find(ignored)->state == Pending::LifecycleState::RaisedUnmet);
        CHECK(Pending::Find(deferred)->state == Pending::LifecycleState::RaisedUnmet);
        CHECK(!Pending::Find(met));
        CHECK(Pending::LedgerSuppresses(kSerana, "her father"));
        CHECK(Pending::LedgerSuppresses(kLydia, "a drink"));
        CHECK(Pending::LedgerSuppresses(kSerana + 1, "the road north"));
        CHECK(Pending::PartySnapshot().size() == 3);
        CHECK(scheduler.Snapshot().paidBatches == 2);
    }


    void SchedulerCheckpointIncludesParaphrasesAcrossFollowers()
    {
        Begin("checkpoint batches all due entries while retaining paraphrase references");
        Scheduling::Scheduler scheduler;
        scheduler.BeginSave("paraphrase");

        auto raised = Carried(kSerana, "Serana", "Relationship", "the Kagrenac carvings");
        raised.id = 111;
        raised.speakerId = 500;
        raised.state = Pending::LifecycleState::RaisedUnmet;
        auto otherSerana = Carried(kSerana, "Serana", "Aspiration", "returning to Castle Volkihar");
        otherSerana.id = 112;
        otherSerana.speakerId = 500;
        auto lydia = Carried(kLydia, "Lydia", "Activity", "a drink");
        lydia.id = 113;
        lydia.speakerId = 600;
        const Pending::Entry entries[]{ raised, otherSerana, lydia };
        std::uint64_t sequence = 0;

        scheduler.Enqueue({ "before-raise", 1, 500, "dialogue_player_text", "Where should we go?", 1 });
        Scheduling::RawEvent owner{ "owned-raise", 500, 1, "dialogue_npc",
                                    "I need to discuss the Kagrenac carvings.", 2 };
        owner.raisedEntryId = raised.id;
        scheduler.Enqueue(std::move(owner));
        CHECK(!scheduler.TryDispatch(entries, 3, 240000, 1000000, [&] { return ++sequence; }));

        scheduler.Enqueue({ "paraphrased-answer", 1, 500, "dialogue_player_text",
                            "Those Dwemer inscriptions can wait until morning.", 3 });
        const auto batch = scheduler.TryDispatch(entries, 3, 240000, 1000000, [&] { return ++sequence; });
        CHECK(batch && batch->trigger == Scheduling::Trigger::EventCheckpoint);
        CHECK(batch && batch->entries.size() == 3);
        if (batch) {
            CHECK(batch->entries[0].entry.id == raised.id);
            CHECK(batch->entries[0].relevantEventIds.size() == 1);
            CHECK(batch->entries[1].entry.id == otherSerana.id);
            CHECK(batch->entries[2].entry.id == lydia.id);
        }
    }

    void SkyrimNetCallbackEnvelopeRetainsDialogueEvidence()
    {
        Begin("SkyrimNet callback envelope retains UUIDs and nested dialogue text");
        const auto event = AgencyEngine::SkyrimNetAPI::ParseEventCallbackPayload(
            R"({"type":"dialogue","data":"{\"text\":\"I would rather take the road north.\",\"speaker\":\"Kaidan\"}","originatingActorUUID":12345,"targetActorUUID":67890,"originatingActorFormId":1234,"targetActorFormId":5678,"id":42})",
            "dialogue", 9001);
        CHECK(event.sourceId == "42");
        CHECK(event.actorId == 12345);
        CHECK(event.targetId == 67890);
        CHECK(event.type == "dialogue");
        CHECK(event.text == "I would rather take the road north.");
        CHECK(event.arrivalMs == 9001);
    }

    void RecentEventPollingRecoversMissedCallbacksExactlyOnce()
    {
        Begin("recent-event polling baselines history and recovers missed callbacks exactly once");
        AgencyEngine::SkyrimNetAPI::RecentEventRecovery recovery{ 8 };
        recovery.BeginSave("A");

        constexpr auto baseline =
            R"([{"id":100,"type":"dialogue_player_text","originatingActor":11,"targetActor":22,"localTime":1000.0,"data":{"dialogue":"old"}},{"id":101,"type":"dialogue","originatingActor":22,"targetActor":11,"localTime":1001.0,"data":{"dialogue":"also old"}}])";
        const auto first = recovery.Poll(baseline, 50000, 1010000);
        CHECK(first.valid);
        CHECK(first.establishedBaseline);
        CHECK(first.tailSize == 2);
        CHECK(first.events.empty());

        const AgencyEngine::SkyrimNetAPI::RawDialogueEvent callback{
            "102", 11, 22, "dialogue_player_text", "delivered callback", 49500
        };
        recovery.ObserveCallbacks(std::span{ &callback, 1 });

        constexpr auto next =
            R"([{"id":101,"type":"dialogue","originatingActor":22,"targetActor":11,"localTime":1001.0,"data":{"dialogue":"also old"}},{"id":102,"type":"dialogue_player_text","originatingActor":11,"targetActor":22,"localTime":1009.5,"data":{"dialogue":"delivered callback"}},{"id":103,"type":"dialogue","originatingActor":22,"targetActor":11,"localTime":1009.75,"data":{"dialogue":"recovered line"}}])";
        const auto recovered = recovery.Poll(next, 50000, 1010000);
        CHECK(recovered.valid);
        CHECK(!recovered.establishedBaseline);
        CHECK(recovered.tailSize == 3);
        CHECK(recovered.events.size() == 1);
        if (recovered.events.size() == 1) {
            CHECK(recovered.events[0].sourceId == "103");
            CHECK(recovered.events[0].actorId == 22);
            CHECK(recovered.events[0].targetId == 11);
            CHECK(recovered.events[0].type == "dialogue");
            CHECK(recovered.events[0].text == "recovered line");
            CHECK(recovered.events[0].arrivalMs == 49750);
        }

        CHECK(recovery.Poll(next, 65000, 1025000).events.empty());

        recovery.BeginSave("A");
        const auto reloaded = recovery.Poll(next, 70000, 1030000);
        CHECK(reloaded.establishedBaseline);
        CHECK(reloaded.events.empty());
    }

    void RecentEventPollingRejectsMalformedTails()
    {
        Begin("recent-event polling rejects malformed tails without establishing a baseline");
        AgencyEngine::SkyrimNetAPI::RecentEventRecovery recovery;
        recovery.BeginSave("A");
        CHECK(!recovery.Poll("{bad", 1, 1).valid);
        const auto baseline = recovery.Poll("[]", 2, 2);
        CHECK(baseline.valid);
        CHECK(baseline.establishedBaseline);
    }

}

int main()
{
    std::printf("AgencyEngine pending lifecycle and memory tests\n");
    StableIdsRejectStaleCommands();
    LifecycleAndMemoryTransitionAtomically();
    StateAwareExpiryPreservesOnlyConfirmedMemory();
    UnicodeNormalizationIsStrictAndDeterministic();
    PersonalSuppressionSpansLensesWithoutCrossRingEviction();
    PartyMemoryScopesEchoesAndRetiresUntouchedCarries();
    PartyRetentionIsBoundedAndExpires();
    ForgetActionsRespectOriginOwnership();
    FloorOwnershipIsStableUntilClosure();
    CoalescedCueUsesNewestPreEmissionOwner();
    LegacySidecarMigratesAndReloads();
    PartyMemoryPersistsAcrossReload();
    EvidenceSequenceContinuesAbovePersistedWatermark();
    ExistingPerLensRingContractsRemainCovered();
    ExistingCarryAndRenderContractsRemainCovered();
    ExistingProvisionalAndActorClearContractsRemainCovered();
    ConfirmedDuplicatesNeverMoveOrDowngrade();
    RenamedAndLegacyRingsRemainUsable();
    SchedulerBoundsAndAssociatesEvidence();
    SchedulerSaveTokensRejectABAResults();
    SchedulerTracksIndependentEntryCheckpoints();
    SchedulerTriggersBatchOnceAndRetainsInflightArrivals();
    SchedulerFallbacksCoalesceAndReleaseAfterFailure();
    SchedulerCheckpointCooldownRetainsDueEvidence();
    SchedulerPollsEveryThirtyAcceptedEvents();
    SchedulerParserRejectsUnknownDuplicateAndRegressiveVerdicts();
    ScriptedResolutionScenarioKeepsStateAndMemoryConsistent();
    SchedulerCheckpointIncludesParaphrasesAcrossFollowers();
    SkyrimNetCallbackEnvelopeRetainsDialogueEvidence();
    RecentEventPollingRecoversMissedCallbacksExactlyOnce();
    RecentEventPollingRejectsMalformedTails();
    std::printf("%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
