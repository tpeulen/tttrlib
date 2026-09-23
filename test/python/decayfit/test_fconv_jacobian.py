"""`fconv_per_cs_jacobian`: the decay and its exact derivatives in one pass.

The kernels in `DecayConvolution.h` that end in `_ad` are templates on the
scalar type, written so they can be instantiated with `tttrlib::Dual` and yield
derivatives from the same recursion that yields the curve. Only the `double`
instantiation used to reach Python, so a caller who wanted derivatives had to
finite-difference the model or wrap it in someone else's autograd — which costs
`2 * n_parameters` model evaluations for the first, and gives no exact Hessian
for the second, because a hand-written backward has no second derivative for AD
to find.

What the derivatives are checked against here is central differences. That is
the right reference and not a circular one: the two are computed from different
code (`fconv_per_cs` at perturbed parameters, versus the dual recursion), and
they agree to central-difference accuracy rather than to round-off, which is
the honest expectation — the finite difference is the inaccurate side.

The column order is the contract: one column per entry of the lifetime spectrum
in `x`'s own interleaved order, then one for the timeshift.
"""

import unittest

import numpy as np

import tttrlib


def gaussian_irf(n_points, centre=40.0, width=7.0):
    return np.ascontiguousarray(
        np.exp(-0.5 * ((np.arange(n_points) - centre) / width) ** 2))


def spectrum(n_exp):
    """`(amplitude, lifetime)` pairs, well separated so no column is degenerate."""
    return np.ascontiguousarray(
        np.array([v for k in range(n_exp)
                  for v in (0.2 + 0.1 * k, 0.8 + 1.3 * k)], dtype=np.float64))


class TestTheJacobian(unittest.TestCase):

    n_points = 512
    period = 32.0

    def setUp(self):
        self.dt = self.period / self.n_points
        self.irf = gaussian_irf(self.n_points)

    def model(self, x, time_shift):
        """The same model by the shipped scalar route: shift, then convolve.

        Note the argument order of `shift_lamp` from Python -- `(lamp, lampsh)`,
        the reverse of the C++ signature. Getting that backwards silently
        returns zeros, which is how the first version of this test "failed".
        """
        shifted = np.zeros(self.n_points)
        tttrlib.shift_lamp(self.irf, shifted, time_shift)
        fit = np.zeros(self.n_points)
        tttrlib.fconv_per_cs(fit=fit, irf=shifted, x=x, period=self.period,
                             conv_stop=self.n_points - 1, stop=-1, dt=self.dt)
        return fit

    def jacobian(self, x, time_shift):
        fit = np.zeros(self.n_points)
        jac = np.zeros((self.n_points, len(x) + 1))
        tttrlib.fconv_per_cs_jacobian(
            fit=fit, jacobian=jac, irf=self.irf, x=x, period=self.period,
            time_shift=time_shift, conv_stop=self.n_points - 1, stop=-1,
            dt=self.dt)
        return fit, jac

    def test_the_curve_is_the_one_the_scalar_kernel_gives(self):
        """The value must not drift from `fconv_per_cs`: it is the same model,
        and a Jacobian attached to a slightly different curve is a trap."""
        for n_exp in (1, 2, 5, 9):
            for time_shift in (0.0, 1.7, -2.3):
                with self.subTest(n_exp=n_exp, time_shift=time_shift):
                    x = spectrum(n_exp)
                    fit, _ = self.jacobian(x, time_shift)
                    np.testing.assert_allclose(fit, self.model(x, time_shift),
                                               rtol=0, atol=1e-12)

    def test_every_column_matches_a_central_difference(self):
        """Including the last one, which is the timeshift.

        The parameter counts straddle the block width deliberately: 9 lifetimes
        is 19 parameters, so three passes with the last one only partly filled,
        which is where an off-by-one in the seeding would show.
        """
        for n_exp in (1, 2, 5, 9):
            x = spectrum(n_exp)
            time_shift = 1.7
            _, jac = self.jacobian(x, time_shift)
            self.assertEqual(jac.shape, (self.n_points, 2 * n_exp + 1))
            for column in range(2 * n_exp + 1):
                with self.subTest(n_exp=n_exp, column=column):
                    h = 1e-6 * (max(1.0, abs(x[column]))
                                if column < len(x) else 1.0)
                    xp, xm = x.copy(), x.copy()
                    shift_p = shift_m = time_shift
                    if column < len(x):
                        xp[column] += h
                        xm[column] -= h
                    else:
                        shift_p += h
                        shift_m -= h
                    central = (self.model(xp, shift_p)
                               - self.model(xm, shift_m)) / (2 * h)
                    scale = max(np.abs(central).max(), 1e-30)
                    self.assertLess(
                        np.abs(central - jac[:, column]).max() / scale, 1e-6,
                        "column %d does not match a central difference" % column)

    def test_a_zero_amplitude_still_has_a_lifetime_derivative_of_zero(self):
        """A species with no amplitude contributes nothing, so moving its
        lifetime moves nothing — a structural check the finite difference
        would also pass, kept because it is the one case where a padding lane
        in the blocked kernel could leak into a real column."""
        x = np.ascontiguousarray(np.array([0.7, 1.0, 0.0, 3.0]))
        _, jac = self.jacobian(x, 0.0)
        np.testing.assert_allclose(jac[:, 3], 0.0, atol=1e-15)
        self.assertGreater(np.abs(jac[:, 2]).max(), 0.0,
                           "the amplitude derivative of that species is not zero")

    def test_it_rejects_a_jacobian_of_the_wrong_shape(self):
        x = spectrum(3)
        fit = np.zeros(self.n_points)
        for shape in ((self.n_points, len(x)),          # forgot the timeshift
                      (self.n_points, len(x) + 2),
                      (self.n_points - 1, len(x) + 1)):
            with self.subTest(shape=shape):
                with self.assertRaises(Exception):
                    tttrlib.fconv_per_cs_jacobian(
                        fit=fit, jacobian=np.zeros(shape), irf=self.irf, x=x,
                        period=self.period, time_shift=0.0,
                        conv_stop=self.n_points - 1, stop=-1, dt=self.dt)

    def test_both_outputs_are_written_in_place(self):
        """A fit loop allocates once; the arrays it passes must be the arrays
        that get filled, and must not need clearing between calls."""
        x = spectrum(4)
        fit = np.full(self.n_points, 7.0)
        jac = np.full((self.n_points, len(x) + 1), 7.0)
        tttrlib.fconv_per_cs_jacobian(
            fit=fit, jacobian=jac, irf=self.irf, x=x, period=self.period,
            time_shift=0.5, conv_stop=self.n_points - 1, stop=-1, dt=self.dt)
        again_fit, again_jac = self.jacobian(x, 0.5)
        np.testing.assert_array_equal(fit, again_fit)
        np.testing.assert_array_equal(jac, again_jac)


if __name__ == "__main__":
    unittest.main()
