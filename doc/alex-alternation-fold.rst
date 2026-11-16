Folding the µs-ALEX alternation
===============================

Under microsecond alternating-laser excitation (µs-ALEX) the donor and the
acceptor laser take turns with a period :math:`P` of typically 50--100 µs. In
every period the donor-excitation photons arrive in one time slot and the
acceptor-excitation photons in another. Any analysis that models photon arrival
times on the raw macro-time clock sees that alternation, and it cannot tell it
from a property of the molecule:

* a photon-by-photon hidden Markov model (H2MM) of FRET bursts with the streams
  DD, DA and AA fits states that cycle between donor- and acceptor-excitation
  photons at the alternation rate. In a µs-ALEX data set of a membrane
  transporter (period 100 µs) the model selected six such states with rates of
  about 60 000 per second, which describe the lasers;
* a correlation curve oscillates with the period :math:`P`;
* a time window shorter than :math:`P` sees only one of the lasers.

Folding removes the alternation from the clock. Every period is mapped onto the
length of one excitation slot and the slots are overlaid, as if both lasers were
on at the same time. The data then look like pulsed-interleaved excitation
(PIE) data, and slower dynamics are unaffected.

Definition
----------

The period is divided into slots that start at the phases
:math:`s_0 < s_1 < \dots` (in circular order from :math:`s_0`); :math:`L` is the
length of the longest slot. A photon at macro time :math:`t` with alternation
phase :math:`\phi` is mapped to

.. math::

   \psi = (\phi - s_0) \bmod P, \qquad
   c = t - \psi, \qquad
   t' = \left\lfloor \frac{c L}{P} \right\rfloor + (\psi - (s_k - s_0)) + L

where :math:`k` is the slot that contains :math:`\psi`. Consecutive periods are
:math:`L` folded ticks apart and the position of a photon inside its slot is
kept. For two equal slots starting at phase 0 this is
:math:`t' = \lfloor t/P \rfloor \, P/2 + (t \bmod P) \bmod (P/2) + P/2`. The
constant :math:`L` keeps photons of the first, incomplete period of a file
non-negative; it does not change any time difference.

Time scale
----------

One folded tick corresponds on average to :math:`P/L` macro-time ticks of real
time (``AlternationFold.get_tick_scale``). For a 50/50 duty cycle this is 2; for
an unequal duty cycle it is :math:`P/L_\mathrm{max}`. Rates, dwell times and
correlation lags computed on folded times are converted to seconds with
``macro_time_resolution * tick_scale``.

Phase and slot layout
---------------------

After ``TTTR.alex_to_microtime`` the micro time of every photon is its
alternation phase. The cycle start :math:`c = t - \psi` is then computed per
photon, so the fold is exact even when one container concatenates files whose
clocks start at different phases. For a stream that was not converted the phase
is :math:`(t - \text{period shift}) \bmod P`
(``AlternationFold.fold_macro_times``).

Every excitation window must lie inside one slot. If a window crosses a slot
boundary, part of it is moved to the other end of the folded period.
``AlternationFold.from_windows(period, [start, stop, ...])`` places the slots
for given inclusive phase windows. Overlapping windows are merged. If the period
divides into as many equal slots as there are windows and every window fits into
its own slot, those slots are used, with the boundaries centred in the gaps.
Otherwise every slot boundary is put in the middle of the gap between two
windows.

Which quantities use the folded clock
-------------------------------------

Folded:

* photon-by-photon hidden Markov models and likelihoods (H2MM, Gopich--Szabo);
* time windows of burst variance analysis (BVA) and stoichiometry variance
  analysis (SVA); photon-count slices are not affected;
* time windows of burst-wise PDA: a window of real length :math:`T` then always
  contains the same donor-excitation time;
* correlation at lags shorter than the period.

Not folded:

* burst search, burst durations and count rates. They describe how long a
  molecule spent in the focus and how many photons it emitted per second of real
  time. A folded duration would be shorter by the tick scale and would not mean
  anything else.

The folded times are returned in event order. They are not sorted within a
period: a late photon of the first slot folds after an early photon of the
second slot. Analyses that need ordered times sort them per burst with a stable
sort and permute the photon attributes with them.

Example
-------

.. code-block:: python

   import numpy as np
   import tttrlib

   t = tttrlib.TTTR("measurement_alex.ptu")       # after alex_to_microtime
   print(t.get_alternation_period())              # e.g. 8000; 0 for TCSPC data
   fold = tttrlib.AlternationFold.from_windows(8000, [4616, 7784, 280, 3800])
   folded = np.asarray(t.get_folded_macro_times(fold))
   seconds_per_tick = t.header.macro_time_resolution * fold.tick_scale

ChiSurf applies the fold by default to ALEX data in its H2MM, Gopich--Szabo,
BVA, SVA, burst-wise PDA and burst-wise FCS tools, with a setting to turn it
off; the choice is recorded with each result.
