#pragma once

#include <juce_core/juce_core.h>
#include <limits>
#include <vector>

namespace zynforge::channelcsv
{
    // Parse a channel-name list out of a CSV a console (or a spreadsheet)
    // exported. Tolerant by design: accepts one-name-per-line, or rows like
    // "1,Kick" / "Ch 1,Kick,..." (numeric first column = channel number ->
    // take the next field), quoted fields, and skips an obvious header row.
    // The result is indexed by channel: a numbered row lands on its own
    // channel, and a channel with no name is an empty entry.

    inline juce::StringArray splitCsvLine (const juce::String& line)
    {
        juce::StringArray fields;
        juce::String cur;
        bool inQuotes = false;
        for (int i = 0; i < line.length(); ++i)
        {
            const auto c = line[i];
            if (c == '"')
            {
                if (inQuotes && i + 1 < line.length() && line[i + 1] == '"') { cur << '"'; ++i; }
                else inQuotes = ! inQuotes;
            }
            else if ((c == ',' || c == ';' || c == '\t') && ! inQuotes)
            {
                fields.add (cur); cur.clear();
            }
            else cur << c;
        }
        fields.add (cur);
        return fields;
    }

    // The channel number a first-column label carries -- "3", "Ch 3", "CH03",
    // "Ch.3", "Input 3" -- or -1 when the field is not a channel label.
    inline int channelNumberOf (const juce::String& field)
    {
        const auto f = field.trim();
        int digitsAt = f.length();
        while (digitsAt > 0 && juce::CharacterFunctions::isDigit (f[digitsAt - 1])) --digitsAt;
        if (digitsAt == f.length()) return -1;
        const auto label = f.substring (0, digitsAt).trimCharactersAtEnd (" .#:").toLowerCase();
        if (label.isNotEmpty() && label != "ch" && label != "chan" && label != "channel"
            && label != "in" && label != "input")
            return -1;
        const auto digits = f.substring (digitsAt);
        return digits.length() > 6 ? std::numeric_limits<int>::max() : digits.getIntValue();
    }

    inline juce::StringArray parseNames (const juce::String& text)
    {
        struct Row { int channel; juce::String name; };   // channel -1 = unnumbered
        std::vector<Row> rows;
        bool headerChecked = false, zeroBased = false;
        for (auto line : juce::StringArray::fromLines (text))
        {
            line = line.trim();
            if (line.isEmpty()) continue;

            const auto fields = splitCsvLine (line);
            juce::String name;
            int channel = -1;
            if (fields.size() == 1)
                name = fields[0];
            else
            {
                // Channel-number first column -> the name is the next field.
                channel = channelNumberOf (fields[0]);
                name = channel >= 0 ? fields[1] : fields[0];
                if (channel < 0 && name.trim().isEmpty()) name = fields[1];
            }
            name = name.trim();
            // A numbered row keeps its channel even without a name; dropping
            // it shifted every later name onto the wrong channel.
            if (name.isEmpty() && channel < 0) continue;

            if (! headerChecked && name.isNotEmpty())
            {
                headerChecked = true;
                const auto low = name.toLowerCase();
                if (low == "name" || low == "channel" || low == "label"
                    || low == "ch" || low == "channel name")
                    continue;   // drop the header row
            }
            zeroBased = zeroBased || channel == 0;
            rows.push_back ({ channel, name });
        }

        // Numbered rows go to their own channel (first one wins), so a gap in
        // the numbering leaves that channel unnamed instead of pulling the
        // next name onto it. Unnumbered rows fill in order, as before.
        constexpr int kMaxChannels = 256;
        juce::StringArray out;
        for (const auto& r : rows)
        {
            const int slot = r.channel < 0 || r.channel == std::numeric_limits<int>::max()
                           ? -1 : r.channel - (zeroBased ? 0 : 1);
            if (slot >= 0 && slot < kMaxChannels)
            {
                while (out.size() <= slot) out.add ({});
                if (out[slot].isEmpty()) out.set (slot, r.name);
            }
            else if (r.name.isNotEmpty())
                out.add (r.name);
        }
        for (const auto& name : out)
            if (name.isNotEmpty()) return out;
        return {};
    }
}
