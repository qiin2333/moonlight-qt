#pragma once

#include "renderer.h"
#include "coreaudiospatial.h"

#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>

#include <atomic>
#include <cstring>
#include <vector>

// Lock-free FIFO between the audio decoder thread (the only writer) and the
// CoreAudio I/O thread (the only reader). Reads and writes are all-or-nothing.
class CoreAudioRingBuffer
{
public:
    void init(size_t capacity)
    {
        m_Buffer.assign(capacity, 0);
        m_ReadPos.store(0, std::memory_order_relaxed);
        m_WritePos.store(0, std::memory_order_relaxed);
    }

    bool write(const void* data, size_t bytes)
    {
        size_t writePos = m_WritePos.load(std::memory_order_relaxed);
        size_t readPos = m_ReadPos.load(std::memory_order_acquire);
        if (m_Buffer.size() - (writePos - readPos) < bytes) {
            return false;
        }

        // Split the copy where it wraps past the end of the ring
        size_t offset = writePos % m_Buffer.size();
        size_t first = qMin(bytes, m_Buffer.size() - offset);
        memcpy(m_Buffer.data() + offset, data, first);
        memcpy(m_Buffer.data(), static_cast<const unsigned char*>(data) + first, bytes - first);

        m_WritePos.store(writePos + bytes, std::memory_order_release);
        return true;
    }

    bool read(void* data, size_t bytes)
    {
        size_t readPos = m_ReadPos.load(std::memory_order_relaxed);
        size_t writePos = m_WritePos.load(std::memory_order_acquire);
        if (writePos - readPos < bytes) {
            return false;
        }

        size_t offset = readPos % m_Buffer.size();
        size_t first = qMin(bytes, m_Buffer.size() - offset);
        memcpy(data, m_Buffer.data() + offset, first);
        memcpy(static_cast<unsigned char*>(data) + first, m_Buffer.data(), bytes - first);

        m_ReadPos.store(readPos + bytes, std::memory_order_release);
        return true;
    }

private:
    std::vector<unsigned char> m_Buffer;

    // Total bytes ever read and written. 64 bits never wrap in practice, so
    // the fill level is simply their difference.
    std::atomic<size_t> m_ReadPos { 0 };
    std::atomic<size_t> m_WritePos { 0 };
};

class CoreAudioRenderer : public IAudioRenderer
{
public:
    CoreAudioRenderer();

    virtual ~CoreAudioRenderer();

    virtual bool prepareForPlayback(const OPUS_MULTISTREAM_CONFIGURATION* opusConfig);

    virtual void* getAudioBuffer(int* size);

    virtual bool submitAudio(int bytesWritten);

    virtual AudioFormat getAudioBufferFormat();

private:
    bool openOutputUnit();
    AUSpatialMixerOutputType getSpatialMixerOutputType();
    void addListeners();
    void removeListeners();

    static OSStatus renderCallback(void* inRefCon,
                                   AudioUnitRenderActionFlags* ioActionFlags,
                                   const AudioTimeStamp* inTimeStamp,
                                   UInt32 inBusNumber,
                                   UInt32 inNumberFrames,
                                   AudioBufferList* ioData);

    static OSStatus deviceChanged(AudioObjectID inObjectID,
                                  UInt32 inNumberAddresses,
                                  const AudioObjectPropertyAddress* inAddresses,
                                  void* inClientData);

    AudioUnit m_OutputUnit;
    AudioDeviceID m_OutputDevice;
    bool m_ListenersAdded;
    std::atomic<bool> m_NeedsReinit;

    UInt32 m_FrameSize;
    std::vector<unsigned char> m_PacketBuffer;
    CoreAudioRingBuffer m_RingBuffer;

    // Only used for spatial audio, which needs the interleaved samples in one
    // piece before splitting them up by channel
    CoreAudioSpatialMixer* m_SpatialMixer;
    std::vector<unsigned char> m_RenderBuffer;
};
