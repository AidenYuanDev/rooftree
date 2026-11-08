/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * A REST API needs every method and paths like /api/posts/{id}, so routes are kept in a tree of path
 * segments instead:
 *
 *  - a segment of a pattern is either literal ("posts") or a parameter ("{id}") that matches any one
 *    non-empty segment and is handed to the handler percent-decoded
 *  - literal segments win over parameters, whatever the registration order: with /posts/{id} and
 *    /posts/slugs, "/posts/slugs" goes to the second one. If the literal branch has no route for the
 *    rest of the path, the parameter branch is tried next
 *  - a path that matches but has no handler for the method is a 405, with the methods it has; HEAD is
 *    answered by GET handlers
 *
 * Matching only reads the tree, so it is safe from every network thread once the routes are registered
 * (they all are before StartNetwork).
 */

#ifndef ROOFTREE_HTTP_ROUTER_H
#define ROOFTREE_HTTP_ROUTER_H

#include "Define.h"
#include "Optional.h"
#include <boost/beast/http/verb.hpp>
#include <algorithm>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Rooftree::Net::Http
{
// Values of the {name} segments of the matched route, percent-decoded
class PathParameters
{
public:
    // Empty for a name the route doesn't have
    std::string_view Get(std::string_view name) const
    {
        for (auto const& [parameterName, value] : _values)
            if (parameterName == name)
                return value;

        return {};
    }

    std::vector<std::pair<std::string, std::string>> const& GetAll() const { return _values; }

    void Prepend(std::string name, std::string value) { _values.emplace(_values.begin(), std::move(name), std::move(value)); }
    void Clear() { _values.clear(); }

private:
    std::vector<std::pair<std::string, std::string>> _values;
};

enum class RouteMatchResult
{
    Found,
    MethodNotAllowed,
    NotFound
};

template<typename Handler>
class Router
{
public:
    // "/api/posts/{id}". False if the pattern is malformed ("{}", "{a", a parameter name used twice) or
    // the method already has a handler for it
    bool Add(boost::beast::http::verb method, std::string_view pattern, Handler handler)
    {
        Optional<std::vector<std::string_view>> segments = SplitPattern(pattern);
        if (!segments)
            return false;

        Node* node = &_root;
        std::vector<std::string> names;
        for (std::string_view segment : *segments)
        {
            if (segment.starts_with('{'))
            {
                std::string name(segment.substr(1, segment.size() - 2));
                if (std::ranges::find(names, name) != names.end())
                    return false;

                if (!node->Parameter)
                {
                    node->Parameter = std::make_unique<Node>();
                    node->ParameterName = name;
                }
                // /a/{id} and /a/{slug}/b would hand one value to two names, keep a single one
                else if (node->ParameterName != name)
                    return false;

                names.push_back(std::move(name));
                node = node->Parameter.get();
            }
            else
                node = &node->Literals[std::string(segment)];
        }

        return node->Handlers.try_emplace(method, std::move(handler)).second;
    }

    // segments: the decoded path segments (DecodePathSegments). On Found, handler and parameters are
    // set; on MethodNotAllowed, allowedMethods lists the path's methods for the Allow header
    RouteMatchResult Find(boost::beast::http::verb method, std::span<std::string const> segments, Handler const*& handler,
        PathParameters& parameters, std::string* allowedMethods = nullptr) const
    {
        parameters.Clear();
        Node const* node = Find(_root, segments, parameters);
        if (!node)
            return RouteMatchResult::NotFound;

        auto itr = node->Handlers.find(method);
        if (itr == node->Handlers.end() && method == boost::beast::http::verb::head)
            itr = node->Handlers.find(boost::beast::http::verb::get);

        if (itr != node->Handlers.end())
        {
            handler = &itr->second;
            return RouteMatchResult::Found;
        }

        if (allowedMethods)
        {
            allowedMethods->clear();
            for (auto const& [allowed, _] : node->Handlers)
            {
                if (!allowedMethods->empty())
                    allowedMethods->append(", ");
                allowedMethods->append(boost::beast::http::to_string(allowed));
                if (allowed == boost::beast::http::verb::get && !node->Handlers.contains(boost::beast::http::verb::head))
                    allowedMethods->append(", HEAD");
            }
        }

        parameters.Clear();
        return RouteMatchResult::MethodNotAllowed;
    }

private:
    struct Node
    {
        std::map<std::string, Node, std::less<>> Literals;
        std::unique_ptr<Node> Parameter;
        std::string ParameterName;
        std::map<boost::beast::http::verb, Handler> Handlers;
    };

    // The deepest node for the whole path that has handlers, literal branches first
    static Node const* Find(Node const& node, std::span<std::string const> segments, PathParameters& parameters)
    {
        if (segments.empty())
            return node.Handlers.empty() ? nullptr : &node;

        std::string const& segment = segments.front();
        auto literal = node.Literals.find(segment);
        if (literal != node.Literals.end())
            if (Node const* found = Find(literal->second, segments.subspan(1), parameters))
                return found;

        if (node.Parameter && !segment.empty())
        {
            if (Node const* found = Find(*node.Parameter, segments.subspan(1), parameters))
            {
                // On the way back up, so a branch that doesn't match adds nothing
                parameters.Prepend(node.ParameterName, segment);
                return found;
            }
        }

        return nullptr;
    }

    // "/" -> {}, "/a/{b}" -> {"a", "{b}"}; empty for anything but an absolute path of valid segments
    static Optional<std::vector<std::string_view>> SplitPattern(std::string_view pattern)
    {
        if (!pattern.starts_with('/'))
            return {};

        std::vector<std::string_view> segments;
        if (pattern == "/")
            return segments;

        pattern.remove_prefix(1);
        while (true)
        {
            std::size_t end = pattern.find('/');
            std::string_view segment = pattern.substr(0, end);
            bool isParameter = segment.starts_with('{') || segment.ends_with('}');
            if (isParameter && (segment.size() < 3 || !segment.starts_with('{') || !segment.ends_with('}')
                || segment.substr(1, segment.size() - 2).find_first_of("{}") != std::string_view::npos))
                return {};

            segments.push_back(segment);
            if (end == std::string_view::npos)
                return segments;

            pattern.remove_prefix(end + 1);
        }
    }

    Node _root;
};
}

#endif // ROOFTREE_HTTP_ROUTER_H
