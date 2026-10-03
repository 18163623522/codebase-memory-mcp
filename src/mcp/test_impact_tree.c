#include "mcp/test_impact_tree_internal.h"

static cbm_pinned_tree_t *tpt_owner(tpt_context *c, const cbm_pinned_tree_options_t *o) {
    cbm_pinned_tree_t temporary = {0};
    temporary.limits = o->limits;
    cbm_arena_init_lazy(&temporary.arena, 65536);
    c->tree = &temporary;
    cbm_pinned_tree_t *t = tpt_alloc(c, 1, sizeof(*t));
    if (!t) {
        cbm_arena_destroy(&temporary.arena);
        c->tree = NULL;
        return NULL;
    }
    *t = temporary;
    t->parent.handle = TPT_INVALID_HANDLE;
    t->parent.directory = true;
    t->parent.parent = SIZE_MAX;
#ifdef _WIN32
    t->token = INVALID_HANDLE_VALUE;
    t->probe = INVALID_HANDLE_VALUE;
    t->impersonation = INVALID_HANDLE_VALUE;
    t->enumeration = INVALID_HANDLE_VALUE;
#else
    t->enumeration_fd = -1;
#endif
    c->tree = t;
    return t;
}

static bool tpt_load(tpt_context *c, const cbm_pinned_tree_options_t *o) {
    if (!tpt_identity_copy(c, o) || !tpt_poll(c))
        return false;
    cbm_git_tree_inventory_t inventory = {0};
    cbm_git_facts_error_t error = {0};
    bool ok = cbm_git_facts_inventory(o->facts, o->revision, &inventory, &error);
    if (!tpt_after_facts(c, ok, &error) || !tpt_plan(c, o, &inventory))
        return false;
    cbm_git_blob_batch_t batch = {0};
    return tpt_batch(c, o, &batch) && tpt_native_prepare(c) && tpt_build(c, &batch) &&
           tpt_audit(c) && tpt_manifest(c) && tpt_poll(c);
}

static void tpt_cleanup_error(tpt_context *c, const cbm_pinned_tree_error_t *primary) {
    cbm_pinned_tree_error_t cleanup = c->error;
    c->error = *primary;
    c->error.status = CBM_PINNED_TREE_CLEANUP_REQUIRED;
    if (primary->status == CBM_PINNED_TREE_OK) {
        c->error.cause = cleanup.status == CBM_PINNED_TREE_CHANGED ? CBM_PINNED_TREE_CHANGED
                                                                   : CBM_PINNED_TREE_IO;
        memcpy(c->error.diagnostic, cleanup.diagnostic, sizeof(c->error.diagnostic));
    }
    cbm_pinned_tree_t *t = c->tree;
    for (size_t i = 0; t->objects && i < t->object_count; i++) {
        if (t->objects[i].created) {
            memcpy(c->error.cleanup_path, t->root_path, TPT_PATH_CAP);
            break;
        }
    }
}

cbm_pinned_tree_status_t cbm_pinned_tree_create(const cbm_pinned_tree_options_t *o,
                                                cbm_pinned_tree_t **out,
                                                cbm_pinned_tree_error_t *error) {
    if (out)
        *out = NULL;
    tpt_context c = {0};
    tpt_error_init(&c.error);
    if (error)
        *error = c.error;
    if (!out || !tpt_options_valid(o)) {
        tpt_fail(&c, CBM_PINNED_TREE_INVALID, "invalid materializer options");
    } else {
        c.control = &o->control;
        c.building = true;
        if (tpt_owner(&c, o) && tpt_load(&c, o)) {
            c.tree->ready = true;
            *out = c.tree;
        } else if (c.tree) {
            cbm_pinned_tree_error_t primary = c.error;
            tpt_error_init(&c.error);
            c.control = NULL;
            if (tpt_cleanup(&c)) {
                tpt_dispose_memory(c.tree);
                c.error = primary;
            } else {
                tpt_cleanup_error(&c, &primary);
                *out = c.tree;
            }
        }
    }
    if (error)
        *error = c.error;
    return c.error.status;
}

const cbm_pinned_tree_view_t *cbm_pinned_tree_view(const cbm_pinned_tree_t *t) {
    return t && t->ready ? &t->view : NULL;
}

cbm_pinned_tree_status_t cbm_pinned_tree_verify(cbm_pinned_tree_t *t,
                                                const cbm_pinned_tree_control_t *control,
                                                cbm_pinned_tree_error_t *error) {
    tpt_context c = {.tree = t, .control = control};
    tpt_error_init(&c.error);
    if (!t || !t->ready || !control || !control->deadline_ms) {
        tpt_fail(&c, CBM_PINNED_TREE_INVALID, "invalid verification arguments");
    } else {
        t->ready = false;
        if (tpt_poll(&c) && tpt_audit(&c) && tpt_poll(&c))
            t->ready = true;
    }
    if (error)
        *error = c.error;
    return c.error.status;
}

cbm_pinned_tree_status_t cbm_pinned_tree_close(cbm_pinned_tree_t **owner,
                                               cbm_pinned_tree_error_t *error) {
    tpt_context c = {0};
    tpt_error_init(&c.error);
    if (!owner) {
        tpt_fail(&c, CBM_PINNED_TREE_INVALID, "missing owner pointer");
    } else if (*owner) {
        c.tree = *owner;
        c.tree->ready = false;
        if (tpt_cleanup(&c)) {
            tpt_dispose_memory(c.tree);
            *owner = NULL;
        } else {
            cbm_pinned_tree_error_t primary;
            tpt_error_init(&primary);
            tpt_cleanup_error(&c, &primary);
        }
    }
    if (error)
        *error = c.error;
    return c.error.status;
}
