/* OpenVAS
* $Id$
* Description: Advanced wrapper from nmap
*
* Authors:
* Henri Doreau <henri.doreau@greenbone.net>
*
* Copyright:
* Copyright (C) 2011 Greenbone Networks GmbH
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2,
* as published by the Free Software Foundation
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
*
*
*/

/**
 * @file nasl_builtin_nmap.c
 *
 * @brief Advanced wrapper for nmap. Perform comprehensive network scanning.
 *
 * This plugin was designed to be executed only once per network. It generates
 * the nmap command line according to the specified options, runs nmap, parses
 * the output and stores results for each host in the knowledge base.
 */

/**
 * @internal
 * The plugin reconstructs host "objects" from nmap' XML output and dump then
 * into the KB.
 *
 * Parsing is performed using a two steps callbacks system.
 *   - The Glib SAX parser calls start/end_element() functions when
 *   entering/leaving a section.
 *   - On recognized sections, these first callbacks execute specialized ones
 *   (xml_open_*() and xml_close_*()).
 *
 * This system can be seen as a 1-1 mapping between XML tag names and
 * corresponding handlers.
 *
 * When leaving a XML &lt;host&gt; section, the gathered information about the
 * current host is stored into the knowledge base. Then the process is
 * repeated for the next host.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <glib.h>

#include "../misc/arglists.h"
#include "../misc/scanners_utils.h"
#include "../misc/plugutils.h"
#include "../misc/popen.h"
#include "../misc/kb.h"

#include "nasl_lex_ctxt.h"


#ifndef NDEBUG
  #define dbg(...) do { fprintf (stderr, __VA_ARGS__); } while (0)
  #define err(x) do { perror (x); } while (0)
#else
  #define dbg(...)
  #define err(x)
#endif


/**
 * @brief Input chunks size for the XML parser.
 */
#define CHUNK_LEN 512

/**
 * @brief Maximum number of hops to the target.
 */
#define MAX_TRACE_HOPS  64

/**
 * @brief Nmap command to call.
 */
#define NMAP_CMD    "nmap"


/* -- script options -- */

/**
 * @brief Plugin parameter description: skip alive hosts discovery phase.
 */
#define PREF_TREAT_ALL_HOST_ONLINE  "Treat all hosts as online"

/**
 * @brief Plugin parameter description: perform traceroute.
 */
#define PREF_TRACEROUTE             "Trace hop path to each host"

/**
 * @brief Plugin parameter description: don't perform reverse resolution on
 *        discovered IP addresses.
 */
#define PREF_NO_DNS                 "Disable DNS resolution"

/**
 * @brief Plugin parameter description: TCP port scanning technique to use.
 */
#define PREF_TCP_SCANNING_TECHNIQUE "TCP scanning technique"

/**
 * @brief Plugin parameter description: perform UDP port scan.
 */
#define PREF_UDP_PORT_SCAN          "UDP port scan"

/**
 * @brief Plugin parameter description: perform service/version detection
 *        scan.
 */
#define PREF_SERVICE_SCAN           "Service scan"

/**
 * @brief Plugin parameter description: perform RPC port scan.
 */
#define PREF_RPC_PORT_SCAN          "RPC port scan"

/**
 * @brief Plugin parameter description: perform remote OS fingerprinting.
 */
#define PREF_IDENTIFY_REMOTE_OS     "Identify the remote OS"

/**
 * @brief Plugin parameter description: guess OS from closest match if
 *        necessary.
 */
#define PREF_AGGRESSIVE_OS_DETECT   "Aggressive OS detection"

/**
 * @brief Plugin parameter description: try to evade defense by fragmenting IP
 *        packets.
 */
#define PREF_FRAGMENT_IP            "Fragment IP packets (bypasses firewalls)"

/**
 * @brief Plugin parameter description: set source port.
 */
#define PREF_SOURCE_PORT            "Source port"

/**
 * @brief Plugin parameter description: select timing template.
 */
#define PREF_TIMING_POLICY          "Timing policy"

/**
 * @brief Plugin parameter description: give up on host after this time
 *        elapsed.
 */
#define PREF_HOST_TIMEOUT           "Host Timeout (ms)"

/**
 * @brief Plugin parameter description: probe round trip time hint (minimal
 * value)
 */
#define PREF_MIN_RTT_TIMEOUT        "Min RTT Timeout (ms)"

/**
 * @brief Plugin parameter description: probe round trip time hint (maximal
 *        value).
 */
#define PREF_MAX_RTT_TIMEOUT        "Max RTT Timeout (ms)"

/**
 * @brief Plugin parameter description: probe round trip time hint (initial
 *        value).
 */
#define PREF_INITIAL_RTT_TIMEOUT    "Initial RTT timeout (ms)"

/**
 * @brief Plugin parameter description: force minimum number of parallel active
 *        probes.
 */
#define PREF_MIN_PARALLELISM        "Ports scanned in parallel (min)"

/**
 * @brief Plugin parameter description: force maximum number of parallel active
 *        probes.
 */
#define PREF_MAX_PARALLELISM        "Ports scanned in parallel (max)"

/**
 * @brief Plugin parameter description: force minimum number of hosts to scan in
 *        parallel.
 */
#define PREF_MIN_HOSTGROUP          "Hosts scanned in parallel (min)"

/**
 * @brief Plugin parameter description: force maximum number of hosts to scan in
 *        parallel.
 */
#define PREF_MAX_HOSTGROUP          "Hosts scanned in parallel (max)"

/**
 * @brief Plugin parameter description: set idle interval between probes.
 */
#define PREF_INTERPROBE_DELAY       "Minimum wait between probes (ms)"

/**
 * @brief Plugin parameter description: comma-separated list of hosts to exclude
 *        from the scan.
 */
#define PREF_EXCLUDE_HOSTS          "Exclude hosts"

/**
 * @brief Plugin parameter description: import XML file.
 */
#define PREF_IMPORT_XML_FILE        "File containing XML results"


/**
 * @brief Checkbox value (when set).
 */
#define OPT_SET   "yes"

/**
 * @brief Checkbox value (when unset).
 */
#define OPT_UNSET "no"


/**
 * @brief Handle the results of a NSE script.
 */
struct nse_script
{
  gchar *name;              /**< NSE script id (or name) */
  gchar *output;            /**< NSE script output */
  struct nse_script *next;  /**< pointer to the next element or NULL */
};

/**
 * @brief Describe a detected hop on the route.
 */
struct traceroute_hop
{
  gchar *addr;  /**< Host IP address. */
  gchar *host;  /**< Hostname (or NULL if unavailable). */
  gchar *rtt;   /**< Smoothed round time trip (or NULL if unavailable). */
};

/**
 * @brief Store port information.
 */
struct nmap_port
{
  gchar *proto;     /**< Layer 4 protocol. */
  gchar *portno;    /**< Port number. */
  gchar *state;     /**< Port state (open/closed/filtered...). */
  gchar *service;   /**< Service name (can be different from the standard port/service combo). */
  gchar *version;   /**< Discovered product, version, extrainfo (version detection). */
  struct nse_script *port_scripts;  /**< NULL terminated list of related port script results. */
  struct nmap_port *next;           /**< Next item in the linked list. */
};

/**
 * @brief Store host information.
 *
 * Most entries are represented as gchar* despite they represent numbers.
 * Conversion would be overkill as these entries are read as strings and
 * reported as strings as well.
 */
struct nmap_host
{
  gchar *addr;    /**< Host IP address. */
  gchar *state;   /**< Host aliveness. */
  gchar *best_os; /**< Best OS fingerprinting guess. */
  gchar *tcpseq_index;      /**< TCP sequence index. */
  gchar *tcpseq_difficulty; /**< TCP sequence prediction difficulty. */
  gchar *ipidseq;           /**< IP ID sequence. */

  int distance;  /**< Hop count to the target. */
  struct traceroute_hop trace[MAX_TRACE_HOPS];  /**< Traceroute results. */
  int os_confidence;                /**< OS detection confidence score. */
  struct nse_script *host_scripts;  /**< NULL termiated list of related host script results. */
  struct nmap_port *ports;          /**< NULL terminated list of ports. */
};

/**
 * @brief Handle states for XML parsing.
 */
struct nmap_parser
{
  GHashTable *opentag;    /**< Parsing callbacks for opening tags. */
  GHashTable *closetag;   /**< Parsing callbacks for closing tags. */

  gboolean in_host;       /**< Parsing flag: mark host section. */
  gboolean in_ports;      /**< Parsing flag: mark ports section. */
  gboolean in_port;       /**< Parsing flag: mark port section. */
  gboolean in_hostscript; /**< Parsing flag: mark hostscript section. */
};

/**
 * @brief Main nmap execution handler.
 */
typedef struct
{
  /* Command line generation */
  gchar **args;
  int arg_idx;

  /* External XML file parsing */
  const gchar *filename;

  /* General execution environment */
  struct arglist *env;

  /* XML parsing states */
  struct nmap_parser parser;

  struct nmap_host tmphost;
  struct nmap_port tmpport;
} nmap_t;

/**
 * @brief Describe an nmap command line option.
 */
typedef struct
{
  gchar *optname;             /**< NASL option name as exported to the user. */
  gchar *flag;                /**< Corresponding nmap flag to set. */
  gboolean argument_required; /**< Add option value to the command line. */
} nmap_opt_t;


/* --------------------- INTERNAL FUNCTIONS PROTOTYPES ---------------------- */

/*
 * Nmap handler ctor/dtor.
 */
static nmap_t *nmap_create (lex_ctxt * lexic);
static void nmap_destroy (nmap_t * nmap);


/*
 * Command line generation from supplied options and parameters.
 */
static int build_cmd_line (nmap_t * nmap);
static int add_arg (nmap_t * nmap, const gchar * name, const gchar * value);
static int add_nse_arguments (nmap_t * nmap);
static gchar *get_script_list (nmap_t * nmap);
static gchar *get_script_args (nmap_t * nmap);
static int add_scantype_arguments (nmap_t * nmap);
static int add_timing_arguments (nmap_t * nmap);
static int add_portrange (nmap_t * nmap);
static int cmp (const void *p1, const void *p2);
static gchar *get_default_portrange (void);
static void setup_xml_parser (nmap_t * nmap);
static void set_opentag_callbacks (GHashTable * open);
static void set_closetag_callbacks (GHashTable * close);
static int add_target (nmap_t * nmap);
static void dbg_display_cmdline (nmap_t * nmap);


/*
 * Execution control and high level results parsing.
 */
static int nmap_run_and_parse (nmap_t * nmap);
static void current_host_reset (nmap_t * nmap);
static void port_destroy (struct nmap_port *port);
static void nse_script_destroy (struct nse_script *script);
static void tmphost_add_port (nmap_t * nmap);
static void tmphost_add_nse_hostscript (nmap_t * nmap, gchar * name,
                                        gchar * output);
static void tmphost_add_nse_portscript (nmap_t * nmap, gchar * name,
                                        gchar * output);


/*
 * Top level callbacks to handle opening/closing XML elements.
 */
static void
xml_start_element (GMarkupParseContext * context, const gchar * element_name,
                   const gchar ** attribute_names,
                   const gchar ** attribute_values, gpointer user_data,
                   GError ** error);
static void
xml_end_element (GMarkupParseContext * context, const gchar * element_name,
                 gpointer user_data, GError ** error);


/*
 * Callbacks for opening recognized elements.
 */
static void xmltag_open_host (nmap_t * nmap, const gchar ** attrnames,
                              const gchar ** attrval);
static void xmltag_open_status (nmap_t * nmap, const gchar ** attrnames,
                                const gchar ** attrval);
static void xmltag_open_address (nmap_t * nmap, const gchar ** attrnames,
                                 const gchar ** attrval);
static void xmltag_open_ports (nmap_t * nmap, const gchar ** attrnames,
                               const gchar ** attrval);
static void xmltag_open_port (nmap_t * nmap, const gchar ** attrnames,
                              const gchar ** attrval);
static void xmltag_open_state (nmap_t * nmap, const gchar ** attrnames,
                               const gchar ** attrval);
static void xmltag_open_service (nmap_t * nmap, const gchar ** attrnames,
                                 const gchar ** attrval);
static void xmltag_open_hostscript (nmap_t * nmap, const gchar ** attrnames,
                                    const gchar ** attrval);
static void xmltag_open_osmatch (nmap_t * nmap, const gchar ** attrnames,
                                 const gchar ** attrval);
static void xmltag_open_script (nmap_t * nmap, const gchar ** attrnames,
                                const gchar ** attrval);
static void xmltag_open_tcpsequence (nmap_t * nmap, const gchar ** attrnames,
                                     const gchar ** attrval);
static void xmltag_open_ipidsequence (nmap_t * nmap, const gchar ** attrnames,
                                      const gchar ** attrval);
static void xmltag_open_hop (nmap_t * nmap, const gchar ** attrnames,
                             const gchar ** attrval);
static void xmltag_open_distance (nmap_t * nmap, const gchar ** attrnames,
                                  const gchar ** attrval);


/*
 * Callbacks for closing recognized elements.
 */
static void xmltag_close_host (nmap_t * nmap);
static void xmltag_close_ports (nmap_t * nmap);
static void xmltag_close_port (nmap_t * nmap);
static void xmltag_close_hostscript (nmap_t * nmap);


/*
 * Helper function to get the strdup'ed value of a given attribute.
 */
static gchar *get_attr_value (const gchar * name,
                              const gchar ** attribute_names,
                              const gchar ** attribute_values);


/*
 * Store host results in the KB.
 */
static void current_host_saveall (nmap_t * nmap);
static void save_host_state (nmap_t * nmap);
static void save_open_ports (nmap_t * nmap);
static void register_service (nmap_t * nmap, struct nmap_port *p);
static void save_detected_os (nmap_t * nmap);
static void save_tcpseq_details (nmap_t * nmap);
static void save_ipidseq_details (nmap_t * nmap);
static void save_traceroute_details (nmap_t * nmap);
static void save_portscripts (nmap_t * nmap);
static void save_hostscripts (nmap_t * nmap);

/* -------------------------------------------------------------------------- */


/**
 * @brief Run the nmap_net subsystem.
 *
 * @param[in] lexic NASL state.
 *
 * @return NULL on error, FAKE_CELL on success.
 */
tree_cell *
plugin_run_nmap (lex_ctxt * lexic)
{
  nmap_t *nmap;

  dbg ("Starting Nmap builtin wrapper\n");

  /* Initialize our nmap handler */
  if ((nmap = nmap_create (lexic)) == NULL)
    {
      dbg ("Unable to initialize Nmap\n");
      return NULL;
    }

  /* Execute nmap and store results */
  nmap_run_and_parse (nmap);

  /* release resources */
  nmap_destroy (nmap);

  return FAKE_CELL;
}

/**
 * @brief Instanciate a new nmap handler, rebuild command line or open XML file
 *        to parse.
 *
 * @param[in] lexic NASL state
 *
 * @return The newly allocated nmap handler or NULL on error.
 */
nmap_t *
nmap_create (lex_ctxt * lexic)
{
  gchar *pref;
  nmap_t *nmap;

  nmap = (nmap_t *) g_malloc0 (sizeof (nmap_t));

  nmap->env = lexic->script_infos;

  /* import results from external file? */
  pref = get_plugin_preference (nmap->env, PREF_IMPORT_XML_FILE);
  if (!pref || !strlen (pref))
    {
      /* no: build command line to execute */
      if (build_cmd_line (nmap) < 0)
        {
          nmap_destroy (nmap);
          return NULL;
        }

      /* Display command line to use */
      dbg ("Nmap initialized: ");
      dbg_display_cmdline (nmap);
    }
  else
    {
      /* yes: store filename */
      nmap->filename = get_plugin_preference_fname (nmap->env, pref);
      dbg ("Reading nmap results from file: %s\n", nmap->filename);
    }

  setup_xml_parser (nmap);
  return nmap;
}

/**
 * @brief Release a nmap handler and associated resources.
 *
 * @param[in,out] nmap  Handler to free.
 */
void
nmap_destroy (nmap_t * nmap)
{
  if (!nmap)
    return;

  if (nmap->args)
    {
      int i;

      for (i = 0; i < nmap->arg_idx; i++)
        g_free (nmap->args[i]);

      g_free (nmap->args);
    }

  if (nmap->parser.opentag)
    g_hash_table_destroy (nmap->parser.opentag);

  if (nmap->parser.closetag)
    g_hash_table_destroy (nmap->parser.closetag);

  g_free (nmap);
}

/**
 * @brief Rebuild command line to run according to plugin parameters.
 *
 * @param[in,out] nmap  Handler to use.
 *
 * @return -1 on failure and 1 on success.
 */
int
build_cmd_line (nmap_t * nmap)
{
  int i;
  /* this list handles basic options (simple flag or name/value) */
  nmap_opt_t options[] = {
    /* --- Host discovery --- */
    {PREF_TREAT_ALL_HOST_ONLINE, "-Pn", FALSE},
    {PREF_TRACEROUTE, "--traceroute", FALSE},
    {PREF_NO_DNS, "-n", FALSE},

    /* --- Scan techniques --- */
    {PREF_UDP_PORT_SCAN, "-sU", FALSE},
    {PREF_SERVICE_SCAN, "-sV", FALSE},
    {PREF_RPC_PORT_SCAN, "-sR", FALSE},

    /* --- OS Detection --- */
    {PREF_IDENTIFY_REMOTE_OS, "-O", FALSE},
    {PREF_AGGRESSIVE_OS_DETECT, "--osscan-guess", FALSE},

    /* --- Firewall/IDS evasion --- */
    {PREF_FRAGMENT_IP, "-f", FALSE},
    {PREF_SOURCE_PORT, "-g", TRUE},

    /* --- Timing and performances --- */
    {PREF_HOST_TIMEOUT, "--host-timeout", TRUE},
    {PREF_MIN_RTT_TIMEOUT, "--min-rtt-timeout", TRUE},
    {PREF_MAX_RTT_TIMEOUT, "--max-rtt-timeout", TRUE},
    {PREF_INITIAL_RTT_TIMEOUT, "--initial-rtt-timeout", TRUE},
    {PREF_MIN_PARALLELISM, "--min-parallelism", TRUE},
    {PREF_MAX_PARALLELISM, "--max-parallelism", TRUE},
    {PREF_MIN_HOSTGROUP, "--min-hostgroup", TRUE},
    {PREF_MAX_HOSTGROUP, "--max-hostgroup", TRUE},
    {PREF_INTERPROBE_DELAY, "--delay", TRUE},

    /* --- Targets specification --- */
    {PREF_EXCLUDE_HOSTS, "--exclude", TRUE},

    {NULL, NULL, FALSE}
  };

  /* Nmap invocation */
  add_arg (nmap, NMAP_CMD, NULL);

  /* Enable XML output on stdout */
  add_arg (nmap, "-oX", "-");

  for (i = 0; options[i].optname; i++)
    {
      gchar *optval;

      optval = get_plugin_preference (nmap->env, options[i].optname);
      if (!optval)
        continue;

      if (options[i].argument_required)
        {
          if (strlen (optval) > 0)
            if (add_arg (nmap, options[i].flag, optval) < 0)
              return -1;
        }
      else
        {
          if (g_strcmp0 (optval, OPT_SET) == 0)
            if (add_arg (nmap, options[i].flag, NULL) < 0)
              return -1;
        }
    }

  if (add_portrange (nmap) < 0)
    return -1;

  /* Scan technique */
  if (add_scantype_arguments (nmap) < 0)
    return -1;

  /* Timing policy */
  if (add_timing_arguments (nmap) < 0)
    return -1;

  /* Script scan */
  if (add_nse_arguments (nmap) < 0)
    return -1;

  if (add_target (nmap) < 0)
    return -1;

  return 1;
}

/**
 * @brief Add a couple argument/value on the command line.
 *
 * @param[in,out] nmap  Handler to use.
 * @param[in] name  Name of the flag/option.
 * @param[in] value Value of the option (or NULL for simple flags).
 *
 * @return -1 on failure or 1 on success.
 */
int
add_arg (nmap_t * nmap, const gchar * name, const gchar * value)
{
  if (!name)
    return -1;

  if (!nmap->args)
    {
      /* Initial call, instanciate the NULL terminated list of arguments */
      nmap->args = (gchar **) g_malloc (sizeof (gchar **));
      nmap->arg_idx = 0;
    }

  if (!value)
    {
      /* simple flag (no value) */
      nmap->args = g_realloc (nmap->args,
                              (nmap->arg_idx + 2) * sizeof (gchar *));
      nmap->args[nmap->arg_idx++] = g_strdup (name);
    }
  else
    {
      /* name->value argument */
      nmap->args = g_realloc (nmap->args,
                              (nmap->arg_idx + 3) * sizeof (gchar *));
      nmap->args[nmap->arg_idx++] = g_strdup (name);
      nmap->args[nmap->arg_idx++] = g_strdup (value);
    }

  /* NULL-terminate the list */
  nmap->args[nmap->arg_idx] = NULL;

  return 1;
}

/**
 * @brief Add NSE (nmap scripting engine) related arguments to the command
 *        line according to user script selection and preferences.
 *
 * @param[in,out] nmap  Handler to use.
 *
 * @return 1 success
 */
int
add_nse_arguments (nmap_t * nmap)
{
  gchar *pscript, *pargs;

  pscript = get_script_list (nmap);
  pargs = get_script_args (nmap);
  if (strlen (pscript))
    {
      /* Add script flags if user requested some NSE */
      add_arg (nmap, "--script", pscript);

      if (strlen (pargs))
        add_arg (nmap, "--script-args", pargs);
    }
  g_free (pscript);
  g_free (pargs);

  return 1;
}

/**
 * @brief Make the comma-separated list of NSE scripts selected by the user.
 *
 * @param[in,out] nmap  Handler to use.
 *
 * @return A dynamically allocated string containing the list of NSE scripts to
 *         run.
 */
gchar *
get_script_list (nmap_t * nmap)
{
  struct kb_item **kb = plug_get_kb (nmap->env);
  struct kb_item *top, *res;
  gchar **scriptv, *scriptstr;
  int i = 0;

  scriptv = NULL;

  /* Read list of scripts from the KB */
  top = res = kb_item_get_all (kb, "NmapNSE/scripts");
  while (res)
    {
      scriptv = (gchar **) g_realloc (scriptv, (i + 1) * sizeof (gchar *));
      scriptv[i++] = g_strdup (res->v.v_str);
      res = res->next;
    }

  scriptv = (gchar **) g_realloc (scriptv, (i + 1) * sizeof (gchar *));
  scriptv[i] = NULL;

  kb_item_get_all_free (top);

  scriptstr = g_strjoinv (",", scriptv);

  for (i = 0; scriptv[i]; i++)
    g_free (scriptv[i]);

  g_free (scriptv);

  return scriptstr;
}

/**
 * @brief Make the comma-separated list of NSE arguments set by the user.
 *
 * @param[in,out] nmap  Handler to use.
 *
 * @return A dynamically allocated string containing the list of NSE arguments to
 *         use
 */
gchar *
get_script_args (nmap_t * nmap)
{
  struct kb_item **kb = plug_get_kb (nmap->env);
  struct kb_item *top, *res;
  gchar **argv, *argstr;
  int i = 0;

  argv = NULL;

  top = res = kb_item_get_all (kb, "NmapNSE/arguments");
  while (res)
    {
      argv = (gchar **) g_realloc (argv, (i + 1) * sizeof (gchar *));
      argv[i++] = g_strdup (res->v.v_str);
      res = res->next;
    }

  argv = (gchar **) g_realloc (argv, (i + 1) * sizeof (gchar *));
  argv[i] = NULL;

  kb_item_get_all_free (top);

  argstr = g_strjoinv (",", argv);

  for (i = 0; argv[i]; i++)
    g_free (argv[i]);
  g_free (argv);

  return argstr;
}

/**
 * @brief Add the TCP scantype flag to the command line.
 *
 * @param[in,out] nmap  Handler to use.
 *
 * @return -1 on failure or 1 on success.
 */
int
add_scantype_arguments (nmap_t * nmap)
{
  int i;
  gchar *scantype;
  nmap_opt_t flagmap[] = {
    {"connect()", "-sT", FALSE},
    {"SYN", "-sS", FALSE},
    {"ACK", "-sA", FALSE},
    {"FIN", "-sF", FALSE},
    {"Window", "-sW", FALSE},
    {"Maimon", "-sM", FALSE},
    {"Xmas tree", "-sX", FALSE},
    {"Null", "-sN", FALSE},
    {"SCTP Init", "-sY", FALSE},
    {"SCTP COOKIE_ECHO", "-sZ", FALSE},
    {NULL, NULL, FALSE}
  };

  scantype = get_plugin_preference (nmap->env, PREF_TCP_SCANNING_TECHNIQUE);
  if (!scantype)
    return -1;

  for (i = 0; flagmap[i].optname; i++)
    if (g_strcmp0 (scantype, flagmap[i].optname) == 0)
      return add_arg (nmap, flagmap[i].flag, NULL);

  return -1;
}

/**
 * @brief Add timing template argument to the command line.
 *
 * @param[in,out] nmap  Handler to use.
 *
 * @return -1 on failure or 1 on success.
 */
int
add_timing_arguments (nmap_t * nmap)
{
  int i;
  gchar *timing;
  nmap_opt_t flagmap[] = {
    {"Paranoid", "-T0", FALSE},
    {"Sneaky", "-T1", FALSE},
    {"Polite", "-T2", FALSE},
    {"Normal", "-T3", FALSE},
    {"Aggressive", "-T4", FALSE},
    {"Insane", "-T5", FALSE},
    {NULL, NULL, FALSE}
  };

  timing = get_plugin_preference (nmap->env, PREF_TIMING_POLICY);
  if (!timing)
    return -1;

  for (i = 0; flagmap[i].optname; i++)
    if (g_strcmp0 (timing, flagmap[i].optname) == 0)
      return add_arg (nmap, flagmap[i].flag, NULL);

  return -1;
}

/**
 * @brief Add the range of ports to scan to the command line.
 *
 * @param[in,out] nmap  Handler to use.
 *
 * @return -1 on failure or 1 on success.
 */
int
add_portrange (nmap_t * nmap)
{
  int ret;
  struct arglist *pref;
  gchar *portrange;

  pref = arg_get_value (nmap->env, "preferences");
  if (!pref)
    {
      dbg ("Invalid environment: unavailable \"preferences\"\n");
      return -1;
    }

  portrange = arg_get_value (pref, "port_range");
  if (!portrange)
    {
      dbg ("Invalid environment: unavailable \"port_range\"\n");
      return -1;
    }

  if (g_strcmp0 (portrange, "default") == 0)
    {
      gchar *pr_default = get_default_portrange ();

      if (!pr_default)
        {
          dbg ("Invalid default port range\n");
          return -1;
        }
      ret = add_arg (nmap, "-p", pr_default);
      g_free (pr_default);
    }
  else
    ret = add_arg (nmap, "-p", portrange);

  return ret;
}

/**
 * @brief Compares two unsigned shorts located at p1 and p2.
 *
 * @param[in] p1 location of the first unsigned short integer (pp1)
 * @param[in] p2 location of the second unsigned short integer (pp2)
 *
 * @return A positive value if pp1 > pp2, a negative one if pp1 < pp2 and zero
 *         if pp1 == pp2.
 */
int
cmp (const void *p1, const void *p2)
{
  unsigned short *pp1 = (unsigned short *) p1;
  unsigned short *pp2 = (unsigned short *) p2;

  return (*pp1) - (*pp2);
}

/**
 * @brief Get the default portrange as a reduced string (x-y,z).
 *
 * @return A newly allocated string describing the portrange.
 */
gchar *
get_default_portrange (void)
{
  gchar *portrange = NULL;
  unsigned short *ports;
  int i, plen;
  int start = -1, stop = -1;

  ports = getpts ("default", &plen);
  if (!ports || !plen)
    return NULL;

  qsort (ports, plen, sizeof (unsigned short), cmp);

  for (i = 0; i < plen; i++)
    {
      gboolean last_run = (i == plen - 1);
      gchar *tmp, chunk[16];

      if (start == -1)
        {
          start = stop = ports[i];
          if (!last_run)
            continue;
        }
      else if (ports[i] == stop + 1)
        {
          stop = ports[i];
          if (!last_run)
            continue;
        }

      if (start != stop)
        g_snprintf (chunk, sizeof (chunk), "%d-%d", start, stop);
      else
        g_snprintf (chunk, sizeof (chunk), "%d", start);

      start = stop = ports[i];

      if (portrange)
        tmp = g_strdup_printf ("%s,%s", portrange, chunk);
      else
        tmp = g_strdup_printf ("%s", chunk);
      g_free (portrange);        /* g_free'ing NULL pointers is harmless */
      portrange = tmp;
    }

  return portrange;
}

/**
 * @brief Setup XML parser internals.
 *
 * @param[in,out] nmap  Handler to use.
 */
void
setup_xml_parser (nmap_t * nmap)
{
  /* reset internal states */
  nmap->parser.in_host = FALSE;
  nmap->parser.in_ports = FALSE;
  nmap->parser.in_port = FALSE;
  nmap->parser.in_hostscript = FALSE;

  nmap->parser.opentag = g_hash_table_new (g_str_hash, g_str_equal);
  nmap->parser.closetag = g_hash_table_new (g_str_hash, g_str_equal);

  set_opentag_callbacks (nmap->parser.opentag);
  set_closetag_callbacks (nmap->parser.closetag);
}

/**
 * @brief Populate the callbacks hashtable with handlers for opening tags.
 *
 * @param[out] open The hashtable to populate.
 */
void
set_opentag_callbacks (GHashTable * open)
{
  const struct
  {
    const gchar *tag;
    void (*func) (nmap_t *, const gchar **, const gchar **);
  } callbacks[] = {
    {"hop", xmltag_open_hop},
    {"osmatch", xmltag_open_osmatch},
    {"port", xmltag_open_port},
    {"service", xmltag_open_service},
    {"state", xmltag_open_state},
    {"status", xmltag_open_status},
    {"host", xmltag_open_host},
    {"address", xmltag_open_address},
    {"script", xmltag_open_script},
    {"ports", xmltag_open_ports},
    {"distance", xmltag_open_distance},
    {"hostscript", xmltag_open_hostscript},
    {"tcpsequence", xmltag_open_tcpsequence},
    {"ipidsequence", xmltag_open_ipidsequence},
    {NULL, NULL}
  };
  int i;

  for (i = 0; callbacks[i].tag; i++)
    g_hash_table_insert (open, (void *) callbacks[i].tag, callbacks[i].func);
}

/**
 * @brief Populate the callbacks hashtable with handlers for closing tags.
 *
 * @param[out] close The hashtable to populate.
 */
void
set_closetag_callbacks (GHashTable * close)
{
  const struct
  {
    const gchar *tag;
    void (*func) (nmap_t *);
  } callbacks[] = {
    {"host", xmltag_close_host},
    {"ports", xmltag_close_ports},
    {"port", xmltag_close_port},
    {"hostscript", xmltag_close_hostscript},
    {NULL, NULL}
  };
  int i;

  for (i = 0; callbacks[i].tag; i++)
    g_hash_table_insert (close, (void *) callbacks[i].tag, callbacks[i].func);
}

/**
 * @brief Append scan target to the command line.
 *
 * @param[in,out] nmap  Handler to use.
 *
 * @return -1 on failure or 1 on success.
 */
int
add_target (nmap_t * nmap)
{
  struct arglist *globals;
  gchar *network;

  globals = arg_get_value (nmap->env, "globals");
  if (!globals)
    {
      dbg ("Invalid environment: unavailable \"globals\"\n");
      return -1;
    }

  network = arg_get_value (globals, "network_targets");
  if (!network)
    {
      dbg ("Invalid environment: unavailable \"network_targets\"\n");
      return -1;
    }

  return add_arg (nmap, network, NULL);
}

/**
 * @brief Display the final command line for debug.
 *
 * @param[in,out] nmap  Handler to use.
 */
void
dbg_display_cmdline (nmap_t * nmap)
{
  int i;

  for (i = 0; nmap->args[i]; i++)
    dbg ("%s ", nmap->args[i]);

  if (i == 0)
    dbg ("<empty>");

  dbg ("\n");
}

/**
 * @brief Run nmap and parse its XML output (or load an external file if
 *        requested).
 *
 * @param[in,out] nmap  Handler to use.
 *
 * @return -1 on failure or 1 on success.
 */
int
nmap_run_and_parse (nmap_t * nmap)
{
  FILE *fproc;
  size_t len;
  int ret = 1; /* success */
  pid_t pid;
  gchar chunk[CHUNK_LEN];
  GMarkupParseContext *ctx;
  const GMarkupParser callbacks = {
    xml_start_element,
    xml_end_element,
    NULL,     /* text */
    NULL,     /* passthrough */
    NULL      /* error */
  };

  if (nmap->filename)
    /* read results from external file */
    fproc = fopen (nmap->filename, "r");
  else
    /* execute nmap and read results from the process output */
    fproc = openvas_popen4 (nmap->args[0], nmap->args, &pid, 0);

  if (!fproc)
    {
      err ("nmap_run_and_parse()");
      return -1;
    }

  ctx = g_markup_parse_context_new (&callbacks, 0, nmap, NULL);

  while ((len = fread (chunk, sizeof (gchar), CHUNK_LEN, fproc)) > 0)
    {
      GError *err = NULL;

      if (!g_markup_parse_context_parse (ctx, chunk, len, &err))
        {
          if (err)
            {
              dbg ("g_markup_parse_context_parse() failed (%s)\n",
                   err->message);
              g_error_free (err);

              /* display the problematic chunk */
              chunk[len] = '\0';
              dbg ("Error occured while parsing: %s\n", chunk);

              ret = -1;
            }
          break;
        }
    }

  if (nmap->filename && ferror (fproc))
    {
      err ("nmap_run_and_parse()");
      ret = -1;
    }

  if (nmap->filename)
    fclose (fproc);
  else
    openvas_pclose (fproc, pid);

  g_markup_parse_context_free (ctx);

  return ret;
}

/**
 * @brief Clear the current host object.
 *
 * @param[in,out] nmap  Handler to use.
 */
void
current_host_reset (nmap_t * nmap)
{
  int i;
  struct nmap_port *p;
  struct nse_script *s;

  g_free (nmap->tmphost.addr);
  g_free (nmap->tmphost.state);
  g_free (nmap->tmphost.best_os);
  g_free (nmap->tmphost.tcpseq_index);
  g_free (nmap->tmphost.tcpseq_difficulty);
  g_free (nmap->tmphost.ipidseq);

  for (i = 0; i < MAX_TRACE_HOPS; i++)
    {
      g_free (nmap->tmphost.trace[i].addr);
      g_free (nmap->tmphost.trace[i].rtt);
      g_free (nmap->tmphost.trace[i].host);
    }

  p = nmap->tmphost.ports;
  while (p != NULL)
    {
      struct nmap_port *next;

      next = p->next;
      port_destroy (p);
      p = next;
    }

  s = nmap->tmphost.host_scripts;
  while (s != NULL)
    {
      struct nse_script *next;

      next = s->next;
      nse_script_destroy (s);
      s = next;
    }

  memset (&nmap->tmphost, 0x00, sizeof (struct nmap_host));
}

/**
 * @brief Completely release a port object.
 *
 * @param[in] port  The port structure to free.
 */
void
port_destroy (struct nmap_port *port)
{
  if (port)
    {
      struct nse_script *s;

      g_free (port->proto);
      g_free (port->portno);
      g_free (port->state);
      g_free (port->service);
      g_free (port->version);

      s = port->port_scripts;
      while (s != NULL)
        {
          struct nse_script *next;

          next = s->next;
          nse_script_destroy (s);
          s = next;
        }
      g_free (port);
    }
}

/**
 * @brief Completely release a NSE script object.
 *
 * @param[in] script  The nse script structure to free.
 */
void
nse_script_destroy (struct nse_script *script)
{
  if (script)
    {
      g_free (script->name);
      g_free (script->output);
      g_free (script);
    }
}

/**
 * @brief Add port information to the current host object.
 *
 * @param[in,out] nmap  Handler to use.
 */
void
tmphost_add_port (nmap_t * nmap)
{
  struct nmap_port *newport;

  newport = (struct nmap_port *) g_malloc (sizeof (struct nmap_port));
  memcpy (newport, &nmap->tmpport, sizeof (struct nmap_port));
  newport->next = nmap->tmphost.ports;
  nmap->tmphost.ports = newport;
}

/**
 * @brief Add NSE hostscript result to the current host object.
 *
 * @param[in,out] nmap  Handler to use.
 * @param[in] name  Name of the NSE script that produced the output.
 * @param[in] output  Output produced by this NSE script.
 */
void
tmphost_add_nse_hostscript (nmap_t * nmap, gchar * name, gchar * output)
{
  struct nse_script *s;

  s = (struct nse_script *) g_malloc (sizeof (struct nse_script));
  s->name = name;
  s->output = output;
  s->next = nmap->tmphost.host_scripts;
  nmap->tmphost.host_scripts = s;
}

/**
 * @brief Add NSE portscript result to a port of the current host.
 *
 * @param[in,out] nmap  Handler to use.
 * @param[in] name  Name of the NSE script that produced the output.
 * @param[in] output  Output produced by this NSE script.
 */
void
tmphost_add_nse_portscript (nmap_t * nmap, gchar * name, gchar * output)
{
  struct nse_script *s;

  s = (struct nse_script *) g_malloc (sizeof (struct nse_script));
  s->name = name;
  s->output = output;
  s->next = nmap->tmpport.port_scripts;
  nmap->tmpport.port_scripts = s;
}

/**
 * @brief Top level XML parser callback: handle an opening tag and call the
 *        corresponding method.
 *
 * @param[in] context  The XML parser.
 * @param[in] element_name  The name of the current tag.
 * @param[in] attribute_names  NULL terminated list of attributes names.
 * @param[in] attribute_values  NULL terminated list of attributes values.
 * @param[in] user_data  A pointer to the current nmap_t structure.
 * @param[in] error  Return location of a GError.
 */
void
xml_start_element (GMarkupParseContext * context, const gchar * element_name,
                   const gchar ** attribute_names,
                   const gchar ** attribute_values, gpointer user_data,
                   GError ** error)
{
  nmap_t *nmap = (nmap_t *) user_data;
  void (*callback) (nmap_t *, const gchar **, const gchar **);

  callback = g_hash_table_lookup (nmap->parser.opentag, element_name);
  if (callback)
    callback (nmap, attribute_names, attribute_values);
}

/**
 * @brief Top level XML parser callback: handle an closing tag and call the
 *        corresponding method.
 *
 * @param[in] context  The XML parser.
 * @param[in] element_name  The name of the current tag.
 * @param[in] attribute_names  NULL terminated list of attributes names.
 * @param[in] attribute_values  NULL terminated list of attributes values.
 * @param[in] user_data  A pointer to the current nmap_t structure.
 * @param[in] error  Return location of a GError.
 */
void
xml_end_element (GMarkupParseContext * context, const gchar * element_name,
                 gpointer user_data, GError ** error)
{
  nmap_t *nmap = (nmap_t *) user_data;
  void (*callback) (nmap_t *);

  callback = g_hash_table_lookup (nmap->parser.closetag, element_name);
  if (callback)
    callback (nmap);
}

/**
 * @brief Sublevel XML parser callback: handle an opening host tag.
 *
 * @param[in] nmap  Handler to use.
 * @param[in] attrnames  NULL terminated list of attributes names.
 * @param[in] attrval  NULL terminated list of attributes values.
 */
void
xmltag_open_host (nmap_t * nmap, const gchar ** attrnames,
                  const gchar ** attrval)
{
  nmap->parser.in_host = TRUE;
}

/**
 * @brief Sublevel XML parser callback: handle an opening status tag.
 *
 * @param[in] nmap  Handler to use.
 * @param[in] attrnames  NULL terminated list of attributes names.
 * @param[in] attrval  NULL terminated list of attributes values.
 */
void
xmltag_open_status (nmap_t * nmap, const gchar ** attrnames,
                    const gchar ** attrval)
{
  if (!nmap->parser.in_host)
    dbg ("Error: opening <status> tag out of host description\n");
  else
    nmap->tmphost.state = get_attr_value ("state", attrnames, attrval);
}

/**
 * @brief Sublevel XML parser callback: handle an opening address tag.
 *
 * @param[in] nmap  Handler to use.
 * @param[in] attrnames  NULL terminated list of attributes names.
 * @param[in] attrval  NULL terminated list of attributes values.
 */
void
xmltag_open_address (nmap_t * nmap, const gchar ** attrnames,
                     const gchar ** attrval)
{
  if (!nmap->parser.in_host)
    dbg ("Error: opening <address> tag out of host description\n");
  else
    nmap->tmphost.addr = get_attr_value ("addr", attrnames, attrval);
}

/**
 * @brief Sublevel XML parser callback: handle an opening ports tag.
 *
 * @param[in] nmap  Handler to use.
 * @param[in] attrnames  NULL terminated list of attributes names.
 * @param[in] attrval  NULL terminated list of attributes values.
 */
void
xmltag_open_ports (nmap_t * nmap, const gchar ** attrnames,
                   const gchar ** attrval)
{
  nmap->parser.in_ports = TRUE;
}

/**
 * @brief Sublevel XML parser callback: handle an opening port tag.
 *
 * @param[in] nmap  Handler to use.
 * @param[in] attrnames  NULL terminated list of attributes names.
 * @param[in] attrval  NULL terminated list of attributes values.
 */
void
xmltag_open_port (nmap_t * nmap, const gchar ** attrnames,
                  const gchar ** attrval)
{
  nmap->parser.in_port = TRUE;
  nmap->tmpport.proto = get_attr_value ("protocol", attrnames, attrval);
  nmap->tmpport.portno = get_attr_value ("portid", attrnames, attrval);
}

/**
 * @brief Sublevel XML parser callback: handle an opening state tag.
 *
 * @param[in] nmap  Handler to use.
 * @param[in] attrnames  NULL terminated list of attributes names.
 * @param[in] attrval  NULL terminated list of attributes values.
 */
void
xmltag_open_state (nmap_t * nmap, const gchar ** attrnames,
                   const gchar ** attrval)
{
  if (!nmap->parser.in_port || !nmap->tmpport.proto || !nmap->tmpport.portno)
    dbg ("Error: opening <state> tag out of port description\n");
  else
    nmap->tmpport.state = get_attr_value ("state", attrnames, attrval);
}

/**
 * @brief Sublevel XML parser callback: handle an opening service tag.
 *
 * @param[in] nmap  Handler to use.
 * @param[in] attrnames  NULL terminated list of attributes names.
 * @param[in] attrval  NULL terminated list of attributes values.
 */
void
xmltag_open_service (nmap_t * nmap, const gchar ** attrnames,
                     const gchar ** attrval)
{
  if (!nmap->parser.in_port || !nmap->tmpport.proto || !nmap->tmpport.portno)
    dbg ("Error: opening <service> tag out of port description\n");
  else
    {
      gchar *product, *version, *extrainfo;

      nmap->tmpport.service = get_attr_value ("name", attrnames, attrval);

      /* also store version detection results if available */
      product = get_attr_value ("product", attrnames, attrval);
      version = get_attr_value ("version", attrnames, attrval);
      extrainfo = get_attr_value ("extrainfo", attrnames, attrval);

      if (product || version || extrainfo)
#define PRINT_NOT_NULL(x) ((x) ? (x) : "")
        nmap->tmpport.version = g_strdup_printf ("%s %s %s",
                                                 PRINT_NOT_NULL(product),
                                                 PRINT_NOT_NULL(version),
                                                 PRINT_NOT_NULL(extrainfo));
#undef PRINT_NOT_NULL

      /* g_free'ing NULLs is harmless */
      g_free (product);
      g_free (version);
      g_free (extrainfo);
    }
}

/**
 * @brief Sublevel XML parser callback: handle an opening hostscript tag.
 *
 * @param[in] nmap  Handler to use.
 * @param[in] attrnames  NULL terminated list of attributes names.
 * @param[in] attrval  NULL terminated list of attributes values.
 */
void
xmltag_open_hostscript (nmap_t * nmap, const gchar ** attrnames,
                        const gchar ** attrval)
{
  nmap->parser.in_hostscript = TRUE;
}

/**
 * @brief Sublevel XML parser callback: handle an opening osmatch tag.
 *
 * @param[in] nmap  Handler to use.
 * @param[in] attrnames  NULL terminated list of attributes names.
 * @param[in] attrval  NULL terminated list of attributes values.
 */
void
xmltag_open_osmatch (nmap_t * nmap, const gchar ** attrnames,
                     const gchar ** attrval)
{
  gchar *confstr;

  confstr = get_attr_value ("accuracy", attrnames, attrval);
  if (confstr)
    {
      int confidence;

      confidence = atoi (confstr);
      if (confidence > nmap->tmphost.os_confidence)
        {
          g_free (nmap->tmphost.best_os);
          nmap->tmphost.best_os = get_attr_value ("name", attrnames, attrval);
          nmap->tmphost.os_confidence = confidence;
        }

      g_free (confstr);
    }
}

/**
 * @brief Sublevel XML parser callback: handle an opening script tag.
 *
 * @param[in] nmap  Handler to use.
 * @param[in] attrnames  NULL terminated list of attributes names.
 * @param[in] attrval  NULL terminated list of attributes values.
 */
void
xmltag_open_script (nmap_t * nmap, const gchar ** attrnames,
                    const gchar ** attrval)
{
  gchar *name, *output;

  if (!nmap->parser.in_host)
    return;

  name = get_attr_value ("id", attrnames, attrval);
  output = get_attr_value ("output", attrnames, attrval);

  if (nmap->parser.in_port)
    tmphost_add_nse_portscript (nmap, name, output);
  else
    tmphost_add_nse_hostscript (nmap, name, output);
}

/**
 * @brief Sublevel XML parser callback: handle an opening tcpsequence tag.
 *
 * @param[in] nmap  Handler to use.
 * @param[in] attrnames  NULL terminated list of attributes names.
 * @param[in] attrval  NULL terminated list of attributes values.
 */
void
xmltag_open_tcpsequence (nmap_t * nmap, const gchar ** attrnames,
                         const gchar ** attrval)
{
  if (!nmap->parser.in_host)
    return;

  nmap->tmphost.tcpseq_index = get_attr_value ("index", attrnames, attrval);
  nmap->tmphost.tcpseq_difficulty =
    get_attr_value ("difficulty", attrnames, attrval);
}

/**
 * @brief Sublevel XML parser callback: handle an opening ipidsequence tag.
 *
 * @param[in] nmap  Handler to use.
 * @param[in] attrnames  NULL terminated list of attributes names.
 * @param[in] attrval  NULL terminated list of attributes values.
 */
void
xmltag_open_ipidsequence (nmap_t * nmap, const gchar ** attrnames,
                          const gchar ** attrval)
{
  if (!nmap->parser.in_host)
    return;

  nmap->tmphost.ipidseq = get_attr_value ("class", attrnames, attrval);
}

/**
 * @brief Sublevel XML parser callback: handle an opening distance tag.
 *
 * @param[in] nmap  Handler to use.
 * @param[in] attrnames  NULL terminated list of attributes names.
 * @param[in] attrval  NULL terminated list of attributes values.
 */
void
xmltag_open_distance (nmap_t * nmap, const gchar ** attrnames,
                      const gchar ** attrval)
{
  gchar *diststr;

  if (!nmap->parser.in_host)
    return;

  diststr = get_attr_value ("value", attrnames, attrval);
  if (diststr)
    {
      nmap->tmphost.distance = atoi (diststr);
      g_free (diststr);
    }
}

/**
 * @brief Sublevel XML parser callback: handle an opening hop tag.
 *
 * @param[in] nmap  Handler to use.
 * @param[in] attrnames  NULL terminated list of attributes names.
 * @param[in] attrval  NULL terminated list of attributes values.
 */
void
xmltag_open_hop (nmap_t * nmap, const gchar ** attrnames,
                 const gchar ** attrval)
{
  int ttl;
  gchar *ttl_str;

  if (!nmap->parser.in_host)
    return;

  ttl_str = get_attr_value ("ttl", attrnames, attrval);
  ttl = atoi (ttl_str) - 1;        /* decrease ttl by one to use it as index */
  g_free (ttl_str);

  if (ttl < MAX_TRACE_HOPS)
    {
      if (!nmap->tmphost.trace[ttl].addr && !nmap->tmphost.trace[ttl].host
          && !nmap->tmphost.trace[ttl].rtt)
        {
          nmap->tmphost.trace[ttl].addr = get_attr_value ("ipaddr", attrnames,
                                                          attrval);
          nmap->tmphost.trace[ttl].host = get_attr_value ("host", attrnames,
                                                          attrval);
          nmap->tmphost.trace[ttl].rtt = get_attr_value ("rtt", attrnames,
                                                         attrval);
        }
      else
        dbg ("Inconsistent results: duplicate traceroute information!");
    }
  else
    dbg ("Trace TTL out of bounds: %d (max=%d)", ttl, MAX_TRACE_HOPS);
}

/**
 * @brief Sublevel XML parser callback: handle an closing host tag.
 *
 * @param[in] nmap  Handler to use.
 */
void
xmltag_close_host (nmap_t * nmap)
{
  nmap->parser.in_host = FALSE;
  current_host_saveall (nmap);
  current_host_reset (nmap);
}

/**
 * @brief Sublevel XML parser callback: handle an closing host tag.
 *
 * @param[in] nmap  Handler to use.
 */
void
xmltag_close_ports (nmap_t * nmap)
{
  nmap->parser.in_ports = FALSE;
}

/**
 * @brief Sublevel XML parser callback: handle an closing port tag.
 *
 * @param[in] nmap  Handler to use.
 */
void
xmltag_close_port (nmap_t * nmap)
{
  nmap->parser.in_port = FALSE;
  tmphost_add_port (nmap);
  memset (&nmap->tmpport, 0x00, sizeof (struct nmap_port));
}

/**
 * @brief Sublevel XML parser callback: handle an closing hostscript tag.
 *
 * @param[in] nmap  Handler to use.
 */
void
xmltag_close_hostscript (nmap_t * nmap)
{
  nmap->parser.in_hostscript = FALSE;
}

/**
 * @brief Helper function: get attribute value from the separate name/value
 *        tables.
 *
 * @param[in] name  Name of the attribute to lookup.
 * @param[in] attribute_names  Table of the attribute names.
 * @param[in] attribute_values  Table of the attribute values.
 *
 * @return the desired value or NULL if nothing was found
 */
gchar *
get_attr_value (const gchar * name, const gchar **
                attribute_names, const gchar ** attribute_values)
{
  int i;

  for (i = 0; attribute_names[i]; i++)
    if (g_strcmp0 (attribute_names[i], name) == 0)
      return g_strdup (attribute_values[i]);
  return NULL;
}

/**
 * @brief Dump current host object state into the knowledge base.
 *
 * @param[in] nmap  Handler to use.
 */
void
current_host_saveall (nmap_t * nmap)
{
  /* Host state: dead or alive */
  save_host_state (nmap);

  /* Open ports and services (all protocols included) */
  save_open_ports (nmap);

  /* OS fingerprinting results */
  save_detected_os (nmap);

  /* TCP/IP sensitive fields details */
  save_tcpseq_details (nmap);
  save_ipidseq_details (nmap);

  /* Traceroute */
  save_traceroute_details (nmap);

  /* NSE results */
  save_hostscripts (nmap);
  save_portscripts (nmap);
}

/**
 * @brief Store host state (host alive/dead) into the knowledge base.
 *
 * @param[in] nmap  Handler to use.
 */
void
save_host_state (nmap_t * nmap)
{
  gchar key[32];

  if (!nmap->tmphost.state)
    return;

  g_snprintf (key, sizeof (key), "%s/Host/State", nmap->tmphost.addr);
  plug_set_key (nmap->env, key, ARG_STRING, nmap->tmphost.state);
}

/**
 * @brief Save information about open ports for the current host into the
 *        knowledge base.
 *
 * @param[in] nmap  Handler to use.
 */
void
save_open_ports (nmap_t * nmap)
{
  struct nmap_port *p;

  for (p = nmap->tmphost.ports; p != NULL; p = p->next)
    {
      if (g_strcmp0 (p->state, "open") == 0)
        {
          gchar key[64];

          g_snprintf (key, sizeof (key), "%s/Ports/%s/%s", nmap->tmphost.addr,
                      p->proto, p->portno);
          plug_set_key (nmap->env, key, ARG_INT, (void *) 1);

          /* Register detected service */
          register_service (nmap, p);
        }
    }
}

/**
 * @brief Save information about a detected service (version) into the knowledge
 *        base.
 *
 * @param[in] nmap  Handler to use.
 * @param[in] p  Service description.
 */
void
register_service (nmap_t * nmap, struct nmap_port *p)
{
  gchar key[64];

  if (!p->portno || !p->proto || !p->service)
    return;

  /* TCP services aren't stored with the same syntax than the other layer 4
   * protocols. */
  if (g_strcmp0 (p->proto, "tcp") == 0)
    g_snprintf (key, sizeof (key), "%s/Services/%s", nmap->tmphost.addr,
                p->service);
  else
    g_snprintf (key, sizeof (key), "%s/Services/%s/%s", nmap->tmphost.addr,
                p->proto, p->service);
  plug_set_key (nmap->env, key, ARG_INT, GINT_TO_POINTER (atoi (p->portno)));

  /* The service detection system requires discovered services to be
   * registered under the "Known" label too */
  g_snprintf (key, sizeof (key), "%s/Known/%s/%s", nmap->tmphost.addr,
              p->proto, p->portno);
  plug_set_key (nmap->env, key, ARG_STRING, p->service);

  if (p->version)
    {
      /* Store version detection results if available */
      g_snprintf (key, sizeof (key), "%s/Version/%s/%s", nmap->tmphost.addr,
                  p->proto, p->portno);
      plug_set_key (nmap->env, key, ARG_STRING, p->version);
  }
}

/**
 * @brief Save information about detected operating system into the knowledge
 *        base.
 *
 * @param[in] nmap  Handler to use.
 */
void
save_detected_os (nmap_t * nmap)
{
  gchar key[32];

  if (!nmap->tmphost.best_os)
    return;

  g_snprintf (key, sizeof (key), "%s/Host/OS", nmap->tmphost.addr);
  plug_set_key (nmap->env, key, ARG_STRING, nmap->tmphost.best_os);
}

/**
 * @brief Save information about TCP sequence number generation into the
 *        knowledge base.
 *
 * @param[in] nmap  Handler to use.
 */
void
save_tcpseq_details (nmap_t * nmap)
{
  gchar key[64];

  if (!nmap->tmphost.tcpseq_index || !nmap->tmphost.tcpseq_difficulty)
    return;

  g_snprintf (key, sizeof (key), "%s/Host/tcp_seq_index", nmap->tmphost.addr);
  plug_set_key (nmap->env, key, ARG_STRING, nmap->tmphost.tcpseq_index);

  g_snprintf (key, sizeof (key), "%s/Host/tcp_seq_difficulty",
              nmap->tmphost.addr);
  plug_set_key (nmap->env, key, ARG_STRING, nmap->tmphost.tcpseq_difficulty);
}

/**
 * @brief Save information about IP ID generation into the knowledge base.
 *
 * @param[in] nmap  Handler to use.
 */
void
save_ipidseq_details (nmap_t * nmap)
{
  gchar key[32];

  if (!nmap->tmphost.ipidseq)
    return;

  g_snprintf (key, sizeof (key), "%s/Host/ipidseq", nmap->tmphost.addr);
  plug_set_key (nmap->env, key, ARG_STRING, nmap->tmphost.ipidseq);
}

/**
 * @brief Save information about network topology to the target (traceroute)
 *        into the knowledge base.
 *
 * @param[in] nmap  Handler to use.
 */
void
save_traceroute_details (nmap_t * nmap)
{
  int i;
  gchar key[64];

  if (!nmap->tmphost.distance || nmap->tmphost.distance >= MAX_TRACE_HOPS)
    return;

  g_snprintf (key, sizeof (key), "%s/Host/distance", nmap->tmphost.addr);
  plug_set_key (nmap->env, key, ARG_INT,
                GINT_TO_POINTER (nmap->tmphost.distance));

  for (i = 0; i < nmap->tmphost.distance; i++)
    {
      g_snprintf (key, sizeof (key), "%s/Host/traceroute/hops/%d",
                  nmap->tmphost.addr, i + 1);
      plug_set_key (nmap->env, key, ARG_STRING, nmap->tmphost.trace[i].addr);

      g_snprintf (key, sizeof (key), "%s/Host/traceroute/hops/%d/rtt",
                  nmap->tmphost.addr, i + 1);
      plug_set_key (nmap->env, key, ARG_STRING, nmap->tmphost.trace[i].rtt);

      g_snprintf (key, sizeof (key), "%s/Host/traceroute/hops/%d/host",
                  nmap->tmphost.addr, i + 1);
      plug_set_key (nmap->env, key, ARG_STRING, nmap->tmphost.trace[i].host);
    }
}

/**
 * @brief Save information about postrule NSE scripts into the knowledge base.
 *
 * @param[in] nmap  Handler to use.
 */
void
save_portscripts (nmap_t * nmap)
{
  struct nmap_port *port;

  for (port = nmap->tmphost.ports; port != NULL; port = port->next)
    {
      struct nse_script *script;

      for (script = port->port_scripts; script; script = script->next)
        {
          gchar key[128], portspec[16];

          g_snprintf (key, sizeof (key), "%s/NmapNSE/results/%s",
                      nmap->tmphost.addr, script->name);

          g_snprintf (portspec, sizeof (portspec), "%s/%s", port->proto,
                      port->portno);
          plug_set_key (nmap->env, key, ARG_STRING, portspec);

          g_strlcat (key, "/", sizeof (key));
          g_strlcat (key, portspec, sizeof (key));
          plug_set_key (nmap->env, key, ARG_STRING, script->output);
        }
    }
}

/**
 * @brief Save information about hostrule NSE scripts into the knowledge base.
 *
 * @param[in] nmap  Handler to use.
 */
void
save_hostscripts (nmap_t * nmap)
{
  struct nse_script *script;

  for (script = nmap->tmphost.host_scripts; script != NULL;
       script = script->next)
    {
      gchar key[128];

      g_snprintf (key, sizeof (key), "%s/NmapNSE/results/hostscripts/%s",
                  nmap->tmphost.addr, script->name);
      plug_set_key (nmap->env, key, ARG_STRING, script->output);
    }
}
