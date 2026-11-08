/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * The console's command table:
 *
 *  - a tree of named commands: "account set password a b c" runs the handler of password under set
 *    under account with "a b c"
 *  - a word may be any prefix that is unique among its siblings ("acc s pass"); a word that
 *    matches nothing or several, or a command with subcommands given none, lists what there is
 *  - a handler gets the rest of the line. It returns false for wrong arguments: its help is printed
 *  - "help [command]" is built in
 *  - Complete() gives the names that could follow a partial line, for a client's Tab completion
 *
 * No permissions: the console may run everything.
 */

#ifndef ROOFTREE_CONSOLE_COMMAND_H
#define ROOFTREE_CONSOLE_COMMAND_H

#include "Define.h"
#include "StringFormat.h"
#include "Task.h"
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Rooftree::Server
{
class CommandHandler;
class Session;

using CommandFunction = std::function<bool(CommandHandler& handler, std::string_view args)>;

struct Command
{
    std::string Name;
    std::string Help;               // "Syntax: account set password <account> <password> <password>\n..."
    CommandFunction Handler;        // none: only a group of subcommands
    std::vector<Command> Subcommands;
};

class CommandTable
{
public:
    explicit CommandTable(std::vector<Command> commands) : _commands(std::move(commands)) { }

    // Main thread. The full name of the command that ran ("account set password"), empty if none did
    std::string Execute(CommandHandler& handler, std::string_view line) const;

    // The names the last word of line could become (all of them after a space), help's included
    std::vector<std::string> Complete(std::string_view line) const;

    std::vector<Command> const& GetCommands() const { return _commands; }

private:
    void ExecuteHelp(CommandHandler& handler, std::string_view args) const;

    std::vector<Command> _commands;
};

// What a command prints goes to the console. Created per command and shared: a command
// that goes on later keeps it, with Spawn() or with shared_from_this() (AddQueryCallback). Destroying the
// last reference ends the command
class CommandHandler : public std::enable_shared_from_this<CommandHandler>
{
public:
    using PrintFunction = std::function<void(std::string_view text)>;
    using AnswerFunction = std::function<void(CommandHandler& handler, std::string answer)>;
    // The console's: prompt for a line, a secret one not echoed, and hand it to answer on the main thread
    using AskFunction = std::function<void(std::string_view prompt, bool secret, std::function<void(std::string)> answer)>;

    CommandHandler(Session& session, PrintFunction print, std::function<void()> finished = {}, AskFunction ask = {})
        : _session(session), _print(std::move(print)), _finished(std::move(finished)), _ask(std::move(ask)) { }
    CommandHandler(CommandHandler const&) = delete;
    CommandHandler& operator=(CommandHandler const&) = delete;
    ~CommandHandler();

    // The console's session, for asynchronous work
    Session& GetSession() const { return _session; }

    // Runs task on the console's session, the command lasting until it ended: a coroutine taking this
    // handler by reference may use it after a co_await. Only for a handler owned by a shared_ptr
    void Spawn(Rooftree::Task<void> task);

    // One line, or several separated by '\n'
    void SendSysMessage(std::string_view text);

    template<typename... Args>
    void PSendSysMessage(Rooftree::FormatString<Args...> fmt, Args&&... args)
    {
        SendSysMessage(Rooftree::StringFormat(fmt, std::forward<Args>(args)...));
    }

    // Something was printed: a handler returning false gets no help after its own error
    bool HasSentMessage() const { return _sentMessage; }

    // The next line typed on the console goes to answer instead of running as a command (a password:
    // secret, not echoed). The handler is kept until then. False: this console can't ask. Only for a
    // handler owned by a shared_ptr
    bool Ask(std::string_view prompt, bool secret, AnswerFunction answer);

private:
    Session& _session;
    PrintFunction _print;
    std::function<void()> _finished;
    AskFunction _ask;
    bool _sentMessage = false;
};

// The words of args, separated by spaces; "double quotes" keep spaces in a word
std::vector<std::string> SplitCommandArgs(std::string_view args);
}

#endif // ROOFTREE_CONSOLE_COMMAND_H
