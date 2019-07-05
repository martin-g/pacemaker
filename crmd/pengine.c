/* 
 * Copyright 2004-2019 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <crm_internal.h>

#include <unistd.h>  /* pid_t, sleep, ssize_t */

#include <crm/cib.h>
#include <crm/cluster.h>
#include <crm/common/xml.h>
#include <crm/crm.h>
#include <crm/msg_xml.h>

#include <crmd.h>
#include <crmd_fsa.h>
#include <crmd_messages.h>  /* register_fsa_error_adv */


struct crm_subsystem_s *pe_subsystem = NULL;
void do_pe_invoke_callback(xmlNode * msg, int call_id, int rc, xmlNode * output, void *user_data);

static void
save_cib_contents(xmlNode * msg, int call_id, int rc, xmlNode * output, void *user_data)
{
    char *id = user_data;

    register_fsa_error_adv(C_FSA_INTERNAL, I_ERROR, NULL, NULL, __FUNCTION__);
    CRM_CHECK(id != NULL, return);

    if (rc == pcmk_ok) {
        int len = 15;
        char *filename = NULL;

        len += strlen(id);
        len += strlen(PE_STATE_DIR);

        filename = calloc(1, len);
        CRM_CHECK(filename != NULL, return);

        sprintf(filename, PE_STATE_DIR "/pe-core-%s.bz2", id);
        if (write_xml_file(output, filename, TRUE) < 0) {
            crm_err("Could not save Cluster Information Base to %s after Policy Engine crash",
                    filename);
        } else {
            crm_notice("Saved Cluster Information Base to %s after Policy Engine crash",
                       filename);
        }

        free(filename);
    }
}

static void
pe_ipc_destroy(gpointer user_data)
{
    if (is_set(fsa_input_register, pe_subsystem->flag_required)) {
        int rc = pcmk_ok;
        char *uuid_str = crm_generate_uuid();

        crm_crit("Connection to the Policy Engine failed "
                 CRM_XS " pid=%d uuid=%s", pe_subsystem->pid, uuid_str);

        /*
         * The PE died...
         *
         * Save the current CIB so that we have a chance of
         * figuring out what killed it.
         *
         * Delay raising the I_ERROR until the query below completes or
         * 5s is up, whichever comes first.
         *
         */
        rc = fsa_cib_conn->cmds->query(fsa_cib_conn, NULL, NULL, cib_scope_local);
        fsa_register_cib_callback(rc, FALSE, uuid_str, save_cib_contents);

    } else {
        if (is_heartbeat_cluster()) {
            stop_subsystem(pe_subsystem, FALSE);
        }
        crm_info("Connection to the Policy Engine released");
    }

    // If we aren't connected to the scheduler, we can't expect a reply
    controld_expect_sched_reply(NULL);

    clear_bit(fsa_input_register, pe_subsystem->flag_connected);
    pe_subsystem->pid = -1;
    pe_subsystem->source = NULL;
    pe_subsystem->client = NULL;

    mainloop_set_trigger(fsa_source);
    return;
}

static int
pe_ipc_dispatch(const char *buffer, ssize_t length, gpointer userdata)
{
    xmlNode *msg = string2xml(buffer);

    if (msg) {
        route_message(C_IPC_MESSAGE, msg);
    }

    free_xml(msg);
    return 0;
}

/*	 A_PE_START, A_PE_STOP, A_TE_RESTART	*/
void
do_pe_control(long long action,
              enum crmd_fsa_cause cause,
              enum crmd_fsa_state cur_state,
              enum crmd_fsa_input current_input, fsa_data_t * msg_data)
{
    struct crm_subsystem_s *this_subsys = pe_subsystem;

    long long stop_actions = A_PE_STOP;
    long long start_actions = A_PE_START;

    static struct ipc_client_callbacks pe_callbacks = {
        .dispatch = pe_ipc_dispatch,
        .destroy = pe_ipc_destroy
    };

    if (action & stop_actions) {
        // If we aren't connected to the scheduler, we can't expect a reply
        controld_expect_sched_reply(NULL);

        clear_bit(fsa_input_register, pe_subsystem->flag_required);

        mainloop_del_ipc_client(pe_subsystem->source);
        pe_subsystem->source = NULL;

        clear_bit(fsa_input_register, pe_subsystem->flag_connected);
    }

    if ((action & start_actions) && (is_set(fsa_input_register, R_PE_CONNECTED) == FALSE)) {
        if (cur_state != S_STOPPING) {
            set_bit(fsa_input_register, pe_subsystem->flag_required);

            pe_subsystem->source =
                mainloop_add_ipc_client(CRM_SYSTEM_PENGINE, G_PRIORITY_DEFAULT,
                                        5 * 1024 * 1024 /* 5MB */ , NULL, &pe_callbacks);

            if (pe_subsystem->source == NULL) {
                crm_warn("Setup of client connection failed, not adding channel to mainloop");
                register_fsa_error(C_FSA_INTERNAL, I_FAIL, NULL);
                return;
            }

            /* if (is_openais_cluster()) { */
            /*     pe_subsystem->pid = pe_subsystem->ipc->farside_pid; */
            /* } */

            set_bit(fsa_input_register, pe_subsystem->flag_connected);

        } else {
            crm_info("Ignoring request to start %s while shutting down", this_subsys->name);
        }
    }
}

int fsa_pe_query = 0;
char *fsa_pe_ref = NULL;
static mainloop_timer_t *controld_sched_timer = NULL;

// @TODO Make this a configurable cluster option if there's demand for it
#define SCHED_TIMEOUT_MS (120000)

/*!
 * \internal
 * \brief Handle a timeout waiting for scheduler reply
 *
 * \param[in] user_data  Ignored
 *
 * \return FALSE (indicating that timer should not be restarted)
 */
static gboolean
controld_sched_timeout(gpointer user_data)
{
    if (AM_I_DC) {
        /* If this node is the DC but can't communicate with the scheduler, just
         * exit (and likely get fenced) so this node doesn't interfere with any
         * further DC elections.
         *
         * @TODO We could try something less drastic first, like disconnecting
         * and reconnecting to the scheduler, but something is likely going
         * seriously wrong, so perhaps it's better to just fail as quickly as
         * possible.
         */
        crmd_exit(DAEMON_RESPAWN_STOP);
    }
    return FALSE;
}

void
controld_stop_sched_timer()
{
    if (controld_sched_timer && fsa_pe_ref) {
        crm_trace("Stopping timer for scheduler reply %s", fsa_pe_ref);
    }
    mainloop_timer_stop(controld_sched_timer);
}

/*!
 * \internal
 * \brief Set the scheduler request currently being waited on
 *
 * \param[in] msg  Request to expect reply to (or NULL for none)
 */
void
controld_expect_sched_reply(xmlNode *msg)
{
    char *ref = NULL;

    if (msg) {
        ref = crm_element_value_copy(msg, XML_ATTR_REFERENCE);
        CRM_ASSERT(ref != NULL);

        if (controld_sched_timer == NULL) {
            controld_sched_timer = mainloop_timer_add("scheduler_reply_timer",
                                                      SCHED_TIMEOUT_MS, FALSE,
                                                      controld_sched_timeout,
                                                      NULL);
        }
        mainloop_timer_start(controld_sched_timer);
    } else {
        controld_stop_sched_timer();
    }
    free(fsa_pe_ref);
    fsa_pe_ref = ref;
}

/*!
 * \internal
 * \brief Free the scheduler reply timer
 */
void
controld_free_sched_timer()
{
    if (controld_sched_timer != NULL) {
        mainloop_timer_del(controld_sched_timer);
        controld_sched_timer = NULL;
    }
}

/*	 A_PE_INVOKE	*/
void
do_pe_invoke(long long action,
             enum crmd_fsa_cause cause,
             enum crmd_fsa_state cur_state,
             enum crmd_fsa_input current_input, fsa_data_t * msg_data)
{
    if (AM_I_DC == FALSE) {
        crm_err("Not invoking Policy Engine because not DC: %s",
                fsa_action2string(action));
        return;
    }

    if (is_set(fsa_input_register, R_PE_CONNECTED) == FALSE) {
        if (is_set(fsa_input_register, R_SHUTDOWN)) {
            crm_err("Cannot shut down gracefully without the Policy Engine");
            register_fsa_input_before(C_FSA_INTERNAL, I_TERMINATE, NULL);

        } else {
            crm_info("Waiting for the Policy Engine to connect");
            crmd_fsa_stall(FALSE);
            register_fsa_action(A_PE_START);
        }
        return;
    }

    if (cur_state != S_POLICY_ENGINE) {
        crm_notice("Not invoking Policy Engine because in state %s",
                   fsa_state2string(cur_state));
        return;
    }
    if (is_set(fsa_input_register, R_HAVE_CIB) == FALSE) {
        crm_err("Attempted to invoke Policy Engine without consistent Cluster Information Base!");

        /* start the join from scratch */
        register_fsa_input_before(C_FSA_INTERNAL, I_ELECTION, NULL);
        return;
    }

    fsa_pe_query = fsa_cib_conn->cmds->query(fsa_cib_conn, NULL, NULL, cib_scope_local);

    crm_debug("Query %d: Requesting the current CIB: %s", fsa_pe_query,
              fsa_state2string(fsa_state));

    controld_expect_sched_reply(NULL);
    fsa_register_cib_callback(fsa_pe_query, FALSE, NULL, do_pe_invoke_callback);
}

static void
force_local_option(xmlNode *xml, const char *attr_name, const char *attr_value)
{
    int max = 0;
    int lpc = 0;
    int xpath_max = 1024;
    char *xpath_string = NULL;
    xmlXPathObjectPtr xpathObj = NULL;

    xpath_string = calloc(1, xpath_max);
    lpc = snprintf(xpath_string, xpath_max, "%.128s//%s//nvpair[@name='%.128s']",
                       get_object_path(XML_CIB_TAG_CRMCONFIG), XML_CIB_TAG_PROPSET, attr_name);
    CRM_LOG_ASSERT(lpc > 0);

    xpathObj = xpath_search(xml, xpath_string);
    max = numXpathResults(xpathObj);
    free(xpath_string);

    for (lpc = 0; lpc < max; lpc++) {
        xmlNode *match = getXpathResult(xpathObj, lpc);
        crm_trace("Forcing %s/%s = %s", ID(match), attr_name, attr_value);
        crm_xml_add(match, XML_NVPAIR_ATTR_VALUE, attr_value);
    }

    if(max == 0) {
        xmlNode *configuration = NULL;
        xmlNode *crm_config = NULL;
        xmlNode *cluster_property_set = NULL;

        crm_trace("Creating %s-%s for %s=%s",
                  CIB_OPTIONS_FIRST, attr_name, attr_name, attr_value);

        configuration = find_entity(xml, XML_CIB_TAG_CONFIGURATION, NULL);
        if (configuration == NULL) {
            configuration = create_xml_node(xml, XML_CIB_TAG_CONFIGURATION);
        }

        crm_config = find_entity(configuration, XML_CIB_TAG_CRMCONFIG, NULL);
        if (crm_config == NULL) {
            crm_config = create_xml_node(configuration, XML_CIB_TAG_CRMCONFIG);
        }

        cluster_property_set = find_entity(crm_config, XML_CIB_TAG_PROPSET, NULL);
        if (cluster_property_set == NULL) {
            cluster_property_set = create_xml_node(crm_config, XML_CIB_TAG_PROPSET);
            crm_xml_add(cluster_property_set, XML_ATTR_ID, CIB_OPTIONS_FIRST);
        }

        xml = create_xml_node(cluster_property_set, XML_CIB_TAG_NVPAIR);

        crm_xml_set_id(xml, "%s-%s", CIB_OPTIONS_FIRST, attr_name);
        crm_xml_add(xml, XML_NVPAIR_ATTR_NAME, attr_name);
        crm_xml_add(xml, XML_NVPAIR_ATTR_VALUE, attr_value);
    }
    freeXpathObject(xpathObj);
}

void
do_pe_invoke_callback(xmlNode * msg, int call_id, int rc, xmlNode * output, void *user_data)
{
    int sent;
    xmlNode *cmd = NULL;
    pid_t watchdog = pcmk_locate_sbd();

    if (rc != pcmk_ok) {
        crm_err("Could not retrieve the Cluster Information Base: %s "
                CRM_XS " call=%d", pcmk_strerror(rc), call_id);
        register_fsa_error_adv(C_FSA_INTERNAL, I_ERROR, NULL, NULL, __FUNCTION__);
        return;

    } else if (call_id != fsa_pe_query) {
        crm_trace("Skipping superseded CIB query: %d (current=%d)", call_id, fsa_pe_query);
        return;

    } else if (AM_I_DC == FALSE || is_set(fsa_input_register, R_PE_CONNECTED) == FALSE) {
        crm_debug("No need to invoke the PE anymore");
        return;

    } else if (fsa_state != S_POLICY_ENGINE) {
        crm_debug("Discarding PE request in state: %s", fsa_state2string(fsa_state));
        return;

    /* this callback counts as 1 */
    } else if (num_cib_op_callbacks() > 1) {
        crm_debug("Re-asking for the CIB: %d other peer updates still pending",
                  (num_cib_op_callbacks() - 1));
        sleep(1);
        register_fsa_action(A_PE_INVOKE);
        return;

    } else if (fsa_state != S_POLICY_ENGINE) {
        crm_err("Invoking PE in state: %s", fsa_state2string(fsa_state));
        return;
    }

    CRM_LOG_ASSERT(output != NULL);

    /* Refresh the remote node cache and the known node cache when the
     * scheduler is invoked */
    crm_peer_caches_refresh(output);

    crm_xml_add(output, XML_ATTR_DC_UUID, fsa_our_uuid);
    crm_xml_add_int(output, XML_ATTR_HAVE_QUORUM, fsa_has_quorum);

    force_local_option(output, XML_ATTR_HAVE_WATCHDOG, watchdog?"true":"false");

    if (ever_had_quorum && crm_have_quorum == FALSE) {
        crm_xml_add_int(output, XML_ATTR_QUORUM_PANIC, 1);
    }

    cmd = create_request(CRM_OP_PECALC, output, NULL, CRM_SYSTEM_PENGINE, CRM_SYSTEM_DC, NULL);

    sent = crm_ipc_send(mainloop_get_ipc_client(pe_subsystem->source), cmd, 0, 0, NULL);
    if (sent <= 0) {
        crm_err("Could not contact the pengine: %d", sent);
        register_fsa_error_adv(C_FSA_INTERNAL, I_ERROR, NULL, NULL, __FUNCTION__);
    } else {
        controld_expect_sched_reply(cmd);
        crm_debug("Invoking the PE: query=%d, ref=%s, seq=%llu, quorate=%d",
                  fsa_pe_query, fsa_pe_ref, crm_peer_seq, fsa_has_quorum);
    }
    free_xml(cmd);
}
