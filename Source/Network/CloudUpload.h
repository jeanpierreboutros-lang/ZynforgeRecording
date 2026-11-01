#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>

namespace zynforge::cloud
{
// The configured command is a shell command: engineers write `~`, `&&` and
// pipes in it. {SESSION} becomes a reference to the shell's $1 instead of the
// path itself, so a session name holding a quote, `$` or backtick stays data.
// How it must be spelled depends on the quoting around it.
inline juce::String shellScriptFor (const juce::String& commandTemplate)
{
    static const juce::String token ("{SESSION}");
    juce::String script;
    bool inSingle = false, inDouble = false;
    for (int i = 0; i < commandTemplate.length();)
    {
        if (commandTemplate.substring (i, i + token.length()) == token)
        {
            script << (inDouble ? "${1}" : inSingle ? "'\"${1}\"'" : "\"${1}\"");
            i += token.length();
            continue;
        }
        const auto c = commandTemplate[i];
        if (c == '\\' && ! inSingle && i + 1 < commandTemplate.length())
        {
            script << juce::String::charToString (c)
                   << juce::String::charToString (commandTemplate[i + 1]);
            i += 2;
            continue;
        }
        if (c == '\'' && ! inDouble) inSingle = ! inSingle;
        else if (c == '"' && ! inSingle) inDouble = ! inDouble;
        script << juce::String::charToString (c);
        ++i;
    }
    return script;
}

struct UploadOutcome
{
    bool ok = false;
    juce::String problem;   // why not, in words the status line can carry
};

// 127 is the shell's own code for a command it could not find, which is the
// usual failure on a Mac: the app gets launchd's PATH, not the Terminal's.
inline juce::String describeExitCode (int code)
{
    if (code == 127) return "the upload command was not found";
    if (code == 126) return "the upload command could not be run";
    return "the upload command exited with code " + juce::String (code);
}

// Runs an engineer-configured uploader and reports how it really ended. An
// upload can outlast a show, so this is its own thread and never a session job:
// a job holds sessionIoBusy, which would refuse RECORD for hours.
//
// Success is what the shell itself records after the command returns. JUCE
// reports a child killed by a signal as exit code 0, and a process that never
// got as far as writing the status is not a finished upload.
class UploadRun final : private juce::Thread
{
public:
    using Finished = std::function<void (UploadOutcome)>;

    UploadRun() : juce::Thread ("Cloud upload") {}
    ~UploadRun() override { stopThread (-1); }

    bool isActive() const { return isThreadRunning(); }

    // `finished` runs on this thread, once, unless the app quits first; the
    // uploader then keeps going with nobody listening.
    bool start (const juce::String& commandTemplate, const juce::File& sessionDirectory, Finished finished)
    {
        if (isThreadRunning() || commandTemplate.trim().isEmpty() || ! sessionDirectory.isDirectory())
            return false;
        status = juce::File::createTempFile (".zynforge-upload-status");
        status.deleteFile();
        // The subshell keeps a bare `exit` in the command from skipping the
        // status line. Homebrew's folders are not on launchd's PATH.
        const auto script = "PATH=\"/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:$PATH\"; export PATH\n("
                          + shellScriptFor (commandTemplate) + "\n)\nprintf '%s' \"$?\" > \"$2\"\n";
        // Output goes nowhere. A pipe nobody reads would stall a verbose
        // uploader, and closing it would SIGPIPE it.
        const juce::StringArray arguments { "/bin/sh", "-c", script, "zynforge-upload",
                                            sessionDirectory.getFullPathName(),
                                            status.getFullPathName() };
        child = std::make_unique<juce::ChildProcess>();
        if (! child->start (arguments, 0))
        {
            child.reset();
            return false;
        }
        onFinished = std::move (finished);
        startThread (juce::Thread::Priority::background);
        return true;
    }

private:
    void run() override
    {
        while (child->isRunning())
        {
            wait (250);
            if (threadShouldExit()) return;
        }

        UploadOutcome outcome;
        const auto recorded = status.loadFileAsString().trim();
        status.deleteFile();
        if (recorded.isEmpty())
            outcome.problem = "the upload was killed or its command line is malformed";
        else if (recorded.getIntValue() == 0 && recorded.containsOnly ("0123456789"))
            outcome.ok = true;
        else
            outcome.problem = describeExitCode (recorded.getIntValue());
        if (onFinished) onFinished (std::move (outcome));
    }

    std::unique_ptr<juce::ChildProcess> child;
    juce::File status;
    Finished onFinished;
};
}
