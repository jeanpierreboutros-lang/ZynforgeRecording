#pragma once

#include <juce_core/juce_core.h>

namespace zynforge::channelcsv
{
    // Parse a channel-name list out of a CSV a console (or a spreadsheet)
    // exported. Tolerant by design: accepts one-name-per-line, or rows like
    // "1,Kick" / "Ch 1,Kick,..." (numeric first column = channel number ->
    // take the next field), quoted fields, and skips an obvious header row.

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

    inline juce::StringArray parseNames (const juce::String& text)
    {
        juce::StringArray out;
        bool headerChecked = false;
        for (auto line : juce::StringArray::fromLines (text))
        {
            line = line.trim();
            if (line.isEmpty()) continue;

            const auto fields = splitCsvLine (line);
            juce::String name;
            if (fields.size() == 1)
                name = fields[0];
            else
            {
                // Numeric first column = channel number -> the name is the
                // first non-numeric, non-empty field after it.
                const bool firstIsNumber = fields[0].trim().containsOnly ("0123456789");
                name = firstIsNumber ? fields[1] : fields[0];
                if (name.trim().isEmpty() && fields.size() > 1) name = fields[1];
            }
            name = name.trim();
            if (name.isEmpty()) continue;

            if (! headerChecked)
            {
                headerChecked = true;
                const auto low = name.toLowerCase();
                if (low == "name" || low == "channel" || low == "label"
                    || low == "ch" || low == "channel name")
                    continue;   // drop the header row
            }
            out.add (name);
        }
        return out;
    }
}
