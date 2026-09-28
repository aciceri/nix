#pragma once
///@file

#include "nix/cmd/installable-value.hh"

#include <nlohmann/json_fwd.hpp>

namespace nix {

struct InstallableFlake;

/**
 * Delegation of flake evaluations to a running `nix eval-daemon`, enabled
 * by the `eval-daemon-socket` setting. Every function returns nothing
 * when the request cannot be delegated (the setting is empty, the request
 * uses something the daemon does not support, the daemon does not answer,
 * or its answer is not usable in this store); the caller then evaluates
 * locally. Evaluation errors reported by the daemon are thrown.
 */
namespace eval_daemon {

/**
 * The settings that determine an evaluation, as strings by name. A
 * request carries the client's; the daemon refuses a request whose
 * settings differ from its own.
 */
nlohmann::json forwardedSettings();

/**
 * `Installable::toDerivedPaths()` through the daemon: the derivations must
 * be valid in the installable's store.
 */
std::optional<DerivedPathsWithInfo> derivedPaths(InstallableFlake & installable);

/**
 * The value rendered as JSON, as `nix eval --json` prints it.
 */
std::optional<nlohmann::json> jsonValue(InstallableFlake & installable);

/**
 * The value coerced to a string, as `nix eval --raw` prints it.
 */
std::optional<std::string> rawString(InstallableFlake & installable);

/**
 * The value printed in Nix syntax, as `nix eval` prints it by default.
 */
std::optional<std::string> nixString(InstallableFlake & installable);

/**
 * The app of `nix run`: its program and the derived paths of its context.
 */
std::optional<App> app(InstallableFlake & installable);

} // namespace eval_daemon

} // namespace nix
