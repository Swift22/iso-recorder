ISO Recorder 0.4.0
One file per source, while you stream.
=====================================================

WHAT IT DOES
  While you stream, it records every source you pick to its own file —
  the game to one, your character to another, the microphone to another —
  instead of one flat recording of everything mixed together. Hand the
  folder to an editor and every piece is still separate.


INSTALL
  1. Close OBS Studio if it is running.

  2. Double-click Install.cmd.
     Windows will ask for permission once. Say yes.

  3. Start OBS Studio. Open the Docks menu and tick "ISO Recorder".


USE
  The panel lists your sources under two headings:

    Picture   scenes and anything with a picture
    Sound     audio-only sources, like a microphone

  Tick the ones you want. Press Record. Press it again when you are done.

  Your ticks are remembered. Next time you open OBS, and after you
  stop a recording, the same sources are ticked again. Untick them
  all if you would rather start from a clean slate.

  Leave "Start and stop with the stream" ticked and it follows your
  stream instead, so you cannot forget.

  Tick "Also record the live scene" if you also want one reference file
  of the program exactly as viewers saw it.

  "Recording size" starts at the same size as your stream. Pick something
  smaller to save room and take work off the graphics card. It only ever
  scales down, never up.

  A source with see-through parts records them as green, not black —
  a plain green panel sits behind every picture. There is nothing to set up.

  Playing more than one game? Press the blue "New game" button between
  them. It carries on recording without a break, but starts a new folder
  for what comes next. Press it as often as you like.


WHERE THE FILES GO
  One folder per session, under the folder shown in "Save to":

    Videos/ISO Recorder/2026-09-13 21-04-18/
        01_Game/
            01_Game.mov
            02_Character.mov
            01_Game.wav
            02_Microphone.wav
        02_Game/          after you press "New game"
            01_Game.mov
            02_Character.mov
        03_Game/          and so on, as many as you need
        session.json      the exact start time of every file
        README.txt        a plain note for whoever edits it

  Every file is timed from the same clock, so they line up. If you
  switch a track on partway through, its file starts later and its start
  time is written down — put the clip there and it still lines up.

  Nothing is cut, mixed or ducked. Each file is that one source exactly
  as OBS saw it.


IF SOMETHING GOES WRONG
  A source that cannot record — a window that closed, a device that is
  not plugged in — is written into session.json as failed, with the
  reason, and everything else keeps recording. The panel shows it too.


WHAT IT NEEDS
  OBS Studio 32.x, 64-bit Windows.

  Each visual source is its own encoder running at once, alongside your
  stream. Recording four or five at a time is fine on a modern GPU;
  past that, files can come out short. The panel warns you before you
  get there, and a file that does come out short is marked incomplete
  instead of passing as finished.


LICENSE
  GPL-2.0. It links OBS's own library, which is GPL-2.0. See LICENSE.txt.
  Source: https://github.com/Swift22/iso-recorder
