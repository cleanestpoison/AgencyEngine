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
        CHECK(text.find("ledgerSlots") == std::string::npos);
        CHECK(text.find("intervalGameMinutes") == std::string::npos);
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
        Begin("a lens the config has never heard of arrives at its shipped weight");

        // A file that predates every builtin but one, and moves that one.
        WriteConfig(R"({ "lenses": { "aspiration": { "weight": 10 } } })");

        Settings s;
        CHECK(s.Load());

        CHECK(Find(s, "aspiration") != nullptr && Find(s, "aspiration")->weight == 10);
        for (const auto& builtin : kBuiltinLenses) {
            const auto* loaded = Find(s, builtin.id);
            CHECK(loaded != nullptr);
            if (loaded && std::string_view{ builtin.id } != "aspiration") {
                CHECK(loaded->weight == builtin.weight);
                CHECK(loaded->ledgerSlots == builtin.ledgerSlots);
                CHECK(std::string_view{ loaded->prompt } == builtin.prompt);
            }
        }
    }

    // Name, prompt file and proposal semantics describe a file in the archive.
    // A config that disagreed with the build about them could only be wrong.
    void ContentFieldsComeFromTheBuildNotTheFile()
    {
        Begin("a config cannot rename a shipped lens or repoint its prompt");

        WriteConfig(R"({ "lenses": { "activity": {
            "weight": 7, "ledgerSlots": 9,
            "name": "Whatever", "prompt": "does_not_exist", "proposal": false } } })");

        Settings s;
        CHECK(s.Load());

        const auto* activity = Find(s, "activity");
        CHECK(activity != nullptr);
        if (activity) {
            CHECK(activity->weight == 7);        // yours
            CHECK(activity->ledgerSlots == 9);   // yours
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
    // predating this change has on disk.
    void TheOldArrayFormatKeepsYourWeights()
    {
        Begin("migrating the old lens array keeps the weights and drops the roster");

        WriteConfig(R"({ "lenses": [
            { "name": "Aspiration", "prompt": "agencyengine_impulse_aspiration", "weight": 5 },
            { "name": "Relationship", "prompt": "agencyengine_impulse_relationship", "weight": 60 },
            { "name": "Activity", "prompt": "agencyengine_impulse_activity", "weight": 35,
              "proposal": true, "ledgerSlots": 4 } ] })");

        Settings s;
        CHECK(s.Load());

        CHECK(Find(s, "aspiration")->weight == 5);
        CHECK(Find(s, "relationship")->weight == 60);
        CHECK(Find(s, "activity")->weight == 35);
        CHECK(Find(s, "activity")->ledgerSlots == 4);

        // And it is written back in the new shape, so the migration happens once.
        CHECK(s.Save());
        const auto text = ReadConfig();
        CHECK(text.find("\"aspiration\"") != std::string::npos);
        CHECK(text.find("agencyengine_impulse_aspiration") == std::string::npos);
    }

    // Deleting a row was how the old settings page said "never ask this". The
    // new page says it with weight 0, and the migration has to carry the intent
    // across — a deleted lens coming back at its shipped weight is the one
    // outcome nobody who deleted it expects.
    void ADeletedRowMigratesToWeightZero()
    {
        Begin("a lens deleted under the old format stays switched off");

        WriteConfig(R"({ "lenses": [
            { "name": "Aspiration", "prompt": "agencyengine_impulse_aspiration", "weight": 50 } ] })");

        Settings s;
        CHECK(s.Load());

        CHECK(Find(s, "aspiration")->weight == 50);
        CHECK(Find(s, "relationship")->weight == 0);
        CHECK(Find(s, "activity")->weight == 0);
        // Switched off, not erased: the prompt file is still there to be raised.
        CHECK(std::string_view{ Find(s, "activity")->prompt } == "agencyengine_impulse_activity");
    }

    // A renamed row is still the shipped lens. The rename is dropped, because
    // the name is content now — but the weight it was carrying is not.
    void ARenamedRowIsStillTheShippedLens()
    {
        Begin("a row renamed under the old format keeps its weight and loses the name");

        WriteConfig(R"({ "lenses": [
            { "name": "Dreams", "prompt": "agencyengine_impulse_aspiration", "weight": 70 },
            { "name": "Relationship", "prompt": "agencyengine_impulse_relationship", "weight": 30 },
            { "name": "Activity", "prompt": "agencyengine_impulse_activity", "weight": 40,
              "proposal": true, "ledgerSlots": 3 } ] })");

        Settings s;
        CHECK(s.Load());

        CHECK(Find(s, "aspiration")->weight == 70);
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
            CHECK(mine->weight == 15);
            CHECK(mine->proposal);
            CHECK(mine->ledgerSlots == 2);
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
    ContentFieldsComeFromTheBuildNotTheFile();
    AnUnknownLensIdIsIgnored();
    TheOldArrayFormatKeepsYourWeights();
    ADeletedRowMigratesToWeightZero();
    ARenamedRowIsStillTheShippedLens();
    AHandWrittenLensSurvives();
    MoreLensesThanFitAreDropped();
    AMalformedFileLeavesTheDefaultsAlone();

    std::printf("%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
