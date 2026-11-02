#pragma once

#include <juce_core/juce_core.h>

#include <cmath>

// Sessions saved before VCAs, buses and sends, mix automation, the tempo map
// and the click were cut. Their files still hold that data. Nothing loads it,
// so the first save would drop it for good. Before that can happen the
// session's documents are copied, once, to one folder the engineer can go back
// to. Audio is never touched, and a session with none of this data is left
// alone: earlier builds wrote these keys on every save, so a key alone means
// nothing and only a value the engineer set counts.
//
// Pure (file ops only) so it is unit-testable without the UI.
namespace zynforge::removeddata
{
    inline const char* const kFolderName = "Before Feature Removal";

    inline juce::File folderFor (const juce::File& sessionDir)
    {
        return sessionDir.getChildFile ("Session File Backups").getChildFile (kFolderName);
    }

    namespace detail
    {
        inline bool lanesHold (const juce::var& lanes)
        {
            auto* tracks = lanes.getArray();
            if (tracks == nullptr) return false;
            for (const auto& track : *tracks)
            {
                for (const char* lane : { "volume", "pan", "mute" })
                    if (auto* points = track[lane].getArray(); points != nullptr && ! points->isEmpty())
                        return true;
                if ((bool) track["safe"]
                    || std::abs ((double) track["vTrim"]) > 1.0e-6
                    || std::abs ((double) track["pTrim"]) > 1.0e-6)
                    return true;
            }
            return false;
        }

        inline bool nearly (const juce::var& value, double target)
        {
            return std::abs ((double) value - target) < 1.0e-3;
        }

        inline bool mixHolds (const juce::var& mix)
        {
            auto* root = mix.getDynamicObject();
            if (root == nullptr) return false;
            if (root->hasProperty ("tempoBpm") && ! nearly (mix["tempoBpm"], 120.0)) return true;
            if (root->hasProperty ("timeSigNum") && (int) mix["timeSigNum"] != 4) return true;
            if (root->hasProperty ("timeSigDenom") && (int) mix["timeSigDenom"] != 4) return true;
            if (auto* map = mix["tempoMap"].getArray(); map != nullptr && ! map->isEmpty()) return true;
            if (auto* vcas = mix["vcas"].getArray())
                for (const auto& vca : *vcas)
                    if (! nearly (vca["gainDb"], 0.0) || (bool) vca["muted"] || (bool) vca["soloed"])
                        return true;
            if (auto* strips = mix["strips"].getArray())
                for (const auto& strip : *strips)
                {
                    auto* fields = strip.getDynamicObject();
                    if (fields == nullptr) continue;
                    if ((bool) strip["isBus"] || (bool) strip["referenceMedia"]) return true;
                    if (! fields->hasProperty ("referenceMedia") && strip["name"].toString() == "Click"
                        && fields->hasProperty ("inRoute") && (int) strip["inRoute"] < 0)
                        return true;    // a generated click, saved before the flag
                    if (fields->hasProperty ("vcaGroup") && (int) strip["vcaGroup"] >= 0) return true;
                    if (auto* sends = strip["sends"].getArray())
                        for (const auto& send : *sends)
                            if (send.hasProperty ("bus") && (int) send["bus"] >= 0) return true;
                }
            return false;
        }

        inline bool projectHolds (const juce::var& project)
        {
            if (lanesHold (project["automation"])) return true;
            if (auto* cues = project["setlist"].getArray())
                for (const auto& cue : *cues)
                {
                    if (lanesHold (cue["automation"])) return true;
                    if (auto* curve = cue["tempoCurve"].getArray(); curve != nullptr && ! curve->isEmpty())
                        return true;
                    if ((int) cue["transition"] != 0 || std::abs ((double) cue["fadeBeats"]) > 1.0e-6)
                        return true;
                    const double bpm = (double) cue["tempoBpm"];
                    if (bpm > 0.0 && ! nearly (bpm, 120.0)) return true;
                }
            return false;
        }

        inline juce::Array<juce::File> projects (const juce::File& sessionDir)
        {
            return sessionDir.findChildFiles (juce::File::findFiles | juce::File::ignoreHiddenFiles,
                                              false, "*.zfproj");
        }
    }

    // True when a save of this session would lose something a build with these
    // features put there. Damaged files read as nothing here: opening keeps those.
    inline bool holdsData (const juce::File& sessionDir)
    {
        if (detail::mixHolds (juce::JSON::parse (sessionDir.getChildFile ("session_mix.json"))))
            return true;
        for (const auto& project : detail::projects (sessionDir))
            if (detail::projectHolds (juce::JSON::parse (project))) return true;
        return false;
    }

    enum class Kept
    {
        nothingToDo,   // no such data, or its originals were kept earlier
        justCopied,    // this call copied them; tell the engineer
        failed         // they could not be copied; nothing may overwrite them
    };

    // The whole set lands or none of it does: a folder that exists is complete,
    // and "once" can rest on it. A failed copy leaves the session as it was.
    inline Kept keepOriginals (const juce::File& sessionDir)
    {
        const auto folder = folderFor (sessionDir);
        if (folder.isDirectory() || ! holdsData (sessionDir)) return Kept::nothingToDo;

        const auto backups = folder.getParentDirectory();
        if (backups.isSymbolicLink() || ! backups.createDirectory().wasOk()) return Kept::failed;

        const auto staging = backups.getChildFile ("." + juce::String (kFolderName) + "-"
                                                   + juce::Uuid().toString());
        if (! staging.createDirectory().wasOk()) return Kept::failed;
        const juce::ScopeGuard removeStaging { [&staging] { staging.deleteRecursively (false); } };

        juce::Array<juce::File> documents = detail::projects (sessionDir);
        for (const auto* name : { "session_mix.json", "session_settings.json", "markers.json" })
            if (const auto file = sessionDir.getChildFile (name); file.existsAsFile())
                documents.add (file);

        for (const auto& document : documents)
        {
            const auto copy = staging.getChildFile (document.getFileName());
            if (! document.copyFileTo (copy) || ! document.hasIdenticalContentTo (copy))
                return Kept::failed;
        }

        // A second thread that got here first has done the same job.
        return staging.moveFileTo (folder) || folder.isDirectory() ? Kept::justCopied : Kept::failed;
    }

    inline juce::String refusal (const juce::File& sessionDir)
    {
        return "the original files of " + sessionDir.getFileName()
             + " could not be copied to Session File Backups, so its saved files were left untouched";
    }
}
