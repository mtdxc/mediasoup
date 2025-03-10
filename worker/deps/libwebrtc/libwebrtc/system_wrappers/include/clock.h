/*
 *  Copyright (c) 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef SYSTEM_WRAPPERS_INCLUDE_CLOCK_H_
#define SYSTEM_WRAPPERS_INCLUDE_CLOCK_H_

#include <stdint.h>

#include <memory>

#include "api/units/timestamp.h"
#include "system_wrappers/include/ntp_time.h"
#if 0
#include "rtc_base/synchronization/rw_lock_wrapper.h"
#endif
namespace webrtc {

// January 1970, in NTP seconds.
const uint32_t kNtpJan1970 = 2208988800UL;

// Magic NTP fractional unit.
const double kMagicNtpFractionalUnit = 4.294967296E+9;

// A clock interface that allows reading of absolute and relative timestamps.
class Clock {
 public:
  virtual ~Clock() {}
  // Return a timestamp relative to an unspecified epoch.
  virtual Timestamp CurrentTime() {
    return Timestamp::us(TimeInMicroseconds());
  }
  virtual int64_t TimeInMilliseconds() { return CurrentTime().ms(); }
  virtual int64_t TimeInMicroseconds() { return CurrentTime().us(); }

  // Retrieve an NTP absolute timestamp.
  virtual NtpTime CurrentNtpTime() = 0;

  // Retrieve an NTP absolute timestamp in milliseconds.
  virtual int64_t CurrentNtpInMilliseconds() = 0;

  // Converts an NTP timestamp to a millisecond timestamp.
  static int64_t NtpToMs(uint32_t seconds, uint32_t fractions) {
    return NtpTime(seconds, fractions).ToMs();
  }

  // Returns an instance of the real-time system clock implementation.
  static Clock* GetRealTimeClock();
};
#if 0
class SimulatedClock : public Clock {
 public:
  explicit SimulatedClock(int64_t initial_time_us);
  explicit SimulatedClock(Timestamp initial_time);

  ~SimulatedClock() override;

  // Return a timestamp relative to some arbitrary source; the source is fixed
  // for this clock.
  Timestamp CurrentTime() override;

  // Retrieve an NTP absolute timestamp.
  NtpTime CurrentNtpTime() override;

  // Converts an NTP timestamp to a millisecond timestamp.
  int64_t CurrentNtpInMilliseconds() override;

  // Advance the simulated clock with a given number of milliseconds or
  // microseconds.
  void AdvanceTimeMilliseconds(int64_t milliseconds);
  void AdvanceTimeMicroseconds(int64_t microseconds);
  void AdvanceTime(TimeDelta delta);

 private:
  Timestamp time_;
  std::unique_ptr<RWLockWrapper> lock_;
};
#endif
}  // namespace webrtc

#endif  // SYSTEM_WRAPPERS_INCLUDE_CLOCK_H_
