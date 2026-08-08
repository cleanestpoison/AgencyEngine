#include "PendingImpulse.h"

#include "Logging.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <fstream>
#include <limits>

namespace AgencyEngine::PendingImpulses
{
    namespace
    {
        // Its own lock, not the Status one — see the header. Held only for a
        // copy or a small vector edit; the decorator runs inside SkyrimNet's
        // prompt rendering and must never wait on the UI.
        std::mutex              g_lock;
        std::vector<Entry>      g_entries;
        // Newest last, per character. Shares g_lock with the entries: the two
        // are written together on every dispatch and every clear, and a second
        // lock would only create an order to get wrong.
        std::vector<LedgerSlot> g_ledger;

        // Slots per character. Kept here rather than threaded through Clear()
        // because Clear is called from six places that have no business knowing
        // about Settings — the UI, the TTL sweep, the restore check — and only
        // one of them could supply it. The Director republishes it every tick.
        std::atomic<std::size_t> g_cap{ 6 };

        // Topics are compared loosely — the model writes the slug fresh each
        // time, so "the coin split", "The coin split." and "the  coin split"
        // all have to land on the same subject or the ledger suppresses nothing.
        std::string Normalize(std::string_view topic)
        {
            std::string out;
            out.reserve(topic.size());
            bool pendingSpace = false;
            for (const unsigned char ch : topic) {
                if (std::isalnum(ch)) {
                    if (pendingSpace && !out.empty()) {
                        out.push_back(' ');
                    }
                    pendingSpace = false;
                    out.push_back(static_cast<char>(std::tolower(ch)));
                } else {
                    pendingSpace = true;
                }
            }
            return out;
        }

        // Which save the in-memory set belongs to, and whether it differs from
        // what is on disk. Both under g_lock.
        std::string  g_loadedSaveId;
        bool         g_dirty = false;
        std::int64_t g_lastWriteMs = 0;

        // How often a dirty set is written back. A pending impulse changes at
        // in-game-hour cadence, so anything faster is pure I/O.
        constexpr std::int64_t kWriteIntervalMs = 15000;

        // Save IDs kept in the file. Each is a handful of short strings, so the
        // cap is about the file staying readable rather than about size — but
        // without one it grows forever across playthroughs.
        constexpr std::size_t kMaxSaves = 20;

        std::int64_t NowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

        // g_lock must be held.
        Entry* FindLocked(std::uint32_t formID)
        {
            const auto it = std::ranges::find_if(g_entries, [&](const Entry& e) { return e.formID == formID; });
            return it == g_entries.end() ? nullptr : &*it;
        }

        double GameMinutesToDays(float minutes)
        {
            return static_cast<double>(minutes) / (24.0 * 60.0);
        }

        // Where the TTL counts from. Speaking restarts the clock: the carried
        // phase and the spoken phase are different states with different bio
        // wording and a different question asked about them, so each gets its
        // own full window rather than sharing one.
        double AgeAnchor(const Entry& e)
        {
            return e.spoken ? e.spokenGameDays : e.createdGameDays;
        }

        nlohmann::json ToJson(const Entry& e)
        {
            return nlohmann::json{
                { "formID", e.formID },
                { "speakerName", e.speakerName },
                { "targetName", e.targetName },
                { "text", e.text },
                { "topic", e.topic },
                { "lens", e.lens },
                { "createdGameDays", e.createdGameDays },
                { "lastCheckGameDays", e.lastCheckGameDays },
                { "spoken", e.spoken },
                { "spokenGameDays", e.spokenGameDays },
            };
        }

        Entry FromJson(const nlohmann::json& j)
        {
            Entry e;
            e.formID = j.value("formID", 0u);
            e.speakerName = j.value("speakerName", std::string{});
            e.targetName = j.value("targetName", std::string{});
            e.text = j.value("text", std::string{});
            e.topic = j.value("topic", std::string{});
            e.lens = j.value("lens", std::string{});
            e.createdGameDays = j.value("createdGameDays", 0.0);
            e.lastCheckGameDays = j.value("lastCheckGameDays", e.createdGameDays);
            e.spoken = j.value("spoken", false);
            e.spokenGameDays = j.value("spokenGameDays", e.createdGameDays);
            // Everything off disk is suspect until the Director has matched the
            // FormID against a live form. See the header.
            e.unverified = true;
            return e;
        }

        // Reads the whole sidecar. Returns an empty object on anything unusual —
        // a missing file is the normal first-run case, and a corrupt one is not
        // worth losing a session over.
        nlohmann::json ReadFile()
        {
            const auto path = FilePath();
            std::error_code ec;
            if (!std::filesystem::exists(path, ec)) {
                return nlohmann::json::object();
            }
            try {
                std::ifstream file{ path };
                nlohmann::json j;
                file >> j;
                if (!j.is_object()) {
                    return nlohmann::json::object();
                }
                return j;
            } catch (const std::exception& e) {
                logger::warn("Pending impulses: could not read {} ({}) — starting empty", path.string(), e.what());
                return nlohmann::json::object();
            }
        }

        // g_lock must be held.
        void WriteLocked()
        {
            if (g_loadedSaveId.empty()) {
                return;
            }

            const auto path = FilePath();
            try {
                auto root = ReadFile();
                auto& saves = root["saves"];
                if (!saves.is_object()) {
                    saves = nlohmann::json::object();
                }

                // A write counter rather than a timestamp: Director ticks give
                // us no wall clock we would trust across sessions, and all this
                // has to do is order the saves for pruning.
                const auto seq = root.value("seq", 0ull) + 1ull;
                root["seq"] = seq;

                nlohmann::json impulses = nlohmann::json::array();
                for (const auto& entry : g_entries) {
                    impulses.push_back(ToJson(entry));
                }

                // The ledger persists for the same reason the entries do: it is
                // the record that outlives the event tail, and one that reset on
                // every load would be no better than the tail it replaces.
                nlohmann::json ledger = nlohmann::json::array();
                for (const auto& slot : g_ledger) {
                    ledger.push_back(nlohmann::json{
                        { "formID", slot.formID },
                        { "speakerName", slot.speakerName },
                        { "topic", slot.topic },
                        { "provisional", slot.provisional },
                    });
                }

                saves[g_loadedSaveId] = nlohmann::json{ { "seq", seq },
                                                        { "impulses", std::move(impulses) },
                                                        { "ledger", std::move(ledger) } };

                // Oldest-written saves fall off the end. The current one was
                // just stamped with the highest seq, so it can never be the one
                // pruned.
                while (saves.size() > kMaxSaves) {
                    std::string oldestKey;
                    unsigned long long oldestSeq = std::numeric_limits<unsigned long long>::max();
                    for (const auto& [key, value] : saves.items()) {
                        const auto candidate = value.is_object() ? value.value("seq", 0ull) : 0ull;
                        if (candidate < oldestSeq) {
                            oldestSeq = candidate;
                            oldestKey = key;
                        }
                    }
                    if (oldestKey.empty()) {
                        break;
                    }
                    saves.erase(oldestKey);
                }

                std::error_code ec;
                std::filesystem::create_directories(path.parent_path(), ec);
                std::ofstream file{ path, std::ios::trunc };
                file << root.dump(2);

                g_dirty = false;
                g_lastWriteMs = NowMs();
                logger::debug("Pending impulses: wrote {} entr(ies) for save '{}'", g_entries.size(), g_loadedSaveId);
            } catch (const std::exception& e) {
                // Don't clear the dirty flag: the next tick will try again, and
                // a transient failure (the file open in an editor) should not
                // silently drop the state.
                logger::error("Pending impulses: failed to write {}: {}", path.string(), e.what());
            }
        }
    }

    std::filesystem::path FilePath()
    {
        // Relative to the game's working directory, exactly like Settings — so
        // it resolves through the mod manager's VFS and writes land in MO2's
        // overwrite rather than in a real mod folder.
        return std::filesystem::path{ "Data/SKSE/Plugins/AgencyEngine_Pending.json" };
    }

    void Set(Entry entry)
    {
        entry.lastCheckGameDays = entry.createdGameDays;
        entry.unverified = false;

        // The decorator's return value is substituted into a template that has
        // already been through Inja, so it is prose, not markup — but it lands
        // inside a markdown blockquote, and a newline in it would silently end
        // the quote and leave the rest as body text. Double quotes go the same
        // way, for the same reason: the rendered string travels through enough
        // string handling that it is not worth finding out where a stray one
        // bites. The impulse prompt forbids
        // quoted speech anyway, so in practice neither of these fires.
        std::replace(entry.text.begin(), entry.text.end(), '\n', ' ');
        std::replace(entry.text.begin(), entry.text.end(), '\r', ' ');
        std::replace(entry.text.begin(), entry.text.end(), '"', '\'');

        // What the incoming entry displaced, if anything. Captured under the lock
        // and dealt with after it, because deciding a ledger slot takes the same
        // lock.
        std::uint32_t supersededFormID = 0;
        std::string   supersededTopic;
        bool          supersededWasSpoken = false;

        {
            std::scoped_lock lock{ g_lock };
            if (auto* existing = FindLocked(entry.formID)) {
                logger::info("{} was already {}; the new impulse supersedes it. Was: {}", entry.speakerName,
                             existing->spoken ? "waiting on an answer about something else"
                                              : "carrying something unsaid",
                             OneLine(existing->text));
                supersededFormID = existing->formID;
                supersededTopic = existing->topic;
                supersededWasSpoken = existing->spoken;
                *existing = std::move(entry);
            } else {
                g_entries.push_back(std::move(entry));
            }
            g_dirty = true;
        }

        // A superseded *spoken* entry leaves a provisional slot behind with
        // nothing left to decide it — Set replaces the entry in place and never
        // goes through Clear, so without this the slot sits provisional for the
        // rest of the session, suppressing its subject with no route to release.
        // Withdraw is the honest verdict: we never learned whether anyone met it,
        // and unanswered subjects are the ones allowed back.
        if (supersededWasSpoken && !supersededTopic.empty()) {
            LedgerDecide(supersededFormID, supersededTopic, Disposition::Withdraw);
        }
    }

    std::string Get(std::uint32_t formID)
    {
        std::scoped_lock lock{ g_lock };
        const auto* entry = FindLocked(formID);
        // An unverified entry is withheld rather than rendered. It is one
        // Director tick from being either confirmed or dropped, and rendering a
        // possibly-misattributed agenda into somebody's bio is the one failure
        // this whole design exists to avoid.
        if (!entry || entry->unverified) {
            return {};
        }
        return entry->text;
    }

    bool Clear(std::uint32_t formID, std::string_view reason, Disposition disposition)
    {
        Entry removed;
        {
            std::scoped_lock lock{ g_lock };
            auto* entry = FindLocked(formID);
            if (!entry) {
                return false;
            }
            removed = std::move(*entry);
            std::erase_if(g_entries, [&](const Entry& e) { return e.formID == formID; });
            g_dirty = true;
        }

        logger::info("Pending impulse cleared ({}): {} was carrying — {}", reason, removed.speakerName,
                     OneLine(removed.text));

        if (!removed.topic.empty()) {
            if (disposition == Disposition::Confirm) {
                // Settled is settled, whoever settled it. A subject the talk
                // covered without her ever raising it is just as spent as one
                // she raised — and if it takes no slot, nothing stops the loop
                // proposing it again the moment the event tail drains, which is
                // the whole failure this ledger exists to prevent. So the slot
                // is created here when she never spoke and therefore never got
                // one at dispatch.
                LedgerRecord(formID, removed.speakerName, removed.topic, g_cap.load());
                LedgerDecide(formID, removed.topic, Disposition::Confirm);
            } else if (removed.spoken) {
                // Withdraw only ever releases a slot that exists. An entry that
                // was never spoken never had one, and nothing was answered, so
                // there is nothing to release and nothing to suppress.
                LedgerDecide(formID, removed.topic, Disposition::Withdraw);
            }
        }
        return true;
    }

    std::string State(std::uint32_t formID)
    {
        std::scoped_lock lock{ g_lock };
        const auto* entry = FindLocked(formID);
        if (!entry || entry->unverified) {
            return {};
        }
        return entry->spoken ? "spoken" : "carried";
    }

    void LedgerRecord(std::uint32_t formID, std::string speakerName, std::string topic, std::size_t cap)
    {
        if (formID == 0 || topic.empty() || cap == 0) {
            return;
        }
        const auto key = Normalize(topic);
        if (key.empty()) {
            return;
        }

        // Neutral wording and debug level: this is called both when she speaks
        // (provisional, pending a verdict) and when a subject is confirmed
        // settled without her having said it. The two call sites each log their
        // own info line saying which happened; this one only says a slot exists,
        // because "why did she not raise that" needs to be answerable from the
        // log rather than from the sidecar on disk.
        logger::debug("Ledger: '{}' now occupies a slot for {}", OneLine(topic), speakerName);

        std::scoped_lock lock{ g_lock };

        // A repeat moves to newest rather than taking a second slot — the same
        // subject raised twice is one subject, and two slots would halve the
        // memory for everything else.
        std::erase_if(g_ledger,
                      [&](const LedgerSlot& s) { return s.formID == formID && Normalize(s.topic) == key; });

        g_ledger.push_back(LedgerSlot{ formID, std::move(speakerName), std::move(topic), true });

        // Oldest-first within this character only. Other characters' slots are
        // untouched, so a talkative follower cannot evict a quiet one's memory.
        std::size_t held = 0;
        for (const auto& slot : g_ledger) {
            held += (slot.formID == formID) ? 1 : 0;
        }
        while (held > cap) {
            const auto it = std::ranges::find_if(g_ledger, [&](const LedgerSlot& s) { return s.formID == formID; });
            if (it == g_ledger.end()) {
                break;
            }
            logger::debug("Ledger: {} is full at {} — '{}' drops off and can be raised again", it->speakerName, cap,
                          OneLine(it->topic));
            g_ledger.erase(it);
            held -= 1;
        }
        g_dirty = true;
    }

    void LedgerDecide(std::uint32_t formID, std::string_view topic, Disposition disposition)
    {
        const auto key = Normalize(topic);
        if (key.empty()) {
            return;
        }

        std::string name;
        bool        found = false;
        {
            std::scoped_lock lock{ g_lock };
            const auto it = std::ranges::find_if(g_ledger, [&](const LedgerSlot& s) {
                return s.formID == formID && Normalize(s.topic) == key;
            });
            if (it == g_ledger.end()) {
                return;
            }
            found = true;
            name = it->speakerName;
            if (disposition == Disposition::Confirm) {
                it->provisional = false;
            } else {
                g_ledger.erase(it);
            }
            g_dirty = true;
        }

        if (found) {
            if (disposition == Disposition::Confirm) {
                logger::info("Ledger: '{}' is settled for {} — it stays suppressed until six others displace it",
                             OneLine(std::string{ topic }), name);
            } else {
                logger::info("Ledger: '{}' was never answered for {} — released, it can come back",
                             OneLine(std::string{ topic }), name);
            }
        }
    }

    std::vector<std::string> LedgerTopics(std::uint32_t formID)
    {
        std::scoped_lock lock{ g_lock };
        std::vector<std::string> out;
        for (const auto& slot : g_ledger) {
            if (slot.formID == formID) {
                out.push_back(slot.topic);
            }
        }
        return out;
    }

    bool LedgerSuppresses(std::uint32_t formID, std::string_view topic)
    {
        const auto key = Normalize(topic);
        if (key.empty()) {
            return false;
        }
        std::scoped_lock lock{ g_lock };
        return std::ranges::any_of(g_ledger, [&](const LedgerSlot& s) {
            return s.formID == formID && Normalize(s.topic) == key;
        });
    }

    void SetLedgerCap(std::size_t cap)
    {
        g_cap.store(cap == 0 ? 1 : cap);
    }

    std::vector<LedgerSlot> LedgerSnapshot()
    {
        std::scoped_lock lock{ g_lock };
        return g_ledger;
    }

    std::vector<Entry> Snapshot()
    {
        std::scoped_lock lock{ g_lock };
        return g_entries;
    }

    std::size_t Count()
    {
        std::scoped_lock lock{ g_lock };
        return g_entries.size();
    }

    void ExpireOlderThan(double nowGameDays, float ttlGameMinutes)
    {
        if (ttlGameMinutes <= 0.0f) {
            return;
        }
        const auto ttlDays = GameMinutesToDays(ttlGameMinutes);

        std::vector<std::uint32_t> expired;
        {
            std::scoped_lock lock{ g_lock };
            for (const auto& entry : g_entries) {
                // Only forward: loading an older save moves game time backwards,
                // and an impulse from a future that no longer happened should be
                // dropped by Reset(), not aged out to a negative number here.
                if (nowGameDays - AgeAnchor(entry) > ttlDays) {
                    expired.push_back(entry.formID);
                }
            }
        }
        for (const auto formID : expired) {
            Clear(formID, "ttl");
        }
    }

    std::optional<Entry> NextDueForCheck(double nowGameDays, float intervalGameMinutes)
    {
        if (intervalGameMinutes <= 0.0f) {
            return std::nullopt;
        }
        const auto intervalDays = GameMinutesToDays(intervalGameMinutes);

        std::scoped_lock lock{ g_lock };
        const Entry* best = nullptr;
        for (const auto& entry : g_entries) {
            if (entry.unverified) {
                continue;
            }
            if (nowGameDays - entry.lastCheckGameDays < intervalDays) {
                continue;
            }
            if (!best || entry.lastCheckGameDays < best->lastCheckGameDays) {
                best = &entry;
            }
        }
        return best ? std::optional<Entry>{ *best } : std::nullopt;
    }

    void MarkChecked(std::uint32_t formID, double nowGameDays)
    {
        std::scoped_lock lock{ g_lock };
        if (auto* entry = FindLocked(formID)) {
            entry->lastCheckGameDays = nowGameDays;
            g_dirty = true;
        }
    }

    std::vector<Entry> TakeUnverified()
    {
        std::scoped_lock lock{ g_lock };
        std::vector<Entry> out;
        for (auto& entry : g_entries) {
            if (entry.unverified) {
                out.push_back(entry);
                entry.unverified = false;
            }
        }
        return out;
    }

    void Reset()
    {
        std::size_t dropped = 0;
        {
            std::scoped_lock lock{ g_lock };
            dropped = g_entries.size();
            g_entries.clear();
            g_ledger.clear();
            // Forget the save too, so the next SyncPersistence reads whatever
            // belongs to the save that was just loaded.
            g_loadedSaveId.clear();
            g_dirty = false;
        }
        if (dropped > 0) {
            logger::debug("Pending impulses: dropped {} in-memory entr(ies) on load — reloading from the sidecar",
                          dropped);
        }
    }

    void SyncPersistence(const std::string& saveId, double nowGameDays, float ttlGameMinutes)
    {
        if (saveId.empty()) {
            // No save loaded, or this SkyrimNet predates PublicGetSaveUniqueID.
            // Either way there is nothing to key the file off, so the feature
            // degrades to session-only — which still works.
            return;
        }

        bool needLoad = false;
        {
            std::scoped_lock lock{ g_lock };
            needLoad = g_loadedSaveId != saveId;
        }

        if (needLoad) {
            const auto              root = ReadFile();
            std::vector<Entry>      loaded;
            std::vector<LedgerSlot> ledger;
            if (root.contains("saves") && root["saves"].is_object() && root["saves"].contains(saveId)) {
                const auto& record = root["saves"][saveId];
                if (record.is_object() && record.contains("impulses") && record["impulses"].is_array()) {
                    for (const auto& item : record["impulses"]) {
                        if (item.is_object()) {
                            loaded.push_back(FromJson(item));
                        }
                    }
                }
                if (record.is_object() && record.contains("ledger") && record["ledger"].is_array()) {
                    for (const auto& item : record["ledger"]) {
                        if (!item.is_object()) {
                            continue;
                        }
                        LedgerSlot slot;
                        slot.formID = item.value("formID", 0u);
                        slot.speakerName = item.value("speakerName", std::string{});
                        slot.topic = item.value("topic", std::string{});
                        slot.provisional = item.value("provisional", false);
                        if (slot.formID != 0 && !slot.topic.empty()) {
                            ledger.push_back(std::move(slot));
                        }
                    }
                }
            }

            // Anything already past its TTL against the *current* game time
            // never comes back — an impulse from three in-game days ago is not
            // something she has been "meaning to raise", it is something she
            // forgot.
            std::size_t stale = 0;
            if (ttlGameMinutes > 0.0f) {
                const auto ttlDays = GameMinutesToDays(ttlGameMinutes);
                stale = std::erase_if(loaded, [&](const Entry& e) {
                    return nowGameDays - AgeAnchor(e) > ttlDays || nowGameDays < AgeAnchor(e);
                });
            }

            // After the TTL pass, not before: a provisional slot is only
            // meaningful while the entry that will decide it is still around,
            // and an entry dropped just above has stopped being around. An
            // orphan would sit provisional forever with nothing left to release
            // it, so it is withdrawn — the recoverable direction, and the same
            // answer a TTL expiry would have given it anyway.
            const auto orphaned = std::erase_if(ledger, [&](const LedgerSlot& s) {
                if (!s.provisional) {
                    return false;
                }
                return !std::ranges::any_of(loaded, [&](const Entry& e) {
                    return e.formID == s.formID && Normalize(e.topic) == Normalize(s.topic);
                });
            });

            {
                std::scoped_lock lock{ g_lock };
                g_entries = std::move(loaded);
                g_ledger = std::move(ledger);
                g_loadedSaveId = saveId;
                g_dirty = stale > 0 || orphaned > 0;
                g_lastWriteMs = NowMs();
            }

            logger::info("Pending impulses: save '{}' — restored {} entr(ies), {} ledger slot(s){}{}", saveId, Count(),
                         LedgerSnapshot().size(),
                         stale > 0 ? std::format(", dropped {} already past their TTL", stale) : std::string{},
                         orphaned > 0 ? std::format(", released {} undecided slot(s)", orphaned) : std::string{});
            return;
        }

        std::scoped_lock lock{ g_lock };
        if (g_dirty && NowMs() - g_lastWriteMs >= kWriteIntervalMs) {
            WriteLocked();
        }
    }
}
