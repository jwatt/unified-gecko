/* vim: set shiftwidth=2 tabstop=8 autoindent cindent expandtab: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_AnimationPlayer_h
#define mozilla_dom_AnimationPlayer_h

#include <algorithm> // for std::max
#include "nsWrapperCache.h"
#include "nsCycleCollectionParticipant.h"
#include "mozilla/Attributes.h"
#include "mozilla/TimeStamp.h" // for TimeStamp, TimeDuration
#include "mozilla/dom/Animation.h" // for Animation
#include "mozilla/dom/AnimationTimeline.h" // for AnimationTimeline
#include "nsCSSProperty.h" // for nsCSSProperty

// X11 has a #define for CurrentTime.
#ifdef CurrentTime
#undef CurrentTime
#endif

struct JSContext;

namespace mozilla {

struct ElementPropertyTransition;

namespace dom {

class AnimationPlayer : public nsWrapperCache
{
protected:
  virtual ~AnimationPlayer() { }

public:
  explicit AnimationPlayer(AnimationTimeline* aTimeline)
    : mIsRunningOnCompositor(false)
    , mIsFinishedTransition(false)
    , mLastNotification(LAST_NOTIFICATION_NONE)
    , mTimeline(aTimeline)
  {
    SetIsDOMBinding();
  }

  NS_INLINE_DECL_CYCLE_COLLECTING_NATIVE_REFCOUNTING(AnimationPlayer)
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_NATIVE_CLASS(AnimationPlayer)

  AnimationTimeline* GetParentObject() const { return mTimeline; }
  virtual JSObject* WrapObject(JSContext* aCx) MOZ_OVERRIDE;

  // AnimationPlayer methods
  Animation* GetSource() const { return mSource; }
  AnimationTimeline* Timeline() const { return mTimeline; }
  double StartTime() const;
  double CurrentTime() const;

  void SetSource(Animation* aSource);
  void Tick();

  // FIXME: If we succeed in moving transition-specific code to a type of
  // AnimationEffect (as per the Web Animations API) we should remove these
  // virtual methods.
  virtual ElementPropertyTransition* AsTransition() { return nullptr; }
  virtual const ElementPropertyTransition* AsTransition() const {
    return nullptr;
  }

  bool IsPaused() const {
    return mPlayState == NS_STYLE_ANIMATION_PLAY_STATE_PAUSED;
  }

  // After transitions finish they need to be retained for one throttle-able
  // cycle (for reasons see explanation in nsTransitionManager.cpp). In the
  // meantime, however, they should be ignored.
  bool IsFinishedTransition() const {
    return mIsFinishedTransition;
  }
  void SetFinishedTransition() {
    MOZ_ASSERT(AsTransition(),
               "Calling SetFinishedTransition but it's not a transition");
    mIsFinishedTransition = true;
  }

  bool IsRunning() const;
  bool IsCurrent() const;

  // Return the duration since the start time of the player, taking into
  // account the pause state.  May be negative.
  // Returns a null value if the timeline associated with this object has a
  // current timestamp that is null or if the start time of this object is
  // null.
  Nullable<TimeDuration> GetCurrentTimeDuration() const {
    const TimeStamp& timelineTime = mTimeline->GetCurrentTimeStamp();
    // FIXME: In order to support arbitrary timelines we will need to fix
    // the pause logic to handle the timeline time going backwards.
    MOZ_ASSERT(timelineTime.IsNull() || !IsPaused() ||
               timelineTime >= mPauseStart,
               "if paused, any non-null value of aTime must be at least"
               " mPauseStart");

    Nullable<TimeDuration> result; // Initializes to null
    if (!timelineTime.IsNull() && !mStartTime.IsNull()) {
      result.SetValue((IsPaused() ? mPauseStart : timelineTime) - mStartTime);
    }
    return result;
  }

  nsString mName;
  // The beginning of the delay period.
  TimeStamp mStartTime;
  TimeStamp mPauseStart;
  uint8_t mPlayState;
  bool mIsRunningOnCompositor;
  // A flag to mark transitions that have finished and are due to
  // be removed on the next throttle-able cycle.
  bool mIsFinishedTransition;

  enum {
    LAST_NOTIFICATION_NONE = uint64_t(-1),
    LAST_NOTIFICATION_END = uint64_t(-2)
  };
  // One of the above constants, or an integer for the iteration
  // whose start we last notified on.
  uint64_t mLastNotification;

  nsRefPtr<AnimationTimeline> mTimeline;
  nsRefPtr<Animation> mSource;
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_AnimationPlayer_h
