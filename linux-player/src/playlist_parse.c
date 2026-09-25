#include "playlist_parse.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *jstr(cJSON *o, const char *key, const char *fallback) {
    cJSON *j = cJSON_GetObjectItem(o, key);
    return (j && cJSON_IsString(j)) ? j->valuestring : fallback;
}
static double jnum(cJSON *o, const char *key, double fallback) {
    cJSON *j = cJSON_GetObjectItem(o, key);
    return (j && cJSON_IsNumber(j)) ? j->valuedouble : fallback;
}
static bool jbool(cJSON *o, const char *key, bool fallback) {
    cJSON *j = cJSON_GetObjectItem(o, key);
    if (!j) return fallback;
    /* SQLite-backed playlist flags (including `muted`) are sent as 0/1,
     * while some clients send JSON booleans.  Accept both wire forms. */
    if (cJSON_IsNumber(j)) return j->valuedouble != 0;
    return cJSON_IsTrue(j);
}

static void parse_zone(cJSON *z, stp_zone_t *out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->id, sizeof(out->id), "%s", jstr(z, "id", ""));
    snprintf(out->name, sizeof(out->name), "%s", jstr(z, "name", "Zone"));
    out->x_pct = jnum(z, "x_percent", 0);
    out->y_pct = jnum(z, "y_percent", 0);
    out->w_pct = jnum(z, "width_percent", 100);
    out->h_pct = jnum(z, "height_percent", 100);
    out->z_index = (int)jnum(z, "z_index", 0);
    snprintf(out->fit_mode, sizeof(out->fit_mode), "%s", jstr(z, "fit_mode", "contain"));
    snprintf(out->background_color, sizeof(out->background_color), "%s",
             jstr(z, "background_color", "#000000"));
}

static void parse_layout(cJSON *l, stp_layout_t *out) {
    memset(out, 0, sizeof(*out));
    if (!l || cJSON_IsNull(l)) return;
    snprintf(out->id, sizeof(out->id), "%s", jstr(l, "id", ""));
    out->width = (int)jnum(l, "width", 1920);
    out->height = (int)jnum(l, "height", 1080);
    cJSON *zones = cJSON_GetObjectItem(l, "zones");
    if (cJSON_IsArray(zones)) {
        int i = 0;
        cJSON *z;
        cJSON_ArrayForEach(z, zones) {
            if (i >= (int)(sizeof(out->zones) / sizeof(out->zones[0]))) break;
            parse_zone(z, &out->zones[i]);
            i++;
        }
        out->zone_count = i;
    }
}

static void parse_item(cJSON *a, stp_assignment_t *out) {
    memset(out, 0, sizeof(*out));
    cJSON *content_id = cJSON_GetObjectItem(a, "content_id");
    cJSON *widget_id = cJSON_GetObjectItem(a, "widget_id");

    if (widget_id && cJSON_IsString(widget_id) && widget_id->valuestring[0]) {
        out->kind = ITEM_WIDGET;
        snprintf(out->widget_id, sizeof(out->widget_id), "%s", widget_id->valuestring);
        snprintf(out->widget_type, sizeof(out->widget_type), "%s", jstr(a, "widget_type", ""));
        cJSON *cfg = cJSON_GetObjectItem(a, "config");
        if (cfg) {
            char *s = cJSON_PrintUnformatted(cfg);
            if (s) { snprintf(out->widget_config_json, sizeof(out->widget_config_json), "%s", s); free(s); }
        }
    } else {
        out->kind = ITEM_CONTENT;
        if (content_id && cJSON_IsString(content_id))
            snprintf(out->content_id, sizeof(out->content_id), "%s", content_id->valuestring);
        snprintf(out->filepath, sizeof(out->filepath), "%s", jstr(a, "filepath", ""));
        snprintf(out->mime_type, sizeof(out->mime_type), "%s", jstr(a, "mime_type", ""));
        snprintf(out->remote_url, sizeof(out->remote_url), "%s", jstr(a, "remote_url", ""));
        out->content_rev = (long)jnum(a, "content_rev", 0);
    }

    snprintf(out->zone_id, sizeof(out->zone_id), "%s", jstr(a, "zone_id", ""));
    out->sort_order = (int)jnum(a, "sort_order", 0);
    out->duration_sec = (int)jnum(a, "duration_sec", 10);
    out->muted = jbool(a, "muted", false);
    snprintf(out->schedule_start, sizeof(out->schedule_start), "%s", jstr(a, "schedule_start", ""));
    snprintf(out->schedule_end, sizeof(out->schedule_end), "%s", jstr(a, "schedule_end", ""));
    snprintf(out->schedule_days, sizeof(out->schedule_days), "%s", jstr(a, "schedule_days", ""));
    out->enabled = jbool(a, "enabled", true);
}

static int cmp_sort_order(const void *a, const void *b) {
    const stp_assignment_t *ia = a, *ib = b;
    /* group by zone so the player engine can walk one zone's queue at a
     * time; within a zone, honour sort_order */
    int zc = strcmp(ia->zone_id, ib->zone_id);
    if (zc) return zc;
    return ia->sort_order - ib->sort_order;
}

void playlist_parse(cJSON *payload, stp_playlist_t *out) {
    memset(out, 0, sizeof(*out));
    if (!payload) return;

    out->suspended = jbool(payload, "suspended", false);
    if (out->suspended) {
        snprintf(out->suspend_message, sizeof(out->suspend_message), "%s", jstr(payload, "message", "Suspended"));
        return;
    }

    snprintf(out->orientation, sizeof(out->orientation), "%s", jstr(payload, "orientation", "landscape"));
    snprintf(out->background_color, sizeof(out->background_color), "%s",
             jstr(payload, "background_color", "#000000"));

    cJSON *layout = cJSON_GetObjectItem(payload, "layout");
    /* The server strips zone IDs when there are fewer than two zones, making
     * a one-zone layout a fullscreen playlist on the wire. */
    if (layout && !cJSON_IsNull(layout)) {
        parse_layout(layout, &out->layout);
        out->has_layout = out->layout.zone_count > 1;
    }

    cJSON *assignments = cJSON_GetObjectItem(payload, "assignments");
    if (cJSON_IsArray(assignments)) {
        int i = 0;
        cJSON *a;
        cJSON_ArrayForEach(a, assignments) {
            if (i >= (int)(sizeof(out->items) / sizeof(out->items[0]))) break;
            stp_assignment_t item;
            parse_item(a, &item);
            if (item.enabled) out->items[i++] = item;
        }
        out->item_count = i;
        qsort(out->items, out->item_count, sizeof(out->items[0]), cmp_sort_order);
    }
}
