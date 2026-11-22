#include "route.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/memory.h"
#include "core_sdk/net.h"
#include "json.h"

#define ROUTE_USER_AGENT "BruceOS-Maps/0.1"
#define ROUTE_TIMEOUT_MS 20000
#define ROUTE_MAX_BYTES (384u * 1024u)
/* Segments behind the last match still searched, for GPS jitter. */
#define ROUTE_TRACK_BACK 5

static const char *const s_profiles[ROUTE_PROFILE_COUNT] = {"car", "bike", "foot"};

/* Fixed five decimals, without relying on %f support. */
static void route__format_degrees(char *out, size_t capacity, double value) {
    long scaled = (long)floor(fabs(value) * 100000.0 + 0.5);
    snprintf(out, capacity, "%s%ld.%05ld", value < 0 ? "-" : "", scaled / 100000, scaled % 100000);
}

bool route__build_url(const char *base, route_profile_t profile, double from_lat, double from_lon, double to_lat,
                      double to_lon, char *out, size_t capacity) {
    if (profile >= ROUTE_PROFILE_COUNT) return false;
    char degrees[4][16];
    route__format_degrees(degrees[0], sizeof(degrees[0]), from_lon);
    route__format_degrees(degrees[1], sizeof(degrees[1]), from_lat);
    route__format_degrees(degrees[2], sizeof(degrees[2]), to_lon);
    route__format_degrees(degrees[3], sizeof(degrees[3]), to_lat);
    int len = snprintf(out, capacity, "%s/routed-%s/route/v1/driving/%s,%s;%s,%s?overview=false&steps=true",
                       base, s_profiles[profile], degrees[0], degrees[1], degrees[2], degrees[3]);
    return len > 0 && (size_t)len < capacity;
}

/* ---- Parsing --------------------------------------------------------------- */

typedef struct {
    route_t *route;
    size_t point_capacity;
    size_t step_capacity;
    bool no_memory;
    bool no_route;
} route_builder_t;

static bool route__add_point(route_builder_t *builder, geo_point_t point) {
    route_t *route = builder->route;
    if (route->point_count == builder->point_capacity) {
        size_t capacity = builder->point_capacity ? builder->point_capacity * 2 : 256;
        geo_point_t *points = memory__realloc(route->points, capacity * sizeof(*points));
        if (points == NULL) {
            builder->no_memory = true;
            return false;
        }
        route->points = points;
        builder->point_capacity = capacity;
    }
    route->points[route->point_count++] = point;
    return true;
}

static route_step_t *route__add_step(route_builder_t *builder) {
    route_t *route = builder->route;
    if (route->step_count == builder->step_capacity) {
        size_t capacity = builder->step_capacity ? builder->step_capacity * 2 : 32;
        route_step_t *steps = memory__realloc(route->steps, capacity * sizeof(*steps));
        if (steps == NULL) {
            builder->no_memory = true;
            return NULL;
        }
        route->steps = steps;
        builder->step_capacity = capacity;
    }
    route_step_t *step = &route->steps[route->step_count++];
    memset(step, 0, sizeof(*step));
    return step;
}

/* Google's encoded polyline, five decimals, straight from the JSON string
 * (whose only possible escape is a doubled backslash). A step starts where the
 * previous one ended, so that shared point is kept once. Returns the index of
 * the step's first point. */
static bool route__decode_geometry(json_t *json, route_builder_t *builder, uint32_t *out_first) {
    route_t *route = builder->route;
    if (!json__eat(json, '"')) return false;
    int32_t value[2] = {0, 0};
    int which = 0, shift = 0;
    uint32_t result = 0;
    size_t first = route->point_count;
    bool any = false;
    while (json->p < json->end) {
        char c = *json->p++;
        if (c == '"') {
            *out_first = (uint32_t)(any ? first : (route->point_count ? route->point_count - 1 : 0));
            return which == 0 && shift == 0;
        }
        if (c == '\\') {
            if (json->p >= json->end) return false;
            c = *json->p++;
        }
        int bits = c - 63;
        if (bits < 0 || bits > 63 || shift > 30) return false;
        result |= (uint32_t)(bits & 0x1F) << shift;
        shift += 5;
        if (bits >= 0x20) continue;
        value[which] += (result & 1) ? ~(int32_t)(result >> 1) : (int32_t)(result >> 1);
        result = 0;
        shift = 0;
        which ^= 1;
        if (which != 0) continue;
        geo_point_t point = geo__from_lat_lon(value[0] / 1e5, value[1] / 1e5, ROUTE_ZOOM);
        bool repeat = route->point_count > 0 && route->points[route->point_count - 1].x == point.x &&
                      route->points[route->point_count - 1].y == point.y;
        if (repeat) {
            if (!any) first = route->point_count - 1;
        } else if (!route__add_point(builder, point)) {
            return false;
        }
        any = true;
    }
    return false;
}

static route_turn_t route__turn_from(const char *type) {
    static const struct {
        const char *name;
        route_turn_t turn;
    } types[] = {
        {"depart", ROUTE_DEPART},         {"arrive", ROUTE_ARRIVE},
        {"turn", ROUTE_TURN},             {"roundabout turn", ROUTE_TURN},
        {"new name", ROUTE_CONTINUE},     {"continue", ROUTE_CONTINUE},
        {"notification", ROUTE_CONTINUE}, {"exit roundabout", ROUTE_CONTINUE},
        {"exit rotary", ROUTE_CONTINUE},  {"use lane", ROUTE_CONTINUE},
        {"merge", ROUTE_MERGE},           {"on ramp", ROUTE_ON_RAMP},
        {"off ramp", ROUTE_OFF_RAMP},     {"fork", ROUTE_FORK},
        {"end of road", ROUTE_END_OF_ROAD}, {"roundabout", ROUTE_ROUNDABOUT},
        {"rotary", ROUTE_ROUNDABOUT},
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        if (strcmp(type, types[i].name) == 0) return types[i].turn;
    }
    return ROUTE_TURN; /* What OSRM asks clients to do with types they do not know. */
}

static int16_t route__angle_from(const char *modifier) {
    static const struct {
        const char *name;
        int16_t angle;
    } modifiers[] = {
        {"uturn", 180},       {"sharp right", 135}, {"right", 90},  {"slight right", 45},
        {"straight", 0},      {"slight left", -45}, {"left", -90},  {"sharp left", -135},
    };
    for (size_t i = 0; i < sizeof(modifiers) / sizeof(modifiers[0]); ++i) {
        if (strcmp(modifier, modifiers[i].name) == 0) return modifiers[i].angle;
    }
    return 0;
}

static bool route__parse_maneuver(json_t *json, route_step_t *step) {
    if (!json__eat(json, '{')) return false;
    if (json__eat(json, '}')) return true;
    do {
        char key[16], text[24];
        double number = 0;
        if (!json__string(json, key, sizeof(key)) || !json__eat(json, ':')) return false;
        bool ok = true;
        if (strcmp(key, "type") == 0) {
            ok = json__string(json, text, sizeof(text));
            step->turn = route__turn_from(text);
        } else if (strcmp(key, "modifier") == 0) {
            ok = json__string(json, text, sizeof(text));
            step->angle = route__angle_from(text);
        } else if (strcmp(key, "exit") == 0) {
            ok = json__number(json, &number);
            step->exit = number > 0 && number < 256 ? (uint8_t)number : 0;
        } else if (strcmp(key, "bearing_after") == 0) {
            ok = json__number(json, &number);
            step->bearing = number >= 0 && number < 360 ? (uint16_t)number : 0;
        } else {
            ok = json__skip_value(json);
        }
        if (!ok) return false;
    } while (json__eat(json, ','));
    return json__eat(json, '}');
}

static bool route__parse_step(json_t *json, route_builder_t *builder) {
    route_step_t *step = route__add_step(builder);
    if (step == NULL || !json__eat(json, '{')) return false;
    char ref[ROUTE_NAME_MAX] = "";
    bool has_geometry = false;
    if (!json__eat(json, '}')) {
        do {
            char key[16];
            double number = 0;
            if (!json__string(json, key, sizeof(key)) || !json__eat(json, ':')) return false;
            bool ok = true;
            if (strcmp(key, "geometry") == 0) {
                ok = has_geometry = route__decode_geometry(json, builder, &step->point);
            } else if (strcmp(key, "maneuver") == 0) {
                ok = route__parse_maneuver(json, step);
            } else if (strcmp(key, "name") == 0) {
                ok = json__string(json, step->name, sizeof(step->name));
            } else if (strcmp(key, "ref") == 0) {
                ok = json__string(json, ref, sizeof(ref));
            } else if (strcmp(key, "duration") == 0) {
                ok = json__number(json, &number);
                step->duration_s = (float)number;
            } else {
                ok = json__skip_value(json);
            }
            if (!ok) return false;
        } while (json__eat(json, ','));
        if (!json__eat(json, '}')) return false;
    }
    if (!has_geometry) step->point = builder->route->point_count ? (uint32_t)builder->route->point_count - 1 : 0;
    if (step->name[0] == '\0') snprintf(step->name, sizeof(step->name), "%s", ref);
    return true;
}

/* Each element of an array, through `parse`. */
static bool route__parse_array(json_t *json, route_builder_t *builder,
                               bool (*parse)(json_t *json, route_builder_t *builder)) {
    if (!json__eat(json, '[')) return false;
    if (json__eat(json, ']')) return true;
    do {
        if (!parse(json, builder)) return false;
    } while (json__eat(json, ','));
    return json__eat(json, ']');
}

/* Calls `field` for each key of an object; it parses or skips the value. */
static bool route__parse_object(json_t *json, route_builder_t *builder,
                                bool (*field)(json_t *json, route_builder_t *builder, const char *key)) {
    if (!json__eat(json, '{')) return false;
    if (json__eat(json, '}')) return true;
    do {
        char key[16];
        if (!json__string(json, key, sizeof(key)) || !json__eat(json, ':')) return false;
        if (!field(json, builder, key)) return false;
    } while (json__eat(json, ','));
    return json__eat(json, '}');
}

static bool route__leg_field(json_t *json, route_builder_t *builder, const char *key) {
    if (strcmp(key, "steps") == 0) return route__parse_array(json, builder, route__parse_step);
    return json__skip_value(json);
}

static bool route__parse_leg(json_t *json, route_builder_t *builder) {
    return route__parse_object(json, builder, route__leg_field);
}

static bool route__route_field(json_t *json, route_builder_t *builder, const char *key) {
    if (strcmp(key, "legs") == 0) return route__parse_array(json, builder, route__parse_leg);
    return json__skip_value(json);
}

/* The first route only; alternatives are skipped. */
static bool route__parse_routes(json_t *json, route_builder_t *builder) {
    if (!json__eat(json, '[')) return false;
    if (json__eat(json, ']')) return true;
    if (!route__parse_object(json, builder, route__route_field)) return false;
    while (json__eat(json, ',')) {
        if (!json__skip_value(json)) return false;
    }
    return json__eat(json, ']');
}

static bool route__top_field(json_t *json, route_builder_t *builder, const char *key) {
    if (strcmp(key, "routes") == 0) return route__parse_routes(json, builder);
    if (strcmp(key, "code") == 0) {
        char code[16];
        if (!json__string(json, code, sizeof(code))) return false;
        builder->no_route = strcmp(code, "Ok") != 0;
        return !builder->no_route;
    }
    return json__skip_value(json);
}

static void route__measure(route_t *route) {
    if (route->point_count == 0) return;
    route->along[0] = 0;
    for (size_t i = 1; i < route->point_count; ++i) {
        geo_point_t a = route->points[i - 1], b = route->points[i];
        double lat = 0, lon = 0;
        geo__to_lat_lon((geo_point_t){(a.x + b.x) / 2, (a.y + b.y) / 2}, ROUTE_ZOOM, &lat, &lon);
        double pixels = hypot((double)b.x - a.x, (double)b.y - a.y);
        route->along[i] = route->along[i - 1] + (float)(pixels * geo__meters_per_pixel(lat, ROUTE_ZOOM));
    }
}

bruce_result_t route__parse(const char *text, size_t len, route_t *route) {
    memset(route, 0, sizeof(*route));
    route_builder_t builder = {.route = route};
    json_t json = {.p = text, .end = text + len};
    bool ok = route__parse_object(&json, &builder, route__top_field);
    if (ok && (route->point_count < 2 || route->step_count < 2)) ok = false;
    if (ok) {
        route->along = memory__malloc(route->point_count * sizeof(*route->along));
        if (route->along == NULL) builder.no_memory = true;
        else route__measure(route);
    }
    if (!ok || builder.no_memory) {
        route__free(route);
        return builder.no_memory  ? BRUCE_ERR_NO_MEMORY
               : builder.no_route ? BRUCE_ERR_NOT_FOUND
                                  : BRUCE_ERR_INVALID_ARGUMENT;
    }
    return BRUCE_OK;
}

void route__free(route_t *route) {
    memory__free(route->points);
    memory__free(route->along);
    memory__free(route->steps);
    memset(route, 0, sizeof(*route));
}

float route__length_m(const route_t *route) {
    return route->point_count > 0 ? route->along[route->point_count - 1] : 0;
}

float route__duration_s(const route_t *route) {
    float total = 0;
    for (size_t i = 0; i < route->step_count; ++i) total += route->steps[i].duration_s;
    return total;
}

/* ---- Text ------------------------------------------------------------------ */

static const char *route__side(int angle) {
    switch (angle) {
        case -135: return "sharp left";
        case -90: return "left";
        case -45: return "slightly left";
        case 45: return "slightly right";
        case 90: return "right";
        case 135: return "sharp right";
        default: return "straight";
    }
}

static const char *route__compass(int bearing) {
    static const char *const names[] = {"north", "northeast", "east", "southeast",
                                        "south", "southwest", "west", "northwest"};
    return names[((bearing + 22) % 360) / 45];
}

void route__instruction(const route_step_t *step, char *out, size_t capacity) {
    const char *side = route__side(step->angle);
    const char *left_right = step->angle < 0 ? " on the left" : step->angle > 0 ? " on the right" : "";
    bool uturn = step->angle == 180;
    char head[48];
    switch (step->turn) {
        case ROUTE_DEPART: snprintf(head, sizeof(head), "Head %s", route__compass(step->bearing)); break;
        case ROUTE_ARRIVE: snprintf(out, capacity, "Arrive at the destination"); return;
        case ROUTE_CONTINUE:
            if (uturn) snprintf(head, sizeof(head), "Make a U-turn");
            else if (step->angle == 0) snprintf(head, sizeof(head), "Continue");
            else snprintf(head, sizeof(head), "Continue %s", side);
            break;
        case ROUTE_MERGE:
            snprintf(head, sizeof(head), step->angle == 0 ? "Merge" : "Merge %s", side);
            break;
        case ROUTE_ON_RAMP: snprintf(head, sizeof(head), "Take the ramp%s", left_right); break;
        case ROUTE_OFF_RAMP: snprintf(head, sizeof(head), "Take the exit%s", left_right); break;
        case ROUTE_FORK: snprintf(head, sizeof(head), "Keep %s", step->angle < 0 ? "left" : step->angle > 0 ? "right" : "straight"); break;
        case ROUTE_END_OF_ROAD: snprintf(head, sizeof(head), "At the end of the road turn %s", side); break;
        case ROUTE_ROUNDABOUT:
            if (step->exit > 0) snprintf(head, sizeof(head), "At the roundabout take exit %u", step->exit);
            else snprintf(head, sizeof(head), "Enter the roundabout");
            break;
        case ROUTE_TURN:
        default:
            if (uturn) snprintf(head, sizeof(head), "Make a U-turn");
            else if (step->angle == 0) snprintf(head, sizeof(head), "Go straight");
            else snprintf(head, sizeof(head), "Turn %s", side);
            break;
    }
    if (step->name[0] == '\0') snprintf(out, capacity, "%s", head);
    else snprintf(out, capacity, "%s %s %s", head, step->turn == ROUTE_DEPART ? "on" : "onto", step->name);
}

void route__format_distance(char *out, size_t capacity, double meters) {
    long m = (long)(meters + 0.5);
    if (m < 1000) snprintf(out, capacity, "%ld m", m);
    else if (m < 10000) snprintf(out, capacity, "%ld.%ld km", m / 1000, m % 1000 / 100);
    else snprintf(out, capacity, "%ld km", m / 1000);
}

void route__format_countdown(char *out, size_t capacity, double meters) {
    long step = meters < 100 ? 10 : meters < 1000 ? 50 : 100;
    long rounded = (long)(meters / step + 0.5) * step;
    route__format_distance(out, capacity, (double)rounded);
}

void route__format_duration(char *out, size_t capacity, double seconds) {
    long minutes = (long)(seconds / 60 + 0.5);
    if (minutes < 1) minutes = 1;
    if (minutes < 60) snprintf(out, capacity, "%ld min", minutes);
    else snprintf(out, capacity, "%ld h %02ld min", minutes / 60, minutes % 60);
}

/* ---- Following along ------------------------------------------------------- */

void route__track(const route_t *route, double lat, double lon, size_t from_segment, route_progress_t *out) {
    memset(out, 0, sizeof(*out));
    if (route->point_count < 2) return;
    geo_point_t me = geo__from_lat_lon(lat, lon, ROUTE_ZOOM);
    size_t start = from_segment > ROUTE_TRACK_BACK ? from_segment - ROUTE_TRACK_BACK : 0;
    double best = INFINITY, best_t = 0;
    size_t best_segment = start;
    for (size_t i = start; i + 1 < route->point_count; ++i) {
        double ax = route->points[i].x, ay = route->points[i].y;
        double dx = route->points[i + 1].x - ax, dy = route->points[i + 1].y - ay;
        double length2 = dx * dx + dy * dy;
        double t = length2 > 0 ? ((me.x - ax) * dx + (me.y - ay) * dy) / length2 : 0;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        double ex = ax + t * dx - me.x, ey = ay + t * dy - me.y;
        double d2 = ex * ex + ey * ey;
        if (d2 < best) {
            best = d2;
            best_t = t;
            best_segment = i;
        }
    }
    float along = route->along[best_segment] +
                  (float)best_t * (route->along[best_segment + 1] - route->along[best_segment]);
    out->segment = best_segment;
    out->off_m = (float)(sqrt(best) * geo__meters_per_pixel(lat, ROUTE_ZOOM));
    out->left_m = route__length_m(route) - along;
    size_t next = route->step_count - 1;
    for (size_t i = 1; i < route->step_count; ++i) {
        if (route->along[route->steps[i].point] > along + 1) {
            next = i;
            break;
        }
    }
    out->next = next;
    out->to_next_m = route->along[route->steps[next].point] - along;
    if (out->to_next_m < 0) out->to_next_m = 0;
    /* The current step's time in proportion to what is left of it. */
    const route_step_t *current = &route->steps[next - 1];
    float step_m = route->along[route->steps[next].point] - route->along[current->point];
    out->left_s = step_m > 0 ? current->duration_s * out->to_next_m / step_m : 0;
    for (size_t i = next; i < route->step_count; ++i) out->left_s += route->steps[i].duration_s;
}

/* ---- Request --------------------------------------------------------------- */

static bruce_result_t route__request(route_request_t *request) {
    static const char *const headers[] = {"User-Agent", ROUTE_USER_AGENT};
    bruce_http_request_t http = {
        .url = request->url,
        .headers = headers,
        .header_count = 1,
        .timeout_ms = ROUTE_TIMEOUT_MS,
        .max_response_bytes = ROUTE_MAX_BYTES,
    };
    bruce_result_t result = http__request_start(&http, &request->job);
    request->state = result == BRUCE_OK ? ROUTE_RUNNING : ROUTE_IDLE;
    return result;
}

bruce_result_t route__start(route_request_t *request, const char *base, route_profile_t profile, double from_lat,
                            double from_lon, double to_lat, double to_lon) {
    route__cancel(request);
    if (!route__build_url(base, profile, from_lat, from_lon, to_lat, to_lon, request->url, sizeof(request->url))) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (net__is_online()) return route__request(request);
    bruce_result_t result = net__connect_start();
    if (result == BRUCE_OK) request->state = ROUTE_CONNECTING;
    return result;
}

static bruce_result_t route__collect(route_request_t *request, route_t *out) {
    bruce_http_response_t response;
    bruce_result_t result = http__request_poll(request->job, &response);
    if (result == BRUCE_ERR_BUSY) return result;
    request->state = ROUTE_IDLE;
    request->job = BRUCE_HTTP_JOB_ID_INVALID;
    if (result != BRUCE_OK) return result;
    /* OSRM answers "no route" with 400 and a code in the body. */
    if (response.status_code == 200 || response.status_code == 400) {
        result = route__parse(response.body, response.body_len, out);
    } else {
        result = BRUCE_ERR_IO;
    }
    http__response_free(&response);
    return result;
}

bruce_result_t route__poll(route_request_t *request, route_t *out) {
    if (request->state == ROUTE_CONNECTING) {
        bruce_result_t result = net__connect_poll();
        if (result == BRUCE_ERR_BUSY) return result;
        if (result != BRUCE_OK) {
            request->state = ROUTE_IDLE;
            return result;
        }
        result = route__request(request);
        return result == BRUCE_OK ? BRUCE_ERR_BUSY : result;
    }
    if (request->state == ROUTE_RUNNING) return route__collect(request, out);
    return BRUCE_ERR_INVALID_STATE;
}

void route__cancel(route_request_t *request) {
    if (request->state == ROUTE_RUNNING) http__request_cancel(request->job);
    request->state = ROUTE_IDLE;
    request->job = BRUCE_HTTP_JOB_ID_INVALID;
}
