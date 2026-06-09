#pragma once

// Per-area command registration entry points. Application::registerCommands()
// is being decomposed from one ~4,500-line function into these modules under
// src/cmd/commands/; each registers a related family of commands on the given
// registry. Handlers use only the public Application API.

namespace molterm {

class Application;
class CommandRegistry;

// Session lifecycle: :q / :quit / :qa.
void registerSessionCommands(Application& app, CommandRegistry& reg);

}  // namespace molterm
