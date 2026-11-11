sample-base {version} demos for the Akai MPC Sample
===================================================

A proof of concept: a standalone app that runs on the MPC Sample in place
of the stock app, started from the microSD card. No computer, SSH or
network connection is needed.

THIS IS A PROOF OF CONCEPT. A crash, or any failure to start, puts the
stock MPC app back; nothing on the MPC itself is changed, only files on the
microSD card. Back up the card first anyway.


Requirements
------------
- MPC Sample on firmware 1.3.0.12 exactly. On any other firmware the demos
  do not start and the stock app keeps running.
- A microSD card for the MPC (the one you already use is fine).


Install
-------
1. Back up the card. If it already has a folder named "autorun" at its top
   level (MPC Sample Plugins or Melodee use one, for example), move it
   somewhere safe: this one replaces it.
2. Copy the "autorun" folder from this zip to the top level of the card,
   so the card has autorun/autorun.sh, autorun/autorun.json,
   autorun/autorun.check and autorun/sample-base/.
   Do not edit or rename anything inside it: the MPC refuses an autorun
   whose files changed.
3. Put the card in the MPC and switch it on. The stock app starts first;
   a few seconds later the demos take over the screen.


What to try
-----------
- Pads: play C major from C4 to D6 on a small sine/triangle synth.
  The main volume knob sets the level. External MIDI plays it too.
- Sample: record Line in, Mic or USB-C for up to 60 seconds and play it
  back (Sample Record, Play, Stop). The buttons above the screen pick the
  input and channels.
- Pad FX: a looping light show over every LED on the surface.
- 16 Levels: see each pad's velocity and pressure.
- SEQ: an eight-drum, sixteen-step sequencer with a house groove.
  Play and Stop run it; Tap Tempo, the wheel, - and + set the tempo.
- Knob FX: a live system dashboard (CPU, temperatures, audio, ports,
  storage).

The right button above the screen returns to the synth from any page.


Back to the stock app
---------------------
- Push the wheel: the stock app comes back until the next time the MPC is
  switched on.
- To stop the demos starting at all, delete the "autorun" folder from the
  card (and put back any autorun folder you moved away).


Reporting problems
------------------
Say what you pressed, what you saw and the version line below. If the demos
did not start at all, check that the firmware is 1.3.0.12 and that the
autorun folder sits at the top level of the card.


Version and source
------------------
sample-base {version}, source commit {commit}
Source code: {source}

sample-base is free software under the GNU Affero General Public License,
version 3 (licenses/LICENSE-AGPL-3.0.txt). It uses JUCE under the AGPLv3;
see licenses/. Not affiliated with or endorsed by inMusic or Akai
Professional.
