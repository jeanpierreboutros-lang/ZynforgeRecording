#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>

namespace zynforge
{
    // A writer owns its stream and closes it in a destructor that reports
    // nothing, and FileOutputStream drops a short write of its last buffer
    // without a word. So a disk that fills on the final flush or the header
    // rewrite would leave a short file that looks finished. This stream outlives
    // nothing the caller can ask, so it reports through a flag the caller keeps:
    // once the writer is gone, the file must be as long as what was written.
    class CheckedFileStream final : public juce::OutputStream
    {
    public:
        using Failed = std::shared_ptr<std::atomic<bool>>;
        static Failed makeFailedFlag() { return std::make_shared<std::atomic<bool>> (false); }

        static std::unique_ptr<CheckedFileStream> open (const juce::File& file, Failed failed)
        {
            auto stream = file.createOutputStream();
            if (stream == nullptr) return nullptr;
            return std::unique_ptr<CheckedFileStream> (new CheckedFileStream (file, std::move (stream),
                                                                              std::move (failed)));
        }

        ~CheckedFileStream() override
        {
            stream->flush();
            const bool streamOk = stream->getStatus().wasOk();
            stream.reset();   // closes the file
            if (! streamOk || file.getSize() != written) failed->store (true);
        }

        void flush() override { stream->flush(); }
        bool setPosition (juce::int64 position) override { return stream->setPosition (position); }
        juce::int64 getPosition() override { return stream->getPosition(); }

        bool write (const void* data, size_t numBytes) override
        {
            const bool ok = stream->write (data, numBytes);
            written = juce::jmax (written, stream->getPosition());
            return ok;
        }

    private:
        CheckedFileStream (const juce::File& target, std::unique_ptr<juce::FileOutputStream> output, Failed flag)
            : file (target), stream (std::move (output)), failed (std::move (flag)) {}

        juce::File file;
        std::unique_ptr<juce::FileOutputStream> stream;
        Failed failed;
        juce::int64 written { 0 };
    };
}
