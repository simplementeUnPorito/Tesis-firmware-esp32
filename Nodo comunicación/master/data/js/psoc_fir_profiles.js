// Perfiles reproducibles para el bloque Filter del PSoC (128 palabras Q1.23).
// El perfil 2000 Hz fue generado por lab/antialias/generate_coefficients.py:
// 127 taps Kaiser simétricos + un cero de relleno para conservar delay=63.

export const FACTORY_FIR_2000_Q23 = Object.freeze([
  822, 1059, 821, 0, -1221, -2359, -2786, -2014, 0, 2694, 4991, 5682,
  3979, 0, -5039, -9122, -10171, -6985, 0, 8552, 15251, 16769, 11370, 0,
  -13603, -24008, -26145, -17570, 0, 20690, 36261, 39236, 26214, 0,
  -30562, -53344, -57519, -38318, 0, 44506, 77620, 83692, 55800, 0,
  -65105, -113995, -123560, -82941, 0, 98635, 174958, 192678, 131880, 0,
  -165454, -304525, -351437, -255546, 0, 388316, 841545, 1266403,
  1568396, 1677626, 1568396, 1266403, 841545, 388316, 0, -255546,
  -351437, -304525, -165454, 0, 131880, 192678, 174958, 98635, 0, -82941,
  -123560, -113995, -65105, 0, 55800, 83692, 77620, 44506, 0, -38318,
  -57519, -53344, -30562, 0, 26214, 39236, 36261, 20690, 0, -17570,
  -26145, -24008, -13603, 0, 11370, 16769, 15251, 8552, 0, -6985,
  -10171, -9122, -5039, 0, 3979, 5682, 4991, 2694, 0, -2014, -2786,
  -2359, -1221, 0, 821, 1059, 822, 0,
]);

export function factoryFirProfile(fsHz) {
  if (Number.isFinite(fsHz) && Math.abs(fsHz - 2000) <= 1) {
    return {
      id: 'kaiser_lp200_fs2000_v1',
      source: 'web_factory',
      fs_hz: 2000,
      cutoff_hz: 200,
      passband_hz: 150,
      stopband_hz: 250,
      stopband_attenuation_db: 60,
      taps: 127,
      storage_taps: 128,
      group_delay_samples: 63,
      coefficients_q23: FACTORY_FIR_2000_Q23,
    };
  }
  return {
    id: 'psoc_factory_2604_legacy',
    source: 'psoc_factory',
    fs_hz: 2604,
    taps: 128,
    storage_taps: 128,
    group_delay_samples: 63.5,
    coefficients_q23: null,
  };
}
