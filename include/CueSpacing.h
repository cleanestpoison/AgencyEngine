#pragma once

namespace AgencyEngine
{
    // Party-wide gate at the cue dispatch boundary. The Director calls
    // TryBeginAttempt before removing a queued cue, so one successful call both
    // reserves the opening and prevents another cue entering the same batch.
    class CueSpacingGate
    {
    public:
        static constexpr std::int64_t kDurationMs = 60'000;

        [[nodiscard]] bool TryBeginAttempt(std::int64_t nowMs)
        {
            if (RemainingMs(nowMs) > 0) {
                return false;
            }
            attemptedAtMs_ = nowMs;
            return true;
        }

        void Pause(std::int64_t elapsedMs)
        {
            if (attemptedAtMs_ && elapsedMs > 0) {
                *attemptedAtMs_ += elapsedMs;
            }
        }

        void Reset()
        {
            attemptedAtMs_.reset();
        }

        [[nodiscard]] std::int64_t RemainingMs(std::int64_t nowMs) const
        {
            if (!attemptedAtMs_) {
                return 0;
            }
            return std::max<std::int64_t>(0, kDurationMs - (nowMs - *attemptedAtMs_));
        }

        [[nodiscard]] std::int64_t BlockedDurationMs(std::int64_t fromMs, std::int64_t toMs) const
        {
            if (!attemptedAtMs_ || toMs <= fromMs) {
                return 0;
            }
            const auto blockedFrom = std::max(fromMs, *attemptedAtMs_);
            const auto blockedUntil = std::min(toMs, *attemptedAtMs_ + kDurationMs);
            return std::max<std::int64_t>(0, blockedUntil - blockedFrom);
        }

    private:
        std::optional<std::int64_t> attemptedAtMs_;
    };
}
