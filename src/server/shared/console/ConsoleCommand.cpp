/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "ConsoleCommand.h"
#include "Optional.h"
#include "ServerSession.h"
#include "Util.h"

namespace Rooftree::Server
{
namespace
{
using CommandList = std::vector<Command const*>;

Command const HelpCommand
{
    .Name = "help",
    .Help = "Syntax: help [command]\nLists the commands, or shows what a command does and its subcommands.",
};

bool IsSpace(char c) { return c == ' ' || c == '\t'; }

std::string_view TrimSpaces(std::string_view text)
{
    while (!text.empty() && IsSpace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && IsSpace(text.back()))
        text.remove_suffix(1);
    return text;
}

// The first word of rest, rest is left with what follows it
std::string_view NextWord(std::string_view& rest)
{
    rest = TrimSpaces(rest);
    std::size_t end = 0;
    while (end < rest.size() && !IsSpace(rest[end]))
        ++end;
    std::string_view word = rest.substr(0, end);
    rest = TrimSpaces(rest.substr(end));
    return word;
}

CommandList ToList(std::vector<Command> const& commands)
{
    CommandList list;
    for (Command const& command : commands)
        list.push_back(&command);
    return list;
}

// The command named word, else all of those it is a prefix of (abbreviated commands)
CommandList Match(CommandList const& commands, std::string_view word)
{
    CommandList matches;
    for (Command const* command : commands)
    {
        if (StringEqualI(command->Name, word))
            return { command };
        if (StringStartsWithI(command->Name, word))
            matches.push_back(command);
    }
    return matches;
}

std::string ListNames(CommandList const& commands)
{
    std::string text;
    for (Command const* command : commands)
    {
        text += "\n    ";
        text += command->Name;
        if (!command->Subcommands.empty())
            text += " ...";
    }
    return text;
}

struct Resolved
{
    Command const* Target = nullptr;
    std::string Path;
    std::string_view Args;
};

// Follows the words of line down the tree as far as they name subcommands. Prints why when they don't
Optional<Resolved> Resolve(CommandHandler& handler, CommandList const& top, std::string_view line)
{
    Resolved resolved;
    std::string_view rest = line;
    std::string_view word = NextWord(rest);
    CommandList level = top;
    while (true)
    {
        CommandList matches = Match(level, word);
        if (matches.empty())
        {
            if (resolved.Path.empty())
                handler.PSendSysMessage("There is no command {}, 'help' lists them", word);
            else
                handler.PSendSysMessage("{} has no subcommand {}, there are:{}", resolved.Path, word, ListNames(level));
            return {};
        }

        if (matches.size() > 1)
        {
            handler.PSendSysMessage("{} is ambiguous, it could be:{}", word, ListNames(matches));
            return {};
        }

        resolved.Target = matches.front();
        resolved.Path += resolved.Path.empty() ? "" : " ";
        resolved.Path += resolved.Target->Name;
        if (resolved.Target->Subcommands.empty())
            break;

        // The next word is a subcommand, or the arguments of a command that has both
        level = ToList(resolved.Target->Subcommands);
        std::string_view next = rest;
        std::string_view nextWord = NextWord(next);
        if (nextWord.empty() || (resolved.Target->Handler && Match(level, nextWord).empty()))
            break;

        word = nextWord;
        rest = next;
    }

    resolved.Args = rest;
    return resolved;
}
}

CommandHandler::~CommandHandler()
{
    if (_finished)
        _finished();
}

namespace
{
Rooftree::Task<void> KeepHandler(std::shared_ptr<CommandHandler> /*handler*/, Rooftree::Task<void> task)
{
    co_await std::move(task);
}
}

void CommandHandler::Spawn(Rooftree::Task<void> task)
{
    _session.Spawn(KeepHandler(shared_from_this(), std::move(task)));
}

void CommandHandler::SendSysMessage(std::string_view text)
{
    _sentMessage = true;
    _print(text);
}

bool CommandHandler::Ask(std::string_view prompt, bool secret, AnswerFunction answer)
{
    if (!_ask)
        return false;

    _ask(prompt, secret, [self = shared_from_this(), answer = std::move(answer)](std::string text)
    {
        answer(*self, std::move(text));
    });
    return true;
}

std::string CommandTable::Execute(CommandHandler& handler, std::string_view line) const
{
    if (TrimSpaces(line).empty())
        return {};

    CommandList top = ToList(_commands);
    top.push_back(&HelpCommand);
    Optional<Resolved> resolved = Resolve(handler, top, line);
    if (!resolved)
        return {};

    Command const& command = *resolved->Target;
    if (&command == &HelpCommand)
        ExecuteHelp(handler, resolved->Args);
    else if (!command.Handler)
        handler.PSendSysMessage("{} has subcommands:{}", resolved->Path, ListNames(ToList(command.Subcommands)));
    else if (!command.Handler(handler, resolved->Args) && !handler.HasSentMessage())
        handler.SendSysMessage(command.Help.empty() ? "Wrong arguments" : command.Help);

    return resolved->Path;
}

void CommandTable::ExecuteHelp(CommandHandler& handler, std::string_view args) const
{
    CommandList top = ToList(_commands);
    top.push_back(&HelpCommand);
    if (args.empty())
        return handler.PSendSysMessage("Commands, 'help <command>' for more:{}", ListNames(top));

    Optional<Resolved> resolved = Resolve(handler, top, args);
    if (!resolved)
        return;

    if (!resolved->Args.empty())
        return handler.PSendSysMessage("{} has no subcommand {}", resolved->Path, resolved->Args);

    Command const& command = *resolved->Target;
    std::string text = command.Help.empty() ? resolved->Path : command.Help;
    if (!command.Subcommands.empty())
        text += "\nSubcommands:" + ListNames(ToList(command.Subcommands));
    handler.SendSysMessage(text);
}

std::vector<std::string> CommandTable::Complete(std::string_view line) const
{
    CommandList top = ToList(_commands);
    top.push_back(&HelpCommand);

    // The words before the last one each name one command, the last one is being typed
    std::vector<std::string_view> words;
    std::string_view rest = line;
    for (std::string_view word = NextWord(rest); !word.empty(); word = NextWord(rest))
        words.push_back(word);
    std::string_view partial;
    if (!words.empty() && !line.empty() && !IsSpace(line.back()))
    {
        partial = words.back();
        words.pop_back();
    }

    CommandList level = top;
    for (std::size_t i = 0; i < words.size(); ++i)
    {
        CommandList matches = Match(level, words[i]);
        if (matches.size() != 1)
            return {};

        // help <command>: the commands again
        if (matches.front() == &HelpCommand && i == 0)
            continue;
        // Arguments from here on
        if (matches.front()->Subcommands.empty())
            return {};
        level = ToList(matches.front()->Subcommands);
    }

    std::vector<std::string> names;
    for (Command const* command : level)
        if (StringStartsWithI(command->Name, partial))
            names.push_back(command->Name);
    return names;
}

std::vector<std::string> SplitCommandArgs(std::string_view args)
{
    std::vector<std::string> words;
    std::size_t i = 0;
    while (i < args.size())
    {
        if (IsSpace(args[i]))
        {
            ++i;
            continue;
        }

        std::string word;
        if (args[i] == '"')
        {
            std::size_t end = args.find('"', i + 1);
            if (end == std::string_view::npos)
                end = args.size();
            word = args.substr(i + 1, end - i - 1);
            i = end + 1;
        }
        else
        {
            std::size_t end = i;
            while (end < args.size() && !IsSpace(args[end]))
                ++end;
            word = args.substr(i, end - i);
            i = end;
        }
        words.push_back(std::move(word));
    }
    return words;
}
}
