/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3 - an improved tiling window manager
 * © 2009 Michael Stapelberg and contributors (see also: LICENSE)
 *
 * load_layout.c: Restore (parts of) the layout, for example after an inplace
 *                restart.
 *
 */
#include "all.h"

#include "jsmn.h"
#include <locale.h>

/* TODO: refactor the whole parsing thing */

static char *last_key;
static int incomplete;
static Con *json_node;
static Con *to_focus;
static bool parsing_gaps;
static bool parsing_swallows;
static bool parsing_rect;
static bool parsing_actual_deco_rect;
static bool parsing_deco_rect;
static bool parsing_window_rect;
static bool parsing_geometry;
static bool parsing_focus;
static bool parsing_marks;
struct Match *current_swallow;
static bool swallow_is_empty;
static int num_marks;
/* We need to save each container that needs to be marked if we want to support
 * marking non-leaf containers. In their case, the end_map for their children is
 * called before their own end_map, so marking json_node would end up marking
 * the latest child. We can't just mark containers immediately after we parse a
 * mark because of #2511. */
struct pending_marks {
    char *mark;
    Con *con_to_be_marked;
} *marks;

/* This list is used for reordering the focus stack after parsing the 'focus'
 * array. */
struct focus_mapping {
    int old_id;
    TAILQ_ENTRY(focus_mapping) focus_mappings;
};

static TAILQ_HEAD(focus_mappings_head, focus_mapping) focus_mappings =
    TAILQ_HEAD_INITIALIZER(focus_mappings);

static int json_start_map() {
    LOG("start of map, last_key = %s\n", last_key);
    if (parsing_swallows) {
        LOG("creating new swallow\n");
        current_swallow = smalloc(sizeof(Match));
        match_init(current_swallow);
        current_swallow->dock = M_DONTCHECK;
        TAILQ_INSERT_TAIL(&(json_node->swallow_head), current_swallow, matches);
        swallow_is_empty = true;
    } else {
        if (!parsing_rect &&
            !parsing_actual_deco_rect &&
            !parsing_deco_rect &&
            !parsing_window_rect &&
            !parsing_geometry &&
            !parsing_gaps) {
            if (last_key && strcasecmp(last_key, "floating_nodes") == 0) {
                DLOG("New floating_node\n");
                Con *ws = con_get_workspace(json_node);
                json_node = con_new_skeleton(NULL, NULL);
                json_node->name = NULL;
                json_node->parent = ws;
                DLOG("Parent is workspace = %p\n", ws);
            } else {
                Con *parent = json_node;
                json_node = con_new_skeleton(NULL, NULL);
                json_node->name = NULL;
                json_node->parent = parent;
            }
            /* json_node is incomplete and should be removed if parsing fails */
            incomplete++;
            DLOG("incomplete = %d\n", incomplete);
        }
    }
    return 1;
}

static int json_end_map() {
    LOG("end of map\n");
    if (!parsing_swallows &&
        !parsing_rect &&
        !parsing_actual_deco_rect &&
        !parsing_deco_rect &&
        !parsing_window_rect &&
        !parsing_geometry &&
        !parsing_gaps) {
        /* Set a few default values to simplify manually crafted layout files. */
        if (json_node->layout == L_DEFAULT) {
            DLOG("Setting layout = L_SPLITH\n");
            json_node->layout = L_SPLITH;
        }

        /* Sanity check: swallow criteria don’t make any sense on a split
         * container. */
        if (con_is_split(json_node) > 0 && !TAILQ_EMPTY(&(json_node->swallow_head))) {
            DLOG("sanity check: removing swallows specification from split container\n");
            while (!TAILQ_EMPTY(&(json_node->swallow_head))) {
                Match *match = TAILQ_FIRST(&(json_node->swallow_head));
                TAILQ_REMOVE(&(json_node->swallow_head), match, matches);
                match_free(match);
                free(match);
            }
        }

        if (json_node->type == CT_WORKSPACE) {
            /* Ensure the workspace has a name. */
            DLOG("Attaching workspace. name = %s\n", json_node->name);
            if (json_node->name == NULL || strcmp(json_node->name, "") == 0) {
                json_node->name = sstrdup("unnamed");
            }

            /* Prevent name clashes when appending a workspace, e.g. when the
             * user tries to restore a workspace called “1” but already has a
             * workspace called “1”. */
            char *base = sstrdup(json_node->name);
            int cnt = 1;
            while (get_existing_workspace_by_name(json_node->name) != NULL) {
                FREE(json_node->name);
                sasprintf(&(json_node->name), "%s_%d", base, cnt++);
            }
            free(base);

            /* Set num accordingly so that i3bar will properly sort it. */
            json_node->num = ws_name_to_number(json_node->name);
        }

        // When appending JSON layout files that only contain the workspace
        // _contents_, we might not have an upfront signal that the
        // container we’re currently parsing is a floating container (like
        // the “floating_nodes” key of the workspace container itself).
        // That’s why we make sure the con is attached at the right place
        // in the hierarchy in case it’s floating.
        if (json_node->type == CT_FLOATING_CON) {
            DLOG("fixing parent which currently is %p / %s\n", json_node->parent, json_node->parent->name);
            json_node->parent = con_get_workspace(json_node->parent);

            // Also set a size if none was supplied, otherwise the placeholder
            // window cannot be created as X11 requests with width=0 or
            // height=0 are invalid.
            if (rect_equals(json_node->rect, (Rect){0, 0, 0, 0})) {
                DLOG("Geometry not set, combining children\n");
                Con *child;
                TAILQ_FOREACH (child, &(json_node->nodes_head), nodes) {
                    DLOG("child geometry: %d x %d\n", child->geometry.width, child->geometry.height);
                    json_node->rect.width += child->geometry.width;
                    json_node->rect.height = max(json_node->rect.height, child->geometry.height);
                }
            }

            floating_check_size(json_node, false);
        }

        if (num_marks > 0) {
            for (int i = 0; i < num_marks; i++) {
                Con *con = marks[i].con_to_be_marked;
                char *mark = marks[i].mark;
                con_mark(con, mark, MM_ADD);
                free(mark);
            }

            FREE(marks);
            num_marks = 0;
        }

        LOG("attaching\n");
        con_attach(json_node, json_node->parent, true);
        LOG("Creating window\n");
        x_con_init(json_node);

        /* Fix erroneous JSON input regarding floating containers to avoid
         * crashing, see #3901. */
        const int old_floating_mode = json_node->floating;
        if (old_floating_mode >= FLOATING_AUTO_ON && json_node->parent->type != CT_FLOATING_CON) {
            LOG("Fixing floating node without CT_FLOATING_CON parent\n");

            /* Force floating_enable to work */
            json_node->floating = FLOATING_AUTO_OFF;
            floating_enable(json_node, false);
            json_node->floating = old_floating_mode;
        }

        json_node = json_node->parent;
        incomplete--;
        DLOG("incomplete = %d\n", incomplete);
    }

    if (parsing_swallows && swallow_is_empty) {
        /* We parsed an empty swallow definition. This is an invalid layout
         * definition, hence we reject it. */
        ELOG("Layout file is invalid: found an empty swallow definition.\n");
        return 0;
    }

    parsing_gaps = false;
    parsing_rect = false;
    parsing_actual_deco_rect = false;
    parsing_deco_rect = false;
    parsing_window_rect = false;
    parsing_geometry = false;
    return 1;
}

static int json_end_array() {
    LOG("end of array\n");
    if (!parsing_swallows && !parsing_focus && !parsing_marks) {
        con_fix_percent(json_node);
    }
    if (parsing_swallows) {
        parsing_swallows = false;
    }
    if (parsing_marks) {
        parsing_marks = false;
    }

    if (parsing_focus) {
        /* Clear the list of focus mappings */
        struct focus_mapping *mapping;
        TAILQ_FOREACH_REVERSE (mapping, &focus_mappings, focus_mappings_head, focus_mappings) {
            LOG("focus (reverse) %d\n", mapping->old_id);
            Con *con;
            TAILQ_FOREACH (con, &(json_node->focus_head), focused) {
                if (con->old_id != mapping->old_id) {
                    continue;
                }
                LOG("got it! %p\n", con);
                /* Move this entry to the top of the focus list. */
                TAILQ_REMOVE(&(json_node->focus_head), con, focused);
                TAILQ_INSERT_HEAD(&(json_node->focus_head), con, focused);
                break;
            }
        }
        while (!TAILQ_EMPTY(&focus_mappings)) {
            mapping = TAILQ_FIRST(&focus_mappings);
            TAILQ_REMOVE(&focus_mappings, mapping, focus_mappings);
            free(mapping);
        }
        parsing_focus = false;
    }
    return 1;
}

static int json_key(const char *val, size_t len) {
    LOG("key: %.*s\n", (int)len, val);
    FREE(last_key);
    last_key = scalloc(len + 1, 1);
    memcpy(last_key, val, len);
    if (strcasecmp(last_key, "swallows") == 0) {
        parsing_swallows = true;
    }

    if (strcasecmp(last_key, "gaps") == 0) {
        parsing_gaps = true;
    }

    if (strcasecmp(last_key, "rect") == 0) {
        parsing_rect = true;
    }

    if (strcasecmp(last_key, "actual_deco_rect") == 0) {
        parsing_actual_deco_rect = true;
    }

    if (strcasecmp(last_key, "deco_rect") == 0) {
        parsing_deco_rect = true;
    }

    if (strcasecmp(last_key, "window_rect") == 0) {
        parsing_window_rect = true;
    }

    if (strcasecmp(last_key, "geometry") == 0) {
        parsing_geometry = true;
    }

    if (strcasecmp(last_key, "focus") == 0) {
        parsing_focus = true;
    }

    if (strcasecmp(last_key, "marks") == 0) {
        num_marks = 0;
        parsing_marks = true;
    }

    return 1;
}

static int json_string(const char *val, size_t len) {
    LOG("string: %.*s for key %s\n", (int)len, val, last_key);
    if (parsing_swallows) {
        char *sval;
        sasprintf(&sval, "%.*s", len, val);
        if (strcasecmp(last_key, "class") == 0) {
            current_swallow->class = regex_new(sval);
            swallow_is_empty = false;
        } else if (strcasecmp(last_key, "instance") == 0) {
            current_swallow->instance = regex_new(sval);
            swallow_is_empty = false;
        } else if (strcasecmp(last_key, "window_role") == 0) {
            current_swallow->window_role = regex_new(sval);
            swallow_is_empty = false;
        } else if (strcasecmp(last_key, "title") == 0) {
            current_swallow->title = regex_new(sval);
            swallow_is_empty = false;
        } else if (strcasecmp(last_key, "machine") == 0) {
            current_swallow->machine = regex_new(sval);
            swallow_is_empty = false;
        } else {
            ELOG("swallow key %s unknown\n", last_key);
        }
        free(sval);
    } else if (parsing_marks) {
        char *mark;
        sasprintf(&mark, "%.*s", (int)len, val);

        marks = srealloc(marks, (++num_marks) * sizeof(struct pending_marks));
        marks[num_marks - 1].mark = sstrdup(mark);
        marks[num_marks - 1].con_to_be_marked = json_node;
    } else {
        if (strcasecmp(last_key, "name") == 0) {
            json_node->name = scalloc(len + 1, 1);
            memcpy(json_node->name, val, len);
        } else if (strcasecmp(last_key, "title_format") == 0) {
            json_node->title_format = scalloc(len + 1, 1);
            memcpy(json_node->title_format, val, len);
        } else if (strcasecmp(last_key, "sticky_group") == 0) {
            json_node->sticky_group = scalloc(len + 1, 1);
            memcpy(json_node->sticky_group, val, len);
            LOG("sticky_group of this container is %s\n", json_node->sticky_group);
        } else if (strcasecmp(last_key, "orientation") == 0) {
            /* Upgrade path from older versions of i3 (doing an inplace restart
             * to a newer version):
             * "orientation" is dumped before "layout". Therefore, we store
             * whether the orientation was horizontal or vertical in the
             * last_split_layout. When we then encounter layout == "default",
             * we will use the last_split_layout as layout instead. */
            char *buf = NULL;
            sasprintf(&buf, "%.*s", (int)len, val);
            if (strcasecmp(buf, "none") == 0 ||
                strcasecmp(buf, "horizontal") == 0) {
                json_node->last_split_layout = L_SPLITH;
            } else if (strcasecmp(buf, "vertical") == 0) {
                json_node->last_split_layout = L_SPLITV;
            } else {
                LOG("Unhandled orientation: %s\n", buf);
            }
            free(buf);
        } else if (strcasecmp(last_key, "border") == 0) {
            char *buf = NULL;
            sasprintf(&buf, "%.*s", (int)len, val);
            if (strcasecmp(buf, "none") == 0) {
                json_node->max_user_border_style = json_node->border_style = BS_NONE;
            } else if (strcasecmp(buf, "1pixel") == 0) {
                json_node->max_user_border_style = json_node->border_style = BS_PIXEL;
                json_node->current_border_width = 1;
            } else if (strcasecmp(buf, "pixel") == 0) {
                json_node->max_user_border_style = json_node->border_style = BS_PIXEL;
            } else if (strcasecmp(buf, "normal") == 0) {
                json_node->max_user_border_style = json_node->border_style = BS_NORMAL;
            } else {
                LOG("Unhandled \"border\": %s\n", buf);
            }
            free(buf);
        } else if (strcasecmp(last_key, "type") == 0) {
            char *buf = NULL;
            sasprintf(&buf, "%.*s", (int)len, val);
            if (strcasecmp(buf, "root") == 0) {
                json_node->type = CT_ROOT;
            } else if (strcasecmp(buf, "output") == 0) {
                json_node->type = CT_OUTPUT;
            } else if (strcasecmp(buf, "con") == 0) {
                json_node->type = CT_CON;
            } else if (strcasecmp(buf, "floating_con") == 0) {
                json_node->type = CT_FLOATING_CON;
            } else if (strcasecmp(buf, "workspace") == 0) {
                json_node->type = CT_WORKSPACE;
            } else if (strcasecmp(buf, "dockarea") == 0) {
                json_node->type = CT_DOCKAREA;
            } else {
                LOG("Unhandled \"type\": %s\n", buf);
            }
            free(buf);
        } else if (strcasecmp(last_key, "layout") == 0) {
            char *buf = NULL;
            sasprintf(&buf, "%.*s", (int)len, val);
            if (strcasecmp(buf, "default") == 0) {
                /* This set above when we read "orientation". */
                json_node->layout = json_node->last_split_layout;
            } else if (strcasecmp(buf, "stacked") == 0) {
                json_node->layout = L_STACKED;
            } else if (strcasecmp(buf, "tabbed") == 0) {
                json_node->layout = L_TABBED;
            } else if (strcasecmp(buf, "dockarea") == 0) {
                json_node->layout = L_DOCKAREA;
            } else if (strcasecmp(buf, "output") == 0) {
                json_node->layout = L_OUTPUT;
            } else if (strcasecmp(buf, "splith") == 0) {
                json_node->layout = L_SPLITH;
            } else if (strcasecmp(buf, "splitv") == 0) {
                json_node->layout = L_SPLITV;
            } else {
                LOG("Unhandled \"layout\": %s\n", buf);
            }
            free(buf);
        } else if (strcasecmp(last_key, "workspace_layout") == 0) {
            char *buf = NULL;
            sasprintf(&buf, "%.*s", (int)len, val);
            if (strcasecmp(buf, "default") == 0) {
                json_node->workspace_layout = L_DEFAULT;
            } else if (strcasecmp(buf, "stacked") == 0) {
                json_node->workspace_layout = L_STACKED;
            } else if (strcasecmp(buf, "tabbed") == 0) {
                json_node->workspace_layout = L_TABBED;
            } else {
                LOG("Unhandled \"workspace_layout\": %s\n", buf);
            }
            free(buf);
        } else if (strcasecmp(last_key, "last_split_layout") == 0) {
            char *buf = NULL;
            sasprintf(&buf, "%.*s", (int)len, val);
            if (strcasecmp(buf, "splith") == 0) {
                json_node->last_split_layout = L_SPLITH;
            } else if (strcasecmp(buf, "splitv") == 0) {
                json_node->last_split_layout = L_SPLITV;
            } else {
                LOG("Unhandled \"last_splitlayout\": %s\n", buf);
            }
            free(buf);
        } else if (strcasecmp(last_key, "mark") == 0) {
            DLOG("Found deprecated key \"mark\".\n");

            char *buf = NULL;
            sasprintf(&buf, "%.*s", (int)len, val);

            con_mark(json_node, buf, MM_REPLACE);
        } else if (strcasecmp(last_key, "floating") == 0) {
            char *buf = NULL;
            sasprintf(&buf, "%.*s", (int)len, val);
            if (strcasecmp(buf, "auto_off") == 0) {
                json_node->floating = FLOATING_AUTO_OFF;
            } else if (strcasecmp(buf, "auto_on") == 0) {
                json_node->floating = FLOATING_AUTO_ON;
            } else if (strcasecmp(buf, "user_off") == 0) {
                json_node->floating = FLOATING_USER_OFF;
            } else if (strcasecmp(buf, "user_on") == 0) {
                json_node->floating = FLOATING_USER_ON;
            }
            free(buf);
        } else if (strcasecmp(last_key, "scratchpad_state") == 0) {
            char *buf = NULL;
            sasprintf(&buf, "%.*s", (int)len, val);
            if (strcasecmp(buf, "none") == 0) {
                json_node->scratchpad_state = SCRATCHPAD_NONE;
            } else if (strcasecmp(buf, "fresh") == 0) {
                json_node->scratchpad_state = SCRATCHPAD_FRESH;
            } else if (strcasecmp(buf, "changed") == 0) {
                json_node->scratchpad_state = SCRATCHPAD_CHANGED;
            }
            free(buf);
        } else if (strcasecmp(last_key, "previous_workspace_name") == 0) {
            FREE(previous_workspace_name);
            previous_workspace_name = sstrndup((const char *)val, len);
        }
    }
    return 1;
}

static int json_int(long long val) {
    LOG("int %lld for key %s\n", val, last_key);
    /* For backwards compatibility with i3 < 4.8 */
    if (strcasecmp(last_key, "type") == 0) {
        json_node->type = val;
    }

    if (strcasecmp(last_key, "fullscreen_mode") == 0) {
        json_node->fullscreen_mode = val;
    }

    if (strcasecmp(last_key, "num") == 0) {
        json_node->num = val;
    }

    if (strcasecmp(last_key, "current_border_width") == 0) {
        json_node->current_border_width = val;
    }

    if (strcasecmp(last_key, "window_icon_padding") == 0) {
        json_node->window_icon_padding = val;
    }

    if (strcasecmp(last_key, "depth") == 0) {
        json_node->depth = val;
    }

    if (!parsing_swallows && strcasecmp(last_key, "id") == 0) {
        json_node->old_id = val;
    }

    if (parsing_focus) {
        struct focus_mapping *focus_mapping = scalloc(1, sizeof(struct focus_mapping));
        focus_mapping->old_id = val;
        TAILQ_INSERT_TAIL(&focus_mappings, focus_mapping, focus_mappings);
    }

    if (parsing_rect || parsing_window_rect || parsing_geometry) {
        Rect *r;
        if (parsing_rect) {
            r = &(json_node->rect);
        } else if (parsing_window_rect) {
            r = &(json_node->window_rect);
        } else {
            r = &(json_node->geometry);
        }
        if (strcasecmp(last_key, "x") == 0) {
            r->x = val;
        } else if (strcasecmp(last_key, "y") == 0) {
            r->y = val;
        } else if (strcasecmp(last_key, "width") == 0) {
            r->width = val;
        } else if (strcasecmp(last_key, "height") == 0) {
            r->height = val;
        } else {
            ELOG("WARNING: unknown key %s in rect\n", last_key);
        }
        DLOG("rect now: (%d, %d, %d, %d)\n",
             r->x, r->y, r->width, r->height);
    }
    if (parsing_swallows) {
        if (strcasecmp(last_key, "id") == 0) {
            current_swallow->id = val;
            swallow_is_empty = false;
        }
        if (strcasecmp(last_key, "dock") == 0) {
            current_swallow->dock = val;
            swallow_is_empty = false;
        }
        if (strcasecmp(last_key, "insert_where") == 0) {
            current_swallow->insert_where = val;
            swallow_is_empty = false;
        }
    }
    if (parsing_gaps) {
        if (strcasecmp(last_key, "inner") == 0) {
            json_node->gaps.inner = val;
        } else if (strcasecmp(last_key, "top") == 0) {
            json_node->gaps.top = val;
        } else if (strcasecmp(last_key, "right") == 0) {
            json_node->gaps.right = val;
        } else if (strcasecmp(last_key, "bottom") == 0) {
            json_node->gaps.bottom = val;
        } else if (strcasecmp(last_key, "left") == 0) {
            json_node->gaps.left = val;
        }
    }

    return 1;
}

static int json_bool(int val) {
    LOG("bool %d for key %s\n", val, last_key);
    if (strcasecmp(last_key, "focused") == 0 && val) {
        to_focus = json_node;
    }

    if (strcasecmp(last_key, "sticky") == 0) {
        json_node->sticky = val;
    }

    if (parsing_swallows) {
        if (strcasecmp(last_key, "restart_mode") == 0) {
            current_swallow->restart_mode = val;
            swallow_is_empty = false;
        }
    }

    return 1;
}

static int json_double(double val) {
    LOG("double %f for key %s\n", val, last_key);
    if (strcasecmp(last_key, "percent") == 0) {
        json_node->percent = val;
    }
    return 1;
}

/*
 * Returns true if the provided JSON could be parsed by yajl.
 *
 */
bool json_validate(const char *buf, const size_t len) {
    jsmn_parser parser;
    jsmntok_t tokens[1024]; /* Adjust size as needed */
    jsmn_init(&parser);

    int ret = jsmn_parse(&parser, buf, len, tokens, sizeof(tokens) / sizeof(tokens[0]));

    if (ret < 0) {
        ELOG("JSON parsing error: %d\n", ret);
        return false;
    }
    return true;
}

/* Parses the given JSON file until it encounters the first “type” property to
 * determine whether the file contains workspaces or regular containers, which
 * is important to know when deciding where (and how) to append the contents.
 * */
json_content_t json_determine_content(const char *buf, const size_t len) {
    jsmn_parser parser;
    jsmntok_t tokens[1024];  // Adjust size as needed
    jsmn_init(&parser);

    int ret = jsmn_parse(&parser, buf, len, tokens, sizeof(tokens) / sizeof(tokens[0]));

    if (ret < 0) {
        ELOG("JSON parsing error: %d\n", ret);
        return JSON_CONTENT_CON;  // Default fallback
    }

    // Traverse tokens to find the first "type" key
    for (int i = 1; i < ret; i++) {
        if (tokens[i].type == JSMN_STRING && strncmp(buf + tokens[i].start, "type", tokens[i].end - tokens[i].start) == 0) {
            if (i + 1 < ret && tokens[i + 1].type == JSMN_STRING) {
                if (strncmp(buf + tokens[i + 1].start, "workspace", tokens[i + 1].end - tokens[i + 1].start) == 0) {
                    return JSON_CONTENT_WORKSPACE;
                }
            }
        }
    }

    return JSON_CONTENT_CON;
}

// Hand-written function to convert a string to an integer
static bool str2int(const char *str, size_t len) {
    long long int ret = 0;
    size_t i = 0;

    long long sign = 1;
    if (str[0] == '-') {
        sign = -1;
        i++;
    } else if (str[0] == '+') {
        i++;
    }

    for (; i < len; ++i) {
        if (str[i] < '0' || str[i] > '9') {
            return false;
        }
        ret = ret * 10 + (str[i] - '0');
    }
    json_int(sign * ret);
    return true;
}

// Hand-written function to convert a string to a floating-point number
static void str2double(const char *str, size_t len) {
    double ret = 0.0;
    double factor = 1.0;
    size_t i = 0;

    double sign = 1.0;
    if (str[0] == '-') {
        sign = -1;
        i++;
    } else if (str[0] == '+') {
        i++;
    }

    // Handle integer part
    while (i < len && str[i] >= '0' && str[i] <= '9') {
        ret = ret * 10 + (str[i] - '0');
        i++;
    }

    // Handle decimal point, if any
    if (i < len && str[i] == '.') {
        i++;
        while (i < len && str[i] >= '0' && str[i] <= '9') {
            factor *= 0.1;
            ret += (str[i] - '0') * factor;
            i++;
        }
    }

    json_double(sign * ret);
}

void handle_primitive(const char *str, size_t len) {
    if (str == NULL || len == 0) {
        return;
    }

    if (!str2int(str, len)) {
        str2double(str, len);
    }
}

static int rec_parse(const char *js, jsmntok_t *t, size_t count) {
    if (count == 0) {
        return 0;
    }

    const char *v = js + t->start;
    const size_t len = t->end - t->start;
    if (t->type == JSMN_PRIMITIVE) {
        if (v[0] == 't' || v[0] == 'f') {
            json_bool(v[0] == 't');
        } else {
            handle_primitive(v, len);
        }
        return 1;
    } else if (t->type == JSMN_STRING) {
        json_string(v, len);
        return 1;
    } else if (t->type == JSMN_OBJECT) {
        json_start_map();
        int j = 0;
        for (int i = 0; i < t->size; i++) {
            jsmntok_t *key = t + 1 + j;
            json_key(js + key->start, key->end - key->start);
            j++;
            if (key->size > 0) {
                j += rec_parse(js, t + 1 + j, count - j);
            }
        }
        json_end_map();
        return j + 1;
    } else if (t->type == JSMN_ARRAY) {
        int j = 0;
        for (int i = 0; i < t->size; i++) {
            j += rec_parse(js, t + 1 + j, count - j);
        }
        json_end_array();
        return j + 1;
    }
    return 0;
}

int jsmn_parse_all(const char *js, const size_t len) {
    jsmn_parser parser;
    jsmntok_t tokens[1024 * 10];  // Adjust size as needed
    jsmn_init(&parser);

    int ret = jsmn_parse(&parser, js, len, tokens, sizeof(tokens) / sizeof(tokens[0]));
    if (ret < 0) {
        ELOG("error at %d: %.*s\n", parser.pos, 50, js + parser.pos);
        return ret;
    }

    for (unsigned int pos = 0; pos < parser.toknext;) {
        jsmntok_t t = tokens[pos];
        if (t.end == 0) {
            break;
        }
        int parsed = rec_parse(js, tokens + pos, parser.toknext - pos);
        pos += max(parsed, 1);
    }
    return ret;
}

void tree_append_json(Con *con, const char *buf, const size_t len, char **errormsg) {
    json_node = con;
    to_focus = NULL;
    parsing_gaps = false;
    incomplete = 0;
    parsing_swallows = false;
    parsing_rect = false;
    parsing_actual_deco_rect = false;
    parsing_deco_rect = false;
    parsing_window_rect = false;
    parsing_geometry = false;
    parsing_focus = false;
    parsing_marks = false;

    int ret = jsmn_parse_all(buf, len);
    if (ret < 0) {
        // TODO: JSMN_ERROR_NOMEM
        ELOG("JSON parsing error: %d\n", ret);
        if (errormsg != NULL) {
            *errormsg = sstrdup("JSON parsing error");
        }
    }
    while (incomplete-- > 0) {
        Con *parent = json_node->parent;
        DLOG("freeing incomplete container %p\n", json_node);
        if (json_node == to_focus) {
            to_focus = NULL;
        }
        con_free(json_node);
        json_node = parent;
    }

    /* In case not all containers were restored, we need to fix the
     * percentages, otherwise i3 will crash immediately when rendering the
     * next time. */
    con_fix_percent(con);
    if (to_focus) {
        con_activate(to_focus);
    }
}
