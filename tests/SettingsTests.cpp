// The config file, tested through the same Load and Save the plugin calls.
//
// Worth an offline test for the same reason the ledger is: it is pure state
// with no game behind it, and it regresses without a symptom anyone can trace.
// The failure this guards against is specifically an *upgrade* failure — a
// weight someone set two versions ago silently reverting, or a retuned default
// never reaching an install that already exists. Neither shows up in a log, and
// neither is reproducible without the old file, which by then has been
// overwritten.
//
// Load and Save both resolve Data/SKSE/Plugins/AgencyEngine.json relative to
// the working directory, so each case runs in a scratch directory of its own.

#include "Settings.h"

#include <cstdio>
#include <fstream>

namespace
{
    using AgencyEngine::kBuiltinLensCount;
    using AgencyEngine::kBuiltinLenses;
    using AgencyEngine::kMaxLenses;
    using AgencyEngine::Lens;
    using AgencyEngine::Settings;

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

    // A scratch working directory, so the fixed relative config path lands
    // somewhere disposable rather than in the source tree.
    void Begin(const char* name)
    {
        std::printf("- %s\n", name);
        static int  counter = 0;
        const auto  root = std::filesystem::temp_directory_path() / std::format("agencyengine-cfg-{}", ++counter);
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
        std::filesystem::current_path(root, ec);
    }

    void WriteConfig(std::string_view body)
    {
        const auto path = Settings::FilePath();
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream file{ path, std::ios::trunc };
        file << body;
    }

    std::string ReadConfig()
    {
        std::ifstream file{ Settings::FilePath() };
        return std::string{ std::istreambuf_iterator<char>{ file }, std::istreambuf_iterator<char>{} };
    }

    const Lens* Find(const Settings& s, std::string_view id)
    {
        for (const auto& lens : s.lenses) {
            if (id == lens.id) {
                return &lens;
            }
        }
        return nullptr;
    }

    // The shipped row, so a case can say "unchanged from the build" without
    // repeating a number that is deliberately free to move between releases.
    const Lens* Builtin(std::string_view id)
    {
        return AgencyEngine::BuiltinLensFor(id);
    }
    int CountCustomLenses(const Settings& s)
    {
        return static_cast<int>(std::ranges::count_if(s.lenses, [](const Lens& lens) {
            return lens.id[0] == '\0' && lens.prompt[0] != '\0';
        }));
    }

    // The whole point of the change: a setting nobody touched is not in the
    // file, so a later version that retunes it reaches this install.
    void SaveWritesOnlyWhatChanged()
    {
        Begin("an untouched setting is not written, so a retuned default still reaches you");

        Settings s;
        s.forcedImpulseChance = 55;
        CHECK(s.Save());

        const auto text = ReadConfig();
        CHECK(text.find("forcedImpulseChance") != std::string::npos);
        // Every one of these is still at its shipped value, and a file that
        // named them would pin them for good.
        CHECK(text.find("quietSeconds") == std::string::npos);
        CHECK(text.find("conversationSettleSeconds") == std::string::npos);
        CHECK(text.find("ledgerSlots") == std::string::npos);
        CHECK(text.find("intervalGameMinutes") == std::string::npos);
        CHECK(text.find("cooldownGameMinutes") == std::string::npos);
        CHECK(text.find("followerEventTypeFilter") == std::string::npos);
        // No lens was moved either, so neither key appears at all.
        CHECK(text.find("lenses") == std::string::npos);
        CHECK(text.find("customLenses") == std::string::npos);
    }

    void AnUntouchedFileRoundTripsToTheDefaults()
    {
        Begin("saving and reloading an untouched install changes nothing");

        const Settings shipped;
        CHECK(shipped.Save());

        Settings loaded;
        CHECK(loaded.Load());
        CHECK(loaded.Summary() == shipped.Summary());
    }

    // The bug that started this: a lens added in a later version has to turn up
    // in a config that already exists.
    void ANewLensAppearsInAnExistingConfig()
    {
        Begin("a lens the config has never heard of arrives at its shipped cadence");

        // A file that predates Curiosity, moves one builtin and already fills
        // every custom slot the previous shipped roster left available.
        WriteConfig(R"({
            "lenses": { "aspiration": { "intervalGameMinutes": 45 } },
            "customLenses": [
                { "name": "One", "prompt": "custom_one" },
                { "name": "Two", "prompt": "custom_two" },
                { "name": "Three", "prompt": "custom_three" }
            ]
        })");

        Settings s;
        CHECK(s.Load());

        CHECK(Find(s, "aspiration") != nullptr && Find(s, "aspiration")->intervalGameMinutes == 45.0f);
        for (const auto& builtin : kBuiltinLenses) {
            const auto* loaded = Find(s, builtin.id);
            CHECK(loaded != nullptr);
            if (loaded && std::string_view{ builtin.id } != "aspiration") {
                CHECK(loaded->enabled == builtin.enabled);
                CHECK(loaded->intervalGameMinutes == builtin.intervalGameMinutes);
                CHECK(loaded->cooldownGameMinutes == builtin.cooldownGameMinutes);
                CHECK(loaded->ledgerSlots == builtin.ledgerSlots);
                CHECK(std::string_view{ loaded->prompt } == builtin.prompt);
            }
        }
        // The one it did move keeps everything else from the build, including
        // the cooldown it never mentioned.
        CHECK(Find(s, "aspiration")->cooldownGameMinutes == kBuiltinLenses[0].cooldownGameMinutes);
        const auto* curiosity = Find(s, "curiosity");
        CHECK(curiosity != nullptr);
        if (curiosity) {
            CHECK(curiosity->enabled);
            CHECK(curiosity->intervalGameMinutes == 360.0f);
            CHECK(curiosity->cooldownGameMinutes == 1440.0f);
            CHECK(!curiosity->proposal);
            CHECK(curiosity->playerTargetOnly);
            CHECK(curiosity->ledgerSlots == 0);
            CHECK(std::string_view{ curiosity->prompt } == "agencyengine_impulse_curiosity");
        }
        CHECK(CountCustomLenses(s) == 3);

        // Saving the old file must preserve its override without pinning the new
        // shipped row; the next load should still receive Curiosity from the build.
        CHECK(s.Save());
        CHECK(ReadConfig().find("curiosity") == std::string::npos);

        Settings reloaded;
        CHECK(reloaded.Load());
        CHECK(Find(reloaded, "aspiration")->intervalGameMinutes == 45.0f);
        CHECK(Find(reloaded, "curiosity") != nullptr);
        CHECK(CountCustomLenses(reloaded) == 3);
    }

    // ADR 0003 retires the weighted draw. Weight 0 was how an install said
    // "never ask this" — usually for a lens whose prompt needs a mod they do not
    // have, where re-enabling it dispatches a prompt that cannot parse — so that
    // one value survives as the enable switch. Every other weight decided a draw
    // that no longer happens.
    void WeightZeroBecomesSwitchedOff()
    {
        Begin("a lens weighted 0 stays switched off, and any other weight is ignored");

        WriteConfig(R"({ "lenses": {
            "relationship": { "weight": 0 },
            "activity": { "weight": 90 } } })");

        Settings s;
        CHECK(s.Load());

        CHECK(!Find(s, "relationship")->enabled);
        // A nonzero weight said nothing about whether the lens runs, so the
        // lens is left exactly as the build ships it.
        CHECK(Find(s, "activity")->enabled);
        CHECK(Find(s, "activity")->intervalGameMinutes == Builtin("activity")->intervalGameMinutes);
        CHECK(Find(s, "activity")->cooldownGameMinutes == Builtin("activity")->cooldownGameMinutes);

        // And the retired key is never written back, so the next save is the
        // last time anyone sees it.
        CHECK(s.Save());
        const auto text = ReadConfig();
        CHECK(text.find("weight") == std::string::npos);
        CHECK(text.find("\"enabled\": false") != std::string::npos);
    }

    // The shared tick is gone with the draw. A file still carrying it must not
    // silently do nothing about it, and must not carry it forward either.
    void TheRetiredIntervalKeyIsIgnoredAndDropped()
    {
        Begin("the old shared interval is ignored and never written back");

        WriteConfig(R"({ "intervalGameMinutes": 30, "forcedImpulseChance": 40 })");

        Settings s;
        CHECK(s.Load());

        // The setting it used to drive no longer exists, so the only thing to
        // check is that everything else loaded and the key did not survive.
        CHECK(s.forcedImpulseChance == 40);
        CHECK(s.Save());
        const auto text = ReadConfig();
        CHECK(text.find("intervalGameMinutes") == std::string::npos);
        CHECK(text.find("forcedImpulseChance") != std::string::npos);
    }

    // Cadence is per lens now, and both halves of it round-trip.
    void CadenceOverridesRoundTrip()
    {
        Begin("a moved interval and cooldown survive a save and a reload");

        Settings first;
        for (auto& lens : first.lenses) {
            if (std::string_view{ lens.id } == "activity") {
                lens.intervalGameMinutes = 90.0f;
                lens.cooldownGameMinutes = 15.0f;
                lens.enabled = false;
            }
        }
        CHECK(first.Save());

        Settings s;
        CHECK(s.Load());
        CHECK(s.Summary() == first.Summary());
        CHECK(Find(s, "activity")->intervalGameMinutes == 90.0f);
        CHECK(Find(s, "activity")->cooldownGameMinutes == 15.0f);
        CHECK(!Find(s, "activity")->enabled);
        // Untouched lenses stay out of the file entirely.
        const auto text = ReadConfig();
        CHECK(text.find("aspiration") == std::string::npos);
    }

    // Name, prompt file and proposal semantics describe a file in the archive.
    // A config that disagreed with the build about them could only be wrong.
    void ContentFieldsComeFromTheBuildNotTheFile()
    {
        Begin("a config cannot rename a shipped lens or repoint its prompt");

        WriteConfig(R"({ "lenses": { "activity": {
            "intervalGameMinutes": 7, "ledgerSlots": 9,
            "name": "Whatever", "prompt": "does_not_exist", "proposal": false } } })");

        Settings s;
        CHECK(s.Load());

        const auto* activity = Find(s, "activity");
        CHECK(activity != nullptr);
        if (activity) {
            CHECK(activity->intervalGameMinutes == 7.0f);  // yours
            CHECK(activity->ledgerSlots == 9);             // yours
            CHECK(std::string_view{ activity->name } == "Activity");
            CHECK(std::string_view{ activity->prompt } == "agencyengine_impulse_activity");
            CHECK(activity->proposal);
        }
    }

    // A lens id this build doesn't know — a retired one, or one from a version
    // ahead of this install — must not become a row pointing at no prompt file.
    void AnUnknownLensIdIsIgnored()
    {
        Begin("an unknown lens id does not become a row");

        WriteConfig(R"({ "lenses": { "from_the_future": { "weight": 80 } } })");

        Settings s;
        CHECK(s.Load());

        int occupied = 0;
        for (const auto& lens : s.lenses) {
            if (lens.prompt[0] != '\0') {
                occupied += 1;
            }
        }
        CHECK(occupied == kBuiltinLensCount);
    }

    // The old format: the whole roster as an array, which is what every install
    // predating the roster-in-the-build change has on disk. Two migrations run
    // over it now — the array to the override object, and the weight to the
    // enable switch — so what survives is the slot count and whether the lens
    // was switched off at all.
    void TheOldArrayFormatKeepsWhatItStillMeans()
    {
        Begin("migrating the old lens array keeps the slots and drops the roster");

        WriteConfig(R"({ "lenses": [
            { "name": "Aspiration", "prompt": "agencyengine_impulse_aspiration", "weight": 5 },
            { "name": "Relationship", "prompt": "agencyengine_impulse_relationship", "weight": 0 },
            { "name": "Activity", "prompt": "agencyengine_impulse_activity", "weight": 35,
              "proposal": true, "ledgerSlots": 4 } ] })");

        Settings s;
        CHECK(s.Load());

        CHECK(Find(s, "aspiration")->enabled);
        CHECK(!Find(s, "relationship")->enabled);
        CHECK(Find(s, "activity")->enabled);
        CHECK(Find(s, "activity")->ledgerSlots == 4);
        // Cadence is a field the old file never had, so it comes from the build.
        CHECK(Find(s, "aspiration")->intervalGameMinutes == Builtin("aspiration")->intervalGameMinutes);

        // And it is written back in the new shape, so the migration happens once.
        CHECK(s.Save());
        const auto text = ReadConfig();
        CHECK(text.find("\"relationship\"") != std::string::npos);
        CHECK(text.find("agencyengine_impulse_aspiration") == std::string::npos);
        CHECK(text.find("weight") == std::string::npos);
    }

    // Deleting a row was how the old settings page said "never ask this". The
    // new page says it with the enable switch, and the migration has to carry
    // the intent across — a deleted lens coming back switched on is the one
    // outcome nobody who deleted it expects.
    void ADeletedRowMigratesToSwitchedOff()
    {
        Begin("a lens deleted under the old format stays switched off");

        WriteConfig(R"({ "lenses": [
            { "name": "Aspiration", "prompt": "agencyengine_impulse_aspiration", "weight": 50 } ] })");

        Settings s;
        CHECK(s.Load());

        CHECK(Find(s, "aspiration")->enabled);
        CHECK(!Find(s, "relationship")->enabled);
        CHECK(!Find(s, "activity")->enabled);
        // Curiosity did not exist in the legacy roster, so its absence cannot
        // express a user deletion. It arrives enabled at its shipped cadence.
        CHECK(Find(s, "curiosity")->enabled);
        // Switched off, not erased: the prompt file is still there to be raised.
        CHECK(std::string_view{ Find(s, "activity")->prompt } == "agencyengine_impulse_activity");
    }

    // A renamed row is still the shipped lens. The rename is dropped, because
    // the name is content now — but what it was carrying is not.
    void ARenamedRowIsStillTheShippedLens()
    {
        Begin("a row renamed under the old format keeps its settings and loses the name");

        WriteConfig(R"({ "lenses": [
            { "name": "Dreams", "prompt": "agencyengine_impulse_aspiration", "weight": 0 },
            { "name": "Relationship", "prompt": "agencyengine_impulse_relationship", "weight": 30 },
            { "name": "Activity", "prompt": "agencyengine_impulse_activity", "weight": 40,
              "proposal": true, "ledgerSlots": 3 } ] })");

        Settings s;
        CHECK(s.Load());

        CHECK(!Find(s, "aspiration")->enabled);
        CHECK(std::string_view{ Find(s, "aspiration")->name } == "Aspiration");
    }

    // A lens someone wrote themselves has no shipped row behind it, so it is
    // stored whole and survives both formats.
    void AHandWrittenLensSurvives()
    {
        Begin("a lens you wrote yourself round-trips whole");

        WriteConfig(R"({ "lenses": [
            { "name": "Aspiration", "prompt": "agencyengine_impulse_aspiration", "weight": 35 },
            { "name": "Relationship", "prompt": "agencyengine_impulse_relationship", "weight": 25 },
            { "name": "Activity", "prompt": "agencyengine_impulse_activity", "weight": 40,
              "proposal": true, "ledgerSlots": 3 },
            { "name": "Grudges", "prompt": "my_own_lens", "weight": 15,
              "proposal": true, "ledgerSlots": 2 } ] })");
        // Written under the old format, where a weight above 0 was the whole of
        // "ask this one" — the only statement about it the file can still make.

        Settings first;
        CHECK(first.Load());
        CHECK(first.Save());

        Settings s;
        CHECK(s.Load());

        const Lens* mine = nullptr;
        for (const auto& lens : s.lenses) {
            if (std::string_view{ lens.prompt } == "my_own_lens") {
                mine = &lens;
            }
        }
        CHECK(mine != nullptr);
        if (mine) {
            CHECK(mine->id[0] == '\0');  // no shipped row to be an override of
            CHECK(std::string_view{ mine->name } == "Grudges");
            CHECK(mine->enabled);
            CHECK(mine->proposal);
            CHECK(mine->ledgerSlots == 2);
            // A cadence the old file could not have carried, so it starts at the
            // blank row's own default and is the user's to move from there.
            CHECK(mine->intervalGameMinutes > 0.0f);
        }
        // The three shipped rows are still shipped rows, not copies of it.
        int builtins = 0;
        for (const auto& lens : s.lenses) {
            if (lens.id[0] != '\0') {
                builtins += 1;
            }
        }
        CHECK(builtins == kBuiltinLensCount);
    }

    // A file left over from a version with more lenses than this one, or a hand
    // edit, must not run past the end of the table.
    void MoreLensesThanFitAreDropped()
    {
        Begin("more lenses than the table holds are dropped rather than overflowing");

        std::string body = R"({ "customLenses": [)";
        for (int i = 0; i < kMaxLenses + 4; ++i) {
            body += std::format(R"({}{{ "name": "L{}", "prompt": "p{}", "weight": 1 }})", i ? "," : "", i, i);
        }
        body += "] }";
        WriteConfig(body);

        Settings s;
        CHECK(s.Load());

        int occupied = 0;
        for (const auto& lens : s.lenses) {
            if (lens.prompt[0] != '\0') {
                occupied += 1;
            }
        }
        CHECK(occupied == kMaxLenses);
    }

    void AMalformedFileLeavesTheDefaultsAlone()
    {
        Begin("a file that does not parse leaves every setting at its default");

        WriteConfig("{ this is not json");

        Settings s;
        CHECK(!s.Load());
        CHECK(s.Summary() == Settings{}.Summary());
    }
}

int main()
{
    std::printf("AgencyEngine settings tests\n");

    SaveWritesOnlyWhatChanged();
    AnUntouchedFileRoundTripsToTheDefaults();
    ANewLensAppearsInAnExistingConfig();
    WeightZeroBecomesSwitchedOff();
    TheRetiredIntervalKeyIsIgnoredAndDropped();
    CadenceOverridesRoundTrip();
    ContentFieldsComeFromTheBuildNotTheFile();
    AnUnknownLensIdIsIgnored();
    TheOldArrayFormatKeepsWhatItStillMeans();
    ADeletedRowMigratesToSwitchedOff();
    ARenamedRowIsStillTheShippedLens();
    AHandWrittenLensSurvives();
    MoreLensesThanFitAreDropped();
    AMalformedFileLeavesTheDefaultsAlone();

    std::printf("%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
