"""A/B of Photon Distribution Analysis against PAM's PDA histogram library and
the defining formulae of the three-colour physics.

* ``Pda.s1s2`` (2-channel PDA model matrix) against **PAM**
  (Schrimpf et al. 2018, https://gitlab.com/PAM-PIE/PAM at 7319d15d,
  ``functions/PDAFit/histogram_library/PDA_histogram.cpp``), as a **recorded
  fixture** (``test/data/reference/pda_pam_histogram_reference.npz``): the MEX
  source was compiled unmodified through a ``mex.h`` shim and driven with the
  same P(F), p_ch1, backgrounds; single species and mixtures (PAM is one
  species per call -- the mixture is the amplitude-weighted sum). Regenerate
  with ``gen_ab_pda_pam_reference.py`` after re-cloning PAM.
* The independent NumPy transcription of Antonik et al. 2006 (binomial split,
  Poisson background convolution) already pins ``Pda.s1s2`` to 1e-14 in
  ``test_pda_reference.py`` -- cited, not duplicated -- as does the defining
  nested sum for ``PdaBurstLikelihood`` in ``test_pda_burst_likelihood.py``.
* ``channel_probabilities_3c`` against the composition it is --
  ``normalise(excitation @ transfer @ emission)`` -- and ``transfer_matrix_3c``
  against a NumPy transcription of the competing-acceptor cascade
  E_ij = k_ij / (1 + sum_k k_ik), k = (R0/R)^6, propagated down the cascade.
  ``test_pda3c_core.py`` carries the rest of the three-colour core: the
  quadrature grid against moment exactness and the forward model against the
  composition of the two.

ChiSurf is deliberately **not** a reference in either file. This library is its
upstream, so agreement establishes only that two things that move together
still do.
"""
import os
import unittest

import numpy as np

import tttrlib

FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "data", "reference",
                       "pda_pam_histogram_reference.npz")


def _pda(nmax, pf, bg, species):
    pda = tttrlib.Pda(hist2d_nmax=int(nmax), hist2d_nmin=0,
                      background_ch1=float(bg[0]), background_ch2=float(bg[1]), pF=np.asarray(pf).tolist())
    for amplitude, p_ch1 in species:
        pda.append(float(amplitude), float(p_ch1))
    return np.asarray(pda.s1s2)


class TestAgainstPam(unittest.TestCase):
    """PAM's own S1/S2 matrices, recorded by ``gen_ab_pda_pam_reference.py``."""

    @classmethod
    def setUpClass(cls):
        cls.ref = np.load(FIXTURE)

    def test_single_species(self):
        z = self.ref
        for i, p1 in enumerate(z["single_p_ch1"]):
            for j, bg in enumerate(z["single_bg"]):
                with self.subTest(p_ch1=float(p1), bg=tuple(bg)):
                    got = _pda(z["single_nmax"], z["single_pf"], bg, [(1.0, p1)])
                    np.testing.assert_allclose(got, z["single_s1s2"][i, j], rtol=0, atol=1e-15)

    def test_mixture_is_the_weighted_sum_of_pam_species(self):
        z = self.ref
        amps, probs = z["mix_amps"], z["mix_p_ch1"]
        for j, bg in enumerate(z["mix_bg"]):
            with self.subTest(bg=tuple(bg)):
                got = _pda(z["mix_nmax"], z["mix_pf"], bg, zip(amps, probs))
                ref = sum(a * m for a, m in zip(amps, z["mix_species_s1s2"][j]))
                np.testing.assert_allclose(got, ref, rtol=0, atol=1e-15)

    def test_a_non_poisson_pf(self):
        # a bimodal P(F): PAM takes it as given, so must the library
        z = self.ref
        got = _pda(z["bimodal_nmax"], z["bimodal_pf"], z["bimodal_bg"], [(1.0, z["bimodal_p_ch1"])])
        np.testing.assert_allclose(got, z["bimodal_s1s2"], rtol=0, atol=1e-15)


class TestThreeColourPhysics(unittest.TestCase):

    @staticmethod
    def _channel_probabilities(transfer, excitation, emission):
        """The defining composition, in one line of NumPy.

        An excitation vector populates the dyes; the transfer matrix says which
        dye each excitation is finally emitted by (row i = fate of an excitation
        on dye i); the emission matrix routes a photon from a dye into a
        detection channel. So the detected distribution is the product of the
        three, renormalised to a probability:

            p = normalise(excitation @ transfer @ emission)

        Written from that statement rather than from any implementation of it,
        which is the point of an A/B."""
        p = np.asarray(excitation) @ np.asarray(transfer) @ np.asarray(emission)
        return p / p.sum()

    @staticmethod
    def _cascade_transfer(dist, r0):
        """Cascade E for K dyes ordered blue -> green -> red: an excited dye i
        transfers to j>i with k_ij = (R0_ij / R_ij)^6 competing against its
        own decay (rate 1); what reaches j cascades on. Row i = fate of an
        excitation on dye i (fraction emitted by each dye)."""
        K = len(r0.shape) and r0.shape[0]
        T = np.zeros((K, K))
        for i in range(K):
            # excitation of dye i propagates forward
            occupancy = np.zeros(K); occupancy[i] = 1.0
            for a in range(i, K):
                if occupancy[a] == 0.0:
                    continue
                k = np.array([(r0[a, b] / dist[a, b]) ** 6 if b > a else 0.0 for b in range(K)])
                denom = 1.0 + k.sum()
                T[i, a] += occupancy[a] / denom          # emitted by a
                for b in range(a + 1, K):
                    occupancy[b] += occupancy[a] * k[b] / denom
        return T

    def test_transfer_matrix_against_the_cascade_formula(self):
        rng = np.random.default_rng(3)
        for trial in range(20):
            with self.subTest(trial=trial):
                d = rng.uniform(25.0, 90.0, size=3)  # d01, d02, d12
                r0 = rng.uniform(40.0, 60.0, size=3)
                D = np.zeros((3, 3)); D[0, 1] = D[1, 0] = d[0]; D[0, 2] = D[2, 0] = d[1]; D[1, 2] = D[2, 1] = d[2]
                R = np.zeros((3, 3)); R[0, 1] = R[1, 0] = r0[0]; R[0, 2] = R[2, 0] = r0[1]; R[1, 2] = R[2, 1] = r0[2]
                got = np.asarray(tttrlib.transfer_matrix_3c(d.tolist(), r0.tolist(), 3)).reshape(3, 3)
                np.testing.assert_allclose(got, self._cascade_transfer(D, R), rtol=1e-12, atol=1e-14)
                np.testing.assert_allclose(got.sum(axis=1), 1.0, atol=1e-12)

    def test_channel_probabilities_against_the_defining_composition(self):
        """`channel_probabilities_3c` against `excitation @ transfer @ emission`.

        This used to compare against ChiSurf's `pda3c.physics`, which is not a
        valid reference -- this library is ChiSurf's upstream, so "we agree"
        says only that two things that move together still agree -- and it
        needed an absolute path to a checkout, so it skipped everywhere else.
        The composition it is testing is three matrix products and a
        normalisation, so it can simply be stated."""
        r0 = [47.0, 52.0, 58.0]
        # blue/green/red excitation by the blue laser, and an emission matrix
        # with each dye mostly in its own channel plus realistic crosstalk
        excitation = np.array([1.0, 0.06, 0.02])
        emission = np.array([[0.88, 0.10, 0.02],
                             [0.04, 0.85, 0.11],
                             [0.01, 0.07, 0.92]])
        rng = np.random.default_rng(4)
        for trial in range(10):
            with self.subTest(trial=trial):
                d = rng.uniform(30.0, 80.0, size=3)
                T = tttrlib.transfer_matrix_3c(d.tolist(), r0, 3)
                got = np.asarray(tttrlib.channel_probabilities_3c(
                    list(T), excitation.tolist(), emission.flatten().tolist(), 3, 3))
                ref = self._channel_probabilities(
                    np.asarray(T).reshape(3, 3), excitation, emission)
                np.testing.assert_allclose(got, ref, rtol=1e-12, atol=1e-14)
                self.assertAlmostEqual(got.sum(), 1.0, places=12)

    def test_channel_probabilities_are_not_trivially_the_excitation(self):
        """The A/B above passes for a wrong kernel that ignores the transfer
        matrix, unless the transfer matrix actually moves the answer. It does:
        two different distance sets must give different channel
        probabilities."""
        r0 = [47.0, 52.0, 58.0]
        excitation = [1.0, 0.06, 0.02]
        emission = np.array([[0.88, 0.10, 0.02],
                             [0.04, 0.85, 0.11],
                             [0.01, 0.07, 0.92]])
        near = np.asarray(tttrlib.channel_probabilities_3c(
            list(tttrlib.transfer_matrix_3c([30.0, 35.0, 32.0], r0, 3)),
            excitation, emission.flatten().tolist(), 3, 3))
        far = np.asarray(tttrlib.channel_probabilities_3c(
            list(tttrlib.transfer_matrix_3c([90.0, 95.0, 92.0], r0, 3)),
            excitation, emission.flatten().tolist(), 3, 3))
        assert np.max(np.abs(near - far)) > 0.1, (
            f"close and distant dyes gave the same channel probabilities: "
            f"{near} vs {far}")


if __name__ == "__main__":
    unittest.main()
