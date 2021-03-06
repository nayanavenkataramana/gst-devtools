/* GStreamer
 *
 * Copyright (C) 2013 Thibault Saunier <thibault.saunier@collabora.com>
 *
 * gst-validate-utils.c - Some utility functions
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <glib.h>

#ifdef G_OS_UNIX
#include <glib-unix.h>
#include <sys/wait.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "gst-validate-utils.h"
#include <gst/gst.h>

#define PARSER_BOOLEAN_EQUALITY_THRESHOLD (1e-10)
#define PARSER_MAX_TOKEN_SIZE 256
#define PARSER_MAX_ARGUMENT_COUNT 10

static GRegex *_clean_structs_lines = NULL;

typedef struct
{
  const gchar *str;
  gint len;
  gint pos;
  jmp_buf err_jmp_buf;
  const gchar *error;
  void *user_data;
  GstValidateParseVariableFunc variable_func;
} MathParser;

static gdouble _read_power (MathParser * parser);

static void
_error (MathParser * parser, const gchar * err)
{
  parser->error = err;
  longjmp (parser->err_jmp_buf, 1);
}

static gchar
_peek (MathParser * parser)
{
  if (parser->pos < parser->len)
    return parser->str[parser->pos];
  _error (parser, "Tried to read past end of string!");
  return '\0';
}

static gchar
_peek_n (MathParser * parser, gint n)
{
  if (parser->pos + n < parser->len)
    return parser->str[parser->pos + n];
  _error (parser, "Tried to read past end of string!");
  return '\0';
}

static gchar
_next (MathParser * parser)
{
  if (parser->pos < parser->len)
    return parser->str[parser->pos++];
  _error (parser, "Tried to read past end of string!");
  return '\0';
}

static gdouble
_read_double (MathParser * parser)
{
  gchar c, token[PARSER_MAX_TOKEN_SIZE];
  gint pos = 0;
  gdouble val = 0.0;

  c = _peek (parser);
  if (c == '+' || c == '-')
    token[pos++] = _next (parser);

  while (isdigit (_peek (parser)))
    token[pos++] = _next (parser);

  c = _peek (parser);
  if (c == '.')
    token[pos++] = _next (parser);

  while (isdigit (_peek (parser)))
    token[pos++] = _next (parser);

  c = _peek (parser);
  if (c == 'e' || c == 'E') {
    token[pos++] = _next (parser);

    c = _peek (parser);
    if (c == '+' || c == '-') {
      token[pos++] = _next (parser);
    }
  }

  while (isdigit (_peek (parser)))
    token[pos++] = _next (parser);

  token[pos] = '\0';

  if (pos == 0 || sscanf (token, "%lf", &val) != 1)
    _error (parser, "Failed to read real number");

  return val;
}

static gdouble
_read_term (MathParser * parser)
{
  gdouble v0;
  gchar c;

  v0 = _read_power (parser);
  c = _peek (parser);

  while (c == '*' || c == '/') {
    _next (parser);
    if (c == '*') {
      v0 *= _read_power (parser);
    } else if (c == '/') {
      v0 /= _read_power (parser);
    }
    c = _peek (parser);
  }
  return v0;
}

static gdouble
_read_expr (MathParser * parser)
{
  gdouble v0 = 0.0;
  gchar c;

  c = _peek (parser);
  if (c == '+' || c == '-') {
    _next (parser);
    if (c == '+')
      v0 += _read_term (parser);
    else if (c == '-')
      v0 -= _read_term (parser);
  } else {
    v0 = _read_term (parser);
  }

  c = _peek (parser);
  while (c == '+' || c == '-') {
    _next (parser);
    if (c == '+') {
      v0 += _read_term (parser);
    } else if (c == '-') {
      v0 -= _read_term (parser);
    }

    c = _peek (parser);
  }

  return v0;
}

static gdouble
_read_boolean_comparison (MathParser * parser)
{
  gchar c, oper[] = { '\0', '\0', '\0' };
  gdouble v0, v1;


  v0 = _read_expr (parser);
  c = _peek (parser);
  if (c == '>' || c == '<') {
    oper[0] = _next (parser);
    c = _peek (parser);
    if (c == '=')
      oper[1] = _next (parser);


    v1 = _read_expr (parser);

    if (g_strcmp0 (oper, "<") == 0) {
      v0 = (v0 < v1) ? 1.0 : 0.0;
    } else if (g_strcmp0 (oper, ">") == 0) {
      v0 = (v0 > v1) ? 1.0 : 0.0;
    } else if (g_strcmp0 (oper, "<=") == 0) {
      v0 = (v0 <= v1) ? 1.0 : 0.0;
    } else if (g_strcmp0 (oper, ">=") == 0) {
      v0 = (v0 >= v1) ? 1.0 : 0.0;
    } else {
      _error (parser, "Unknown operation!");
    }
  }
  return v0;
}

static gdouble
_read_boolean_equality (MathParser * parser)
{
  gchar c, oper[] = { '\0', '\0', '\0' };
  gdouble v0, v1;

  v0 = _read_boolean_comparison (parser);
  c = _peek (parser);
  if (c == '=' || c == '!') {
    if (c == '!') {
      if (_peek_n (parser, 1) == '=') {
        oper[0] = _next (parser);
        oper[1] = _next (parser);
      } else {
        return v0;
      }
    } else {
      oper[0] = _next (parser);
      c = _peek (parser);
      if (c != '=')
        _error (parser, "Expected a '=' for boolean '==' operator!");
      oper[1] = _next (parser);
    }
    v1 = _read_boolean_comparison (parser);
    if (g_strcmp0 (oper, "==") == 0) {
      v0 = (fabs (v0 - v1) < PARSER_BOOLEAN_EQUALITY_THRESHOLD) ? 1.0 : 0.0;
    } else if (g_strcmp0 (oper, "!=") == 0) {
      v0 = (fabs (v0 - v1) > PARSER_BOOLEAN_EQUALITY_THRESHOLD) ? 1.0 : 0.0;
    } else {
      _error (parser, "Unknown operation!");
    }
  }
  return v0;
}

static gdouble
_read_boolean_and (MathParser * parser)
{
  gchar c;
  gdouble v0, v1;

  v0 = _read_boolean_equality (parser);

  c = _peek (parser);
  while (c == '&') {
    _next (parser);

    c = _peek (parser);
    if (c != '&')
      _error (parser, "Expected '&' to follow '&' in logical and operation!");
    _next (parser);

    v1 = _read_boolean_equality (parser);
    v0 = (fabs (v0) >= PARSER_BOOLEAN_EQUALITY_THRESHOLD
        && fabs (v1) >= PARSER_BOOLEAN_EQUALITY_THRESHOLD) ? 1.0 : 0.0;

    c = _peek (parser);
  }

  return v0;
}

static gdouble
_read_boolean_or (MathParser * parser)
{
  gchar c;
  gdouble v0, v1;

  v0 = _read_boolean_and (parser);

  c = _peek (parser);
  while (c == '|') {
    _next (parser);
    c = _peek (parser);
    if (c != '|')
      _error (parser, "Expected '|' to follow '|' in logical or operation!");
    _next (parser);
    v1 = _read_boolean_and (parser);
    v0 = (fabs (v0) >= PARSER_BOOLEAN_EQUALITY_THRESHOLD
        || fabs (v1) >= PARSER_BOOLEAN_EQUALITY_THRESHOLD) ? 1.0 : 0.0;
    c = _peek (parser);
  }

  return v0;
}

static gboolean
_init (MathParser * parser, const gchar * str,
    GstValidateParseVariableFunc variable_func, void *user_data)
{
  parser->str = str;
  parser->len = strlen (str) + 1;
  parser->pos = 0;
  parser->error = NULL;
  parser->user_data = user_data;
  parser->variable_func = variable_func;

  return TRUE;
}

static gdouble
_parse (MathParser * parser)
{
  gdouble result = 0.0;

  if (!setjmp (parser->err_jmp_buf)) {
    result = _read_expr (parser);
    if (parser->pos < parser->len - 1) {
      _error (parser,
          "Failed to reach end of input expression, likely malformed input");
    } else
      return result;
  }

  return -1.0;
}

static gdouble
_read_argument (MathParser * parser)
{
  gchar c;
  gdouble val;

  val = _read_expr (parser);
  c = _peek (parser);
  if (c == ',')
    _next (parser);

  return val;
}

static gdouble
_read_builtin (MathParser * parser)
{
  gdouble v0 = 0.0, v1 = 0.0;
  gchar c, token[PARSER_MAX_TOKEN_SIZE];
  gint pos = 0;

  c = _peek (parser);
  if (isalpha (c) || c == '_' || c == '$') {
    while (isalpha (c) || isdigit (c) || c == '_' || c == '$') {
      token[pos++] = _next (parser);
      c = _peek (parser);
    }
    token[pos] = '\0';

    if (_peek (parser) == '(') {
      _next (parser);
      if (g_strcmp0 (token, "min") == 0) {
        v0 = _read_argument (parser);
        v1 = _read_argument (parser);
        v0 = MIN (v0, v1);
      } else if (g_strcmp0 (token, "max") == 0) {
        v0 = _read_argument (parser);
        v1 = _read_argument (parser);
        v0 = MAX (v0, v1);
      } else {
        _error (parser, "Tried to call unknown built-in function!");
      }

      if (_next (parser) != ')')
        _error (parser, "Expected ')' in built-in call!");
    } else {
      if (parser->variable_func != NULL
          && parser->variable_func (token, &v1, parser->user_data)) {
        v0 = v1;
      } else {
        gchar *err =
            g_strdup_printf ("Could not look up value for variable %s!", token);
        _error (parser, err);
        g_free (err);
      }
    }
  } else {
    v0 = _read_double (parser);
  }

  return v0;
}

static gdouble
_read_parenthesis (MathParser * parser)
{
  gdouble val;

  if (_peek (parser) == '(') {
    _next (parser);
    val = _read_boolean_or (parser);
    if (_peek (parser) != ')')
      _error (parser, "Expected ')'!");
    _next (parser);
  } else {
    val = _read_builtin (parser);
  }

  return val;
}

static gdouble
_read_unary (MathParser * parser)
{
  gchar c;
  gdouble v0 = 0.0;

  c = _peek (parser);
  if (c == '!') {
    _error (parser, "Expected '+' or '-' for unary expression, got '!'");
  } else if (c == '-') {
    _next (parser);
    v0 = -_read_parenthesis (parser);
  } else if (c == '+') {
    _next (parser);
    v0 = _read_parenthesis (parser);
  } else {
    v0 = _read_parenthesis (parser);
  }
  return v0;
}

static gdouble
_read_power (MathParser * parser)
{
  gdouble v0, v1 = 1.0, s = 1.0;

  v0 = _read_unary (parser);

  while (_peek (parser) == '^') {
    _next (parser);
    if (_peek (parser) == '-') {
      _next (parser);
      s = -1.0;
    }
    v1 = s * _read_power (parser);
    v0 = pow (v0, v1);
  }

  return v0;
}

/**
 * gst_validate_utils_parse_expression: (skip):
 */
gdouble
gst_validate_utils_parse_expression (const gchar * expr,
    GstValidateParseVariableFunc variable_func, gpointer user_data,
    gchar ** error)
{
  gdouble val;
  MathParser parser;
  gchar **spl = g_strsplit (expr, " ", -1);
  gchar *expr_nospace = g_strjoinv ("", spl);

  _init (&parser, expr_nospace, variable_func, user_data);
  val = _parse (&parser);
  g_strfreev (spl);
  g_free (expr_nospace);

  if (error) {
    if (parser.error)
      *error = g_strdup (parser.error);
    else
      *error = NULL;
  }
  return val;
}

/**
 * gst_validate_utils_flags_from_str:
 * @type: The #GType of the flags we are trying to retrieve the flags from
 * @str_flags: The string representation of the value
 *
 * Returns: The flags set in @str_flags
 */
guint
gst_validate_utils_flags_from_str (GType type, const gchar * str_flags)
{
  guint flags;
  GValue value = G_VALUE_INIT;
  g_value_init (&value, type);

  if (!gst_value_deserialize (&value, str_flags)) {
    g_error ("Invalid flags: %s", str_flags);

    return 0;
  }

  flags = g_value_get_flags (&value);
  g_value_unset (&value);

  return flags;
}

/**
 * gst_validate_utils_enum_from_str:
 * @type: The #GType of the enum we are trying to retrieve the enum value from
 * @str_enum: The string representation of the value
 * @enum_value: (out): The value of the enum
 *
 * Returns: %TRUE on success %FALSE otherwise
 */
gboolean
gst_validate_utils_enum_from_str (GType type, const gchar * str_enum,
    guint * enum_value)
{
  GValue value = G_VALUE_INIT;
  g_value_init (&value, type);

  if (!gst_value_deserialize (&value, str_enum)) {
    g_error ("Invalid enum: %s", str_enum);

    return FALSE;
  }

  *enum_value = g_value_get_enum (&value);
  g_value_unset (&value);

  return TRUE;
}

/* Parse file that contains a list of GStructures */
static gchar **
_file_get_lines (GFile * file)
{
  gsize size;

  GError *err = NULL;
  gchar *content = NULL, *escaped_content = NULL, **lines = NULL;

  /* TODO Handle GCancellable */
  if (!g_file_load_contents (file, NULL, &content, &size, NULL, &err)) {
    GST_WARNING ("Failed to load contents: %d %s", err->code, err->message);
    g_error_free (err);
    return NULL;
  }
  if (g_strcmp0 (content, "") == 0) {
    g_free (content);
    return NULL;
  }

  if (_clean_structs_lines == NULL)
    _clean_structs_lines =
        g_regex_new ("\\\\\n|#.*\n", G_REGEX_CASELESS, 0, NULL);

  escaped_content =
      g_regex_replace (_clean_structs_lines, content, -1, 0, "", 0, NULL);
  g_free (content);

  lines = g_strsplit (escaped_content, "\n", 0);
  g_free (escaped_content);

  return lines;
}

static gchar **
_get_lines (const gchar * scenario_file)
{
  GFile *file = NULL;
  gchar **lines = NULL;

  GST_DEBUG ("Trying to load %s", scenario_file);
  if ((file = g_file_new_for_path (scenario_file)) == NULL) {
    GST_WARNING ("%s wrong uri", scenario_file);
    return NULL;
  }

  lines = _file_get_lines (file);

  g_object_unref (file);

  return lines;
}

/* Returns: (transfer full): a #GList of #GstStructure */
static GList *
_lines_get_structures (gchar ** lines)
{
  gint i;
  GList *structures = NULL;

  if (lines == NULL)
    return NULL;

  for (i = 0; lines[i]; i++) {
    GstStructure *structure;

    if (g_strcmp0 (lines[i], "") == 0)
      continue;

    structure = gst_structure_from_string (lines[i], NULL);
    if (structure == NULL) {
      GST_ERROR ("Could not parse action %s", lines[i]);
      goto failed;
    }

    structures = g_list_append (structures, structure);
  }

done:
  g_strfreev (lines);

  return structures;

failed:
  if (structures)
    g_list_free_full (structures, (GDestroyNotify) gst_structure_free);
  structures = NULL;

  goto done;
}

/**
 * gst_validate_utils_structs_parse_from_filename: (skip):
 */
GList *
gst_validate_utils_structs_parse_from_filename (const gchar * scenario_file)
{
  gchar **lines;

  lines = _get_lines (scenario_file);

  if (lines == NULL) {
    GST_DEBUG ("Got no line for file: %s", scenario_file);
    return NULL;
  }

  return _lines_get_structures (lines);
}

/**
 * gst_validate_structs_parse_from_gfile: (skip):
 */
GList *
gst_validate_structs_parse_from_gfile (GFile * scenario_file)
{
  gchar **lines;

  lines = _file_get_lines (scenario_file);

  if (lines == NULL)
    return NULL;

  return _lines_get_structures (lines);
}

static gboolean
strv_contains (GStrv strv, const gchar * str)
{
  guint i;

  for (i = 0; strv[i] != NULL; i++)
    if (g_strcmp0 (strv[i], str) == 0)
      return TRUE;

  return FALSE;
}

gboolean
gst_validate_element_has_klass (GstElement * element, const gchar * klass)
{
  const gchar *tmp;
  gchar **a, **b;
  gboolean result = FALSE;
  guint i;

  tmp = gst_element_class_get_metadata (GST_ELEMENT_GET_CLASS (element),
      GST_ELEMENT_METADATA_KLASS);

  a = g_strsplit (klass, "/", -1);
  b = g_strsplit (tmp, "/", -1);

  /* All the elements in 'a' have to be in 'b' */
  for (i = 0; a[i] != NULL; i++)
    if (!strv_contains (b, a[i]))
      goto done;
  result = TRUE;

done:
  g_strfreev (a);
  g_strfreev (b);
  return result;
}

/**
 * gst_validate_utils_get_clocktime:
 * @structure: A #GstStructure to retrieve @name as a GstClockTime.
 * @name: The name of the field containing a #GstClockTime
 * @retval: (out): The clocktime contained in @structure
 *
 * Get @name from @structure as a #GstClockTime, it handles various types
 * for the value, if it is a double, it considers the value to be in second
 * it can be a gint, gint64 a guint, a gint64.
 *
 * Return: %TRUE in case of success, %FALSE otherwise.
 */
gboolean
gst_validate_utils_get_clocktime (GstStructure * structure, const gchar * name,
    GstClockTime * retval)
{
  gdouble val;
  const GValue *gvalue = gst_structure_get_value (structure, name);

  *retval = GST_CLOCK_TIME_NONE;
  if (gvalue == NULL) {
    return FALSE;
  }

  if (G_VALUE_TYPE (gvalue) == GST_TYPE_CLOCK_TIME) {
    *retval = g_value_get_uint64 (gvalue);

    return TRUE;
  }

  if (G_VALUE_TYPE (gvalue) == G_TYPE_UINT64) {
    *retval = g_value_get_uint64 (gvalue);

    return TRUE;
  }

  if (G_VALUE_TYPE (gvalue) == G_TYPE_UINT) {
    *retval = (GstClockTime) g_value_get_uint (gvalue);

    return TRUE;
  }

  if (G_VALUE_TYPE (gvalue) == G_TYPE_INT) {
    *retval = (GstClockTime) g_value_get_int (gvalue);

    return TRUE;
  }

  if (G_VALUE_TYPE (gvalue) == G_TYPE_INT64) {
    *retval = (GstClockTime) g_value_get_int64 (gvalue);

    return TRUE;
  }

  if (!gst_structure_get_double (structure, name, &val)) {
    return FALSE;
  }

  if (val == -1.0)
    *retval = GST_CLOCK_TIME_NONE;
  else {
    *retval = val * GST_SECOND;
    *retval = GST_ROUND_UP_4 (*retval);
  }

  return TRUE;
}

GstValidateActionReturn
gst_validate_object_set_property (GstValidateReporter * reporter,
    GObject * object, const gchar * property,
    const GValue * value, gboolean optional)
{
  GParamSpec *paramspec;
  GObjectClass *klass = G_OBJECT_GET_CLASS (object);
  GstValidateExecuteActionReturn res = GST_VALIDATE_EXECUTE_ACTION_OK;
  GValue cvalue = G_VALUE_INIT, nvalue = G_VALUE_INIT;

  paramspec = g_object_class_find_property (klass, property);
  if (paramspec == NULL) {
    if (optional)
      return TRUE;
    GST_ERROR ("Target doesn't have property %s", property);
    return FALSE;
  }

  g_value_init (&cvalue, paramspec->value_type);
  if (paramspec->value_type != G_VALUE_TYPE (value) &&
      (G_VALUE_TYPE (value) == G_TYPE_STRING)) {
    if (!gst_value_deserialize (&cvalue, g_value_get_string (value))) {
      GST_VALIDATE_REPORT (reporter, SCENARIO_ACTION_EXECUTION_ERROR,
          "Could not set %" GST_PTR_FORMAT "::%s as value %s"
          " could not be deserialize to %s", object, property,
          g_value_get_string (value), G_PARAM_SPEC_TYPE_NAME (paramspec));

      return GST_VALIDATE_EXECUTE_ACTION_ERROR_REPORTED;
    }
  } else {
    if (!g_value_transform (value, &cvalue)) {
      GST_VALIDATE_REPORT (reporter, SCENARIO_ACTION_EXECUTION_ERROR,
          "Could not set %" GST_PTR_FORMAT " property %s to type %s"
          " (wanted type %s)", object, property, G_VALUE_TYPE_NAME (value),
          G_PARAM_SPEC_TYPE_NAME (paramspec));

      return GST_VALIDATE_EXECUTE_ACTION_ERROR_REPORTED;
    }

  }

  g_object_set_property (object, property, &cvalue);

  g_value_init (&nvalue, paramspec->value_type);
  g_object_get_property (object, property, &nvalue);

  if (gst_value_compare (&cvalue, &nvalue) != GST_VALUE_EQUAL) {
    gchar *nvalstr = gst_value_serialize (&nvalue);
    gchar *cvalstr = gst_value_serialize (&cvalue);
    GST_VALIDATE_REPORT (reporter, SCENARIO_ACTION_EXECUTION_ERROR,
        "Setting value %" GST_PTR_FORMAT "::%s failed, expected value: %s"
        " value after setting %s", object, property, cvalstr, nvalstr);

    res = GST_VALIDATE_EXECUTE_ACTION_ERROR_REPORTED;
    g_free (nvalstr);
    g_free (cvalstr);
  }

  g_value_reset (&cvalue);
  g_value_reset (&nvalue);
  return res;
}

#ifdef G_OS_UNIX
static void
fault_restore (void)
{
  struct sigaction action;

  memset (&action, 0, sizeof (action));
  action.sa_handler = SIG_DFL;

  sigaction (SIGSEGV, &action, NULL);
  sigaction (SIGQUIT, &action, NULL);
}

static void
fault_spin (void)
{
  int spinning = TRUE;

  g_on_error_stack_trace ("GstValidate");

  wait (NULL);

  g_printerr ("Please run 'gdb <process-name> %d' to "
      "continue debugging, Ctrl-C to quit, or Ctrl-\\ to dump core.\n",
      (gint) getpid ());

  while (spinning)
    g_usleep (1000000);
}

static void
fault_handler_sighandler (int signum)
{
  fault_restore ();

  /* printf is used instead of g_print(), since it's less likely to
   * deadlock */
  switch (signum) {
    case SIGSEGV:
      g_printerr ("<Caught SIGNAL: SIGSEGV>\n");
      break;
    case SIGQUIT:
      g_print ("<Caught SIGNAL: SIGQUIT>\n");
      break;
    default:
      g_printerr ("<Caught SIGNAL: %d>\n", signum);
      break;
  }

  fault_spin ();
}

static void
fault_setup (void)
{
  struct sigaction action;

  memset (&action, 0, sizeof (action));
  action.sa_handler = fault_handler_sighandler;

  sigaction (SIGSEGV, &action, NULL);
  sigaction (SIGQUIT, &action, NULL);
}
#endif /* G_OS_UNIX */

void
gst_validate_spin_on_fault_signals (void)
{
#ifdef G_OS_UNIX
  fault_setup ();
#endif
}

/**
 * gst_validate_element_matches_target:
 * @element: a #GstElement to check
 * @s: a #GstStructure to use for matching
 *
 * Check if @element matches one of the 'target-element-name',
 * 'target-element-klass' or 'target-element-factory-name' defined in @s.
 *
 * Return: %TRUE if it matches, %FALSE otherwise or if @s doesn't contain any
 * target-element field.
 */
gboolean
gst_validate_element_matches_target (GstElement * element, GstStructure * s)
{
  const gchar *tmp;

  tmp = gst_structure_get_string (s, "target-element-name");
  if (tmp != NULL && !g_strcmp0 (tmp, GST_ELEMENT_NAME (element)))
    return TRUE;

  tmp = gst_structure_get_string (s, "target-element-klass");
  if (tmp != NULL && gst_validate_element_has_klass (element, tmp))
    return TRUE;

  tmp = gst_structure_get_string (s, "target-element-factory-name");
  if (tmp != NULL && gst_element_get_factory (element)
      && !g_strcmp0 (GST_OBJECT_NAME (gst_element_get_factory (element)), tmp))
    return TRUE;

  return FALSE;
}
