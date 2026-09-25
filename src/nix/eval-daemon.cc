#include "nix/cmd/command.hh"
#include "nix/cmd/common-eval-args.hh"
#include "nix/main/shared.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/attr-path.hh"
#include "nix/expr/value-to-json.hh"
#include "nix/expr/traced-cells.hh"
#include "nix/flake/flake.hh"
#include "nix/store/globals.hh"
#include "nix/store/store-api.hh"
#include "nix/util/current-process.hh"
#include "nix/util/file-system.hh"
#include "nix/util/processes.hh"
#include "nix/util/terminal.hh"
#include "nix/util/unix-domain-socket.hh"

#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/stat.h>

#include <chrono>
#include <fstream>

namespace nix {

using json = nlohmann::json;

namespace {

#ifdef __linux__
/**
 * Zero the part of the main thread's stack below the current frame.
 *
 * Boehm scans the active stack conservatively. A deep evaluation runs over
 * stack slots where the previous deep evaluation left pointers (padding,
 * unwritten locals), so a collection during it would keep much of the
 * previous request's values alive (measured: +1.5 GiB live heap per
 * request on a NixOS workstation). Clearing the stack before each request
 * removes them without a full collection over the (large) live heap.
 *
 * Returns false if the stack could not be located, for example when not
 * called on the main thread.
 */
[[gnu::noinline]] bool clearUnusedStack()
{
    uintptr_t low = 0;
    {
        std::ifstream maps("/proc/self/maps");
        std::string line;
        while (std::getline(maps, line))
            if (line.ends_with("[stack]")) {
                low = std::stoull(line.substr(0, line.find('-')), nullptr, 16);
                break;
            }
    }
    volatile char marker = 0;
    auto sp = reinterpret_cast<uintptr_t>(&marker);
    /* Leave a margin below this frame; the loop makes no calls. */
    auto end = sp - 4096;
    if (!low || low >= end)
        return false;
    for (auto p = reinterpret_cast<volatile uintptr_t *>(low); p < reinterpret_cast<volatile uintptr_t *>(end); ++p)
        *p = 0;
    return true;
}
#endif

/**
 * Evaluations of one request after reused traced cells were found invalid.
 */
constexpr unsigned int maxAttempts = 5;

double secondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

/**
 * Full-GC time from `EvalState::getStatistics()`; the key depends on the
 * GC mode.
 */
double gcTime(const json & stats)
{
    auto & time = stats.at("time");
    for (auto key : {"gc", "gcNonIncremental"})
        if (time.contains(key))
            return time.at(key).get<double>();
    return 0;
}

} // namespace

struct CmdEvalDaemon : MixFlakeOptions, MixReadOnlyOption
{
    std::optional<std::filesystem::path> socketPath;
    bool verify = false;
    bool cells = true;
    bool flakeCells = true;
    std::vector<std::string> cellFiles;
    size_t maxCells = 128;

    CmdEvalDaemon()
    {
        addFlag({
            .longName = "socket",
            .description = "Listen on the Unix domain socket *path* instead of reading requests from standard input.",
            .labels = {"path"},
            .handler = {&socketPath},
        });
        addFlag({
            .longName = "verify",
            .description = "After every `eval` request, evaluate the same installable cold in a child process "
                           "and report whether the results are identical.",
            .handler = {&verify, true},
        });
        addFlag({
            .longName = "no-cells",
            .description = "Only reuse evaluated files; do not reuse applications of cell sites.",
            .handler = {&cells, false},
        });
        addFlag({
            .longName = "no-flake-cells",
            .description = "Do not reuse the outputs of the flake's inputs as cells.",
            .handler = {&flakeCells, false},
        });
        addFlag({
            .longName = "cell-file",
            .description =
                "Make the file-level function of files whose path ends with *suffix* a cell site. "
                "Can be given multiple times; the default is `/pkgs/top-level/impure.nix` (`import nixpkgs { ... }`).",
            .labels = {"suffix"},
            .handler = {[&](std::string s) { cellFiles.push_back(s); }},
        });
        addFlag({
            .longName = "max-cells",
            .description = "Keep at most *n* cell instances (default: 128).",
            .labels = {"n"},
            .handler = {[&](std::string s) { maxCells = string2IntWithUnitPrefix<size_t>(s); }},
        });
    }

    std::string description() override
    {
        return "run a resident evaluator that reuses evaluated files across requests";
    }

    std::string doc() override
    {
        return
#include "eval-daemon.md"
            ;
    }

    Category category() override
    {
        return catSecondary;
    }

    std::optional<ExperimentalFeature> experimentalFeature() override
    {
        return Xp::EvalDaemon;
    }

    void run(ref<Store> store) override
    {
        /* Cached file values are only reusable across requests if nothing
           they depend on can change, see `EvalState::startGeneration()`. */
        if (!evalSettings.pureEval)
            throw UsageError("'nix eval-daemon' does not support impure evaluation");

        if (verify && (!lockFlags.inputOverrides.empty() || !lockFlags.inputUpdates.empty()))
            throw UsageError("'--verify' cannot be combined with flags that change the lock file");

        if (cells) {
            auto state = getEvalState();
            state->enableCells(
                cellFiles.empty() ? std::vector<std::string>{"/pkgs/top-level/impure.nix"} : cellFiles, maxCells);
            state->cells->flakeOutputs = flakeCells;
        }

        if (!socketPath) {
            serve(getStandardInput(), getStandardOutput());
            return;
        }

        struct stat st;
        if (lstat(socketPath->c_str(), &st) == 0 && S_ISSOCK(st.st_mode))
            std::filesystem::remove(*socketPath);

        auto fdSocket = createUnixDomainSocket(*socketPath, 0600);
        AutoDelete deleteSocket(*socketPath, false);

        notice("listening on '%s'", socketPath->string());

        bool quit = false;
        while (!quit) {
            AutoCloseFD conn = accept(fdSocket.get(), nullptr, nullptr);
            if (!conn) {
                if (errno == EINTR)
                    continue;
                throw SysError("accepting a connection on '%s'", socketPath->string());
            }
            quit = serve(conn.get(), conn.get());
        }
    }

    /**
     * Serve requests until EOF. Returns true on `quit`.
     */
    bool serve(Descriptor in, Descriptor out)
    {
        while (true) {
            std::string line;
            try {
                line = readLine(in);
            } catch (EndOfFile &) {
                return false;
            }

            line = trim(line);
            if (line.empty())
                continue;

            auto space = line.find(' ');
            auto command = line.substr(0, space);
            auto arg = space == std::string::npos ? "" : trim(line.substr(space + 1));

            json response;
            if (command == "eval" && !arg.empty())
                response = handleEval(arg);
            else if (command == "stats") {
                auto state = getEvalState();
                response = {{"ok", true}, {"stats", state->getStatistics()}};
                if (state->cells)
                    response["cells"] = state->cells->instancesJson(*state);
            } else if (command == "gc" || command == "reset") {
                auto state = getEvalState();
                if (command == "reset")
                    state->resetFileCache();
                state->fullGC();
                auto stats = state->getStatistics();
                response = {{"ok", true}};
                if (stats.contains("gc")) {
                    auto & gc = stats["gc"];
                    response["heapSize"] = gc["heapSize"];
                    response["liveBytes"] = gc["heapSize"].get<uint64_t>() - gc["freeBytes"].get<uint64_t>()
                                            - gc["unmappedBytes"].get<uint64_t>();
                }
            } else if (command == "quit")
                response = {{"ok", true}};
            else
                response = {
                    {"ok", false},
                    {"error",
                     fmt("unknown request '%s' (expected 'eval <installable>', 'stats', 'gc', 'reset' or 'quit')",
                         line)}};

            writeFull(out, response.dump() + "\n");

            if (command == "quit")
                return true;

            /* Collect the request's garbage while idle, so that the next
               request rarely needs a collection (which marks every
               retained cell). The stack is cleared first so that the
               request's stale pointers do not keep its values alive. */
            if (command == "eval") {
#ifdef __linux__
                clearUnusedStack();
#endif
                getEvalState()->fullGC();
            }
        }
    }

    json handleEval(const std::string & installable)
    {
        auto state = getEvalState();
        state->startGeneration();

        auto start = std::chrono::steady_clock::now();
        auto before = state->getStatistics();
        auto cellsBefore = state->cells ? state->cells->stats : CellTable::Stats{};

        /* The previous request's garbage was collected after it
           responded (see `serve()`); remove any stale pointers left on the
           stack since then. */
#ifdef __linux__
        clearUnusedStack();
#endif
        auto prepareTime = secondsSince(start);
        json response;
        unsigned int attempts = 0;
        while (true) {
            attempts++;
            bool retry = false;
            try {
                response = evaluate(*state, installable);
                /* Reused cells may have deferred part of their validation
                   until the result is known. */
                retry = state->cells && !state->cells->checkDeferred(*state);
            } catch (CellInvalidated &) {
                retry = true;
            } catch (Error & e) {
                /* An error may come from a reused cell that turns out to
                   be invalid; otherwise it is the result. A failed
                   evaluation may leave failed thunks inside cached file
                   values, and the failure may not recur (for example a
                   fetch or build error), so start the next request clean. */
                if (state->cells && !state->cells->checkDeferred(*state))
                    retry = true;
                else {
                    state->resetFileCache();
                    response = {{"ok", false}, {"error", filterANSIEscapes(e.msg(), true)}};
                }
            }
            if (!retry)
                break;
            if (attempts == maxAttempts) {
                state->resetFileCache();
                response = {{"ok", false}, {"error", "traced cells kept being invalidated"}};
                break;
            }
            notice("generation %d: reused traced cells were invalid, evaluating again", state->getGeneration());
            state->cells->retry();
        }

        auto after = state->getStatistics();
        response["stats"] = {
            {"generation", state->getGeneration()},
            {"wallTime", secondsSince(start)},
            {"prepareTime", prepareTime},
            {"attempts", attempts},
            {"cpuTime", after.at("cpuTime").get<double>() - before.at("cpuTime").get<double>()},
            {"gcTime", gcTime(after) - gcTime(before)},
            {"fileEvalCacheSize", state->fileEvalCacheSize()},
        };
        if (after.contains("gc"))
            response["stats"]["heapSize"] = after["gc"]["heapSize"];
        /* Evaluator counters only count with NIX_SHOW_STATS set. */
        if (Counter::enabled)
            for (auto key : {"nrThunks", "nrFunctionCalls"})
                response["stats"][key] = after.at(key).get<uint64_t>() - before.at(key).get<uint64_t>();
        if (state->cells) {
            auto & now = state->cells->stats;
            auto cells = state->cells->statsJson();
            cells["hits"] = now.hits - cellsBefore.hits;
            cells["misses"] = now.misses - cellsBefore.misses;
            cells["rejected"] = now.rejected - cellsBefore.rejected;
            cells["deferred"] = now.deferred - cellsBefore.deferred;
            cells["invalidated"] = now.invalidated - cellsBefore.invalidated;
            cells["validationTime"] = now.validationSeconds - cellsBefore.validationSeconds;
            response["stats"]["cells"] = std::move(cells);
        }

        notice(
            "generation %d: %s: %s in %.2f s",
            state->getGeneration(),
            installable,
            response["ok"].get<bool>() ? "ok" : "error",
            response["stats"]["cpuTime"].get<double>());

        if (verify && response["ok"].get<bool>())
            response["verify"] = verifyCold(installable, response);

        return response;
    }

    /**
     * Evaluate `<flakeref>#<attrpath>`. The attribute path is always
     * absolute (no `packages.<system>` probing); a leading `.` is accepted
     * for symmetry with other commands.
     */
    json evaluate(EvalState & state, const std::string & installable)
    {
        auto [flakeRef, fragment] = parseFlakeRefWithFragment(installable, std::filesystem::current_path());
        auto attrPath = fragment.starts_with(".") ? fragment.substr(1) : fragment;

        auto locked = flake::lockFlake(flakeSettings, state, flakeRef, lockFlags);
        if (state.cells)
            state.cells->excludedRoot = locked.flake.path.parent().path.abs() + "/";
        auto vFlake = state.allocValue();
        flake::callFlake(state, locked, *vFlake);

        auto [v, pos] = findAlongAttrPath(state, attrPath, *getAutoArgs(state), *vFlake);
        state.forceValue(*v, pos);

        json response = {{"ok", true}};
        NixStringContext context;
        if (state.isDerivation(*v)) {
            auto aDrvPath = v->attrs()->get(state.s.drvPath);
            if (!aDrvPath)
                throw Error("derivation '%s' has no 'drvPath' attribute", attrPath);
            auto drvPath = state.coerceToStorePath(
                aDrvPath->pos, *aDrvPath->value, context, "while evaluating the 'drvPath' of a derivation");
            response["kind"] = "drvPath";
            response["value"] = state.store->printStorePath(drvPath);
        } else {
            response["kind"] = "json";
            response["value"] = printValueAsJSON(state, true, *v, pos, context, false);
        }
        state.ensureLazyPathsCopied(context);
        return response;
    }

    /**
     * Evaluate the installable with `nix eval` in a child process, without
     * the evaluation cache, and compare.
     */
    json verifyCold(const std::string & installable, const json & response)
    {
        auto hash = installable.find('#');
        auto flakeRefS = installable.substr(0, hash);
        auto attrPath = hash == std::string::npos ? "" : installable.substr(hash + 1);
        if (attrPath.starts_with("."))
            attrPath = attrPath.substr(1);

        bool isDrv = response["kind"] == "drvPath";
        OsStrings args{"eval", "--no-eval-cache"};
        if (settings.readOnlyMode)
            args.push_back("--read-only");
        args.push_back(isDrv ? "--raw" : "--json");
        args.push_back(string_to_os_string(
            flakeRefS + "#." + attrPath + (isDrv ? (attrPath.empty() ? "drvPath" : ".drvPath") : "")));

        auto start = std::chrono::steady_clock::now();
        auto [status, out] = runProgram(
            RunOptions{
                .program = getSelfExe().value_or("nix"),
                .args = args,
            });

        json result = {{"wallTime", secondsSince(start)}};
        if (!statusOk(status)) {
            result["matches"] = false;
            result["error"] = statusToString(status);
        } else {
            json cold = isDrv ? json(trim(out)) : json::parse(out);
            result["matches"] = cold == response["value"];
            result["value"] = cold;
        }
        if (!result["matches"].get<bool>())
            printError("verify: '%s' differs from a cold evaluation", installable);
        return result;
    }
};

static auto rCmdEvalDaemon = registerCommand<CmdEvalDaemon>("eval-daemon");

} // namespace nix
