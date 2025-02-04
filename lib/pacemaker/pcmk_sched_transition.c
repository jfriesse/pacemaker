/*
 * Copyright 2009-2021 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

#include <sys/stat.h>
#include <sys/param.h>
#include <sys/types.h>
#include <dirent.h>

#include <crm/crm.h>
#include <crm/lrmd.h>           // lrmd_event_data_t, lrmd_free_event()
#include <crm/cib.h>
#include <crm/common/util.h>
#include <crm/common/iso8601.h>
#include <crm/common/xml_internal.h>
#include <crm/lrmd_internal.h>
#include <crm/pengine/status.h>
#include <pacemaker-internal.h>

#include "libpacemaker_private.h"

gboolean bringing_nodes_online = FALSE;

#define STATUS_PATH_MAX 512

#define NEW_NODE_TEMPLATE "//"XML_CIB_TAG_NODE"[@uname='%s']"
#define NODE_TEMPLATE "//"XML_CIB_TAG_STATE"[@uname='%s']"
#define RSC_TEMPLATE "//"XML_CIB_TAG_STATE"[@uname='%s']//"XML_LRM_TAG_RESOURCE"[@id='%s']"


static void
inject_transient_attr(pcmk__output_t *out, xmlNode *cib_node,
                      const char *name, const char *value)
{
    xmlNode *attrs = NULL;
    xmlNode *instance_attrs = NULL;
    const char *node_uuid = ID(cib_node);

    out->message(out, "inject-attr", name, value, cib_node);

    attrs = first_named_child(cib_node, XML_TAG_TRANSIENT_NODEATTRS);
    if (attrs == NULL) {
        attrs = create_xml_node(cib_node, XML_TAG_TRANSIENT_NODEATTRS);
        crm_xml_add(attrs, XML_ATTR_ID, node_uuid);
    }

    instance_attrs = first_named_child(attrs, XML_TAG_ATTR_SETS);
    if (instance_attrs == NULL) {
        instance_attrs = create_xml_node(attrs, XML_TAG_ATTR_SETS);
        crm_xml_add(instance_attrs, XML_ATTR_ID, node_uuid);
    }

    crm_create_nvpair_xml(instance_attrs, NULL, name, value);
}

/*!
 * \internal
 * \brief Inject a fictitious fail count into a scheduler input
 *
 * \param[in] out          Output object for displaying error messages
 * \param[in] cib_node     Node state XML to inject into
 * \param[in] resource     ID of resource for fail count to inject
 * \param[in] task         Action name for fail count to inject
 * \param[in] interval_ms  Action interval (in milliseconds) for fail count
 * \param[in] rc           Action result for fail count to inject (if 0, or 7
 *                         when interval_ms is 0, nothing will be injected)
 */
void
pcmk__inject_failcount(pcmk__output_t *out, xmlNode *cib_node,
                       const char *resource, const char *task,
                       guint interval_ms, int rc)
{
    if (rc == 0) {
        return;

    } else if ((rc == 7) && (interval_ms == 0)) {
        return;

    } else {
        char *name = NULL;
        char *now = pcmk__ttoa(time(NULL));

        name = pcmk__failcount_name(resource, task, interval_ms);
        inject_transient_attr(out, cib_node, name, "value++");
        free(name);

        name = pcmk__lastfailure_name(resource, task, interval_ms);
        inject_transient_attr(out, cib_node, name, now);
        free(name);

        free(now);
    }
}

static void
create_node_entry(cib_t * cib_conn, const char *node)
{
    int rc = pcmk_ok;
    char *xpath = crm_strdup_printf(NEW_NODE_TEMPLATE, node);

    rc = cib_conn->cmds->query(cib_conn, xpath, NULL, cib_xpath | cib_sync_call | cib_scope_local);

    if (rc == -ENXIO) {
        xmlNode *cib_object = create_xml_node(NULL, XML_CIB_TAG_NODE);

        crm_xml_add(cib_object, XML_ATTR_ID, node); // Use node name as ID
        crm_xml_add(cib_object, XML_ATTR_UNAME, node);
        cib_conn->cmds->create(cib_conn, XML_CIB_TAG_NODES, cib_object,
                               cib_sync_call | cib_scope_local);
        /* Not bothering with subsequent query to see if it exists,
           we'll bomb out later in the call to query_node_uuid()... */

        free_xml(cib_object);
    }

    free(xpath);
}

static lrmd_event_data_t *
create_op(xmlNode *cib_resource, const char *task, guint interval_ms,
          int outcome)
{
    lrmd_event_data_t *op = NULL;
    xmlNode *xop = NULL;

    op = lrmd_new_event(ID(cib_resource), task, interval_ms);
    lrmd__set_result(op, outcome, PCMK_EXEC_DONE, "Simulated action result");
    op->params = NULL;          /* TODO: Fill me in */
    op->t_run = (unsigned int) time(NULL);
    op->t_rcchange = op->t_run;

    op->call_id = 0;
    for (xop = pcmk__xe_first_child(cib_resource); xop != NULL;
         xop = pcmk__xe_next(xop)) {

        int tmp = 0;

        crm_element_value_int(xop, XML_LRM_ATTR_CALLID, &tmp);
        if (tmp > op->call_id) {
            op->call_id = tmp;
        }
    }
    op->call_id++;

    return op;
}

/*!
 * \internal
 * \brief Inject a fictitious resource history entry into a scheduler input
 *
 * \param[in] cib_resource  Resource history XML to inject entry into
 * \param[in] op            Action result to inject
 * \param[in] target_rc     Expected result for action to inject
 *
 * \return XML of injected resource history entry
 */
xmlNode *
pcmk__inject_action_result(xmlNode *cib_resource, lrmd_event_data_t *op,
                           int target_rc)
{
    return pcmk__create_history_xml(cib_resource, op, CRM_FEATURE_SET,
                                    target_rc, NULL, crm_system_name,
                                    LOG_TRACE);
}

/*!
 * \internal
 * \brief Inject a fictitious node into a scheduler input
 *
 * \param[in] cib_conn  Scheduler input CIB to inject node into
 * \param[in] node      Name of node to inject
 * \param[in] uuid      UUID of node to inject
 *
 * \return XML of node_state entry for new node
 * \note If the global bringing_nodes_online has been set to true, a node entry
 *       in the configuration section will be added, as well as a node state
 *       entry in the status section.
 */
xmlNode *
pcmk__inject_node(cib_t *cib_conn, const char *node, const char *uuid)
{
    int rc = pcmk_ok;
    xmlNode *cib_object = NULL;
    char *xpath = crm_strdup_printf(NODE_TEMPLATE, node);

    if (bringing_nodes_online) {
        create_node_entry(cib_conn, node);
    }

    rc = cib_conn->cmds->query(cib_conn, xpath, &cib_object,
                               cib_xpath | cib_sync_call | cib_scope_local);

    if ((cib_object != NULL) && (ID(cib_object) == NULL)) {
        crm_err("Detected multiple node_state entries for xpath=%s, bailing",
                xpath);
        crm_log_xml_warn(cib_object, "Duplicates");
        free(xpath);
        crm_exit(CRM_EX_SOFTWARE);
        return NULL; // not reached, but makes static analysis happy
    }

    if (rc == -ENXIO) {
        char *found_uuid = NULL;

        if (uuid == NULL) {
            query_node_uuid(cib_conn, node, &found_uuid, NULL);
        } else {
            found_uuid = strdup(uuid);
        }

        cib_object = create_xml_node(NULL, XML_CIB_TAG_STATE);
        crm_xml_add(cib_object, XML_ATTR_UUID, found_uuid);
        crm_xml_add(cib_object, XML_ATTR_UNAME, node);
        cib_conn->cmds->create(cib_conn, XML_CIB_TAG_STATUS, cib_object,
                               cib_sync_call | cib_scope_local);
        free_xml(cib_object);
        free(found_uuid);

        rc = cib_conn->cmds->query(cib_conn, xpath, &cib_object,
                                   cib_xpath | cib_sync_call | cib_scope_local);
        crm_trace("Injecting node state for %s (rc=%d)", node, rc);
    }

    free(xpath);
    CRM_ASSERT(rc == pcmk_ok);
    return cib_object;
}

/*!
 * \internal
 * \brief Inject a fictitious node state change into a scheduler input
 *
 * \param[in] cib_conn  Scheduler input CIB to inject into
 * \param[in] node      Name of node to inject change for
 * \param[in] up        If true, change state to online, otherwise offline
 *
 * \return XML of changed (or added) node state entry
 */
xmlNode *
pcmk__inject_node_state_change(cib_t *cib_conn, const char *node, bool up)
{
    xmlNode *cib_node = pcmk__inject_node(cib_conn, node, NULL);

    if (up) {
        pcmk__xe_set_props(cib_node,
                           XML_NODE_IN_CLUSTER, XML_BOOLEAN_YES,
                           XML_NODE_IS_PEER, ONLINESTATUS,
                           XML_NODE_JOIN_STATE, CRMD_JOINSTATE_MEMBER,
                           XML_NODE_EXPECTED, CRMD_JOINSTATE_MEMBER,
                           NULL);
    } else {
        pcmk__xe_set_props(cib_node,
                           XML_NODE_IN_CLUSTER, XML_BOOLEAN_NO,
                           XML_NODE_IS_PEER, OFFLINESTATUS,
                           XML_NODE_JOIN_STATE, CRMD_JOINSTATE_DOWN,
                           XML_NODE_EXPECTED, CRMD_JOINSTATE_DOWN,
                           NULL);
    }
    crm_xml_add(cib_node, XML_ATTR_ORIGIN, crm_system_name);
    return cib_node;
}

static xmlNode *
find_resource_xml(xmlNode * cib_node, const char *resource)
{
    xmlNode *match = NULL;
    const char *node = crm_element_value(cib_node, XML_ATTR_UNAME);
    char *xpath = crm_strdup_printf(RSC_TEMPLATE, node, resource);

    match = get_xpath_object(xpath, cib_node, LOG_TRACE);
    free(xpath);
    return match;
}

/*!
 * \internal
 * \brief Inject a resource history element into a scheduler input
 *
 * \param[in] out       Output object for displaying error messages
 * \param[in] cib_node  Node state XML to inject resource history entry into
 * \param[in] resource  ID (in configuration) of resource to inject
 * \param[in] lrm_name  ID of resource as used in history (e.g. clone instance)
 * \param[in] rclass    Resource agent class of resource to inject
 * \param[in] rtype     Resource agent type of resource to inject
 * \param[in] rprovider Resource agent provider of resource to inject
 *
 * \return XML of injected resource history element
 * \note If a history element already exists under either \p resource or
 *       \p lrm_name, this will return it rather than injecting a new one.
 */
xmlNode *
pcmk__inject_resource_history(pcmk__output_t *out, xmlNode *cib_node,
                              const char *resource, const char *lrm_name,
                              const char *rclass, const char *rtype,
                              const char *rprovider)
{
    xmlNode *lrm = NULL;
    xmlNode *container = NULL;
    xmlNode *cib_resource = NULL;

    cib_resource = find_resource_xml(cib_node, resource);
    if (cib_resource != NULL) {
        /* If an existing LRM history entry uses the resource name,
         * continue using it, even if lrm_name is different.
         */
        return cib_resource;
    }

    // Check for history entry under preferred name
    if (strcmp(resource, lrm_name) != 0) {
        cib_resource = find_resource_xml(cib_node, lrm_name);
        if (cib_resource != NULL) {
            return cib_resource;
        }
    }

    if ((rclass == NULL) || (rtype == NULL)) {
        // @TODO query configuration for class, provider, type
        out->err(out, "Resource %s not found in the status section of %s."
                 "  Please supply the class and type to continue", resource, ID(cib_node));
        return NULL;

    } else if (!pcmk__strcase_any_of(rclass,
                                     PCMK_RESOURCE_CLASS_OCF,
                                     PCMK_RESOURCE_CLASS_STONITH,
                                     PCMK_RESOURCE_CLASS_SERVICE,
                                     PCMK_RESOURCE_CLASS_UPSTART,
                                     PCMK_RESOURCE_CLASS_SYSTEMD,
                                     PCMK_RESOURCE_CLASS_LSB, NULL)) {
        out->err(out, "Invalid class for %s: %s", resource, rclass);
        return NULL;

    } else if (pcmk_is_set(pcmk_get_ra_caps(rclass), pcmk_ra_cap_provider)
               && (rprovider == NULL)) {
        // @TODO query configuration for provider
        out->err(out, "Please specify the provider for resource %s", resource);
        return NULL;
    }

    crm_info("Injecting new resource %s into node state '%s'",
             lrm_name, ID(cib_node));

    lrm = first_named_child(cib_node, XML_CIB_TAG_LRM);
    if (lrm == NULL) {
        const char *node_uuid = ID(cib_node);

        lrm = create_xml_node(cib_node, XML_CIB_TAG_LRM);
        crm_xml_add(lrm, XML_ATTR_ID, node_uuid);
    }

    container = first_named_child(lrm, XML_LRM_TAG_RESOURCES);
    if (container == NULL) {
        container = create_xml_node(lrm, XML_LRM_TAG_RESOURCES);
    }

    cib_resource = create_xml_node(container, XML_LRM_TAG_RESOURCE);

    // If we're creating a new entry, use the preferred name
    crm_xml_add(cib_resource, XML_ATTR_ID, lrm_name);

    crm_xml_add(cib_resource, XML_AGENT_ATTR_CLASS, rclass);
    crm_xml_add(cib_resource, XML_AGENT_ATTR_PROVIDER, rprovider);
    crm_xml_add(cib_resource, XML_ATTR_TYPE, rtype);

    return cib_resource;
}

#define XPATH_MAX 1024

static int
find_ticket_state(pcmk__output_t *out, cib_t *the_cib, const char *ticket_id,
                  xmlNode **ticket_state_xml)
{
    int offset = 0;
    int rc = pcmk_ok;
    xmlNode *xml_search = NULL;

    char *xpath_string = NULL;

    CRM_ASSERT(ticket_state_xml != NULL);
    *ticket_state_xml = NULL;

    xpath_string = calloc(1, XPATH_MAX);
    offset += snprintf(xpath_string + offset, XPATH_MAX - offset, "%s", "/cib/status/tickets");

    if (ticket_id) {
        offset += snprintf(xpath_string + offset, XPATH_MAX - offset, "/%s[@id=\"%s\"]",
                           XML_CIB_TAG_TICKET_STATE, ticket_id);
    }
    CRM_LOG_ASSERT(offset > 0);
    rc = the_cib->cmds->query(the_cib, xpath_string, &xml_search,
                              cib_sync_call | cib_scope_local | cib_xpath);

    if (rc != pcmk_ok) {
        goto bail;
    }

    crm_log_xml_debug(xml_search, "Match");
    if (xml_has_children(xml_search)) {
        if (ticket_id) {
            out->err(out, "Multiple ticket_states match ticket_id=%s", ticket_id);
        }
        *ticket_state_xml = xml_search;
    } else {
        *ticket_state_xml = xml_search;
    }

  bail:
    free(xpath_string);
    return rc;
}

static int
set_ticket_state_attr(pcmk__output_t *out, const char *ticket_id,
                      const char *attr_name, const char *attr_value,
                      cib_t *cib, int cib_options)
{
    int rc = pcmk_ok;
    xmlNode *xml_top = NULL;
    xmlNode *ticket_state_xml = NULL;

    rc = find_ticket_state(out, cib, ticket_id, &ticket_state_xml);
    if (rc == pcmk_ok) {
        crm_debug("Found a match state for ticket: id=%s", ticket_id);
        xml_top = ticket_state_xml;

    } else if (rc != -ENXIO) {
        return rc;

    } else {
        xmlNode *xml_obj = NULL;

        xml_top = create_xml_node(NULL, XML_CIB_TAG_STATUS);
        xml_obj = create_xml_node(xml_top, XML_CIB_TAG_TICKETS);
        ticket_state_xml = create_xml_node(xml_obj, XML_CIB_TAG_TICKET_STATE);
        crm_xml_add(ticket_state_xml, XML_ATTR_ID, ticket_id);
    }

    crm_xml_add(ticket_state_xml, attr_name, attr_value);

    crm_log_xml_debug(xml_top, "Update");

    rc = cib->cmds->modify(cib, XML_CIB_TAG_STATUS, xml_top, cib_options);

    free_xml(xml_top);

    return rc;
}

void
modify_configuration(pe_working_set_t * data_set, cib_t *cib, pcmk_injections_t *injections)
{
    int rc = pcmk_ok;
    GList *gIter = NULL;

    xmlNode *cib_op = NULL;
    xmlNode *cib_node = NULL;
    xmlNode *cib_resource = NULL;

    lrmd_event_data_t *op = NULL;

    pcmk__output_t *out = data_set->priv;

    out->message(out, "inject-modify-config", injections->quorum, injections->watchdog);

    if (injections->quorum) {
        xmlNode *top = create_xml_node(NULL, XML_TAG_CIB);

        /* crm_xml_add(top, XML_ATTR_DC_UUID, dc_uuid);      */
        crm_xml_add(top, XML_ATTR_HAVE_QUORUM, injections->quorum);

        rc = cib->cmds->modify(cib, NULL, top, cib_sync_call | cib_scope_local);
        CRM_ASSERT(rc == pcmk_ok);
    }

    if (injections->watchdog) {
        rc = update_attr_delegate(cib, cib_sync_call | cib_scope_local,
                             XML_CIB_TAG_CRMCONFIG, NULL, NULL, NULL, NULL,
                             XML_ATTR_HAVE_WATCHDOG, injections->watchdog, FALSE, NULL, NULL);

        CRM_ASSERT(rc == pcmk_ok);
    }

    for (gIter = injections->node_up; gIter != NULL; gIter = gIter->next) {
        char *node = (char *)gIter->data;

        out->message(out, "inject-modify-node", "Online", node);

        cib_node = pcmk__inject_node_state_change(cib, node, true);
        CRM_ASSERT(cib_node != NULL);

        rc = cib->cmds->modify(cib, XML_CIB_TAG_STATUS, cib_node,
                                      cib_sync_call | cib_scope_local);
        CRM_ASSERT(rc == pcmk_ok);
        free_xml(cib_node);
    }

    for (gIter = injections->node_down; gIter != NULL; gIter = gIter->next) {
        char xpath[STATUS_PATH_MAX];
        char *node = (char *)gIter->data;

        out->message(out, "inject-modify-node", "Offline", node);

        cib_node = pcmk__inject_node_state_change(cib, node, false);
        CRM_ASSERT(cib_node != NULL);

        rc = cib->cmds->modify(cib, XML_CIB_TAG_STATUS, cib_node,
                                      cib_sync_call | cib_scope_local);
        CRM_ASSERT(rc == pcmk_ok);
        free_xml(cib_node);

        snprintf(xpath, STATUS_PATH_MAX, "//node_state[@uname='%s']/%s", node, XML_CIB_TAG_LRM);
        cib->cmds->remove(cib, xpath, NULL,
                                      cib_xpath | cib_sync_call | cib_scope_local);

        snprintf(xpath, STATUS_PATH_MAX, "//node_state[@uname='%s']/%s", node,
                 XML_TAG_TRANSIENT_NODEATTRS);
        cib->cmds->remove(cib, xpath, NULL,
                                      cib_xpath | cib_sync_call | cib_scope_local);

    }

    for (gIter = injections->node_fail; gIter != NULL; gIter = gIter->next) {
        char *node = (char *)gIter->data;

        out->message(out, "inject-modify-node", "Failing", node);

        cib_node = pcmk__inject_node_state_change(cib, node, true);
        crm_xml_add(cib_node, XML_NODE_IN_CLUSTER, XML_BOOLEAN_NO);
        CRM_ASSERT(cib_node != NULL);

        rc = cib->cmds->modify(cib, XML_CIB_TAG_STATUS, cib_node,
                                      cib_sync_call | cib_scope_local);
        CRM_ASSERT(rc == pcmk_ok);
        free_xml(cib_node);
    }

    for (gIter = injections->ticket_grant; gIter != NULL; gIter = gIter->next) {
        char *ticket_id = (char *)gIter->data;

        out->message(out, "inject-modify-ticket", "Granting", ticket_id);

        rc = set_ticket_state_attr(out, ticket_id, "granted", "true",
                                   cib, cib_sync_call | cib_scope_local);

        CRM_ASSERT(rc == pcmk_ok);
    }

    for (gIter = injections->ticket_revoke; gIter != NULL; gIter = gIter->next) {
        char *ticket_id = (char *)gIter->data;

        out->message(out, "inject-modify-ticket", "Revoking", ticket_id);

        rc = set_ticket_state_attr(out, ticket_id, "granted", "false",
                                   cib, cib_sync_call | cib_scope_local);

        CRM_ASSERT(rc == pcmk_ok);
    }

    for (gIter = injections->ticket_standby; gIter != NULL; gIter = gIter->next) {
        char *ticket_id = (char *)gIter->data;

        out->message(out, "inject-modify-ticket", "Standby", ticket_id);

        rc = set_ticket_state_attr(out, ticket_id, "standby", "true",
                                   cib, cib_sync_call | cib_scope_local);

        CRM_ASSERT(rc == pcmk_ok);
    }

    for (gIter = injections->ticket_activate; gIter != NULL; gIter = gIter->next) {
        char *ticket_id = (char *)gIter->data;

        out->message(out, "inject-modify-ticket", "Activating", ticket_id);

        rc = set_ticket_state_attr(out, ticket_id, "standby", "false",
                                   cib, cib_sync_call | cib_scope_local);

        CRM_ASSERT(rc == pcmk_ok);
    }

    for (gIter = injections->op_inject; gIter != NULL; gIter = gIter->next) {
        char *spec = (char *)gIter->data;

        int rc = 0;
        int outcome = PCMK_OCF_OK;
        guint interval_ms = 0;

        char *key = NULL;
        char *node = NULL;
        char *task = NULL;
        char *resource = NULL;

        const char *rtype = NULL;
        const char *rclass = NULL;
        const char *rprovider = NULL;

        pe_resource_t *rsc = NULL;

        out->message(out, "inject-spec", spec);

        key = calloc(1, strlen(spec) + 1);
        node = calloc(1, strlen(spec) + 1);
        rc = sscanf(spec, "%[^@]@%[^=]=%d", key, node, &outcome);
        if (rc != 3) {
            out->err(out, "Invalid operation spec: %s.  Only found %d fields", spec, rc);
            free(key);
            free(node);
            continue;
        }

        parse_op_key(key, &resource, &task, &interval_ms);

        rsc = pe_find_resource(data_set->resources, resource);
        if (rsc == NULL) {
            out->err(out, "Invalid resource name: %s", resource);
        } else {
            rclass = crm_element_value(rsc->xml, XML_AGENT_ATTR_CLASS);
            rtype = crm_element_value(rsc->xml, XML_ATTR_TYPE);
            rprovider = crm_element_value(rsc->xml, XML_AGENT_ATTR_PROVIDER);

            cib_node = pcmk__inject_node(cib, node, NULL);
            CRM_ASSERT(cib_node != NULL);

            pcmk__inject_failcount(out, cib_node, resource, task, interval_ms,
                                   outcome);

            cib_resource = pcmk__inject_resource_history(out, cib_node,
                                                         resource, resource,
                                                         rclass, rtype,
                                                         rprovider);
            CRM_ASSERT(cib_resource != NULL);

            op = create_op(cib_resource, task, interval_ms, outcome);
            CRM_ASSERT(op != NULL);

            cib_op = pcmk__inject_action_result(cib_resource, op, 0);
            CRM_ASSERT(cib_op != NULL);
            lrmd_free_event(op);

            rc = cib->cmds->modify(cib, XML_CIB_TAG_STATUS, cib_node,
                                          cib_sync_call | cib_scope_local);
            CRM_ASSERT(rc == pcmk_ok);
        }
        free(task);
        free(node);
        free(key);
    }

    if (!out->is_quiet(out)) {
        out->end_list(out);
    }
}
