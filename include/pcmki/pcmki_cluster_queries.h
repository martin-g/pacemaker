#include <glib.h>               // gboolean, GMainLoop, etc.

#include <crm/crm.h>
#include <crm/common/output_internal.h>
#include <crm/common/ipc_controld.h>
#include <crm/common/ipc_pacemakerd.h>

int pcmk__controller_status(pcmk__output_t *out, char *dest_node, guint message_timeout_ms);
int pcmk__designated_controller(pcmk__output_t *out, guint message_timeout_ms);
int pcmk__pacemakerd_status(pcmk__output_t *out, char *ipc_name, guint message_timeout_ms);
int pcmk__list_nodes(pcmk__output_t *out, gboolean BASH_EXPORT);

// remove when parameters removed from tools/crmadmin.c
int pcmk__shutdown_controller(pcmk__output_t *out, char *dest_node);
int pcmk__start_election(pcmk__output_t *out);
