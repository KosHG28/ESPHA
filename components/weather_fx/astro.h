#pragma once

// Праздники и небесные события для часов: номера праздников (общие для
// погодного фона, гостей и packages/holidays.yaml) и простая астрономия —
// православная Пасха, солнцестояния и равноденствия, полнолуние,
// суперлуние и лунное затмение. Формулы — сокращённые ряды Ж. Меёса
// («Астрономические алгоритмы»): Луна — до ~0,1°, Солнце — до ~0,01°.
// Этого хватает, чтобы время полнолуния и затмения сходилось с точностью до
// десятка минут.

#include <cmath>
#include <cstdint>

namespace esphome {
namespace weather_fx {

/// Праздник или событие дня. Номер — он же пункт выбора «Показать
/// праздник» в Home Assistant, поэтому новые — только в конец
enum Holiday : int {
  HOL_NONE = 0,
  HOL_NEW_YEAR,     ///< 31 декабря и 1 января: отсчёт, салют, шапка Деда Мороза
  HOL_CHRISTMAS,    ///< 7 января: Вифлеемская звезда
  HOL_FEB23,        ///< 23 февраля: кот в пилотке, красный салют
  HOL_MAR8,         ///< 8 Марта: падают тюльпаны и мимоза
  HOL_MASLENITSA,   ///< Масленичная неделя: солнце-блин, кот ест блины
  HOL_APRIL1,       ///< 1 апреля: розыгрыши
  HOL_COSMOS,       ///< 12 апреля: ракета, кот в шлеме
  HOL_EASTER,       ///< Пасха: крашеные яйца, кулич
  HOL_CAT_DAY,      ///< 1 марта и 8 августа: кот в короне, в гости только коты
  HOL_VICTORY,      ///< 9 Мая: салют в 22:00
  HOL_CHILDREN,     ///< 1 июня: воздушные шарики
  HOL_RUSSIA,       ///< 12 июня: бело-сине-красный салют
  HOL_KUPALA,       ///< 7 июля: светлячки
  HOL_METEORS,      ///< ночи Персеид и Геминид: частые метеоры
  HOL_SEPT1,        ///< 1 сентября: кот с ранцем, кленовые листья
  HOL_PROGRAMMER,   ///< 256-й день года: время в двоичном виде, «матрица»
  HOL_HALLOWEEN,    ///< 31 октября: летучие мыши, оранжевые цифры
  HOL_FRIDAY13,     ///< пятница, 13-е: чёрный кот
  HOL_FULL_MOON,    ///< полнолуние: луна крупнее
  HOL_SUPERMOON,    ///< суперлуние: луна ещё крупнее
  HOL_ECLIPSE,      ///< лунное затмение: луна краснеет
  HOL_SOLSTICE,     ///< солнцестояние
  HOL_EQUINOX,      ///< равноденствие
  HOL_BIRTHDAY,     ///< день рождения (из «Дней рождения»)
  HOL_COUNT
};

static const double ASTRO_PI = 3.14159265358979323846;

inline double deg_sin(double d) { return sin(d * (ASTRO_PI / 180.0)); }
inline double deg_cos(double d) { return cos(d * (ASTRO_PI / 180.0)); }

inline double wrap360(double d) {
  d = fmod(d, 360.0);
  return d < 0 ? d + 360.0 : d;
}

/// Юлианская дата по секундам Unix
inline double julian_day(uint32_t utc) { return utc / 86400.0 + 2440587.5; }

/// Дней от 1970-01-01 до даты григорианского календаря
inline int days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - era * 400;
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

/// Православная Пасха (по юлианской пасхалии), дата по григорианскому
/// календарю. Верно для 1900–2099 годов
inline void orthodox_easter(int year, int &month, int &day) {
  const int a = year % 4, b = year % 7, c = year % 19;
  const int d = (19 * c + 15) % 30;
  const int e = (2 * a + 4 * b - d + 34) % 7;
  const int m = (d + e + 114) / 31, dd = (d + e + 114) % 31 + 1;
  // Юлианская дата + 13 дней
  int n = days_from_civil(year, m, dd) + 13;
  // Обратно в год-месяц-день
  n += 719468;
  const int era = (n >= 0 ? n : n - 146096) / 146097;
  const int doe = n - era * 146097;
  const int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int mp = (5 * doy + 2) / 153;
  day = doy - (153 * mp + 2) / 5 + 1;
  month = mp < 10 ? mp + 3 : mp - 9;
}

/// Долгота Солнца, градусы
inline double sun_longitude(double jd) {
  const double T = (jd - 2451545.0) / 36525.0;
  const double L0 = 280.46646 + 36000.76983 * T + 0.0003032 * T * T;
  const double M = 357.52911 + 35999.05029 * T - 0.0001537 * T * T;
  const double C = (1.914602 - 0.004817 * T - 0.000014 * T * T) * deg_sin(M) +
                   (0.019993 - 0.000101 * T) * deg_sin(2 * M) + 0.000289 * deg_sin(3 * M);
  return wrap360(L0 + C);
}

/// Луна: долгота и широта (градусы), расстояние (км)
inline void moon_position(double jd, double &lon, double &lat, double &dist) {
  const double T = (jd - 2451545.0) / 36525.0;
  const double Lp = 218.3164477 + 481267.88123421 * T;
  const double D = 297.8501921 + 445267.1114034 * T;
  const double M = 357.5291092 + 35999.0502909 * T;
  const double Mp = 134.9633964 + 477198.8675055 * T;
  const double F = 93.2720950 + 483202.0175233 * T;
  const double A1 = 119.75 + 131.849 * T, A2 = 53.09 + 479264.290 * T, A3 = 313.45 + 481266.484 * T;
  const double E = 1.0 - 0.002516 * T - 0.0000074 * T * T;
  // Главные члены рядов: коэффициенты D, M, M', F и амплитуда
  struct Term {
    int8_t d, m, mp, f;
    int32_t l, r;
  };
  static const Term LR[] = {
      {0, 0, 1, 0, 6288774, -20905355}, {2, 0, -1, 0, 1274027, -3699111}, {2, 0, 0, 0, 658314, -2955968},
      {0, 0, 2, 0, 213618, -569925},    {0, 1, 0, 0, -185116, 48888},     {0, 0, 0, 2, -114332, -3149},
      {2, 0, -2, 0, 58793, 246158},     {2, -1, -1, 0, 57066, -152138},   {2, 0, 1, 0, 53322, -170733},
      {2, -1, 0, 0, 45758, -204586},    {0, 1, -1, 0, -40923, -129620},   {1, 0, 0, 0, -34720, 108743},
      {0, 1, 1, 0, -30383, 104755},     {2, 0, 0, -2, 15327, 10321},      {0, 0, 1, 2, -12528, 0},
      {0, 0, 1, -2, 10980, 79661},      {4, 0, -1, 0, 10675, -34782},     {0, 0, 3, 0, 10034, -23210},
      {4, 0, -2, 0, 8548, -21636},      {2, 1, -1, 0, -7888, 24208},      {2, 1, 0, 0, -6766, 30824},
      {1, 0, -1, 0, -5163, -8379},      {1, 1, 0, 0, 4987, -16675},       {2, -1, 1, 0, 4036, -12831},
      {2, 0, 2, 0, 3994, -10445},       {4, 0, 0, 0, 3861, -11650},       {2, 0, -3, 0, 3665, 14403},
      {0, 1, -2, 0, -2689, -7003},      {2, 0, -1, 2, -2602, 0},          {2, -1, -2, 0, 2390, 10056},
      {1, 0, 1, 0, -2348, 6322},        {2, -2, 0, 0, 2236, -9884},
  };
  static const Term B[] = {
      {0, 0, 0, 1, 5128122, 0}, {0, 0, 1, 1, 280602, 0},  {0, 0, 1, -1, 277693, 0}, {2, 0, 0, -1, 173237, 0},
      {2, 0, -1, 1, 55413, 0},  {2, 0, -1, -1, 46271, 0}, {2, 0, 0, 1, 32573, 0},   {0, 0, 2, 1, 17198, 0},
      {2, 0, 1, -1, 9266, 0},   {0, 0, 2, -1, 8822, 0},   {2, -1, 0, -1, 8216, 0},  {2, 0, -2, -1, 4324, 0},
      {2, 0, 1, 1, 4200, 0},    {2, 1, 0, -1, -3359, 0},  {2, -1, -1, 1, 2463, 0},  {2, -1, 0, 1, 2211, 0},
  };
  double sl = 0, sr = 0, sb = 0;
  for (const Term &t : LR) {
    const double arg = t.d * D + t.m * M + t.mp * Mp + t.f * F;
    const double e = t.m == 0 ? 1.0 : (t.m == 1 || t.m == -1 ? E : E * E);
    sl += t.l * e * deg_sin(arg);
    sr += t.r * e * deg_cos(arg);
  }
  for (const Term &t : B) {
    const double arg = t.d * D + t.m * M + t.mp * Mp + t.f * F;
    const double e = t.m == 0 ? 1.0 : (t.m == 1 || t.m == -1 ? E : E * E);
    sb += t.l * e * deg_sin(arg);
  }
  sl += 3958 * deg_sin(A1) + 1962 * deg_sin(Lp - F) + 318 * deg_sin(A2);
  sb += -2235 * deg_sin(Lp) + 382 * deg_sin(A3) + 175 * deg_sin(A1 - F) + 175 * deg_sin(A1 + F) +
        127 * deg_sin(Lp - Mp) - 115 * deg_sin(Lp + Mp);
  lon = wrap360(Lp + sl / 1e6);
  lat = sb / 1e6;
  dist = 385000.56 + sr / 1000.0;
}

/// Что сейчас с Луной: HOL_FULL_MOON — полнолуние (±12 ч), HOL_SUPERMOON —
/// полнолуние, когда Луна ближе 362 тыс. км, HOL_ECLIPSE — Луна в земной
/// тени. red — насколько Луна в тени: 0 — чуть задела, 1 — целиком
inline int moon_event(uint32_t utc, float *red = nullptr) {
  if (red)
    *red = 0;
  if (utc < 1600000000u)
    return HOL_NONE;
  const double jd = julian_day(utc);
  double lon, lat, dist;
  moon_position(jd, lon, lat, dist);
  double el = wrap360(lon - sun_longitude(jd)) - 180.0;  // 0 — точно против Солнца
  if (std::fabs(el) > 6.1)
    return HOL_NONE;
  // Расстояние центра Луны от центра земной тени. Тень ~0,72°, Луна ~0,26°
  const double sep = sqrt(el * el * deg_cos(lat) * deg_cos(lat) + lat * lat);
  if (sep < 0.98) {
    if (red) {
      const double k = (0.98 - sep) / 0.52;
      *red = (float) (k > 1.0 ? 1.0 : k);
    }
    return HOL_ECLIPSE;
  }
  return dist < 362000.0 ? HOL_SUPERMOON : HOL_FULL_MOON;
}

/// Солнцестояние или равноденствие между моментами t0 и t1 (секунды Unix):
/// 0 — нет, 1 — весеннее равноденствие, 2 — летнее солнцестояние,
/// 3 — осеннее равноденствие, 4 — зимнее солнцестояние
inline int season_event(uint32_t t0, uint32_t t1) {
  const int q0 = (int) (sun_longitude(julian_day(t0)) / 90.0);
  const int q1 = (int) (sun_longitude(julian_day(t1)) / 90.0);
  if (q0 == q1)
    return 0;
  return q1 + 1;  // долгота 0° — весна, 90° — лето, 180° — осень, 270° — зима
}

}  // namespace weather_fx
}  // namespace esphome
