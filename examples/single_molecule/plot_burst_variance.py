"""Burst variance: is a photon ratio constant inside a burst?
===========================================================

A static molecule keeps its FRET efficiency and its stoichiometry while it crosses
the focus. Burst variance analyses test that, burst by burst: cut the burst into
slices, compute a photon ratio per slice, and compare the scatter of the ratio
with the scatter shot noise alone gives.

* **BVA** (Torella et al., Biophys. J. 2011,
  `doi:10.1016/j.bpj.2011.01.066 <https://doi.org/10.1016/j.bpj.2011.01.066>`_):
  the proximity ratio :math:`n_A / (n_A + n_D)`; excess scatter is FRET dynamics.
* **SVA**: the stoichiometry; excess scatter is bleaching, blinking or two
  molecules in the focus.

`tttrlib.BurstVariance` is the engine for both: any two photon streams A and B,
photon slices or (period-aligned) time windows, and a **per-burst static bound**,
the quantile of the slice SD of binomial simulations with the burst's own slice
photon numbers. The mean shot-noise line :math:`\\sqrt{p(1-p)/n}` is not a
bound: about half of all static bursts lie above it. This example simulates
static and dynamic bursts and classifies them.
"""
import numpy as np
import matplotlib.pyplot as plt

import tttrlib

rng = np.random.default_rng(7)

# %%
# Simulate bursts
# ---------------
# Channel 0 = donor, 1 = acceptor. Static bursts keep one acceptor probability;
# dynamic bursts switch between 0.2 and 0.8 every 40 photons.
macro, channel, bounds, dynamic = [], [], [], []
t = 0
for k in range(400):
    moving = k % 2 == 1
    p_static = rng.uniform(0.2, 0.8)
    start = len(macro)
    for i in range(int(rng.integers(60, 240))):
        t += int(rng.integers(1, 20))
        p = (0.2 if (i // 40) % 2 == 0 else 0.8) if moving else p_static
        macro.append(t)
        channel.append(1 if rng.random() < p else 0)
    bounds.append((start, len(macro) - 1))
    dynamic.append(moving)
    t += 1_000_000
data = tttrlib.TTTR()
data.append_events(np.asarray(macro, np.uint64), np.zeros(len(macro), np.uint16),
                   np.asarray(channel, np.int8), np.zeros(len(macro), np.int8), False, 0)
bounds = np.asarray(bounds, np.int64)
dynamic = np.asarray(dynamic)

# %%
# Slice and reduce
# ----------------
# A is the acceptor, B the donor: the ratio is the proximity ratio. Slices of
# 10 photons; arguments are positional: mode, photons per slice, window, alignment,
# alignment offset, min photons per slice, min slices, ddof.
bv = tttrlib.BurstVariance(data)
bv.set_stream_a([1])
bv.set_stream_b([0])
bv.compute(bounds, bv.PHOTONS, 10, 0.0, 0.0, 0.0, 1, 2, 1)

# %%
# The static bound and the classes
# --------------------------------
# 300 static simulations per burst, 95 % quantile: 1 static, 0 dynamic, -1 untested.
bound = bv.static_bound(0.95, 300, 11, 1.0, 1)
classes = tttrlib.BurstVariance.classify(bv.sd, bound)
print("static bursts classified dynamic: %.3f" % np.mean(classes[~dynamic] == 0))
print("dynamic bursts classified dynamic: %.3f" % np.mean(classes[dynamic] == 0))

# %%
# Plot
# ----
p = np.linspace(0, 1, 200)
fig, ax = plt.subplots(figsize=(6, 4.5))
for value, colour, label in ((1, "tab:blue", "static (class 1)"), (0, "tab:red", "dynamic (class 0)")):
    sel = classes == value
    ax.scatter(bv.mean[sel], bv.sd[sel], s=8, color=colour, label=label)
shot = tttrlib.BurstVariance.shot_noise_sd(p, [10.0])
ax.plot(p, shot, color="tab:orange", label="shot noise (mean), n = 10")
ax.set_xlabel("proximity ratio (mean of the slices)")
ax.set_ylabel("SD of the slice proximity ratio")
ax.legend()
plt.show()
