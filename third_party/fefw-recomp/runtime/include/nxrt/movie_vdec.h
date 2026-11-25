// SPDX-License-Identifier: GPL-3.0-or-later
// movie::VideoDecoder of the Switch 2 movie library on the host (src/media/movie_vdec.cpp): AV1 through dav1d into the player's CPU frames,
// for a title with game::Info::movie_video_decoder. [media] movies = host (default) | skip (every movie fails to open, core/switch2.cpp).
#pragma once

struct Ctx;

namespace nxrt::media {

// Registers the host decoder and forces main's imports to it. False when off, not such a title, or built without dav1d (NXRT_HAVE_DAV1D).
// Call before the modules are linked; safe to call more than once.
bool install_movie_decoder_hle();

// Registers the host AudioDecoder (Vorbis) as well. The player decodes a movie's sound through that object and waits for its frames,
// so without it the picture stops as soon as the movie's audio starts. Called from install_movie_decoder_hle().
bool install_audio_decoder_hle();

// Hooks the SDK's own movie::Decoder inside subsdk0 (by address) so the library's VideoDecoder/AudioDecoder can run unchanged: the
// layer the player's contract is written against. [media] video_decoder = library installs it and leaves VideoDecoder to the game.
bool install_movie_decoder_layer();

// True once install_movie_decoder_hle() routed the decoder to the host: the game module then puts its player on the CPU-frame path.
bool movie_decoder_enabled();

// True while at least one host movie/cutscene video decoder is active.
bool is_movie_playing();

// Called once per presented frame. Decoded pictures are handed to the player here as well as in its own calls, so a player that waits
// on the output event is woken by the game's frame loop and not only by its previous call (the Switch 1 port, MacNativeGames
// xc2-recomp ov_movie.c movie_tick, does the same). Also reports a decoder that has stopped being called while it still holds work.
void movie_tick();

// The same, on the guest thread that presents: the player's events are signalled from guest code (the SDK's SignalSystemEvent
// takes the event's own critical section), which is how the Switch 1 ports do it and what a host-thread wake cannot guarantee.
void movie_tick_guest(::Ctx* c);

}  // namespace nxrt::media
