/*
 * mod-proximity-voice - wire codec
 *
 * A deliberately tiny line protocol shared with the C# voice server.
 * One record per line:  VERB key=value key=value ... \n
 *
 * Values are escaped so a record can never contain a raw space, '=' or newline.
 * A backslash starts an escape and the next character says what it stands for:
 * a second backslash, s = space, q = equals, n = newline, r = carriage return.
 *
 * Keep this in sync with SanctuaryVoice.Shared/Wire.cs.
 */

#ifndef MOD_PROXIMITY_VOICE_WIRE_H
#define MOD_PROXIMITY_VOICE_WIRE_H

#include "Define.h"
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ProximityVoice
{
    class WireRecord
    {
    public:
        WireRecord() = default;
        explicit WireRecord(std::string verb) : _verb(std::move(verb)) { }

        std::string const& Verb() const { return _verb; }

        WireRecord& Set(std::string_view key, std::string_view value)
        {
            _fields[std::string(key)] = std::string(value);
            return *this;
        }

        WireRecord& Set(std::string_view key, uint64 value) { return Set(key, std::to_string(value)); }
        WireRecord& Set(std::string_view key, int64 value) { return Set(key, std::to_string(value)); }
        WireRecord& Set(std::string_view key, uint32 value) { return Set(key, std::to_string(value)); }
        WireRecord& Set(std::string_view key, int32 value) { return Set(key, std::to_string(value)); }
        WireRecord& Set(std::string_view key, bool value) { return Set(key, std::string(value ? "1" : "0")); }
        WireRecord& Set(std::string_view key, float value);

        bool Has(std::string_view key) const { return _fields.find(std::string(key)) != _fields.end(); }
        std::string GetString(std::string_view key, std::string const& def = {}) const;
        uint64 GetUInt64(std::string_view key, uint64 def = 0) const;
        uint32 GetUInt32(std::string_view key, uint32 def = 0) const;
        float GetFloat(std::string_view key, float def = 0.0f) const;
        bool GetBool(std::string_view key, bool def = false) const;

        /// Serialise to a single line, newline included.
        std::string Encode() const;

        /// Parse one line (without the trailing newline). Returns false on a malformed record.
        static bool Decode(std::string_view line, WireRecord& out);

    private:
        std::string _verb;
        std::unordered_map<std::string, std::string> _fields;
    };
}

#endif // MOD_PROXIMITY_VOICE_WIRE_H
