#include "bandwidth.h"

#include <algorithm>

using namespace std::chrono;

static constexpr std::uint32_t k_DefaultWindowSeconds = 10;
static constexpr std::uint32_t k_DefaultBucketIntervalMs = 250;

static std::uint32_t EffectiveWindowSeconds(std::uint32_t windowSeconds)
{
    return windowSeconds > 0 ? windowSeconds : k_DefaultWindowSeconds;
}

BandwidthTracker::BandwidthTracker(uint32_t windowSec, uint32_t bucketMs)
    : windowSeconds(seconds(EffectiveWindowSeconds(windowSec))),
      bucketIntervalMs(
          bucketMs > 0 ? (int)(std::min)(bucketMs, EffectiveWindowSeconds(windowSec) * 1000U / 4U)
                       : k_DefaultBucketIntervalMs)
{
    // The interval is clamped so the window always contains at least four
    // buckets, keeping bucketCount >= 4 and the average window non-empty
    bucketCount = EffectiveWindowSeconds(windowSec) * 1000U / (uint32_t)bucketIntervalMs;
    buckets.resize(bucketCount);
}

// Add bytes recorded at the current time.
void BandwidthTracker::AddBytes(size_t bytes)
{
    std::lock_guard<std::mutex> lock(mtx);
    auto now = steady_clock::now();
    updateBucket(bytes, now);
}

// We don't want to average the entire window used for peak,
// so average only the newest 25% of complete buckets
double BandwidthTracker::GetAverageMbps()
{
    std::lock_guard<std::mutex> lock(mtx);
    auto now = steady_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch());
    int currentIndex = (ms.count() / bucketIntervalMs) % bucketCount;
    int maxBuckets = bucketCount / 4;
    size_t totalBytes = 0;

    // Sum exactly the maxBuckets completed buckets that precede the
    // in-progress one
    for (int i = 1; i <= maxBuckets; i++) {
        int idx = (currentIndex - i + bucketCount) % bucketCount;
        const Bucket &bucket = buckets[idx];
        if (isValid(bucket, now)) {
            totalBytes += bucket.bytes;
        }
    }

    // The summed buckets always cover exactly maxBuckets bucket intervals,
    // so the rate is normalized against that fixed duration instead of a
    // wall-clock window that shifts with the current bucket phase
    double elapsedSeconds = maxBuckets * (bucketIntervalMs / 1000.0);
    if (elapsedSeconds <= 0.0) {
        return 0.0;
    }

    return totalBytes * 8.0 / 1000000.0 / elapsedSeconds;
}

double BandwidthTracker::GetPeakMbps()
{
    std::lock_guard<std::mutex> lock(mtx);
    auto now = steady_clock::now();
    double peak = 0.0;
    for (const auto &bucket : buckets) {
        if (isValid(bucket, now)) {
            double throughput = getBucketMbps(bucket);
            if (throughput > peak) {
                peak = throughput;
            }
        }
    }
    return peak;
}

unsigned int BandwidthTracker::GetWindowSeconds()
{
    return (unsigned int)windowSeconds.count();
}

/// private methods

inline double BandwidthTracker::getBucketMbps(const Bucket &bucket) const
{
    return bucket.bytes * 8.0 / 1000000.0 / (bucketIntervalMs / 1000.0);
}

// Check if a bucket's data is still valid (within the window)
inline bool BandwidthTracker::isValid(const Bucket &bucket, steady_clock::time_point now) const
{
    return (now - bucket.start) <= windowSeconds;
}

void BandwidthTracker::updateBucket(size_t bytes, steady_clock::time_point now)
{
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count();
    int bucketIndex = (ms / bucketIntervalMs) % bucketCount;
    auto aligned_ms = ms - (ms % bucketIntervalMs);
    auto bucketStart = steady_clock::time_point(milliseconds(aligned_ms));

    Bucket &bucket = buckets[bucketIndex];

    if (now - bucket.start > windowSeconds) {
        bucket.bytes = 0;
        bucket.start = bucketStart;
    }

    if (bucket.start != bucketStart) {
        bucket.bytes = bytes;
        bucket.start = bucketStart;
    } else {
        bucket.bytes += bytes;
    }
}
