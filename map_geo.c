#include "map_geo.h"

#include <math.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Mercator cannot represent the poles; clamp where the projection stays square. */
#define MERCATOR_LAT_LIMIT 85.05112877980659

static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

double map_geo_world_size_px(int zoom)
{
    if (zoom < 0) zoom = 0;
    if (zoom > 22) zoom = 22;
    return (double)MAP_GEO_TILE_SIZE * (double)(1u << zoom);
}

double map_geo_lon_to_world_px(double lon, int zoom)
{
    double n = map_geo_world_size_px(zoom);
    return (clampd(lon, -180.0, 180.0) + 180.0) / 360.0 * n;
}

double map_geo_lat_to_world_py(double lat, int zoom)
{
    double n = map_geo_world_size_px(zoom);
    double rad = clampd(lat, -MERCATOR_LAT_LIMIT, MERCATOR_LAT_LIMIT) * M_PI / 180.0;
    return (1.0 - log(tan(rad) + 1.0 / cos(rad)) / M_PI) / 2.0 * n;
}

double map_geo_world_px_to_lon(double px, int zoom)
{
    double n = map_geo_world_size_px(zoom);
    return px / n * 360.0 - 180.0;
}

double map_geo_world_py_to_lat(double py, int zoom)
{
    double n = map_geo_world_size_px(zoom);
    double rad = atan(sinh(M_PI * (1.0 - 2.0 * py / n)));
    return rad * 180.0 / M_PI;
}

double map_geo_meters_per_pixel(double lat, int zoom)
{
    double rad = clampd(lat, -MERCATOR_LAT_LIMIT, MERCATOR_LAT_LIMIT) * M_PI / 180.0;
    return (2.0 * M_PI * MAP_GEO_EARTH_RADIUS_M * cos(rad)) / map_geo_world_size_px(zoom);
}

double map_geo_distance_m(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = lat1 * M_PI / 180.0;
    double p2 = lat2 * M_PI / 180.0;
    double dp = (lat2 - lat1) * M_PI / 180.0;
    double dl = (lon2 - lon1) * M_PI / 180.0;

    double sdp = sin(dp * 0.5);
    double sdl = sin(dl * 0.5);
    double a = sdp * sdp + cos(p1) * cos(p2) * sdl * sdl;
    if (a > 1.0) a = 1.0;

    return 2.0 * MAP_GEO_EARTH_RADIUS_M * asin(sqrt(a));
}

double map_geo_bearing_deg(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = lat1 * M_PI / 180.0;
    double p2 = lat2 * M_PI / 180.0;
    double dl = (lon2 - lon1) * M_PI / 180.0;

    double y = sin(dl) * cos(p2);
    double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);

    return map_geo_normalize_deg(atan2(y, x) * 180.0 / M_PI);
}

double map_geo_normalize_deg(double deg)
{
    double d = fmod(deg, 360.0);
    if (d < 0.0) d += 360.0;
    return d;
}

double map_geo_angle_diff_deg(double from, double to)
{
    double d = fmod(to - from, 360.0);
    if (d > 180.0)  d -= 360.0;
    if (d <= -180.0) d += 360.0;
    return d;
}

double map_geo_point_segment_dist_m(double plat, double plon,
                                    double alat, double alon,
                                    double blat, double blon,
                                    double *out_t, double *out_lat, double *out_lon)
{
    /* Work in a local metric frame anchored at A. cos(lat) compresses longitude
     * so that both axes are in metres; over a road segment the error is
     * negligible and this keeps the projection a plain 2D dot product. */
    const double m_per_deg_lat = M_PI * MAP_GEO_EARTH_RADIUS_M / 180.0;
    double m_per_deg_lon = m_per_deg_lat * cos(alat * M_PI / 180.0);

    double abx = (blon - alon) * m_per_deg_lon;
    double aby = (blat - alat) * m_per_deg_lat;
    double apx = (plon - alon) * m_per_deg_lon;
    double apy = (plat - alat) * m_per_deg_lat;

    double len2 = abx * abx + aby * aby;
    double t = (len2 > 1e-9) ? (apx * abx + apy * aby) / len2 : 0.0;
    t = clampd(t, 0.0, 1.0);

    double clat = alat + (blat - alat) * t;
    double clon = alon + (blon - alon) * t;

    if (out_t)   *out_t = t;
    if (out_lat) *out_lat = clat;
    if (out_lon) *out_lon = clon;

    double dx = apx - abx * t;
    double dy = apy - aby * t;
    return sqrt(dx * dx + dy * dy);
}

void map_geo_format_distance(double meters, bool imperial, char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) return;
    if (meters < 0.0) meters = 0.0;

    if (imperial) {
        double feet = meters * 3.280839895;
        if (feet < 1000.0) {
            int rounded = (int)(feet / 10.0 + 0.5) * 10;
            if (rounded < 10 && feet >= 1.0) rounded = 10;
            snprintf(buf, buf_size, "%d ft", rounded);
        } else {
            snprintf(buf, buf_size, "%.1f mi", meters / 1609.344);
        }
        return;
    }

    if (meters < 1000.0) {
        int rounded = (int)(meters / 10.0 + 0.5) * 10;
        if (rounded < 10 && meters >= 1.0) rounded = 10;
        snprintf(buf, buf_size, "%d m", rounded);
    } else {
        snprintf(buf, buf_size, "%.1f km", meters / 1000.0);
    }
}

void map_geo_format_duration(uint32_t seconds, char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) return;

    if (seconds < 60) {
        snprintf(buf, buf_size, "%u s", (unsigned)seconds);
    } else if (seconds < 3600) {
        snprintf(buf, buf_size, "%u min", (unsigned)((seconds + 30) / 60));
    } else {
        unsigned total_min = (unsigned)((seconds + 30) / 60);
        snprintf(buf, buf_size, "%u h %02u min", total_min / 60, total_min % 60);
    }
}
