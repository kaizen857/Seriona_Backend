#include "metadata_synchronizer.h"

namespace seriona::metadata {

namespace {

[[nodiscard]] bool sameOptionalString(const std::optional<std::string>& left,
                                      const std::optional<std::string>& right) {
  return left == right;
}

[[nodiscard]] bool sameOptionalPath(const std::optional<std::filesystem::path>& left,
                                    const std::optional<std::filesystem::path>& right) {
  return left == right;
}

[[nodiscard]] bool sameOptionalInt64(const std::optional<std::int64_t>& left,
                                     const std::optional<std::int64_t>& right) {
  return left == right;
}

[[nodiscard]] bool sameTrack(const MetadataTrackIdentityDto& left, const MetadataTrackIdentityDto& right) {
  return left.trackId == right.trackId && left.filePath == right.filePath && left.fileUri == right.fileUri &&
         left.sourceId == right.sourceId && left.libraryId == right.libraryId &&
         left.trackNumber == right.trackNumber;
}

[[nodiscard]] bool sameTrack(const std::optional<control::TrackIdentity>& left,
                             const std::optional<control::TrackIdentity>& right) {
  if (left.has_value() != right.has_value()) {
    return false;
  }

  if (!left) {
    return true;
  }

  return left->trackId == right->trackId && left->filePath == right->filePath && left->sourceId == right->sourceId &&
         left->libraryId == right->libraryId;
}

[[nodiscard]] bool sameArtwork(const MetadataArtworkRefDto& left, const MetadataArtworkRefDto& right) {
  return sameOptionalPath(left.localPath, right.localPath) && sameOptionalString(left.uri, right.uri) &&
         sameOptionalString(left.contentHash, right.contentHash);
}

[[nodiscard]] bool sameFields(const MetadataFieldSet& left, const MetadataFieldSet& right) {
  return sameOptionalString(left.title, right.title) && sameOptionalString(left.artist, right.artist) &&
         sameOptionalString(left.album, right.album) && sameOptionalString(left.albumArtist, right.albumArtist) &&
         sameOptionalString(left.genre, right.genre);
}

[[nodiscard]] bool sameCapabilities(const MetadataCapabilitySetDto& left,
                                    const MetadataCapabilitySetDto& right) {
  return left.canPlay == right.canPlay && left.canPause == right.canPause && left.canStop == right.canStop &&
         left.canSeek == right.canSeek && left.canSkipNext == right.canSkipNext &&
         left.canSkipPrevious == right.canSkipPrevious && left.canSetRepeat == right.canSetRepeat &&
         left.canSetShuffle == right.canSetShuffle && left.canSetVolume == right.canSetVolume;
}

[[nodiscard]] bool sameStaticMetadata(const MetadataMprisSnapshotDto& left, const MetadataMprisSnapshotDto& right) {
  return sameTrack(left.track, right.track) && sameArtwork(left.artwork, right.artwork) &&
         sameFields(left.fields, right.fields) && sameCapabilities(left.capabilities, right.capabilities);
}

[[nodiscard]] bool sameStaticMetadata(const MetadataWindowsSnapshotDto& left,
                                      const MetadataWindowsSnapshotDto& right) {
  return sameTrack(left.track, right.track) && sameArtwork(left.artwork, right.artwork) &&
         sameFields(left.fields, right.fields) && sameCapabilities(left.capabilities, right.capabilities);
}

[[nodiscard]] bool sameTimeline(const MetadataMprisSnapshotDto& left, const MetadataMprisSnapshotDto& right) {
  return left.playbackStatus == right.playbackStatus && left.positionMicros == right.positionMicros &&
         sameOptionalInt64(left.durationMicros, right.durationMicros) &&
         sameOptionalInt64(left.bufferedMicros, right.bufferedMicros) &&
         sameOptionalInt64(left.seekableFromMicros, right.seekableFromMicros) &&
         sameOptionalInt64(left.seekableToMicros, right.seekableToMicros) && left.repeatMode == right.repeatMode &&
         left.shuffle == right.shuffle && left.muted == right.muted && left.volume == right.volume;
}

[[nodiscard]] bool sameTimeline(const MetadataWindowsSnapshotDto& left,
                                const MetadataWindowsSnapshotDto& right) {
  return left.playbackStatus == right.playbackStatus && left.positionMicros == right.positionMicros &&
         sameOptionalInt64(left.durationMicros, right.durationMicros) &&
         sameOptionalInt64(left.bufferedMicros, right.bufferedMicros) &&
         sameOptionalInt64(left.seekableFromMicros, right.seekableFromMicros) &&
         sameOptionalInt64(left.seekableToMicros, right.seekableToMicros) && left.repeatMode == right.repeatMode &&
         left.shuffle == right.shuffle;
}

[[nodiscard]] bool shouldEmitTimelineImmediately(const control::PlayerStateSnapshot& current,
                                                 const std::optional<control::PlayerStateSnapshot>& previous) {
  if (!previous) {
    return true;
  }

  if (!sameTrack(current.currentTrack, previous->currentTrack)) {
    return true;
  }

  if (current.playback.state == control::PlaybackStatus::Stopped) {
    return true;
  }

  if (current.playback.state == control::PlaybackStatus::Paused &&
      previous->playback.state != control::PlaybackStatus::Paused) {
    return true;
  }

  if (current.playback.state == control::PlaybackStatus::Seeking) {
    return true;
  }

  if (current.playback.state == control::PlaybackStatus::Playing &&
      previous->playback.state != control::PlaybackStatus::Playing) {
    return true;
  }

  return false;
}

[[nodiscard]] bool shouldEmitTimelineByCadence(const control::PlayerStateSnapshot& current,
                                               const std::optional<control::PlayerStateSnapshot>& previous) {
  if (!previous) {
    return true;
  }

  return current.playback.state == control::PlaybackStatus::Playing &&
         previous->playback.state == control::PlaybackStatus::Playing &&
         sameTrack(current.currentTrack, previous->currentTrack) &&
         current.timeline.position.count() / 1000 != previous->timeline.position.count() / 1000;
}

}  // namespace

bool MetadataSynchronizer::isFreshEnough(const control::PlayerStateSnapshot& snapshot,
                                         const std::optional<control::SnapshotFreshness>& lastFreshness) {
  if (!lastFreshness) {
    return true;
  }

  if (snapshot.freshness.version > lastFreshness->version) {
    return true;
  }

  if (snapshot.freshness.version < lastFreshness->version) {
    return false;
  }

  return snapshot.freshness.sampledAt >= lastFreshness->sampledAt;
}

bool MetadataSynchronizer::staticMetadataChanged(const MetadataPlatformSnapshotDto& current,
                                                 const MetadataPlatformSnapshotDto& previous) {
  return !sameStaticMetadata(current.mpris, previous.mpris) || !sameStaticMetadata(current.windows, previous.windows);
}

bool MetadataSynchronizer::timelineChanged(const MetadataPlatformSnapshotDto& current,
                                           const MetadataPlatformSnapshotDto& previous) {
  return !sameTimeline(current.mpris, previous.mpris) || !sameTimeline(current.windows, previous.windows);
}

bool MetadataSynchronizer::shouldEmitTimelineImmediately(const control::PlayerStateSnapshot& current,
                                                         const std::optional<control::PlayerStateSnapshot>& previous) {
  return ::seriona::metadata::shouldEmitTimelineImmediately(current, previous);
}

MetadataSyncPlan MetadataSynchronizer::synchronize(const control::PlayerStateSnapshot& snapshot) {
  MetadataSyncPlan plan{};

  if (!isFreshEnough(snapshot, lastFreshness_)) {
    return plan;
  }

  const auto current = mapPlayerStateSnapshot(snapshot);
  const auto metadataChanged = !lastStaticSnapshot_ || staticMetadataChanged(current, *lastStaticSnapshot_);
  const auto timelineImmediate = shouldEmitTimelineImmediately(snapshot, lastSnapshot_);
  const auto timelineCadence = shouldEmitTimelineByCadence(snapshot, lastSnapshot_);

  plan.snapshot = current;
  plan.emitMetadata = metadataChanged;
  plan.emitTimeline = timelineImmediate || timelineCadence;

  lastFreshness_ = snapshot.freshness;
  lastSnapshot_ = snapshot;

  if (plan.emitMetadata) {
    lastStaticSnapshot_ = current;
  }

  if (plan.emitTimeline) {
    lastTimelineSnapshot_ = current;
    lastTimelineEmitAt_ = snapshot.freshness.sampledAt;
    hasTimelineEmitAt_ = true;
  }

  return plan;
}

}  // namespace seriona::metadata
