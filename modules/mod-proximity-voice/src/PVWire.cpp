/*
 * mod-proximity-voice - wire codec
 */

#include "PVWire.h"
#include <charconv>
#include <cstdio>
#include <stdexcept>

namespace ProximityVoice
{
    namespace
    {
        void AppendEscaped(std::string& out, std::string_view in)
        {
            for (char c : in)
            {
                switch (c)
                {
                    case '\\': out += "\\\\"; break;
                    case ' ':  out += "\\s";  break;
                    case '=':  out += "\\q";  break;
                    case '\n': out += "\\n";  break;
                    case '\r': out += "\\r";  break;
                    default:   out += c;      break;
                }
            }
        }

        bool Unescape(std::string_view in, std::string& out)
        {
            out.clear();
            out.reserve(in.size());

            for (std::size_t i = 0; i < in.size(); ++i)
            {
                if (in[i] != '\\')
                {
                    out += in[i];
                    continue;
                }

                if (++i >= in.size())
                    return false;

                switch (in[i])
                {
                    case '\\': out += '\\'; break;
                    case 's':  out += ' ';  break;
                    case 'q':  out += '=';  break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    default:   return false;
                }
            }

            return true;
        }
    }

    WireRecord& WireRecord::Set(std::string_view key, float value)
    {
        // to_chars rather than snprintf: the decimal separator must be '.' no
        // matter what locale the host happens to be in, because the far end
        // always parses it as invariant.
        char buffer[32];
        auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, 3);

        if (ec != std::errc())
            return Set(key, std::string_view("0"));

        return Set(key, std::string_view(buffer, std::size_t(ptr - buffer)));
    }

    std::string WireRecord::GetString(std::string_view key, std::string const& def) const
    {
        auto itr = _fields.find(std::string(key));
        return itr == _fields.end() ? def : itr->second;
    }

    uint64 WireRecord::GetUInt64(std::string_view key, uint64 def) const
    {
        auto itr = _fields.find(std::string(key));
        if (itr == _fields.end() || itr->second.empty())
            return def;

        uint64 value = 0;
        char const* first = itr->second.data();
        char const* last = first + itr->second.size();
        auto [ptr, ec] = std::from_chars(first, last, value);
        return (ec == std::errc() && ptr == last) ? value : def;
    }

    uint32 WireRecord::GetUInt32(std::string_view key, uint32 def) const
    {
        uint64 const value = GetUInt64(key, def);
        return value > 0xFFFFFFFFull ? def : uint32(value);
    }

    float WireRecord::GetFloat(std::string_view key, float def) const
    {
        auto itr = _fields.find(std::string(key));
        if (itr == _fields.end() || itr->second.empty())
            return def;

        // from_chars is locale-independent, so a host running under a comma
        // locale still reads the '.' the sender wrote.
        float value = 0.0f;
        char const* first = itr->second.data();
        char const* last = first + itr->second.size();
        auto [ptr, ec] = std::from_chars(first, last, value);
        return (ec == std::errc() && ptr == last) ? value : def;
    }

    bool WireRecord::GetBool(std::string_view key, bool def) const
    {
        auto itr = _fields.find(std::string(key));
        if (itr == _fields.end())
            return def;

        return itr->second == "1" || itr->second == "true";
    }

    std::string WireRecord::Encode() const
    {
        std::string out;
        out.reserve(64 + _fields.size() * 16);
        out += _verb;

        for (auto const& field : _fields)
        {
            out += ' ';
            out += field.first;
            out += '=';
            AppendEscaped(out, field.second);
        }

        out += '\n';
        return out;
    }

    bool WireRecord::Decode(std::string_view line, WireRecord& out)
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.remove_suffix(1);

        if (line.empty())
            return false;

        out._verb.clear();
        out._fields.clear();

        std::size_t const space = line.find(' ');
        out._verb = std::string(line.substr(0, space == std::string_view::npos ? line.size() : space));

        if (out._verb.empty())
            return false;

        if (space == std::string_view::npos)
            return true;

        std::size_t pos = space + 1;

        while (pos < line.size())
        {
            std::size_t const next = line.find(' ', pos);
            std::string_view const token = line.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos);
            pos = (next == std::string_view::npos) ? line.size() : next + 1;

            if (token.empty())
                continue;

            std::size_t const eq = token.find('=');
            if (eq == std::string_view::npos || eq == 0)
                return false;

            std::string value;
            if (!Unescape(token.substr(eq + 1), value))
                return false;

            out._fields[std::string(token.substr(0, eq))] = std::move(value);
        }

        return true;
    }
}
