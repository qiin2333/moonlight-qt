#pragma once

// Tracks a system-drag press that was successfully queued for the host.
// A failed release remains owned until another event or cleanup can retry it.
class MacSystemDragButtonState
{
public:
    void recordPress(int button)
    {
        m_Button = button;
        m_ReleasePending = false;
        m_PendingAwaitButtonUp = false;
        m_AwaitingButtonUp = false;
    }

    bool hasPendingRelease() const { return m_ReleasePending; }

    template <typename SendRelease> void retryPendingRelease(SendRelease sendRelease)
    {
        if (m_ReleasePending) {
            tryRelease(sendRelease, m_PendingAwaitButtonUp);
        }
    }

    template <typename SendRelease> void releaseForReset(SendRelease sendRelease)
    {
        if (m_Button != 0) {
            tryRelease(sendRelease, true);
        }
    }

    template <typename SendRelease> bool handleButtonUp(SendRelease sendRelease)
    {
        if (m_Button != 0) {
            tryRelease(sendRelease, false);
            return true;
        }
        if (m_AwaitingButtonUp) {
            m_AwaitingButtonUp = false;
            return true;
        }
        return false;
    }

    template <typename SendRelease> bool allowButtonDown(SendRelease sendRelease)
    {
        // Preserve ordering if a previous release failed. Otherwise the new
        // press could overwrite the only record of the held host button.
        if (m_ReleasePending && !tryRelease(sendRelease, false)) {
            // This new press is suppressed, so consume its eventual up if a
            // background retry succeeds before the physical button is raised.
            m_PendingAwaitButtonUp = true;
            return false;
        }
        m_AwaitingButtonUp = false;
        return true;
    }

private:
    template <typename SendRelease> bool tryRelease(SendRelease sendRelease, bool awaitButtonUp)
    {
        if (sendRelease(m_Button) != 0) {
            m_ReleasePending = true;
            m_PendingAwaitButtonUp = awaitButtonUp;
            return false;
        }
        m_Button = 0;
        m_ReleasePending = false;
        m_PendingAwaitButtonUp = false;
        m_AwaitingButtonUp = awaitButtonUp;
        return true;
    }

    int m_Button = 0;
    bool m_ReleasePending = false;
    bool m_PendingAwaitButtonUp = false;
    bool m_AwaitingButtonUp = false;
};
