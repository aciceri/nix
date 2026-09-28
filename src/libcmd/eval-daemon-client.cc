#include "nix/cmd/eval-daemon-client.hh"
#include "nix/cmd/common-eval-args.hh"
#include "nix/cmd/installable-flake.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/fetchers/registry.hh"
#include "nix/flake/flake.hh"
#include "nix/flake/lockfile.hh"
#include "nix/store/globals.hh"
#include "nix/store/store-api.hh"
#include "nix/util/config-global.hh"
#include "nix/util/serialise.hh"
#include "nix/util/unix-domain-socket.hh"

#ifndef _WIN32
#  include "nix/cmd/unix-socket-server.hh"
#  include <unistd.h>
#endif

#include <nlohmann/json.hpp>

namespace nix::eval_daemon {

using json = nlohmann::json;

json forwardedSettings()
{
    std::map<std::string, Config::SettingInfo> all;
    globalConfig.getSettings(all);
    json res = {{"read-only", settings.readOnlyMode ? "true" : "false"}};
    for (auto name :
         {"pure-eval",
          "restrict-eval",
          "allow-import-from-derivation",
          "max-call-depth",
          "use-registries",
          "flake-registry",
          "tarball-ttl",
          "experimental-features"})
        res[name] = all.at(name).value;
    return res;
}

namespace {

/**
 * Log why a request is evaluated locally (informational: shown by
 * default only when standard error is not a terminal).
 */
void fallback(std::string_view what, std::string_view reason)
{
    printInfo("evaluating '%s' locally: %s", what, reason);
}

/**
 * The request the client would send for `installable` in `format`, or
 * nothing if the request cannot be delegated.
 */
std::optional<json> makeRequest(const InstallableFlake & installable, std::string_view format)
{
    auto & evalSettings = installable.state->settings;
    if (evalSettings.evalDaemonSocket.get().empty())
        return std::nullopt;

    auto what = installable.what();

    if (!evalSettings.pureEval) {
        fallback(what, "the evaluation daemon only evaluates in pure mode");
        return std::nullopt;
    }

    auto & lockFlags = installable.lockFlags;
    if (lockFlags.recreateLockFile || lockFlags.commitLockFile || lockFlags.referenceLockFilePath
        || lockFlags.outputLockFilePath) {
        fallback(what, "lock-file flags the evaluation daemon does not support");
        return std::nullopt;
    }

    if (!fetchers::getFlagRegistry()->entries.empty()) {
        fallback(what, "'--override-flake' is not passed to the evaluation daemon");
        return std::nullopt;
    }

    json overrides = json::object();
    for (auto & [path, ref] : lockFlags.inputOverrides)
        overrides[flake::printInputAttrPath(path)] = ref.to_string();
    json updates = json::array();
    for (auto & path : lockFlags.inputUpdates)
        updates.push_back(flake::printInputAttrPath(path));

    /* The attribute paths as the command resolves them, in order; a
       fragment starting with `.` is already absolute. */
    json attrPaths = json::array();
    if (installable.attrPaths.size() == 1 && installable.attrPaths.front().starts_with("."))
        attrPaths.push_back(installable.attrPaths.front().substr(1));
    else {
        for (auto & prefix : installable.prefixes)
            attrPaths.push_back(prefix + installable.attrPaths.front());
        for (auto & s : installable.attrPaths)
            attrPaths.push_back(s);
    }

    return json{
        {"version", 1},
        {"request", "installable"},
        {"what", what},
        {"format", format},
        {"flakeRef", installable.flakeRef.to_string()},
        {"attrPaths", attrPaths},
        {"outputs", installable.extendedOutputsSpec.to_string()},
        {"system", evalSettings.getCurrentSystem()},
        {"storeDir", installable.state->store->storeDir},
        {"settings", forwardedSettings()},
        {"lockFlags",
         {
             {"writeLockFile", lockFlags.writeLockFile},
             {"updateLockFile", lockFlags.updateLockFile},
             {"failOnUnlocked", lockFlags.failOnUnlocked},
             {"allowUnlocked", lockFlags.allowUnlocked},
             {"useRegistries", lockFlags.useRegistries ? json(*lockFlags.useRegistries) : json(nullptr)},
             {"inputOverrides", overrides},
             {"inputUpdates", updates},
         }},
    };
}

/**
 * Send a request and return the daemon's reply if it is `ok`. Returns
 * nothing (logging) if the daemon cannot be reached, is not run by this
 * user, or reports the request as unsupported.
 */
std::optional<json> send(const std::string & socketPath, const json & request)
{
    auto what = request.at("what").get<std::string>();
#ifdef _WIN32
    fallback(what, "the evaluation daemon is not supported on this platform");
    return std::nullopt;
#else
    std::string line;
    try {
        auto fd = connect(std::filesystem::path(socketPath));
        /* The reply is trusted: only talk to a daemon of this user or of
           root. */
        auto peer = unix::getPeerInfo(fd.get());
        if (!peer.uid || (*peer.uid != geteuid() && *peer.uid != 0)) {
            fallback(what, fmt("the evaluation daemon at '%s' is not run by this user", socketPath));
            return std::nullopt;
        }
        writeFull(fd.get(), request.dump() + "\n");
        FdSource from(fd.get());
        line = from.readLine();
    } catch (SystemError & e) {
        fallback(what, fmt("no evaluation daemon at '%s' (%s)", socketPath, e.msg()));
        return std::nullopt;
    } catch (EndOfFile &) {
        fallback(what, fmt("the evaluation daemon at '%s' closed the connection", socketPath));
        return std::nullopt;
    }

    json reply;
    try {
        reply = json::parse(line);
        if (!reply.is_object())
            throw Error("not an object");
        if (reply.value("ok", false)) {
            printInfo(
                "evaluated '%s' by the evaluation daemon at '%s' in %.2f s",
                what,
                socketPath,
                reply.at("stats").at("wallTime").get<double>());
            return reply;
        }
        auto error = reply.at("error").get<std::string>();
        if (reply.value("unsupported", false)) {
            fallback(what, error);
            return std::nullopt;
        }
        throw Error("%s", error);
    } catch (json::exception & e) {
        fallback(what, fmt("the evaluation daemon sent an invalid reply: %s", e.what()));
        return std::nullopt;
    }
#endif
}

/**
 * Whether every store path is valid, holding it as a temporary root so
 * that it stays so; logs the first missing one. In read-only mode a local
 * evaluation would not have written them either, so nothing is checked.
 */
bool pathsValid(InstallableFlake & installable, const std::vector<StorePath> & paths)
{
    auto & store = *installable.state->store;
    if (settings.readOnlyMode)
        return true;
    for (auto & p : paths) {
        store.addTempRoot(p);
        if (!store.isValidPath(p)) {
            fallback(
                installable.what(),
                fmt("'%s' returned by the evaluation daemon is not valid in this store", store.printStorePath(p)));
            return false;
        }
    }
    return true;
}

/**
 * Derived paths of the reply, or nothing if one of them is not valid in
 * the store (the daemon writes to another store).
 */
std::optional<std::vector<DerivedPath>> parseDerivedPaths(InstallableFlake & installable, const json & paths)
{
    auto & store = *installable.state->store;
    std::vector<DerivedPath> res;
    std::vector<StorePath> bases;
    for (auto & p : paths) {
        res.push_back(DerivedPath::parse(store, p.get<std::string>()));
        bases.push_back(res.back().getBaseStorePath());
    }
    if (!pathsValid(installable, bases))
        return std::nullopt;
    return res;
}

/**
 * Delegate the evaluation of `installable` and decode the reply with
 * `decode`, which returns nothing if the reply is not usable; nothing if
 * the request is not delegated or the reply is malformed.
 */
template<typename F>
auto ask(InstallableFlake & installable, std::string_view format, F decode) -> decltype(decode(json()))
{
    auto request = makeRequest(installable, format);
    if (!request)
        return std::nullopt;
    auto reply = send(installable.state->settings.evalDaemonSocket.get(), *request);
    if (!reply)
        return std::nullopt;
    try {
        /* Apply the flake's `nixConfig` as a local evaluation would (with
           the same prompts for untrusted settings). If that changes a
           setting the daemon evaluated with, its result may differ. */
        if (auto & nixConfig = reply->at("nixConfig"); !nixConfig.empty()) {
            flake::ConfigFile config;
            for (auto & [name, value] : nixConfig.items())
                config.settings.emplace(
                    name,
                    value.is_boolean()          ? flake::ConfigFile::ConfigValue{Explicit<bool>{value.get<bool>()}}
                    : value.is_number_integer() ? flake::ConfigFile::ConfigValue{value.get<int64_t>()}
                    : value.is_array()          ? flake::ConfigFile::ConfigValue{value.get<std::vector<std::string>>()}
                                                : flake::ConfigFile::ConfigValue{value.get<std::string>()});
            config.apply(flakeSettings);
            installable.state->store->setOptions();
            if (forwardedSettings() != request->at("settings")) {
                fallback(installable.what(), "the flake's 'nixConfig' changes settings the evaluation daemon uses");
                return std::nullopt;
            }
        }
        /* Store paths the rendered value refers to (copied sources). */
        std::vector<StorePath> paths;
        for (auto & p : reply->value("paths", json::array()))
            paths.push_back(installable.state->store->parseStorePath(p.get<std::string>()));
        if (!pathsValid(installable, paths))
            return std::nullopt;
        return decode(*reply);
    } catch (json::exception & e) {
        fallback(installable.what(), fmt("the evaluation daemon sent an invalid reply: %s", e.what()));
        return std::nullopt;
    } catch (BadStorePath & e) {
        fallback(installable.what(), fmt("the evaluation daemon sent an invalid reply: %s", e.msg()));
        return std::nullopt;
    }
}

} // namespace

std::optional<DerivedPathsWithInfo> derivedPaths(InstallableFlake & installable)
{
    return ask(installable, "derivations", [&](const json & reply) -> std::optional<DerivedPathsWithInfo> {
        DerivedPathsWithInfo res;
        for (auto & item : reply.at("derivedPaths")) {
            auto paths = parseDerivedPaths(installable, json::array({item.at("path")}));
            if (!paths)
                return std::nullopt;
            std::optional<NixInt::Inner> priority;
            if (item.contains("priority") && !item["priority"].is_null())
                priority = item["priority"].get<NixInt::Inner>();
            res.push_back({
                .path = std::move(paths->front()),
                .info = make_ref<ExtraPathInfoFlake>(
                    ExtraPathInfoValue::Value{
                        .priority = priority,
                        .attrPath = item.at("attrPath").get<std::string>(),
                        .extendedOutputsSpec = ExtendedOutputsSpec::parse(item.at("outputs").get<std::string>()).second,
                    },
                    ExtraPathInfoFlake::Flake{
                        .originalRef = installable.flakeRef,
                        .lockedRef = parseFlakeRef(item.at("lockedRef").get<std::string>()),
                    }),
            });
        }
        return res;
    });
}

std::optional<json> jsonValue(InstallableFlake & installable)
{
    return ask(installable, "json", [](const json & reply) -> std::optional<json> { return reply.at("value"); });
}

std::optional<std::string> rawString(InstallableFlake & installable)
{
    return ask(installable, "raw", [](const json & reply) -> std::optional<std::string> {
        return reply.at("value").get<std::string>();
    });
}

std::optional<std::string> nixString(InstallableFlake & installable)
{
    return ask(installable, "nix", [](const json & reply) -> std::optional<std::string> {
        return reply.at("value").get<std::string>();
    });
}

std::optional<App> app(InstallableFlake & installable)
{
    return ask(installable, "app", [&](const json & reply) -> std::optional<App> {
        auto & a = reply.at("app");
        auto context = parseDerivedPaths(installable, a.at("context"));
        if (!context)
            return std::nullopt;
        return App{
            .context = std::move(*context),
            .program = a.at("program").get<std::string>(),
        };
    });
}

} // namespace nix::eval_daemon
