
#ifdef __cplusplus
extern "C" {
#endif
#include <cuishark_includes.h>
#include <capture_file.h>
#include <print_columns.h>
#ifdef __cplusplus
}
#endif

#include <iostream>
#include <queue>
#include <string>
#include <cuishark.h>

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <cuishark_includes.h>

#include <epan/packet.h>
#include <epan/epan.h>
#include <epan/epan_dissect.h>
#include <epan/to_str.h>
#include <epan/expert.h>
#include <epan/column-info.h>
#include <epan/color_filters.h>
#include <epan/prefs.h>
#include <epan/print.h>
#include <epan/charsets.h>
#include <wsutil/filesystem.h>
#include <version_info.h>
#include <wsutil/utf8_entities.h>
#include <ftypes/ftypes-int.h>
#include <wsutil/glib-compat.h>
#include <wiretap/wtap-int.h>


typedef struct csnode {
  std::vector<struct csnode> childs;
  std::string line;
  size_t level;
  bool isopen;
} node_t;


/*
 * For message passing
 * between frontend and backend
 */
typedef struct packet {
  struct csnode node;
  std::vector<uint8_t> data;
} packet_t;

std::queue<struct packet> msgqueue;
std::string ifnamestr = "??no-inited??";

#if 0
#define tshark_debug(...) g_warning(__VA_ARGS__)
#else
#define tshark_debug(...)
#endif

capture_file cfile;
static guint32 cum_bytes;
static frame_data ref_frame;
static frame_data prev_dis_frame;
static frame_data prev_cap_frame;
static gboolean line_buffered;
static output_fields_t* output_fields  = NULL;
static pf_flags protocolfilter_flags = PF_NONE;
static proto_node_children_grouper_func node_children_grouper = proto_node_group_children_by_unique;

static capture_options global_capture_opts;
static capture_session global_capture_session;
static info_data_t global_info_data;

static gboolean capture(void);
static void report_counts(void);
static void capture_cleanup(int);


typedef struct {
    int                  level;
    print_stream_t      *stream;
    gboolean             success;
    GSList              *src_list;
    print_dissections_e  print_dissections;
    gboolean             print_hex_for_data;
    packet_char_enc      encoding;
    GHashTable          *output_only_tables; /* output only these protocols */
} print_data;


static const guint8 *get_field_data(GSList *src_list, field_info *fi);
static gboolean print_hex_data_buffer(print_stream_t *stream, const guchar *cp, guint length, packet_char_enc encoding);
static void proto_tree_get_node_field_values(proto_node *node, gpointer data);
static int proto_data = -1;

size_t node_level(node_t* n) { return n->level; }

void print_csnode(node_t* node, int level)
{
  bool cond0 = level == -1;
  bool cond1 = level >= node->level;
  if (cond0 || cond1) {
    if (node->line != "") {
      for (size_t i=0; i<node->level; i++)
        printf("  ");
      printf("%s\n", node->line.c_str());
    }

    for (size_t i=0; i<node->childs.size(); i++) {
      print_csnode(&(node->childs[i]), level);
    }
  }
}

std::string proto_node_2_str(proto_node* node, gpointer data)
{
    field_info   *fi    = PNODE_FINFO(node);
    print_data   *pdata = (print_data*) data;
    const guint8 *pd;
    gchar         label_str[ITEM_LABEL_LENGTH];
    gchar        *label_ptr;

    /* dissection with an invisible proto tree? */
    g_assert(fi);

    /* Don't print invisible entries. */
    if (PROTO_ITEM_IS_HIDDEN(node) && (prefs.display_hidden_proto_items == FALSE)) {
        return "";
    }

    /* Give up if we've already gotten an error. */
    if (!pdata->success) {
        printf("OKASHII 194910\n");
        exit(1);
        return "OKASHII";
    }

    /* was a free format label produced? */
    if (fi->rep) {
        label_ptr = fi->rep->representation;
    }
    else { /* no, make a generic label */
        label_ptr = label_str;
        proto_item_fill_label(fi, label_str);
    }

    if (PROTO_ITEM_IS_GENERATED(node))
        label_ptr = g_strconcat("[", label_ptr, "]", NULL);

    std::string str = label_ptr;

    if (PROTO_ITEM_IS_GENERATED(node))
        g_free(label_ptr);

    if (!pdata->success)
        return str;

    /*
     * If -O is specified, only display the protocols which are in the
     * lookup table.  Only check on the first level: once we start printing
     * a tree, print the rest of the subtree.  Otherwise we won't print
     * subitems whose abbreviation doesn't match the protocol--for example
     * text items (whose abbreviation is simply "text").
     */
    if ((pdata->output_only_tables != NULL) && (pdata->level == 0)
        && (g_hash_table_lookup(pdata->output_only_tables,
            fi->hfinfo->abbrev) == NULL)) {
        return str;
    }

    /* If it's uninterpreted data, dump it (unless our caller will
       be printing the entire packet in hex). */
    if ((fi->hfinfo->id == proto_data) && (pdata->print_hex_for_data)) {
        /*
         * Find the data for this field.
         */
        pd = get_field_data(pdata->src_list, fi);
        if (pd) {
            if (!print_line(pdata->stream, 0, "")) {
                pdata->success = FALSE;
                return str;
            }
            if (!print_hex_data_buffer(pdata->stream,
                  pd, fi->length, pdata->encoding)) {
                pdata->success = FALSE;
                return str;
            }
        }
    }
    return str;
}


struct csnode proto_node_2_csnode(proto_node* node, gpointer data)
{
  if (!node) {
    printf("OKASHII a193n2\n");
    exit(1);
  }

  print_data   *pdata = (print_data*) data;

  struct csnode csnode;
  csnode.line = proto_node_2_str(node, pdata);
  csnode.level = pdata->level;
  csnode.isopen = false;

  node = node->first_child;
  while (node != NULL) {
    proto_node* current = node;
    node = current->next;

    pdata->level ++;
    struct csnode child_csnode = proto_node_2_csnode((proto_tree*)current, pdata);
    pdata->level --;
    csnode.childs.push_back(child_csnode);
  }

  return csnode;
}


static const guint8 *
get_field_data(GSList *src_list, field_info *fi)
{
    GSList   *src_le;
    tvbuff_t *src_tvb;
    gint      length, tvbuff_length;
    struct data_source *src;

    for (src_le = src_list; src_le != NULL; src_le = src_le->next) {
        src = (struct data_source *)src_le->data;
        src_tvb = get_data_source_tvb(src);
        if (fi->ds_tvb == src_tvb) {
            tvbuff_length = tvb_captured_length_remaining(src_tvb,
                                                 fi->start);
            if (tvbuff_length < 0) {
                return NULL;
            }
            length = fi->length;
            if (length > tvbuff_length)
                length = tvbuff_length;
            return tvb_get_ptr(src_tvb, fi->start, length);
        }
    }
    g_assert_not_reached();
    return NULL;  /* not found */
}



#define MAX_OFFSET_LEN   8       /* max length of hex offset of bytes */
#define BYTES_PER_LINE  16      /* max byte values printed on a line */
#define HEX_DUMP_LEN    (BYTES_PER_LINE*3)
#define DATA_DUMP_LEN   (HEX_DUMP_LEN + 2 + BYTES_PER_LINE)
#define MAX_LINE_LEN    (MAX_OFFSET_LEN + 2 + DATA_DUMP_LEN)
static gboolean
print_hex_data_buffer(print_stream_t *stream, const guchar *cp,
                      guint length, packet_char_enc encoding)
{
    register unsigned int ad, i, j, k, l;
    guchar                c;
    gchar                 line[MAX_LINE_LEN + 1];
    unsigned int          use_digits;

    static gchar binhex[16] = {
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

    /*
     * How many of the leading digits of the offset will we supply?
     * We always supply at least 4 digits, but if the maximum offset
     * won't fit in 4 digits, we use as many digits as will be needed.
     */
    if (((length - 1) & 0xF0000000) != 0)
        use_digits = 8; /* need all 8 digits */
    else if (((length - 1) & 0x0F000000) != 0)
        use_digits = 7; /* need 7 digits */
    else if (((length - 1) & 0x00F00000) != 0)
        use_digits = 6; /* need 6 digits */
    else if (((length - 1) & 0x000F0000) != 0)
        use_digits = 5; /* need 5 digits */
    else
        use_digits = 4; /* we'll supply 4 digits */

    ad = 0;
    i = 0;
    j = 0;
    k = 0;
    while (i < length) {
        if ((i & 15) == 0) {
            /*
             * Start of a new line.
             */
            j = 0;
            l = use_digits;
            do {
                l--;
                c = (ad >> (l*4)) & 0xF;
                line[j++] = binhex[c];
            } while (l != 0);
            line[j++] = ' ';
            line[j++] = ' ';
            memset(line+j, ' ', DATA_DUMP_LEN);

            /*
             * Offset in line of ASCII dump.
             */
            k = j + HEX_DUMP_LEN + 2;
        }
        c = *cp++;
        line[j++] = binhex[c>>4];
        line[j++] = binhex[c&0xf];
        j++;
        if (encoding == PACKET_CHAR_ENC_CHAR_EBCDIC) {
            c = EBCDIC_to_ASCII1(c);
        }
        line[k++] = ((c >= ' ') && (c < 0x7f)) ? c : '.';
        i++;
        if (((i & 15) == 0) || (i == length)) {
            /*
             * We'll be starting a new line, or
             * we're finished printing this buffer;
             * dump out the line we've constructed,
             * and advance the offset.
             */
            line[k] = '\0';
            if (!print_line(stream, 0, line))
                return FALSE;
            ad += 16;
        }
    }
    return TRUE;
}


std::vector<struct csnode>
get_proto_tree(print_dissections_e print_dissections, gboolean print_hex,
                 epan_dissect_t *edt, GHashTable *output_only_tables)
{
    print_data data;
    data.level              = 0;
    data.stream             = print_stream_text_stdio_new(stdout);
    data.success            = TRUE;
    data.src_list           = edt->pi.data_src;
    data.encoding           = (packet_char_enc)edt->pi.fd->flags.encoding;
    data.print_dissections  = print_dissections;
    data.print_hex_for_data = !print_hex;
    data.output_only_tables = output_only_tables;

    std::vector<struct csnode> cstree;
    proto_node *node = edt->tree->first_child;
    while (node != NULL) {
      proto_node *current;
      current = node;
      node    = current->next;

      struct csnode cstree_node = proto_node_2_csnode((proto_tree *)current, &data);
      cstree.push_back(cstree_node);
    }

    return cstree;
}


static gboolean process_cap_file(capture_file *, char *, int, gboolean, int, gint64);
static gboolean process_packet(capture_file *cf,
    epan_dissect_t *edt, gint64 offset, wtap_rec *rec,
    const guchar *pd, guint tap_flags);
static gboolean print_packet(capture_file *cf, epan_dissect_t *edt);
static GHashTable *output_only_tables = NULL;
static gchar *volatile cf_name = NULL;
static volatile int in_file_type = WTAP_TYPE_AUTO;
static volatile int out_file_type = WTAP_FILE_TYPE_SUBTYPE_PCAPNG;
static volatile gboolean out_file_name_res = FALSE;

int
cuishark_init(int argc, char *argv[])
{
  int                  opt;
  gboolean             arg_error = FALSE;
  volatile int         exit_status = EXIT_SUCCESS;
  gboolean             start_capture = FALSE;
  gchar               *dfilter = NULL;
  dfilter_t           *rfcode = NULL;
  dfilter_t           *dfcode = NULL;
  gchar               *err_msg;
  e_prefs             *prefs_p;
  gchar               *output_only = NULL;
  const char          *volatile exp_pdu_filename = NULL;

  tshark_debug("tshark started with %d args", argc);
  setlocale(LC_ALL, "");
  init_process_policies();
  relinquish_special_privs_perm();

  char* init_progfile_dir_error = init_progfile_dir(argv[0], cuishark_init);
  if (init_progfile_dir_error != NULL) {
    fprintf(stderr,
            "tshark: Can't get pathname of directory"
            "containing the tshark program: %s.\n"
            "It won't be possible to capture traffic.\n"
            "Report this to the Wireshark developers.",
            init_progfile_dir_error);
    g_free(init_progfile_dir_error);
  }

  capture_opts_init(&global_capture_opts);
  capture_session_init(&global_capture_session, &cfile);

  timestamp_set_type(TS_RELATIVE);
  timestamp_set_precision(TS_PREC_AUTO);
  timestamp_set_seconds_type(TS_SECONDS_DEFAULT);
  wtap_init(TRUE);

  if (!epan_init(register_all_protocols,
        register_all_protocol_handoffs, NULL, NULL)) {
    exit_status = INIT_FAILED;
    goto clean_exit;
  }

  tshark_debug("tshark reading settings");
  prefs_p = epan_load_settings();
  read_filter_list(CFILTER_LIST);
  cap_file_init(&cfile);
  output_fields = output_fields_new();

  optind = 0; /* don't remove for getopt() */
  while ((opt = getopt(argc, argv, "i:r:Y:")) != -1) {
    switch (opt) {
    case 'i':        /* Use interface x */
      exit_status = capture_opts_add_opt(&global_capture_opts, opt, optarg, &start_capture);
      if (exit_status != 0) {
        goto clean_exit;
      }
      ifnamestr = optarg;
      break;
    case 'Y':
      dfilter = optarg;
      break;
    case 'r':        /* Read capture file x */
      cf_name = g_strdup(optarg);
      ifnamestr = optarg;
      break;
    default:
      exit_status = INVALID_OPTION;
      goto clean_exit;
      break;
    }
  }

  if (optind < argc) {
    if (cf_name != NULL) {
      if (dfilter != NULL) {
        cmdarg_err("Display filters were specified both with \"-d\" "
            "and with additional command-line arguments.");
        exit_status = INVALID_OPTION;
        goto clean_exit;
      }
      dfilter = get_args_as_string(argc, argv, optind);
    } else {

      if (global_capture_opts.default_options.cfilter) {
        cmdarg_err("A default capture filter was specified both with \"-f\""
            " and with additional command-line arguments.");
        exit_status = INVALID_OPTION;
        goto clean_exit;
      }
      for (guint i = 0; i < global_capture_opts.ifaces->len; i++) {
        interface_options *interface_opts;
        interface_opts = &g_array_index(global_capture_opts.ifaces, interface_options, i);
        if (interface_opts->cfilter == NULL) {
          interface_opts->cfilter = get_args_as_string(argc, argv, optind);
        } else {
          cmdarg_err("A capture filter was specified both with \"-f\""
              " and with additional command-line arguments.");
          exit_status = INVALID_OPTION;
          goto clean_exit;
        }
      }
      global_capture_opts.default_options.cfilter = get_args_as_string(argc, argv, optind);

    }
  }

  if (!global_capture_opts.saving_to_file) {
    /* We're not saving the capture to a file; if "-q" wasn't specified,
       we should print packet information */
  } else {
    /* We're saving to a file; if we're writing to the standard output.
       and we'll also be writing dissected packets to the standard
       output, reject the request.  At best, we could redirect that
       to the standard error; we *can't* write both to the standard
       output and have either of them be useful. */
    if (strcmp(global_capture_opts.save_file, "-") == 0) {
      cmdarg_err("You can't write both raw packet data and dissected packets"
          " to the standard output.");
      exit_status = INVALID_OPTION;
      goto clean_exit;
    }
  }

  if (arg_error) {
    exit_status = INVALID_OPTION;
    goto clean_exit;
  }

  if (output_only != NULL) {
    char *ps;

    cmdarg_err("-O requires -V");
    exit_status = INVALID_OPTION;
    goto clean_exit;

    output_only_tables = g_hash_table_new (g_str_hash, g_str_equal);
    for (ps = strtok (output_only, ","); ps; ps = strtok (NULL, ",")) {
      g_hash_table_insert(output_only_tables, (gpointer)ps, (gpointer)ps);
    }
  }

  if (cf_name) {
    /*
     * "-r" was specified, so we're reading a capture file.
     * Capture options don't apply here.
     */

    /* We don't support capture filters when reading from a capture file
       (the BPF compiler doesn't support all link-layer types that we
       support in capture files we read). */
    if (global_capture_opts.default_options.cfilter) {
      cmdarg_err("Only read filters, not capture filters, "
        "can be specified when reading a capture file.");
      exit_status = INVALID_OPTION;
      goto clean_exit;
    }
    if (global_capture_opts.multi_files_on) {
      cmdarg_err("Multiple capture files requested, but "
                 "a capture isn't being done.");
      exit_status = INVALID_OPTION;
      goto clean_exit;
    }
    if (global_capture_opts.has_file_duration) {
      cmdarg_err("Switching capture files after a time period was specified, but "
                 "a capture isn't being done.");
      exit_status = INVALID_OPTION;
      goto clean_exit;
    }
    if (global_capture_opts.has_file_interval) {
      cmdarg_err("Switching capture files after a time interval was specified, but "
                 "a capture isn't being done.");
      exit_status = INVALID_OPTION;
      goto clean_exit;
    }
    if (global_capture_opts.has_ring_num_files) {
      cmdarg_err("A ring buffer of capture files was specified, but "
        "a capture isn't being done.");
      exit_status = INVALID_OPTION;
      goto clean_exit;
    }
    if (global_capture_opts.has_autostop_files) {
      cmdarg_err("A maximum number of capture files was specified, but "
        "a capture isn't being done.");
      exit_status = INVALID_OPTION;
      goto clean_exit;
    }
    if (global_capture_opts.capture_comment) {
      cmdarg_err("A capture comment was specified, but "
        "a capture isn't being done.\nThere's no support for adding "
        "a capture comment to an existing capture file.");
      exit_status = INVALID_OPTION;
      goto clean_exit;
    }

    /* Note: TShark now allows the restriction of a _read_ file by packet count
     * and byte count as well as a write file. Other autostop options remain valid
     * only for a write file.
     */
    if (global_capture_opts.has_autostop_duration) {
      cmdarg_err("A maximum capture time was specified, but "
        "a capture isn't being done.");
      exit_status = INVALID_OPTION;
      goto clean_exit;
    }
  } else {
    /*
     * "-r" wasn't specified, so we're doing a live capture.
     */

    if (global_capture_opts.saving_to_file) {
      /* They specified a "-w" flag, so we'll be saving to a capture file. */

      /* When capturing, we only support writing pcap or pcapng format. */
      if (out_file_type != WTAP_FILE_TYPE_SUBTYPE_PCAP &&
          out_file_type != WTAP_FILE_TYPE_SUBTYPE_PCAPNG) {
        cmdarg_err("Live captures can only be saved in pcap or pcapng format.");
        exit_status = INVALID_OPTION;
        goto clean_exit;
      }
      if (global_capture_opts.capture_comment &&
          out_file_type != WTAP_FILE_TYPE_SUBTYPE_PCAPNG) {
        cmdarg_err("A capture comment can only be written to a pcapng file.");
        exit_status = INVALID_OPTION;
        goto clean_exit;
      }
      if (global_capture_opts.multi_files_on) {
        /* Multiple-file mode doesn't work under certain conditions:
           a) it doesn't work if you're writing to the standard output;
           b) it doesn't work if you're writing to a pipe;
        */
        if (strcmp(global_capture_opts.save_file, "-") == 0) {
          cmdarg_err("Multiple capture files requested, but "
            "the capture is being written to the standard output.");
          exit_status = INVALID_OPTION;
          goto clean_exit;
        }
        if (global_capture_opts.output_to_pipe) {
          cmdarg_err("Multiple capture files requested, but "
            "the capture file is a pipe.");
          exit_status = INVALID_OPTION;
          goto clean_exit;
        }
        if (!global_capture_opts.has_autostop_filesize &&
            !global_capture_opts.has_file_duration &&
            !global_capture_opts.has_file_interval) {
          cmdarg_err("Multiple capture files requested, but "
            "no maximum capture file size, duration or interval was specified.");
          exit_status = INVALID_OPTION;
          goto clean_exit;
        }
      }

      if (dfilter != NULL) {
        cmdarg_err("Display filters aren't supported when capturing and saving the captured packets.");
        exit_status = INVALID_OPTION;
        goto clean_exit;
      }
      global_capture_opts.use_pcapng = (out_file_type == WTAP_FILE_TYPE_SUBTYPE_PCAPNG) ? TRUE : FALSE;
    } else {
      /* They didn't specify a "-w" flag, so we won't be saving to a
         capture file.  Check for options that only make sense if
         we're saving to a file. */
      if (global_capture_opts.has_autostop_filesize) {
        cmdarg_err("Maximum capture file size specified, but "
         "capture isn't being saved to a file.");
        exit_status = INVALID_OPTION;
        goto clean_exit;
      }
      if (global_capture_opts.multi_files_on) {
        cmdarg_err("Multiple capture files requested, but "
          "the capture isn't being saved to a file.");
        exit_status = INVALID_OPTION;
        goto clean_exit;
      }
      if (global_capture_opts.capture_comment) {
        cmdarg_err("A capture comment was specified, but "
          "the capture isn't being saved to a file.");
        exit_status = INVALID_OPTION;
        goto clean_exit;
      }
    }
  }

  prefs_apply_all();

  {
    GSList* it = NULL;
    GSList *invalid_fields = output_fields_valid(output_fields);
    if (invalid_fields != NULL) {

      cmdarg_err("Some fields aren't valid:");
      for (it=invalid_fields; it != NULL; it = g_slist_next(it)) {
        cmdarg_err_cont("\t%s", (gchar *)it->data);
      }
      g_slist_free(invalid_fields);
      exit_status = INVALID_OPTION;
      goto clean_exit;
    }
  }

  /* We currently don't support taps, or printing dissected packets,
     if we're writing to a pipe. */
  if (global_capture_opts.saving_to_file &&
      global_capture_opts.output_to_pipe) {
    if (tap_listeners_require_dissection()) {
      cmdarg_err("Taps aren't supported when saving to a pipe.");
      exit_status = INVALID_OPTION;
      goto clean_exit;
    }
    cmdarg_err("Printing dissected packets isn't supported when saving to a pipe.");
    exit_status = INVALID_OPTION;
    goto clean_exit;
  }

  if (ex_opt_count("read_format") > 0) {
    const gchar* name = ex_opt_get_next("read_format");
    in_file_type = open_info_name_to_type(name);
    if (in_file_type == WTAP_TYPE_AUTO) {
      cmdarg_err("\"%s\" isn't a valid read file format type", name? name : "");
      /* list_read_capture_types(); */
      exit_status = INVALID_OPTION;
      goto clean_exit;
    }
  }

  timestamp_set_type(global_dissect_options.time_format);

  /*
   * Enabled and disabled protocols and heuristic dissectors as per
   * command-line options.
   */
  if (!setup_enabled_and_disabled_protocols()) {
    exit_status = INVALID_OPTION;
    goto clean_exit;
  }

  /* Build the column format array */
  build_column_format_array(&cfile.cinfo, prefs_p->num_cols, TRUE);

  capture_opts_trim_snaplen(&global_capture_opts, MIN_PACKET_SIZE);
  capture_opts_trim_ring_num_files(&global_capture_opts);

  cfile.rfcode = rfcode;
  if (dfilter != NULL) {
    cuishark_apply_dfilter(dfilter);
  }

  /* If we're printing as text or PostScript, we have
     to create a print stream. */
  cfile.provider.frames = new_frame_data_sequence();

clean_exit:
  return exit_status;
} /* cuishark_main */

int cuishark_capture()
{
  int exit_status = 0;

  /*
   * Starting Capture file or network-interface
   * check cf_name was set, If it was set, read capturefile
   * Else, capture the network-interface
   */
  if (cf_name) {
    tshark_debug("tshark: Opening capture file: %s", cf_name);

    int err;
    if (cf_open(&cfile, cf_name, in_file_type, FALSE, &err) != CF_OK) {
      fprintf(stderr, "cf_open failed \n");
      epan_cleanup();
      extcap_cleanup();
      exit(1);
    }

    start_requested_stats();

    /* Do we need to do dissection of packets?  That depends on, among
       other things, what taps are listening, so determine that after
       starting the statistics taps. */

    /* Process the packets in the file */
    tshark_debug("tshark: invoking process_cap_file() to process the packets");
    TRY {
      volatile bool success = process_cap_file(&cfile,
          global_capture_opts.save_file, out_file_type, out_file_name_res,
          global_capture_opts.has_autostop_packets ? global_capture_opts.autostop_packets : 0,
          global_capture_opts.has_autostop_filesize ? global_capture_opts.autostop_filesize : 0);
      if (!success) exit_status = 2;
    }
    CATCH(OutOfMemoryError) {
      fprintf(stderr,
              "Out Of Memory.\n"
              "\n"
              "Sorry, but TShark has to terminate now.\n"
              "\n"
              "More information and workarounds can be found at\n"
              "https://wiki.wireshark.org/KnownBugs/OutOfMemory\n");
    } ENDTRY;

  } else {

    tshark_debug("tshark: performing live capture");
    start_requested_stats();
    capture();
    exit_status = global_capture_session.fork_child_status;
  }

  return exit_status;
}

void cuishark_fini()
{
  g_free(cf_name);
  draw_tap_listeners(TRUE);
  epan_free(cfile.epan);
  epan_cleanup();
  extcap_cleanup();

  output_fields_free(output_fields);
  output_fields = NULL;

  if (cfile.provider.frames != NULL) {
    free_frame_data_sequence(cfile.provider.frames);
    cfile.provider.frames = NULL;
  }
  capture_opts_cleanup(&global_capture_opts);
  col_cleanup(&cfile.cinfo);
  wtap_cleanup();
  free_progdirs();
  free_filter_lists();
  cf_close(&cfile);
}

  bool loop_running = TRUE;
  guint32 packet_count = 0;

bool cuishark_loop_running() { return loop_running; }

typedef struct pipe_input_tag {
  gint             source;
  gpointer         user_data;
  ws_process_id   *child_process;
  pipe_input_cb_t  input_cb;
  guint            pipe_input_id;
} pipe_input_t;

static pipe_input_t pipe_input;

void
pipe_input_set_handler(gint source, gpointer user_data,
    ws_process_id *child_process, pipe_input_cb_t input_cb)
{

  pipe_input.source         = source;
  pipe_input.child_process  = child_process;
  pipe_input.user_data      = user_data;
  pipe_input.input_cb       = input_cb;

}

static gboolean
capture(void)
{
  GString          *str;
  struct sigaction  action, oldaction;

  /* Create new dissection section. */
  epan_free(cfile.epan);
  cfile.epan = tshark_epan_new(&cfile);

  memset(&action, 0, sizeof(action));
  action.sa_handler = capture_cleanup;
  action.sa_flags = SA_RESTART;
  sigemptyset(&action.sa_mask);
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGINT, &action, NULL);
  sigaction(SIGHUP, NULL, &oldaction);
  if (oldaction.sa_handler == SIG_DFL)
    sigaction(SIGHUP, &action, NULL);

  global_capture_session.state = CAPTURE_PREPARING;

  /* Let the user know which interfaces were chosen. */
  for (uint32_t i = 0; i < global_capture_opts.ifaces->len; i++) {
    interface_options *interface_opts;
    interface_opts = &g_array_index(global_capture_opts.ifaces, interface_options, i);
    interface_opts->descr = get_interface_descriptive_name(interface_opts->name);
  }
  str = get_iface_list_string(&global_capture_opts, IFLIST_QUOTE_IF_DESCRIPTION);
  fflush(stderr);
  g_string_free(str, TRUE);

  bool ret = sync_pipe_start(&global_capture_opts, &global_capture_session, &global_info_data, NULL);
  if (!ret) return FALSE;

  loop_running = TRUE;
  TRY {
    while (loop_running)
    {
        // printf("input callback(%d,%p)\n", pipe_input.source, pipe_input.user_data);
        /* Call the real handler */
        if (!pipe_input.input_cb(pipe_input.source, pipe_input.user_data)) {
          g_log(NULL, G_LOG_LEVEL_DEBUG, "input pipe closed");
          return FALSE;
        }
    }
  } CATCH(OutOfMemoryError) {
    fprintf(stderr,
      "Out Of Memory.\n\n"
      "Sorry, but TShark has to terminate now.\n\n"
      "More information and workarounds can be found at\n"
      "https://wiki.wireshark.org/KnownBugs/OutOfMemory\n");
    exit(1);
  } ENDTRY;
  return TRUE;
}

/* capture child detected an error */
void
capture_input_error_message(capture_session *cap_session _U_, char *error_msg, char *secondary_error_msg)
{
  cmdarg_err("%s", error_msg);
  cmdarg_err_cont("%s", secondary_error_msg);
}


/* capture child detected an capture filter related error */
void
capture_input_cfilter_error_message(capture_session *cap_session, guint i, char *error_message)
{
  capture_options *capture_opts = cap_session->capture_opts;
  dfilter_t         *rfcode = NULL;
  interface_options *interface_opts;

  g_assert(i < capture_opts->ifaces->len);
  interface_opts = &g_array_index(capture_opts->ifaces, interface_options, i);

  if (dfilter_compile(interface_opts->cfilter, &rfcode, NULL) && rfcode != NULL) {
    cmdarg_err(
      "Invalid capture filter \"%s\" for interface '%s'.\n"
      "\n"
      "That string looks like a valid display filter; however, it isn't a valid\n"
      "capture filter (%s).\n"
      "\n"
      "Note that display filters and capture filters don't have the same syntax,\n"
      "so you can't use most display filter expressions as capture filters.\n"
      "\n"
      "See the User's Guide for a description of the capture filter syntax.",
      interface_opts->cfilter, interface_opts->descr, error_message);
    dfilter_free(rfcode);
  } else {
    cmdarg_err(
      "Invalid capture filter \"%s\" for interface '%s'.\n"
      "\n"
      "That string isn't a valid capture filter (%s).\n"
      "See the User's Guide for a description of the capture filter syntax.",
      interface_opts->cfilter, interface_opts->descr, error_message);
  }
}


/* capture child tells us we have a new (or the first) capture file */
gboolean
capture_input_new_file(capture_session *cap_session, gchar *new_file)
{
  capture_options *capture_opts = cap_session->capture_opts;
  capture_file *cf = (capture_file *) cap_session->cf;
  g_assert(cap_session->state == CAPTURE_PREPARING || cap_session->state == CAPTURE_RUNNING);
  gboolean is_tempfile = TRUE;
  capture_opts->save_file = g_strdup(new_file);
  ((capture_file *)cap_session->cf)->open_type = WTAP_TYPE_AUTO;

  int err;
  cf_status_t ret = cf_open((capture_file *)cap_session->cf,
      capture_opts->save_file, WTAP_TYPE_AUTO, is_tempfile, &err);
  switch(ret) {
    case CF_OK: break;
    case CF_ERROR:
      g_free(capture_opts->save_file);
      capture_opts->save_file = NULL;
      return FALSE;
  }

  cap_session->state = CAPTURE_RUNNING;
  return TRUE;
}


/* capture child tells us we have new packets to read */
void
capture_input_new_packets(capture_session *cap_session, int to_read)
{
  gboolean      ret;
  int           err;
  gchar        *err_info;
  gint64        data_offset;
  capture_file *cf = (capture_file *)cap_session->cf;
  gboolean      filtering_tap_listeners;
  guint         tap_flags;

  /* Do we have any tap listeners with filters? */
  filtering_tap_listeners = have_filtering_tap_listeners();

  /* Get the union of the flags for all tap listeners. */
  tap_flags = union_of_tap_listener_flags();

  gboolean create_proto_tree;
  epan_dissect_t *edt;

  /*
   * Determine whether we need to create a protocol tree.
   * We do if:
   *
   *    we're going to apply a read filter;
   *    we're going to apply a display filter;
   *    we're going to print the protocol tree;
   *    one of the tap listeners is going to apply a filter;
   *    one of the tap listeners requires a protocol tree;
   *    a postdissector wants field values or protocols
   *    on the first pass;
   *    we have custom columns (which require field values, which
   *    currently requires that we build a protocol tree).
   */
  create_proto_tree = TRUE;

  /* The protocol tree will be "visible", i.e., printed, only if we're
     printing packet details, which is true if we're printing stuff
     ("print_packet_info" is true) and we're in verbose mode
     ("packet_details" is true). */
  edt = epan_dissect_new(cf->epan, create_proto_tree, TRUE);

  while (to_read-- && cf->provider.wth) {

    wtap_cleareof(cf->provider.wth);

    bool ret = wtap_read(cf->provider.wth, &err, &err_info, &data_offset);
    if (ret == FALSE) {
      sync_pipe_stop(cap_session);
      wtap_close(cf->provider.wth);
      cf->provider.wth = NULL;
      break;
    }

    reset_epan_mem(cf, edt, create_proto_tree, TRUE);
    ret = process_packet(cf, edt, data_offset,
             wtap_get_rec(cf->provider.wth),
             wtap_get_buf_ptr(cf->provider.wth), tap_flags);
    if (ret != FALSE) packet_count++;

  } /*  while (to_read-- && cf->provider.wth) */

  epan_dissect_free(edt);
  edt = NULL;
}


static void
capture_cleanup(int signum _U_)
{
  sync_pipe_stop(&global_capture_session);
}


static gboolean
process_cap_file(capture_file *cf, char *save_file, int out_file_type,
    gboolean out_file_name_res, int max_packet_count, gint64 max_byte_count)
{
  gboolean     success = TRUE;
  gint         linktype;
  int          snapshot_length;
  wtap_dumper *pdh;
  guint32      framenum;
  int          err = 0, err_pass1 = 0;
  gchar       *err_info = NULL, *err_info_pass1 = NULL;
  gint64       data_offset;
  gboolean     filtering_tap_listeners;
  guint        tap_flags;
  GArray                      *shb_hdrs = NULL;
  wtapng_iface_descriptions_t *idb_inf = NULL;
  GArray                      *nrb_hdrs = NULL;
  wtap_rec     rec;
  Buffer       buf;
  epan_dissect_t *edt = NULL;
  char                        *shb_user_appl;

  wtap_rec_init(&rec);

  idb_inf = wtap_file_get_idb_info(cf->provider.wth);
  if (idb_inf->interface_data->len > 1) {
    linktype = WTAP_ENCAP_PER_PACKET;
  } else {
    linktype = wtap_file_encap(cf->provider.wth);
  }

  if (save_file != NULL) {
    /* Set up to write to the capture file. */
    snapshot_length = wtap_snapshot_length(cf->provider.wth);
    if (snapshot_length == 0) {
      /* Snapshot length of input file not known. */
      snapshot_length = WTAP_MAX_PACKET_SIZE_STANDARD;
    }
    tshark_debug("tshark: snapshot_length = %d", snapshot_length);

    shb_hdrs = wtap_file_get_shb_for_new_file(cf->provider.wth);
    nrb_hdrs = wtap_file_get_nrb_for_new_file(cf->provider.wth);

    /* If we don't have an application name add Tshark */
    if (wtap_block_get_string_option_value(g_array_index(shb_hdrs, wtap_block_t, 0),
          OPT_SHB_USERAPPL, &shb_user_appl) != WTAP_OPTTYPE_SUCCESS) {
        /* this is free'd by wtap_block_free() later */
        wtap_block_add_string_option_format(g_array_index(shb_hdrs, wtap_block_t, 0),
            OPT_SHB_USERAPPL, "TShark (Wireshark) %s", get_ws_vcs_version_info());
    }

    if (linktype != WTAP_ENCAP_PER_PACKET &&
        out_file_type == WTAP_FILE_TYPE_SUBTYPE_PCAP) {
        tshark_debug("tshark: writing PCAP format to %s", save_file);
        if (strcmp(save_file, "-") == 0) {
          /* Write to the standard output. */
          pdh = wtap_dump_open_stdout(out_file_type, linktype,
              snapshot_length, FALSE /* compressed */, &err);
        } else {
          pdh = wtap_dump_open(save_file, out_file_type, linktype,
              snapshot_length, FALSE /* compressed */, &err);
        }
    } else {
        tshark_debug("tshark: writing format type %d, to %s", out_file_type, save_file);
        if (strcmp(save_file, "-") == 0) {
          /* Write to the standard output. */
          pdh = wtap_dump_open_stdout_ng(out_file_type, linktype,
              snapshot_length, FALSE /* compressed */, shb_hdrs, idb_inf, nrb_hdrs, &err);
        } else {
          pdh = wtap_dump_open_ng(save_file, out_file_type, linktype,
              snapshot_length, FALSE /* compressed */, shb_hdrs, idb_inf, nrb_hdrs, &err);
        }
    }

    g_free(idb_inf);
    idb_inf = NULL;

    if (pdh == NULL) {
      /* We couldn't set up to write to the capture file. */
      cfile_dump_open_failure_message("TShark", save_file, err, out_file_type);
      success = FALSE;

      // goto out;
      wtap_close(cf->provider.wth);
      cf->provider.wth = NULL;
      wtap_block_array_free(shb_hdrs);
      wtap_block_array_free(nrb_hdrs);
      return success;
    }

  } else {
    g_free(idb_inf);
    idb_inf = NULL;
    pdh = NULL;
  }

  /* Do we have any tap listeners with filters? */
  filtering_tap_listeners = have_filtering_tap_listeners();

  /* Get the union of the flags for all tap listeners. */
  tap_flags = union_of_tap_listener_flags();

  framenum = 0;
  gboolean create_proto_tree = FALSE;

  /*
   * Determine whether we need to create a protocol tree.
   * We do if:
   *
   *    we're going to apply a read filter;
   *
   *    we're going to apply a display filter;
   *
   *    we're going to print the protocol tree;
   *
   *    one of the tap listeners is going to apply a filter;
   *
   *    one of the tap listeners requires a protocol tree;
   *
   *    a postdissector wants field values or protocols
   *    on the first pass;
   *
   *    we have custom columns (which require field values, which
   *    currently requires that we build a protocol tree).
   */
  create_proto_tree = TRUE;
  tshark_debug("tshark: create_proto_tree = %s", create_proto_tree ? "TRUE" : "FALSE");

  /* The protocol tree will be "visible", i.e., printed, only if we're
     printing packet details, which is true if we're printing stuff
     ("print_packet_info" is true) and we're in verbose mode
     ("packet_details" is true). */
  edt = epan_dissect_new(cf->epan, create_proto_tree, TRUE);

  while (true) {

    bool ret = wtap_read(cf->provider.wth, &err, &err_info, &data_offset);
    if (!ret) break;

    framenum++;
    reset_epan_mem(cf, edt, create_proto_tree, TRUE);
    ret = process_packet(cf, edt, data_offset,
                  wtap_get_rec(cf->provider.wth),
                  wtap_get_buf_ptr(cf->provider.wth), tap_flags);
    if (ret) {
      /* Either there's no read filtering or this packet passed the
         filter, so, if we're writing to a capture file, write
         this packet out. */
      if (pdh != NULL) {
        tshark_debug("tshark: writing packet #%d to outfile", framenum);
        bool ret = wtap_dump(pdh, wtap_get_rec(cf->provider.wth),
                    wtap_get_buf_ptr(cf->provider.wth), &err, &err_info);
        if (!ret) {
          /* Error writing to a capture file */
          tshark_debug("tshark: error writing to a capture file (%d)", err);
          cfile_write_failure_message("TShark", cf->filename, save_file,
                                      err, err_info, framenum, out_file_type);
          wtap_dump_close(pdh, &err);
          wtap_block_array_free(shb_hdrs);
          wtap_block_array_free(nrb_hdrs);
          exit(2);
        }
      }
    }

    bool exit_cond0 = (--max_packet_count == 0);
    bool exit_cond1 = (max_byte_count != 0 && data_offset >= max_byte_count);
    if (exit_cond0 || exit_cond1) {
      tshark_debug("tshark: max_packet_count (%d) or max_byte_count "
                   "(%" G_GINT64_MODIFIER "d/%" G_GINT64_MODIFIER "d) reached",
                   max_packet_count, data_offset, max_byte_count);
      err = 0;
      break;
    }

  } /* while (true) */

  epan_dissect_free(edt);
  edt = NULL;
  wtap_rec_cleanup(&rec);
  wtap_block_array_free(shb_hdrs);
  wtap_block_array_free(nrb_hdrs);
  return success;
}

static gboolean
process_packet(capture_file *cf, epan_dissect_t *edt, gint64 offset,
                           wtap_rec *rec, const guchar *pd,
                           guint tap_flags)
{
  cf->count++;
  gboolean passed = TRUE;

  frame_data      fdata;
  frame_data_init(&fdata, cf->count, rec, offset, cum_bytes);

  if (edt) {
    if ( (gbl_resolv_flags.mac_name || gbl_resolv_flags.network_name ||
        gbl_resolv_flags.transport_name))
      host_name_lookup_process();

    if (cf->dfcode) epan_dissect_prime_with_dfilter(edt, cf->dfcode);
    prime_epan_dissect_with_postdissector_wanted_hfids(edt);
    col_custom_prime_edt(edt, &cf->cinfo);

    column_info* cinfo = &cf->cinfo;
    frame_data_set_before_dissect(&fdata, &cf->elapsed_time,
                  &cf->provider.ref, cf->provider.prev_dis);
    if (cf->provider.ref == &fdata) {
      ref_frame = fdata;
      cf->provider.ref = &ref_frame;
    }

    epan_dissect_run_with_taps(edt, cf->cd_t, rec,
       frame_tvbuff_new(&cf->provider, &fdata, pd), &fdata, cinfo);

    if (cf->dfcode)
      passed = dfilter_apply_edt(cf->dfcode, edt);
    fdata.flags.passed_dfilter = passed;
  }
  if (passed) cf->displayed_count ++;

  frame_data_set_after_dissect(&fdata, &cum_bytes);
  cf->provider.prev_cap = cf->provider.prev_dis = frame_data_sequence_add(cf->provider.frames, &fdata);

  if (passed) {
    print_packet(cf, edt);
    prev_dis_frame = fdata;
  }

  cf->provider.prev_cap = &prev_cap_frame;

  if (edt) {
    epan_dissect_reset(edt);
  }
  return passed;
}

static gboolean
print_packet(capture_file *cf, epan_dissect_t *edt)
{
  epan_dissect_fill_in_columns(edt, TRUE, TRUE);
  struct packet m;
  m.node.line = get_columns_cstr(cf, edt);
  m.node.childs = get_proto_tree(print_dissections_expanded,
      TRUE, edt, output_only_tables);

  GSList* src_le = edt->pi.data_src;
  struct data_source *src = (struct data_source *)src_le->data;
  tvbuff_t* tvb = get_data_source_tvb(src);
  size_t len = tvb_captured_length(tvb);
  const uint8_t* ptr = tvb_get_ptr(tvb, 0, len);
  m.data.resize(len);
  memcpy(m.data.data(), ptr, len);

  msgqueue.push(m);
  return TRUE;
}

bool cuishark_msg_queue_empty() { return msgqueue.empty(); }
const char* node_line(node_t* node) { return node->line.c_str(); }
size_t node_childs_num(node_t* node) { return node->childs.size(); }
node_t* node_child(node_t* node, size_t idx) { return &(node->childs[idx]); }

const uint8_t* cuishark_msg_data_ptr(packet_t* m) { return m->data.data(); }
size_t cuishark_msg_data_len(packet_t* m) { return m->data.size(); }
node_t* cuishark_msg_node(packet_t* m) { return &m->node; }
packet_t* cuishark_msgqueue_pop()
{
  packet_t* p = new packet_t;
  *p = msgqueue.front();
  msgqueue.pop();
  return p;
}

bool node_isopen(node_t* n) { return n->isopen; }
void node_isopen_switch(node_t* n) { n->isopen = !n->isopen; }
const char* get_interface_name() { return ifnamestr.c_str(); }

static void slot(node_t* root, std::vector<node_t*>& vec)
{
  assert(root);
  vec.push_back(root);
  if (root->isopen) {
    for (size_t i=0; i<root->childs.size(); i++) {
      slot(&root->childs[i], vec);
    }
  }
}

node_t* get_node_from_root(node_t* root, int idx)
{
  std::vector<node_t*> vec;
  slot(root, vec);
  return vec[idx+1];
}

void cuishark_apply_dfilter(const char* dfilter)
{
  tshark_debug("Compiling display filter: '%s'", dfilter);

  gchar *err_msg;
  dfilter_t *dfcode = NULL;
  if (!dfilter_compile(dfilter, &dfcode, &err_msg)) {
    cmdarg_err("%s", err_msg);
    g_free(err_msg);
    epan_cleanup();
    extcap_cleanup();
    exit(INVALID_FILTER);
  }
  cfile.dfcode = dfcode;

  size_t _num_displayed_packets = 0;
  size_t frames_count = cfile.count;
  for (size_t framenum = 1; framenum <= frames_count; framenum++) {
    wtap_cleareof(cfile.provider.wth);

    epan_dissect_t *edt = epan_dissect_new(cfile.epan, TRUE, TRUE);
    frame_data* fdata = frame_data_sequence_find(cfile.provider.frames, framenum);
    if (!fdata) {
      fprintf(stderr, "OKASHIII\n");
      exit(1);
    }

    cfile.provider.ref = fdata;
    cfile.provider.prev_cap = fdata;
    cfile.provider.prev_dis = fdata;
    cfile.provider.wth->random_fh = cfile.provider.wth->fh;
    if (!cf_read_record(&cfile, fdata)) {
      fprintf(stderr, "OKASHII");
      exit(1);
    }
    edt->pi.fd = fdata;
    tvbuff_t* tvb = frame_tvbuff_new(&cfile.provider, fdata, cfile.buf.data);
    add_new_data_source(&edt->pi, tvb, "");
    reset_epan_mem(&cfile, edt, TRUE, TRUE);

    frame_data_set_before_dissect(fdata, &cfile.elapsed_time,
        &cfile.provider.ref, cfile.provider.prev_dis);
    cfile.provider.prev_cap = fdata;
    if (dfcode != NULL) {
      epan_dissect_prime_with_dfilter(edt, dfcode);
    }
    wtap_rec* rec = wtap_get_rec(cfile.provider.wth);
    epan_dissect_run_with_taps(edt, cfile.cd_t, rec,
       frame_tvbuff_new(&cfile.provider, fdata, cfile.buf.data), fdata, &cfile.cinfo);

    g_assert(edt);
    bool passed = true;
    if (cfile.dfcode) {
      passed = dfilter_apply_edt(cfile.dfcode, edt);
    }
    if (passed) _num_displayed_packets ++;
    fdata->flags.passed_dfilter = passed;
    epan_dissect_free(edt);
  }
  cfile.displayed_count = _num_displayed_packets;
}

size_t cuishark_num_displayed_packets() { return cfile.displayed_count; }
size_t cuishark_num_captured_packets() { return cfile.count; }

void cuishark_status_dump()
{
  fprintf(stderr, "\n");
  fprintf(stderr, "[+] DisplayFilter\n");
  if (cfile.dfcode) dfilter_dump(cfile.dfcode);
  else fprintf(stderr, "nil\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "[+] NumPackets\n");
  fprintf(stderr, " displayed packets: %zd\n", cuishark_num_displayed_packets());
  fprintf(stderr, " captured packets: %zd\n", cuishark_num_captured_packets());
  fprintf(stderr, "\n");
}

void cuishark_packets_dump()
{
  epan_dissect_t edt;

  size_t frames_count = cfile.count;
  for (size_t framenum = 1; framenum <= frames_count; framenum++) {
    frame_data* fdata = frame_data_sequence_find(cfile.provider.frames, framenum);
    if (!fdata) {
      fprintf(stderr, "OKASHIII id=991j4b1\n");
      exit(1);
    }

    epan_dissect_init(&edt, cfile.epan, TRUE, TRUE);

    if (fdata->flags.passed_dfilter) {
      cfile.provider.ref = fdata;
      cfile.provider.prev_cap = fdata;
      cfile.provider.prev_dis = fdata;
      cfile.provider.wth->random_fh = cfile.provider.wth->fh;

      if (!cf_read_record(&cfile, fdata)) {
        fprintf(stderr, "OKASHII id=12349\n");
        exit(1);
      }

      edt.pi.fd = fdata;
      frame_data_set_before_dissect(fdata, &cfile.elapsed_time,
          &cfile.provider.ref, cfile.provider.prev_dis);
      wtap_rec* rec = wtap_get_rec(cfile.provider.wth);
      tvbuff_t* tvb = frame_tvbuff_new(&cfile.provider, fdata, cfile.buf.data);
      epan_dissect_run_with_taps(&edt, cfile.cd_t, rec, tvb, fdata, &cfile.cinfo);
      frame_data_set_after_dissect(fdata, &cum_bytes);

      print_packet(&cfile, &edt);
    }
    epan_dissect_cleanup(&edt);
  }
  cfile.provider.wth->random_fh = NULL;

}


