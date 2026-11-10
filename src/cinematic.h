#pragma once
// In-engine cinematics: the game's own "cutscene / photo mode" state, so cinematics that render through the gameplay
// camera go to the theater screen instead of head-tracked stereo. See cinematic.cpp.
namespace cinematic {
void Install();
bool Active();        // a pre-rendered (Bink) movie plays and CinematicTheater is on: theater screen, 16:9
bool MoviePlaying();  // a full-size Bink movie is being decoded  // a cinematic (or photo mode) is running, as the game's frame-rate limiter sees it
}  // namespace cinematic
