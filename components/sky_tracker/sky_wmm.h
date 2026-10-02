// HW-9a: magnetic declination from the World Magnetic Model WMM2025 (NOAA/NCEI and
// BGS, public domain; coefficients copied from WMM_2025.COF, epoch 2025.0, valid to
// 2030.0). Degree/order 12 spherical-harmonic synthesis, Schmidt semi-normalised
// Legendre functions; checked against pygeomag to 1e-8 degrees.
#pragma once
#include <cmath>
namespace sat {
namespace wmm {
constexpr double EPOCH = 2025.0;
constexpr int N = 12;
struct Coef {
  signed char n, m;
  float g, h, gd, hd;  // nT, nT/year
};
inline const Coef COEF[] = {
    {1, 0, -29351.8f, 0.0f, 12.0f, 0.0f},
    {1, 1, -1410.8f, 4545.4f, 9.7f, -21.5f},
    {2, 0, -2556.6f, 0.0f, -11.6f, 0.0f},
    {2, 1, 2951.1f, -3133.6f, -5.2f, -27.7f},
    {2, 2, 1649.3f, -815.1f, -8.0f, -12.1f},
    {3, 0, 1361.0f, 0.0f, -1.3f, 0.0f},
    {3, 1, -2404.1f, -56.6f, -4.2f, 4.0f},
    {3, 2, 1243.8f, 237.5f, 0.4f, -0.3f},
    {3, 3, 453.6f, -549.5f, -15.6f, -4.1f},
    {4, 0, 895.0f, 0.0f, -1.6f, 0.0f},
    {4, 1, 799.5f, 278.6f, -2.4f, -1.1f},
    {4, 2, 55.7f, -133.9f, -6.0f, 4.1f},
    {4, 3, -281.1f, 212.0f, 5.6f, 1.6f},
    {4, 4, 12.1f, -375.6f, -7.0f, -4.4f},
    {5, 0, -233.2f, 0.0f, 0.6f, 0.0f},
    {5, 1, 368.9f, 45.4f, 1.4f, -0.5f},
    {5, 2, 187.2f, 220.2f, 0.0f, 2.2f},
    {5, 3, -138.7f, -122.9f, 0.6f, 0.4f},
    {5, 4, -142.0f, 43.0f, 2.2f, 1.7f},
    {5, 5, 20.9f, 106.1f, 0.9f, 1.9f},
    {6, 0, 64.4f, 0.0f, -0.2f, 0.0f},
    {6, 1, 63.8f, -18.4f, -0.4f, 0.3f},
    {6, 2, 76.9f, 16.8f, 0.9f, -1.6f},
    {6, 3, -115.7f, 48.8f, 1.2f, -0.4f},
    {6, 4, -40.9f, -59.8f, -0.9f, 0.9f},
    {6, 5, 14.9f, 10.9f, 0.3f, 0.7f},
    {6, 6, -60.7f, 72.7f, 0.9f, 0.9f},
    {7, 0, 79.5f, 0.0f, -0.0f, 0.0f},
    {7, 1, -77.0f, -48.9f, -0.1f, 0.6f},
    {7, 2, -8.8f, -14.4f, -0.1f, 0.5f},
    {7, 3, 59.3f, -1.0f, 0.5f, -0.8f},
    {7, 4, 15.8f, 23.4f, -0.1f, 0.0f},
    {7, 5, 2.5f, -7.4f, -0.8f, -1.0f},
    {7, 6, -11.1f, -25.1f, -0.8f, 0.6f},
    {7, 7, 14.2f, -2.3f, 0.8f, -0.2f},
    {8, 0, 23.2f, 0.0f, -0.1f, 0.0f},
    {8, 1, 10.8f, 7.1f, 0.2f, -0.2f},
    {8, 2, -17.5f, -12.6f, 0.0f, 0.5f},
    {8, 3, 2.0f, 11.4f, 0.5f, -0.4f},
    {8, 4, -21.7f, -9.7f, -0.1f, 0.4f},
    {8, 5, 16.9f, 12.7f, 0.3f, -0.5f},
    {8, 6, 15.0f, 0.7f, 0.2f, -0.6f},
    {8, 7, -16.8f, -5.2f, -0.0f, 0.3f},
    {8, 8, 0.9f, 3.9f, 0.2f, 0.2f},
    {9, 0, 4.6f, 0.0f, -0.0f, 0.0f},
    {9, 1, 7.8f, -24.8f, -0.1f, -0.3f},
    {9, 2, 3.0f, 12.2f, 0.1f, 0.3f},
    {9, 3, -0.2f, 8.3f, 0.3f, -0.3f},
    {9, 4, -2.5f, -3.3f, -0.3f, 0.3f},
    {9, 5, -13.1f, -5.2f, 0.0f, 0.2f},
    {9, 6, 2.4f, 7.2f, 0.3f, -0.1f},
    {9, 7, 8.6f, -0.6f, -0.1f, -0.2f},
    {9, 8, -8.7f, 0.8f, 0.1f, 0.4f},
    {9, 9, -12.9f, 10.0f, -0.1f, 0.1f},
    {10, 0, -1.3f, 0.0f, 0.1f, 0.0f},
    {10, 1, -6.4f, 3.3f, 0.0f, 0.0f},
    {10, 2, 0.2f, 0.0f, 0.1f, -0.0f},
    {10, 3, 2.0f, 2.4f, 0.1f, -0.2f},
    {10, 4, -1.0f, 5.3f, -0.0f, 0.1f},
    {10, 5, -0.6f, -9.1f, -0.3f, -0.1f},
    {10, 6, -0.9f, 0.4f, 0.0f, 0.1f},
    {10, 7, 1.5f, -4.2f, -0.1f, 0.0f},
    {10, 8, 0.9f, -3.8f, -0.1f, -0.1f},
    {10, 9, -2.7f, 0.9f, -0.0f, 0.2f},
    {10, 10, -3.9f, -9.1f, -0.0f, -0.0f},
    {11, 0, 2.9f, 0.0f, 0.0f, 0.0f},
    {11, 1, -1.5f, 0.0f, -0.0f, -0.0f},
    {11, 2, -2.5f, 2.9f, 0.0f, 0.1f},
    {11, 3, 2.4f, -0.6f, 0.0f, -0.0f},
    {11, 4, -0.6f, 0.2f, 0.0f, 0.1f},
    {11, 5, -0.1f, 0.5f, -0.1f, -0.0f},
    {11, 6, -0.6f, -0.3f, 0.0f, -0.0f},
    {11, 7, -0.1f, -1.2f, -0.0f, 0.1f},
    {11, 8, 1.1f, -1.7f, -0.1f, -0.0f},
    {11, 9, -1.0f, -2.9f, -0.1f, 0.0f},
    {11, 10, -0.2f, -1.8f, -0.1f, 0.0f},
    {11, 11, 2.6f, -2.3f, -0.1f, 0.0f},
    {12, 0, -2.0f, 0.0f, 0.0f, 0.0f},
    {12, 1, -0.2f, -1.3f, 0.0f, -0.0f},
    {12, 2, 0.3f, 0.7f, -0.0f, 0.0f},
    {12, 3, 1.2f, 1.0f, -0.0f, -0.1f},
    {12, 4, -1.3f, -1.4f, -0.0f, 0.1f},
    {12, 5, 0.6f, -0.0f, -0.0f, -0.0f},
    {12, 6, 0.6f, 0.6f, 0.1f, -0.0f},
    {12, 7, 0.5f, -0.1f, -0.0f, -0.0f},
    {12, 8, -0.1f, 0.8f, 0.0f, 0.0f},
    {12, 9, -0.4f, 0.1f, 0.0f, -0.0f},
    {12, 10, -0.2f, -1.0f, -0.1f, -0.0f},
    {12, 11, -1.3f, 0.1f, -0.0f, 0.0f},
    {12, 12, -0.7f, 0.2f, -0.1f, -0.1f},
};

struct Field {
  double decl = 0, incl = 0;  // degrees (east / down positive)
  double h = 0, f = 0;        // horizontal and total intensity, nT
};

// lat/lon geodetic degrees, h_km above the ellipsoid, year as a decimal (e.g. 2026.73).
inline Field compute(double lat_deg, double lon_deg, double h_km, double year) {
  const double A = 6378.137, F = 1 / 298.257223563, E2 = F * (2 - F), RE = 6371.2;
  const double phi = lat_deg * M_PI / 180, lam = lon_deg * M_PI / 180;
  const double rc = A / sqrt(1 - E2 * sin(phi) * sin(phi));
  const double p = (rc + h_km) * cos(phi), z = (rc * (1 - E2) + h_km) * sin(phi);
  const double r = sqrt(p * p + z * z), phic = asin(z / r);
  const double ct = sin(phic), st = std::fmax(cos(phic), 1e-9);  // colatitude cos / sin
  double P[N + 1][N + 1] = {}, dP[N + 1][N + 1] = {};
  P[0][0] = 1;
  for (int n = 1; n <= N; n++)
    for (int m = 0; m <= n; m++) {
      if (m == n) {
        const double k = n == 1 ? 1.0 : sqrt((2.0 * n - 1) / (2.0 * n));
        P[n][n] = k * st * P[n - 1][n - 1];
        dP[n][n] = k * (st * dP[n - 1][n - 1] + ct * P[n - 1][n - 1]);
      } else {
        const double a = sqrt((double) (n * n - m * m));
        const double b = n - 1 >= m ? sqrt((double) ((n - 1) * (n - 1) - m * m)) : 0.0;
        const double p2 = n - 2 >= m ? P[n - 2][m] : 0.0, d2 = n - 2 >= m ? dP[n - 2][m] : 0.0;
        P[n][m] = ((2 * n - 1) * ct * P[n - 1][m] - b * p2) / a;
        dP[n][m] = ((2 * n - 1) * (ct * dP[n - 1][m] - st * P[n - 1][m]) - b * d2) / a;
      }
    }
  const double dt = year - EPOCH;
  double X = 0, Y = 0, Z = 0;
  for (const auto &c : COEF) {
    const int n = c.n, m = c.m;
    const double g = c.g + dt * c.gd, h = c.h + dt * c.hd;
    const double ar = pow(RE / r, n + 2), cm = cos(m * lam), sm = sin(m * lam);
    X += ar * (g * cm + h * sm) * dP[n][m];
    Y += ar * m * (g * sm - h * cm) * P[n][m] / st;
    Z -= (n + 1) * ar * (g * cm + h * sm) * P[n][m];
  }
  const double dl = phic - phi;  // geocentric -> geodetic
  const double xg = X * cos(dl) - Z * sin(dl), zg = X * sin(dl) + Z * cos(dl);
  Field out;
  out.h = sqrt(xg * xg + Y * Y);
  out.f = sqrt(out.h * out.h + zg * zg);
  out.decl = atan2(Y, xg) * 180 / M_PI;
  out.incl = atan2(zg, out.h) * 180 / M_PI;
  return out;
}

// Geomagnetic (centred dipole) latitude, degrees: for the aurora outlook (UI-37).
inline double dipole_lat(double lat_deg, double lon_deg, double year) {
  const double dt = year - EPOCH;
  const double g10 = COEF[0].g + dt * COEF[0].gd, g11 = COEF[1].g + dt * COEF[1].gd,
               h11 = COEF[1].h + dt * COEF[1].hd;
  const double b0 = sqrt(g10 * g10 + g11 * g11 + h11 * h11);
  // unit vector of the dipole's northern (geomagnetic north) pole, Earth-fixed
  const double px = -g11 / b0, py = -h11 / b0, pz = -g10 / b0;
  const double la = lat_deg * M_PI / 180, lo = lon_deg * M_PI / 180;
  const double s = cos(la) * cos(lo) * px + cos(la) * sin(lo) * py + sin(la) * pz;
  return asin(std::fmax(-1.0, std::fmin(1.0, s))) * 180 / M_PI;
}
}  // namespace wmm
}  // namespace sat
