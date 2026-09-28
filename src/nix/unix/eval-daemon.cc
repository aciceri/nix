#include "nix/cmd/command.hh"
#include "nix/cmd/common-eval-args.hh"
#include "nix/cmd/eval-daemon-client.hh"
#include "nix/cmd/installable-flake.hh"
#include "nix/main/shared.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-gc.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/attr-path.hh"
#include "nix/expr/value-to-json.hh"
#include "nix/expr/print.hh"
#include "nix/expr/traced-cells.hh"
#include "nix/flake/flake.hh"
#include "nix/flake/lockfile.hh"
#include "nix/store/globals.hh"
#include "nix/store/outputs-spec.hh"
#include "nix/store/store-api.hh"
#include "nix/util/current-process.hh"
#include "nix/util/file-system.hh"
#include "nix/util/processes.hh"
#include "nix/util/signals.hh"
#include "nix/util/terminal.hh"
#include "nix/util/unix-domain-socket.hh"

#include <nlohmann/json.hpp>

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <chrono>
#include <deque>
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
constexpr unsigned int maxAttempts = 4;

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

/**
 * A request from a `nix` command (see `eval-daemon-socket`) that the
 * daemon cannot honour exactly; the command evaluates locally.
 */
struct UnsupportedRequest : Error
{
    using Error::Error;
};

/**
 * The reply refusing a request from a `nix` command; the command
 * evaluates locally and logs the reason.
 */
json unsupported(std::string_view reason)
{
    return {
        {"ok", false},
        {"unsupported", true},
        {"error", fmt("the evaluation daemon %s", filterANSIEscapes(reason, true))}};
}

/**
 * The rendering of an evaluation error without the `error:` prefix, which
 * the client adds again when it throws the error.
 */
std::string errorText(Error & e)
{
    auto text = filterANSIEscapes(e.msg(), true);
    return text.starts_with("error: ") ? text.substr(7) : text;
}

/**
 * What a request from a `nix` command asks for.
 */
struct Request
{
    std::string what;
    std::string format;
    FlakeRef flakeRef;
    ExtendedOutputsSpec outputs;
    Strings attrPaths;
    flake::LockFlags flags;
};

} // namespace

struct CmdEvalDaemon : MixFlakeOptions, MixReadOnlyOption
{
    std::optional<std::filesystem::path> socketPath;
    bool verify = false;
    std::vector<std::string> warm;
    /**
     * The settings a request from a `nix` command must match
     * (`eval_daemon::forwardedSettings()`).
     */
    json mySettings;

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
            .longName = "warm",
            .description = "Evaluate *installable* when the daemon starts, before serving requests, so that the "
                           "first request finds its cells. Can be given multiple times.",
            .labels = {"installable"},
            .handler = {[&](std::string s) { warm.push_back(s); }},
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

        /* The daemon evaluates client requests with the same code the
           client would use; it must not delegate them to itself. Results
           are kept in memory, not in the on-disk evaluation cache. Cells
           record which values they created (`CellOwner`), which the
           evaluator only does when told so before it exists. */
        evalSettings.evalDaemonSocket.override("");
        evalSettings.useEvalCache.override(false);
        evalSettings.traceCells = true;
        /* Requests must carry the same values (see `decodeRequest()`). */
        mySettings = eval_daemon::forwardedSettings();

        getEvalState()->enableCells();

        for (auto & installable : warm) {
            auto response = handleEval(installable);
            if (!response["ok"].get<bool>())
                printError("warming up with '%s' failed: %s", installable, response["error"].get<std::string>());
            collectIdle();
        }

        if (!socketPath) {
            serve(getStandardInput(), getStandardOutput());
            return;
        }

        /* Take over a socket left behind by a daemon that is gone, never
           one that answers. */
        struct stat st;
        if (lstat(socketPath->c_str(), &st) == 0 && S_ISSOCK(st.st_mode)) {
            try {
                connect(*socketPath);
                throw Error("an evaluation daemon is already listening on '%s'", socketPath->string());
            } catch (SystemError &) {
                std::filesystem::remove(*socketPath);
            }
        }

        auto fdSocket = createUnixDomainSocket(*socketPath, 0600);
        AutoDelete deleteSocket(*socketPath, false);

        notice("listening on '%s'", socketPath->string());

        bool quit = false;
        while (!quit) {
            AutoCloseFD conn = accept(fdSocket.get(), nullptr, nullptr);
            if (!conn) {
                if (errno == EINTR) {
                    /* A termination signal sets the interrupt flag. */
                    checkInterrupt();
                    continue;
                }
                throw SysError("accepting a connection on '%s'", socketPath->string());
            }
            /* A client that connects and sends nothing must not hold the
               daemon. */
            struct pollfd pfd{.fd = conn.get(), .events = POLLIN};
            if (poll(&pfd, 1, 5000) <= 0)
                continue;
            quit = serve(conn.get(), conn.get());
        }
    }

    /**
     * Serve line requests until EOF, or one request from a `nix` command.
     * Returns true on `quit`.
     */
    bool serve(Descriptor in, Descriptor out)
    {
        while (true) {
            std::string line;
            try {
                line = readLine(in);
            } catch (EndOfFile &) {
                return false;
            } catch (SystemError & e) {
                printError("could not read a request: %s", e.msg());
                return false;
            }

            line = trim(line);
            if (line.empty())
                continue;

            auto space = line.find(' ');
            auto command = line.substr(0, space);
            auto arg = space == std::string::npos ? "" : trim(line.substr(space + 1));

            json response;
            /* Whether the request left garbage to collect. */
            bool evaluated = false;
            if (line.starts_with("{")) {
                /* A request from a `nix` command (see `eval-daemon-socket`):
                   decoded and refused before a generation starts. */
                command = "json";
                try {
                    auto request = decodeRequest(json::parse(line));
                    evaluated = true;
                    response = handleRequest(
                        request.what, [&](EvalState & state) { return evaluateRequest(state, request); }, false);
                } catch (UnsupportedRequest & e) {
                    response = unsupported(e.info().msg.str());
                } catch (json::exception & e) {
                    response = unsupported(fmt("received a malformed request: %s", e.what()));
                } catch (Error & e) {
                    /* A flake reference or output spec it cannot parse. */
                    response = unsupported(fmt("received a malformed request: %s", errorText(e)));
                }
            } else if (command == "eval" && !arg.empty()) {
                evaluated = true;
                response = handleEval(arg);
            } else if (command == "stats") {
                auto state = getEvalState();
                response = {{"ok", true}, {"stats", state->getStatistics()}, {"cells", state->cells->statsJson()}};
            } else if (command == "reset") {
                getEvalState()->resetFileCache();
                evaluated = true;
                response = {{"ok", true}};
            } else if (command == "quit")
                response = {{"ok", true}};
            else
                response = {
                    {"ok", false},
                    {"error",
                     fmt("unknown request '%s' (expected 'eval <installable>', 'stats', 'reset' or 'quit')", line)}};

            /* Error messages may still hold virtual paths. */
            auto text = response.dump();
            getEvalState()->realizeStrings(text);
            try {
                writeFull(out, text + "\n");
            } catch (SystemError & e) {
                /* The client went away (interrupted, or gave up waiting):
                   the result stays in memory for the next request. */
                printError("could not send the reply: %s", e.msg());
            }

            if (evaluated)
                collectIdle();
            /* A `nix` command sends one request per connection. */
            if (command == "json")
                return false;
            if (command == "quit")
                return true;
        }
    }

    json handleEval(const std::string & installable)
    {
        return handleRequest(installable, [&](EvalState & state) { return evaluate(state, installable); }, verify);
    }

    /**
     * Evaluate one request with `body`, evaluating again from scratch
     * when reused cells turn out invalid, and report statistics.
     */
    json handleRequest(const std::string & installable, std::function<json(EvalState &)> body, bool verify)
    {
        auto state = getEvalState();
        state->startGeneration();

        auto start = std::chrono::steady_clock::now();
        auto before = state->getStatistics();
        auto cellsBefore = state->cells->stats;

        /* The previous request's garbage was collected after it
           responded (see `serve()`); remove any stale pointers left on the
           stack since then. */
#ifdef __linux__
        clearUnusedStack();
#endif
        auto prepareTime = secondsSince(start);
        json response;
        unsigned int attempts = 0;
        /* Set once the request is being evaluated without reusing any
           cell or evaluated file. */
        bool fromScratch = false;
        while (true) {
            attempts++;
            enum class Next { Done, Retry, FromScratch } next = Next::Done;
            try {
                response = body(*state);
                /* Reused cells may have deferred part of their validation
                   until the result is known. */
                if (!state->cells->checkDeferred(*state))
                    next = Next::Retry;
                /* Reused values do not write their store derivations
                   again; if a garbage collection deleted one, the result
                   is not in the store (a valid path's references are
                   valid, so checking the result suffices). */
                else if (!fromScratch && !settings.readOnlyMode && !resultPathsValid(*state, response))
                    next = Next::FromScratch;
            } catch (CellInvalidated &) {
                next = Next::Retry;
            } catch (InvalidPath & e) {
                /* Typically a new derivation depending on a store
                   derivation of a reused value that was garbage
                   collected; the daemon's own failure if it recurs from
                   scratch. */
                if (!fromScratch)
                    next = Next::FromScratch;
                else {
                    state->resetFileCache();
                    response = unsupported(fmt("failed: %s", errorText(e)));
                }
            } catch (Error & e) {
                /* An error may come from a reused cell that turns out to
                   be invalid; otherwise it is the result. A failed
                   evaluation may leave failed thunks inside cached file
                   values and inside the cells it touched, and the failure
                   may not recur (for example a fetch or build error), so
                   the next request starts without them; cells the request
                   did not touch stay. */
                if (!state->cells->checkDeferred(*state))
                    next = Next::Retry;
                else {
                    state->resetFileCache(true);
                    state->cells->dropGeneration(*state, state->getGeneration());
                    response = {{"ok", false}, {"error", errorText(e)}};
                }
            }
            if (next == Next::Done)
                break;
            if (attempts == maxAttempts - 1 && next == Next::Retry) {
                /* Cells keep being invalidated (deferred checks that fail
                   at the end of every attempt): the last attempt evaluates
                   without them. */
                notice(
                    "generation %d: traced cells kept being invalidated, evaluating without them",
                    state->getGeneration());
                state->resetFileCache();
                state->cells->suspended = true;
                fromScratch = true;
                continue;
            }
            if (attempts == maxAttempts) {
                state->resetFileCache();
                response = unsupported("kept finding its reused values invalid");
                break;
            }
            if (next == Next::FromScratch) {
                notice(
                    "generation %d: reused values refer to store derivations that are gone, evaluating again from scratch",
                    state->getGeneration());
                state->resetFileCache();
                fromScratch = true;
            } else {
                /* Values computed in this attempt may derive from the
                   invalid cells: the cached files and the instances
                   created or bound in this generation are dropped.
                   Instances the request did not touch (other hosts,
                   other flakes) stay. The site that failed rejects
                   eagerly from now on (`CellTable::noDeferralSites`). */
                notice(
                    "generation %d: reused traced cells were invalid (%s), evaluating again from scratch",
                    state->getGeneration(),
                    state->cells->stats.lastRejection);
                state->resetFileCache(true);
                state->cells->dropGeneration(*state, state->getGeneration());
                state->cells->noDeferrals = true;
                fromScratch = true;
            }
        }

        state->cells->suspended = state->cells->noDeferrals = false;
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
        auto & now = state->cells->stats;
        auto cells = state->cells->statsJson();
        cells["hits"] = now.hits - cellsBefore.hits;
        cells["misses"] = now.misses - cellsBefore.misses;
        cells["rejected"] = now.rejected - cellsBefore.rejected;
        cells["deferred"] = now.deferred - cellsBefore.deferred;
        cells["invalidated"] = now.invalidated - cellsBefore.invalidated;
        cells["validationTime"] = now.validationSeconds - cellsBefore.validationSeconds;
        response["stats"]["cells"] = std::move(cells);

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
     * Collect the request's garbage while idle, so that the next request
     * rarely needs a collection (which marks every retained cell). The
     * stack is cleared first so that the request's stale pointers do not
     * keep its values alive; free heap blocks are returned to the
     * operating system if Boehm was built to do that.
     */
    void collectIdle()
    {
#ifdef __linux__
        clearUnusedStack();
#endif
#if NIX_USE_BOEHMGC
        GC_gcollect_and_unmap();
#endif
    }

    /**
     * Store paths of the most recent copies of the evaluated flakes. Every
     * edit gives a flake a new store path, and its files are evaluated
     * again under it; files under older copies are dropped, except for a
     * few recent ones (an edit is often undone).
     */
    std::deque<std::string> recentRoots;
    static constexpr size_t maxRecentRoots = 8;

    void rememberRoot(EvalState & state, const std::string & root)
    {
        if (auto i = std::ranges::find(recentRoots, root); i != recentRoots.end())
            recentRoots.erase(i);
        recentRoots.push_front(root);
        while (recentRoots.size() > maxRecentRoots) {
            state.dropFileCacheUnder(recentRoots.back());
            recentRoots.pop_back();
        }
    }

    /**
     * Whether every store path a response refers to is valid (the
     * derivations of a result that reused cells may have been collected).
     */
    bool resultPathsValid(EvalState & state, const json & response)
    {
        std::vector<StorePath> paths;
        auto derived = [&](const json & s) {
            paths.push_back(DerivedPath::parse(*state.store, s.get<std::string>()).getBaseStorePath());
        };
        auto kind = response.value("kind", "");
        if (kind == "drvPath")
            paths.push_back(state.store->parseStorePath(response.at("value").get<std::string>()));
        else if (kind == "derivedPaths")
            for (auto & item : response.at("derivedPaths"))
                derived(item.at("path"));
        else if (kind == "app")
            for (auto & s : response.at("app").at("context"))
                derived(s);
        for (auto & p : response.value("paths", json::array()))
            paths.push_back(state.store->parseStorePath(p.get<std::string>()));
        for (auto & p : paths)
            if (!state.store->isValidPath(p))
                return false;
        return true;
    }

    /**
     * Lock the flake for this generation and tell the cells about its
     * root.
     */
    flake::LockedFlake lockForRequest(EvalState & state, const FlakeRef & flakeRef, const flake::LockFlags & flags)
    {
        auto locked = flake::lockFlake(flakeSettings, state, flakeRef, flags);
        auto root = locked.flake.path.parent().path.abs() + "/";
        /* The flake's own files are known under its stable root. */
        state.cells->excludedRoot = state.rootPath(locked.flake.path.parent().path).path.abs() + "/";
        rememberRoot(state, root);
        return locked;
    }

    /**
     * Decode a request from a `nix` command, refusing what the daemon
     * cannot honour exactly: another system, store or settings, a result
     * format it does not know. Throws `UnsupportedRequest` or a JSON
     * exception.
     */
    Request decodeRequest(const json & request)
    {
        if (request.at("version") != 1)
            throw UnsupportedRequest("speaks version 1 of the request protocol");
        if (request.at("request") != "installable")
            throw UnsupportedRequest("does not know the request kind '%s'", request.at("request").get<std::string>());
        if (request.at("system") != evalSettings.getCurrentSystem())
            throw UnsupportedRequest("evaluates for system '%s'", evalSettings.getCurrentSystem());
        auto storeDir = getEvalState()->store->storeDir;
        if (request.at("storeDir") != storeDir)
            throw UnsupportedRequest("uses the store directory '%s'", storeDir);
        for (auto & [name, value] : mySettings.items())
            if (request.at("settings").value(name, "") != value.get<std::string>())
                throw UnsupportedRequest("runs with '%s = %s'", name, value.get<std::string>());

        auto format = request.at("format").get<std::string>();
        if (format != "derivations" && format != "json" && format != "raw" && format != "nix" && format != "app")
            throw UnsupportedRequest("does not know the result format '%s'", format);

        /* The daemon's own lock-file flags apply to every request. */
        flake::LockFlags flags;
        auto & lf = request.at("lockFlags");
        flags.writeLockFile = lockFlags.writeLockFile && lf.at("writeLockFile").get<bool>();
        flags.updateLockFile = lockFlags.updateLockFile && lf.at("updateLockFile").get<bool>();
        flags.failOnUnlocked = lf.at("failOnUnlocked").get<bool>();
        flags.allowUnlocked = lf.at("allowUnlocked").get<bool>();
        if (!lf.at("useRegistries").is_null())
            flags.useRegistries = lf.at("useRegistries").get<bool>();
        for (auto & [path, ref] : lf.at("inputOverrides").items()) {
            auto attrPath = flake::NonEmptyInputAttrPath::parse(path);
            if (!attrPath)
                throw UnsupportedRequest("cannot override the input '%s'", path);
            flags.inputOverrides.emplace(*attrPath, parseFlakeRef(ref.get<std::string>()));
        }
        for (auto & path : lf.at("inputUpdates")) {
            auto attrPath = flake::NonEmptyInputAttrPath::parse(path.get<std::string>());
            if (!attrPath)
                throw UnsupportedRequest("cannot update the input '%s'", path.get<std::string>());
            flags.inputUpdates.insert(*attrPath);
        }

        Strings attrPaths;
        for (auto & s : request.at("attrPaths"))
            attrPaths.push_back(s.get<std::string>());
        if (attrPaths.empty())
            throw UnsupportedRequest("needs at least one attribute path");

        return {
            .what = request.at("what").get<std::string>(),
            .format = format,
            .flakeRef = parseFlakeRef(request.at("flakeRef").get<std::string>()),
            .outputs = ExtendedOutputsSpec::parse(request.at("outputs").get<std::string>()).second,
            .attrPaths = std::move(attrPaths),
            .flags = std::move(flags),
        };
    }

    /**
     * Evaluate a request from a `nix` command: the installable as the
     * command resolved it (attribute paths in order, outputs, lock flags),
     * with the code the command would use, in the format it needs.
     */
    json evaluateRequest(EvalState & state, const Request & request)
    {
        InstallableFlake installable(
            nullptr,
            getEvalState(),
            FlakeRef(request.flakeRef),
            "",
            request.outputs,
            request.attrPaths,
            {},
            request.flags);
        auto locked = lockForRequest(state, installable.flakeRef, request.flags);
        /* The command applies the flake's `nixConfig` to its own settings
           (the daemon's are fixed), and evaluates locally if that changes
           one the result depends on. */
        json nixConfig = json::object();
        for (auto & [name, value] : locked.flake.config.settings)
            std::visit(
                overloaded{
                    [&](const Explicit<bool> & b) { nixConfig[name] = b.t; },
                    [&](const auto & v) { nixConfig[name] = v; },
                },
                value);
        installable._lockedFlake = std::make_shared<flake::LockedFlake>(std::move(locked));

        json response = {{"ok", true}, {"nixConfig", std::move(nixConfig)}};

        if (request.format == "derivations") {
            response["kind"] = "derivedPaths";
            response["derivedPaths"] = json::array();
            for (auto & p : installable.toDerivedPaths()) {
                auto & info = dynamic_cast<ExtraPathInfoFlake &>(*p.info);
                response["derivedPaths"].push_back({
                    {"path", p.path.to_string(*state.store)},
                    {"priority", info.value.priority ? json(*info.value.priority) : json(nullptr)},
                    {"attrPath", info.value.attrPath},
                    {"outputs", info.value.extendedOutputsSpec.to_string()},
                    {"lockedRef", info.flake.lockedRef.to_string()},
                });
            }
        } else if (request.format == "app") {
            auto app = installable.toApp(state).unresolved;
            response["kind"] = "app";
            json ctx = json::array();
            for (auto & p : app.context)
                ctx.push_back(p.to_string(*state.store));
            response["app"] = {{"program", app.program.string()}, {"context", std::move(ctx)}};
        } else {
            auto [v, pos] = installable.toValue(state, AutoCall::No);
            NixStringContext context;
            response["kind"] = "json";
            if (request.format == "raw")
                response["value"] =
                    std::string(*state.coerceToString(noPos, *v, context, "while generating the eval command output"));
            else if (request.format == "nix") {
                ValuePrinter printer(state, *v, PrintOptions{.force = true, .derivationPaths = true}, &context);
                response["value"] = fmt("%s", printer);
            } else
                response["value"] = printValueAsJSON(state, true, *v, pos, context, false);
            /* Strings of stable roots are virtual until they leave the
               evaluator: the result is the boundary. */
            auto text = response["value"].dump();
            if (state.realizeStrings(text, &context))
                response["value"] = json::parse(text);
            state.ensureLazyPathsCopied(context);
            /* The store paths the rendering refers to, which the client
               checks are valid in its store. */
            response["paths"] = json::array();
            for (auto & c : context)
                response["paths"].push_back(state.store->printStorePath(
                    std::visit(
                        overloaded{
                            [](const NixStringContextElem::Opaque & o) -> const StorePath & { return o.path; },
                            [](const NixStringContextElem::DrvDeep & d) -> const StorePath & { return d.drvPath; },
                            [](const NixStringContextElem::Built & b) -> const StorePath & {
                                return b.drvPath->getBaseStorePath();
                            },
                        },
                        c.raw)));
        }
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

        auto locked = lockForRequest(state, flakeRef, lockFlags);
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
            /* Strings of stable roots are virtual until they leave the
               evaluator: the result is the boundary. */
            auto text = response["value"].dump();
            if (state.realizeStrings(text))
                response["value"] = json::parse(text);
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
        OsStrings args{"eval", "--no-eval-cache", "--option", "eval-daemon-socket", ""};
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
