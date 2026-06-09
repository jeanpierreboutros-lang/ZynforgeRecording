#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>

namespace zynforge::crashscan
{
    // Launch-time crash telemetry: find macOS .ips crash reports for this
    // app that appeared since the previous run, and produce a short
    // human-readable summary the engineer can paste into a bug report.
    // Header-only + path-parameterised so tests drive it against temp dirs.

    inline juce::File reportsDirectory()
    {
        return juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                   .getChildFile ("Library/Logs/DiagnosticReports");
    }

    // Zynforge*.ips files modified after `since`, newest first.
    inline juce::Array<juce::File> findNewReports (const juce::File& dir,
                                                   juce::Time since,
                                                   const juce::String& prefix = "Zynforge")
    {
        juce::Array<juce::File> out;
        if (! dir.isDirectory()) return out;
        for (auto& f : dir.findChildFiles (juce::File::findFiles, false, prefix + "*.ips"))
            if (f.getLastModificationTime() > since)
                out.add (f);
        std::sort (out.begin(), out.end(),
                   [] (const juce::File& a, const juce::File& b)
                   { return a.getLastModificationTime() > b.getLastModificationTime(); });
        return out;
    }

    // Pull a field out of the .ips JSON without a full parse -- the
    // header line is flat JSON and the fields we want are simple
    // string / scalar values.
    inline juce::String grabField (const juce::String& text, const juce::String& key)
    {
        const auto k = "\"" + key + "\"";
        const int i = text.indexOf (k);
        if (i < 0) return {};
        auto rest = text.substring (i + k.length())
                        .fromFirstOccurrenceOf (":", false, false).trimStart();
        if (rest.startsWith ("\""))
            return rest.substring (1).upToFirstOccurrenceOf ("\"", false, false);
        return rest.upToFirstOccurrenceOf (",", false, false)
                   .upToFirstOccurrenceOf ("}", false, false).trim();
    }

    // Short summary: when it happened + what killed it. Reads only the
    // head of the file (the stacks below can be hundreds of KB).
    inline juce::String summarize (const juce::File& ips)
    {
        const auto head = ips.loadFileAsString().substring (0, 6000);
        juce::String s;
        s << ips.getFileName() << "\n";
        const auto when = grabField (head, "timestamp");
        if (when.isNotEmpty())   s << "  When:   " << when << "\n";
        const auto sig = grabField (head, "signal");
        const auto exc = grabField (head, "type");
        if (sig.isNotEmpty() || exc.isNotEmpty())
            s << "  Crash:  " << exc << (sig.isNotEmpty() ? " (" + sig + ")" : juce::String()) << "\n";
        const auto term = grabField (head, "terminationReason");
        if (term.isNotEmpty())   s << "  Reason: " << term << "\n";
        return s;
    }
}
