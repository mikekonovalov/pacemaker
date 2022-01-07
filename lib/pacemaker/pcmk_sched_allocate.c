/*
 * Copyright 2004-2022 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <crm/crm.h>
#include <crm/cib.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>
#include <crm/common/xml_internal.h>

#include <glib.h>

#include <crm/pengine/status.h>
#include <pacemaker-internal.h>
#include "libpacemaker_private.h"

CRM_TRACE_INIT_DATA(pacemaker);

extern bool pcmk__is_daemon;

/*!
 * \internal
 * \brief Do deferred action checks after allocation
 *
 * \param[in] data_set  Working set for cluster
 */
static void
check_params(pe_resource_t *rsc, pe_node_t *node, xmlNode *rsc_op,
             enum pe_check_parameters check, pe_working_set_t *data_set)
{
    const char *reason = NULL;
    op_digest_cache_t *digest_data = NULL;

    switch (check) {
        case pe_check_active:
            if (pcmk__check_action_config(rsc, node, rsc_op)
                && pe_get_failcount(node, rsc, NULL, pe_fc_effective, NULL,
                                    data_set)) {

                reason = "action definition changed";
            }
            break;

        case pe_check_last_failure:
            digest_data = rsc_action_digest_cmp(rsc, rsc_op, node, data_set);
            switch (digest_data->rc) {
                case RSC_DIGEST_UNKNOWN:
                    crm_trace("Resource %s history entry %s on %s has no digest to compare",
                              rsc->id, ID(rsc_op), node->details->id);
                    break;
                case RSC_DIGEST_MATCH:
                    break;
                default:
                    reason = "resource parameters have changed";
                    break;
            }
            break;
    }

    if (reason) {
        pe__clear_failcount(rsc, node, reason, data_set);
    }
}

static gboolean
failcount_clear_action_exists(pe_node_t * node, pe_resource_t * rsc)
{
    gboolean rc = FALSE;
    GList *list = pe__resource_actions(rsc, node, CRM_OP_CLEAR_FAILCOUNT, TRUE);

    if (list) {
        rc = TRUE;
    }
    g_list_free(list);
    return rc;
}

static void
common_apply_stickiness(pe_resource_t * rsc, pe_node_t * node, pe_working_set_t * data_set)
{
    if (rsc->children) {
        GList *gIter = rsc->children;

        for (; gIter != NULL; gIter = gIter->next) {
            pe_resource_t *child_rsc = (pe_resource_t *) gIter->data;

            common_apply_stickiness(child_rsc, node, data_set);
        }
        return;
    }

    if (pcmk_is_set(rsc->flags, pe_rsc_managed)
        && rsc->stickiness != 0 && pcmk__list_of_1(rsc->running_on)) {
        pe_node_t *current = pe_find_node_id(rsc->running_on, node->details->id);
        pe_node_t *match = pe_hash_table_lookup(rsc->allowed_nodes, node->details->id);

        if (current == NULL) {

        } else if ((match != NULL)
                   || pcmk_is_set(data_set->flags, pe_flag_symmetric_cluster)) {
            pe_resource_t *sticky_rsc = rsc;

            resource_location(sticky_rsc, node, rsc->stickiness, "stickiness", data_set);
            pe_rsc_debug(sticky_rsc, "Resource %s: preferring current location"
                         " (node=%s, weight=%d)", sticky_rsc->id,
                         node->details->uname, rsc->stickiness);
        } else {
            GHashTableIter iter;
            pe_node_t *nIter = NULL;

            pe_rsc_debug(rsc, "Ignoring stickiness for %s: the cluster is asymmetric"
                         " and node %s is not explicitly allowed", rsc->id, node->details->uname);
            g_hash_table_iter_init(&iter, rsc->allowed_nodes);
            while (g_hash_table_iter_next(&iter, NULL, (void **)&nIter)) {
                crm_err("%s[%s] = %d", rsc->id, nIter->details->uname, nIter->weight);
            }
        }
    }

    /* Check the migration threshold only if a failcount clear action
     * has not already been placed for this resource on the node.
     * There is no sense in potentially forcing the resource from this
     * node if the failcount is being reset anyway.
     *
     * @TODO A clear_failcount operation can be scheduled in stage4() via
     * check_actions_for(), or in stage5() via check_params(). This runs in
     * stage2(), so it cannot detect those, meaning we might check the migration
     * threshold when we shouldn't -- worst case, we stop or move the resource,
     * then move it back next transition.
     */
    if (failcount_clear_action_exists(node, rsc) == FALSE) {
        pe_resource_t *failed = NULL;

        if (pcmk__threshold_reached(rsc, node, &failed)) {
            resource_location(failed, node, -INFINITY, "__fail_limit__",
                              data_set);
        }
    }
}

static void
calculate_system_health(gpointer gKey, gpointer gValue, gpointer user_data)
{
    const char *key = (const char *)gKey;
    const char *value = (const char *)gValue;
    int *system_health = (int *)user_data;

    if (!gKey || !gValue || !user_data) {
        return;
    }

    if (pcmk__starts_with(key, "#health")) {
        int score;

        /* Convert the value into an integer */
        score = char2score(value);

        /* Add it to the running total */
        *system_health = pe__add_scores(score, *system_health);
    }
}

static gboolean
apply_system_health(pe_working_set_t * data_set)
{
    GList *gIter = NULL;
    const char *health_strategy = pe_pref(data_set->config_hash, "node-health-strategy");
    int base_health = 0;

    if (pcmk__str_eq(health_strategy, "none", pcmk__str_null_matches | pcmk__str_casei)) {
        /* Prevent any accidental health -> score translation */
        pcmk__score_red = 0;
        pcmk__score_yellow = 0;
        pcmk__score_green = 0;
        return TRUE;

    } else if (pcmk__str_eq(health_strategy, "migrate-on-red", pcmk__str_casei)) {

        /* Resources on nodes which have health values of red are
         * weighted away from that node.
         */
        pcmk__score_red = -INFINITY;
        pcmk__score_yellow = 0;
        pcmk__score_green = 0;

    } else if (pcmk__str_eq(health_strategy, "only-green", pcmk__str_casei)) {

        /* Resources on nodes which have health values of red or yellow
         * are forced away from that node.
         */
        pcmk__score_red = -INFINITY;
        pcmk__score_yellow = -INFINITY;
        pcmk__score_green = 0;

    } else if (pcmk__str_eq(health_strategy, "progressive", pcmk__str_casei)) {
        /* Same as the above, but use the r/y/g scores provided by the user
         * Defaults are provided by the pe_prefs table
         * Also, custom health "base score" can be used
         */
        base_health = char2score(pe_pref(data_set->config_hash,
                                         "node-health-base"));

    } else if (pcmk__str_eq(health_strategy, "custom", pcmk__str_casei)) {

        /* Requires the admin to configure the rsc_location constaints for
         * processing the stored health scores
         */
        /* TODO: Check for the existence of appropriate node health constraints */
        return TRUE;

    } else {
        crm_err("Unknown node health strategy: %s", health_strategy);
        return FALSE;
    }

    crm_info("Applying automated node health strategy: %s", health_strategy);

    for (gIter = data_set->nodes; gIter != NULL; gIter = gIter->next) {
        int system_health = base_health;
        pe_node_t *node = (pe_node_t *) gIter->data;

        /* Search through the node hash table for system health entries. */
        g_hash_table_foreach(node->details->attrs, calculate_system_health, &system_health);

        crm_info(" Node %s has an combined system health of %d",
                 node->details->uname, system_health);

        /* If the health is non-zero, then create a new location constraint so
         * that the weight will be added later on.
         */
        if (system_health != 0) {

            GList *gIter2 = data_set->resources;

            for (; gIter2 != NULL; gIter2 = gIter2->next) {
                pe_resource_t *rsc = (pe_resource_t *) gIter2->data;

                pcmk__new_location(health_strategy, rsc, system_health, NULL,
                                   node, data_set);
            }
        }
    }

    return TRUE;
}

gboolean
stage0(pe_working_set_t * data_set)
{
    if (data_set->input == NULL) {
        return FALSE;
    }

    if (!pcmk_is_set(data_set->flags, pe_flag_have_status)) {
        crm_trace("Calculating status");
        cluster_status(data_set);
    }

    pcmk__set_allocation_methods(data_set);
    apply_system_health(data_set);
    pcmk__unpack_constraints(data_set);

    return TRUE;
}

static void
rsc_discover_filter(pe_resource_t *rsc, pe_node_t *node)
{
    pe_resource_t *top = uber_parent(rsc);
    pe_node_t *match;

    if (rsc->exclusive_discover == FALSE && top->exclusive_discover == FALSE) {
        return;
    }

    g_list_foreach(rsc->children, (GFunc) rsc_discover_filter, node);

    match = g_hash_table_lookup(rsc->allowed_nodes, node->details->id);
    if (match && match->rsc_discover_mode != pe_discover_exclusive) {
        match->weight = -INFINITY;
    }
}

static time_t
shutdown_time(pe_node_t *node, pe_working_set_t *data_set)
{
    const char *shutdown = pe_node_attribute_raw(node, XML_CIB_ATTR_SHUTDOWN);
    time_t result = 0;

    if (shutdown) {
        long long result_ll;

        if (pcmk__scan_ll(shutdown, &result_ll, 0LL) == pcmk_rc_ok) {
            result = (time_t) result_ll;
        }
    }
    return result? result : get_effective_time(data_set);
}

static void
apply_shutdown_lock(pe_resource_t *rsc, pe_working_set_t *data_set)
{
    const char *class;

    // Only primitives and (uncloned) groups may be locked
    if (rsc->variant == pe_group) {
        g_list_foreach(rsc->children, (GFunc) apply_shutdown_lock, data_set);
    } else if (rsc->variant != pe_native) {
        return;
    }

    // Fence devices and remote connections can't be locked
    class = crm_element_value(rsc->xml, XML_AGENT_ATTR_CLASS);
    if (pcmk__str_eq(class, PCMK_RESOURCE_CLASS_STONITH, pcmk__str_null_matches)
        || pe__resource_is_remote_conn(rsc, data_set)) {
        return;
    }

    if (rsc->lock_node != NULL) {
        // The lock was obtained from resource history

        if (rsc->running_on != NULL) {
            /* The resource was started elsewhere even though it is now
             * considered locked. This shouldn't be possible, but as a
             * failsafe, we don't want to disturb the resource now.
             */
            pe_rsc_info(rsc,
                        "Cancelling shutdown lock because %s is already active",
                        rsc->id);
            pe__clear_resource_history(rsc, rsc->lock_node, data_set);
            rsc->lock_node = NULL;
            rsc->lock_time = 0;
        }

    // Only a resource active on exactly one node can be locked
    } else if (pcmk__list_of_1(rsc->running_on)) {
        pe_node_t *node = rsc->running_on->data;

        if (node->details->shutdown) {
            if (node->details->unclean) {
                pe_rsc_debug(rsc, "Not locking %s to unclean %s for shutdown",
                             rsc->id, node->details->uname);
            } else {
                rsc->lock_node = node;
                rsc->lock_time = shutdown_time(node, data_set);
            }
        }
    }

    if (rsc->lock_node == NULL) {
        // No lock needed
        return;
    }

    if (data_set->shutdown_lock > 0) {
        time_t lock_expiration = rsc->lock_time + data_set->shutdown_lock;

        pe_rsc_info(rsc, "Locking %s to %s due to shutdown (expires @%lld)",
                    rsc->id, rsc->lock_node->details->uname,
                    (long long) lock_expiration);
        pe__update_recheck_time(++lock_expiration, data_set);
    } else {
        pe_rsc_info(rsc, "Locking %s to %s due to shutdown",
                    rsc->id, rsc->lock_node->details->uname);
    }

    // If resource is locked to one node, ban it from all other nodes
    for (GList *item = data_set->nodes; item != NULL; item = item->next) {
        pe_node_t *node = item->data;

        if (strcmp(node->details->uname, rsc->lock_node->details->uname)) {
            resource_location(rsc, node, -CRM_SCORE_INFINITY,
                              XML_CONFIG_ATTR_SHUTDOWN_LOCK, data_set);
        }
    }
}

/*
 * \internal
 * \brief Stage 2 of cluster status: apply node-specific criteria
 *
 * Count known nodes, and apply location constraints, stickiness, and exclusive
 * resource discovery.
 */
gboolean
stage2(pe_working_set_t * data_set)
{
    GList *gIter = NULL;

    if (pcmk_is_set(data_set->flags, pe_flag_shutdown_lock)) {
        g_list_foreach(data_set->resources, (GFunc) apply_shutdown_lock, data_set);
    }

    if (!pcmk_is_set(data_set->flags, pe_flag_no_compat)) {
        // @COMPAT API backward compatibility
        for (gIter = data_set->nodes; gIter != NULL; gIter = gIter->next) {
            pe_node_t *node = (pe_node_t *) gIter->data;

            if (node && (node->weight >= 0) && node->details->online
                && (node->details->type != node_ping)) {
                data_set->max_valid_nodes++;
            }
        }
    }

    pcmk__apply_locations(data_set);

    gIter = data_set->nodes;
    for (; gIter != NULL; gIter = gIter->next) {
        GList *gIter2 = NULL;
        pe_node_t *node = (pe_node_t *) gIter->data;

        gIter2 = data_set->resources;
        for (; gIter2 != NULL; gIter2 = gIter2->next) {
            pe_resource_t *rsc = (pe_resource_t *) gIter2->data;

            common_apply_stickiness(rsc, node, data_set);
            rsc_discover_filter(rsc, node);
        }
    }

    return TRUE;
}

static void
allocate_resources(pe_working_set_t * data_set)
{
    GList *gIter = NULL;

    if (pcmk_is_set(data_set->flags, pe_flag_have_remote_nodes)) {
        /* Allocate remote connection resources first (which will also allocate
         * any colocation dependencies). If the connection is migrating, always
         * prefer the partial migration target.
         */
        for (gIter = data_set->resources; gIter != NULL; gIter = gIter->next) {
            pe_resource_t *rsc = (pe_resource_t *) gIter->data;
            if (rsc->is_remote_node == FALSE) {
                continue;
            }
            pe_rsc_trace(rsc, "Allocating remote connection resource '%s'",
                         rsc->id);
            rsc->cmds->allocate(rsc, rsc->partial_migration_target, data_set);
        }
    }

    /* now do the rest of the resources */
    for (gIter = data_set->resources; gIter != NULL; gIter = gIter->next) {
        pe_resource_t *rsc = (pe_resource_t *) gIter->data;
        if (rsc->is_remote_node == TRUE) {
            continue;
        }
        pe_rsc_trace(rsc, "Allocating %s resource '%s'",
                     crm_element_name(rsc->xml), rsc->id);
        rsc->cmds->allocate(rsc, NULL, data_set);
    }
}

// Clear fail counts for orphaned rsc on all online nodes
static void
cleanup_orphans(pe_resource_t * rsc, pe_working_set_t * data_set)
{
    GList *gIter = NULL;

    for (gIter = data_set->nodes; gIter != NULL; gIter = gIter->next) {
        pe_node_t *node = (pe_node_t *) gIter->data;

        if (node->details->online
            && pe_get_failcount(node, rsc, NULL, pe_fc_effective, NULL,
                                data_set)) {

            pe_action_t *clear_op = NULL;

            clear_op = pe__clear_failcount(rsc, node, "it is orphaned",
                                           data_set);

            /* We can't use order_action_then_stop() here because its
             * pe_order_preserve breaks things
             */
            pcmk__new_ordering(clear_op->rsc, NULL, clear_op,
                               rsc, stop_key(rsc), NULL,
                               pe_order_optional, data_set);
        }
    }
}

gboolean
stage5(pe_working_set_t * data_set)
{
    pcmk__output_t *out = data_set->priv;
    GList *gIter = NULL;

    if (!pcmk__str_eq(data_set->placement_strategy, "default", pcmk__str_casei)) {
        pcmk__sort_resources(data_set);
    }

    gIter = data_set->nodes;
    for (; gIter != NULL; gIter = gIter->next) {
        pe_node_t *node = (pe_node_t *) gIter->data;

        if (pcmk_is_set(data_set->flags, pe_flag_show_utilization)) {
            out->message(out, "node-capacity", node, "Original");
        }
    }

    crm_trace("Allocating services");
    /* Take (next) highest resource, assign it and create its actions */

    allocate_resources(data_set);

    gIter = data_set->nodes;
    for (; gIter != NULL; gIter = gIter->next) {
        pe_node_t *node = (pe_node_t *) gIter->data;

        if (pcmk_is_set(data_set->flags, pe_flag_show_utilization)) {
            out->message(out, "node-capacity", node, "Remaining");
        }
    }

    // Process deferred action checks
    pe__foreach_param_check(data_set, check_params);
    pe__free_param_checks(data_set);

    if (pcmk_is_set(data_set->flags, pe_flag_startup_probes)) {
        crm_trace("Calculating needed probes");
        pcmk__schedule_probes(data_set);
    }

    crm_trace("Handle orphans");
    if (pcmk_is_set(data_set->flags, pe_flag_stop_rsc_orphans)) {
        for (gIter = data_set->resources; gIter != NULL; gIter = gIter->next) {
            pe_resource_t *rsc = (pe_resource_t *) gIter->data;

            /* There's no need to recurse into rsc->children because those
             * should just be unallocated clone instances.
             */
            if (pcmk_is_set(rsc->flags, pe_rsc_orphan)) {
                cleanup_orphans(rsc, data_set);
            }
        }
    }

    crm_trace("Creating actions");

    for (gIter = data_set->resources; gIter != NULL; gIter = gIter->next) {
        pe_resource_t *rsc = (pe_resource_t *) gIter->data;

        rsc->cmds->create_actions(rsc, data_set);
    }

    crm_trace("Creating done");
    return TRUE;
}

static gboolean
is_managed(const pe_resource_t * rsc)
{
    GList *gIter = rsc->children;

    if (pcmk_is_set(rsc->flags, pe_rsc_managed)) {
        return TRUE;
    }

    for (; gIter != NULL; gIter = gIter->next) {
        pe_resource_t *child_rsc = (pe_resource_t *) gIter->data;

        if (is_managed(child_rsc)) {
            return TRUE;
        }
    }

    return FALSE;
}

static gboolean
any_managed_resources(pe_working_set_t * data_set)
{

    GList *gIter = data_set->resources;

    for (; gIter != NULL; gIter = gIter->next) {
        pe_resource_t *rsc = (pe_resource_t *) gIter->data;

        if (is_managed(rsc)) {
            return TRUE;
        }
    }
    return FALSE;
}

/*
 * Create dependencies for stonith and shutdown operations
 */
gboolean
stage6(pe_working_set_t * data_set)
{
    pe_action_t *dc_down = NULL;
    pe_action_t *stonith_op = NULL;
    gboolean integrity_lost = FALSE;
    gboolean need_stonith = TRUE;
    GList *gIter;
    GList *stonith_ops = NULL;
    GList *shutdown_ops = NULL;

    /* Remote ordering constraints need to happen prior to calculating fencing
     * because it is one more place we can mark nodes as needing fencing.
     */
    pcmk__order_remote_connection_actions(data_set);

    crm_trace("Processing fencing and shutdown cases");
    if (any_managed_resources(data_set) == FALSE) {
        crm_notice("Delaying fencing operations until there are resources to manage");
        need_stonith = FALSE;
    }

    /* Check each node for stonith/shutdown */
    for (gIter = data_set->nodes; gIter != NULL; gIter = gIter->next) {
        pe_node_t *node = (pe_node_t *) gIter->data;

        /* Guest nodes are "fenced" by recovering their container resource,
         * so handle them separately.
         */
        if (pe__is_guest_node(node)) {
            if (node->details->remote_requires_reset && need_stonith
                && pe_can_fence(data_set, node)) {
                pcmk__fence_guest(node, data_set);
            }
            continue;
        }

        stonith_op = NULL;

        if (node->details->unclean
            && need_stonith && pe_can_fence(data_set, node)) {

            stonith_op = pe_fence_op(node, NULL, FALSE, "node is unclean", FALSE, data_set);
            pe_warn("Scheduling Node %s for STONITH", node->details->uname);

            pcmk__order_vs_fence(stonith_op, data_set);

            if (node->details->is_dc) {
                // Remember if the DC is being fenced
                dc_down = stonith_op;

            } else {

                if (!pcmk_is_set(data_set->flags, pe_flag_concurrent_fencing)
                    && (stonith_ops != NULL)) {
                    /* Concurrent fencing is disabled, so order each non-DC
                     * fencing in a chain. If there is any DC fencing or
                     * shutdown, it will be ordered after the last action in the
                     * chain later.
                     */
                    order_actions((pe_action_t *) stonith_ops->data,
                                  stonith_op, pe_order_optional);
                }

                // Remember all non-DC fencing actions in a separate list
                stonith_ops = g_list_prepend(stonith_ops, stonith_op);
            }

        } else if (node->details->online && node->details->shutdown &&
                /* TODO define what a shutdown op means for a remote node.
                 * For now we do not send shutdown operations for remote nodes, but
                 * if we can come up with a good use for this in the future, we will. */
                    pe__is_guest_or_remote_node(node) == FALSE) {

            pe_action_t *down_op = pcmk__new_shutdown_action(node, data_set);

            if (node->details->is_dc) {
                // Remember if the DC is being shut down
                dc_down = down_op;
            } else {
                // Remember non-DC shutdowns for later ordering
                shutdown_ops = g_list_prepend(shutdown_ops, down_op);
            }
        }

        if (node->details->unclean && stonith_op == NULL) {
            integrity_lost = TRUE;
            pe_warn("Node %s is unclean!", node->details->uname);
        }
    }

    if (integrity_lost) {
        if (!pcmk_is_set(data_set->flags, pe_flag_stonith_enabled)) {
            pe_warn("YOUR RESOURCES ARE NOW LIKELY COMPROMISED");
            pe_err("ENABLE STONITH TO KEEP YOUR RESOURCES SAFE");

        } else if (!pcmk_is_set(data_set->flags, pe_flag_have_quorum)) {
            crm_notice("Cannot fence unclean nodes until quorum is"
                       " attained (or no-quorum-policy is set to ignore)");
        }
    }

    if (dc_down != NULL) {
        /* Order any non-DC shutdowns before any DC shutdown, to avoid repeated
         * DC elections. However, we don't want to order non-DC shutdowns before
         * a DC *fencing*, because even though we don't want a node that's
         * shutting down to become DC, the DC fencing could be ordered before a
         * clone stop that's also ordered before the shutdowns, thus leading to
         * a graph loop.
         */
        if (pcmk__str_eq(dc_down->task, CRM_OP_SHUTDOWN, pcmk__str_casei)) {
            for (gIter = shutdown_ops; gIter != NULL; gIter = gIter->next) {
                pe_action_t *node_stop = (pe_action_t *) gIter->data;

                crm_debug("Ordering shutdown on %s before %s on DC %s",
                          node_stop->node->details->uname,
                          dc_down->task, dc_down->node->details->uname);

                order_actions(node_stop, dc_down, pe_order_optional);
            }
        }

        // Order any non-DC fencing before any DC fencing or shutdown

        if (pcmk_is_set(data_set->flags, pe_flag_concurrent_fencing)) {
            /* With concurrent fencing, order each non-DC fencing action
             * separately before any DC fencing or shutdown.
             */
            for (gIter = stonith_ops; gIter != NULL; gIter = gIter->next) {
                order_actions((pe_action_t *) gIter->data, dc_down,
                              pe_order_optional);
            }
        } else if (stonith_ops) {
            /* Without concurrent fencing, the non-DC fencing actions are
             * already ordered relative to each other, so we just need to order
             * the DC fencing after the last action in the chain (which is the
             * first item in the list).
             */
            order_actions((pe_action_t *) stonith_ops->data, dc_down,
                          pe_order_optional);
        }
    }
    g_list_free(stonith_ops);
    g_list_free(shutdown_ops);
    return TRUE;
}
