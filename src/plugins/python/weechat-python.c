/*
 * weechat-python.c - python plugin for WeeChat
 *
 * Copyright (C) 2003-2018 Sébastien Helleu <flashcode@flashtux.org>
 * Copyright (C) 2005-2007 Emmanuel Bouthenot <kolter@openics.org>
 * Copyright (C) 2012 Simon Arlott
 *
 * This file is part of WeeChat, the extensible chat client.
 *
 * WeeChat is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * WeeChat is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with WeeChat.  If not, see <http://www.gnu.org/licenses/>.
 */

#undef _

#include <Python.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../weechat-plugin.h"
#include "../plugin-script.h"
#include "weechat-python.h"
#include "weechat-python-api.h"


WEECHAT_PLUGIN_NAME(PYTHON_PLUGIN_NAME);
WEECHAT_PLUGIN_DESCRIPTION(N_("Support of python scripts"));
WEECHAT_PLUGIN_AUTHOR("Sébastien Helleu <flashcode@flashtux.org>");
WEECHAT_PLUGIN_VERSION(WEECHAT_VERSION);
WEECHAT_PLUGIN_LICENSE(WEECHAT_LICENSE);
WEECHAT_PLUGIN_PRIORITY(4000);

struct t_weechat_plugin *weechat_python_plugin = NULL;

struct t_plugin_script_data python_data;

struct t_config_file *python_config_file = NULL;
struct t_config_option *python_config_look_check_license = NULL;
struct t_config_option *python_config_look_eval_keep_context = NULL;

int python_quiet = 0;

struct t_plugin_script *python_script_eval = NULL;
int python_eval_mode = 0;
int python_eval_send_input = 0;
int python_eval_exec_commands = 0;
struct t_gui_buffer *python_eval_buffer = NULL;
char *python_eval_output = NULL;
#define PYTHON_EVAL_SCRIPT                                              \
    "import weechat\n"                                                  \
    "\n"                                                                \
    "def script_python_eval(code):\n"                                   \
    "    exec(code)\n"                                                  \
    "\n"                                                                \
    "weechat.register('" WEECHAT_SCRIPT_EVAL_NAME "', '', '1.0', "      \
    "'" WEECHAT_LICENSE "', 'Evaluation of source code', '', '')\n"

struct t_plugin_script *python_scripts = NULL;
struct t_plugin_script *last_python_script = NULL;
struct t_plugin_script *python_current_script = NULL;
struct t_plugin_script *python_registered_script = NULL;
const char *python_current_script_filename = NULL;
PyThreadState *python_mainThreadState = NULL;
PyThreadState *python_current_interpreter = NULL;
char *python2_bin = NULL;
char **python_buffer_output = NULL;

/* outputs subroutines */
static PyObject *weechat_python_output (PyObject *self, PyObject *args);
static PyMethodDef weechat_python_output_funcs[] = {
    { "write", weechat_python_output, METH_VARARGS, "" },
    { NULL, NULL, 0, NULL }
};

#if PY_MAJOR_VERSION >= 3
/* module definition for python >= 3.x */
static struct PyModuleDef moduleDef = {
    PyModuleDef_HEAD_INIT,
    "weechat",
    NULL,
    -1,
    weechat_python_funcs,
    NULL,
    NULL,
    NULL,
    NULL
};
static struct PyModuleDef moduleDefOutputs = {
    PyModuleDef_HEAD_INIT,
    "weechatOutputs",
    NULL,
    -1,
    weechat_python_output_funcs,
    NULL,
    NULL,
    NULL,
    NULL
};
#endif /* PY_MAJOR_VERSION >= 3 */

/*
 * string used to execute action "install":
 * when signal "python_script_install" is received, name of string
 * is added to this string, to be installed later by a timer (when nothing is
 * running in script)
 */
char *python_action_install_list = NULL;

/*
 * string used to execute action "remove":
 * when signal "python_script_remove" is received, name of string
 * is added to this string, to be removed later by a timer (when nothing is
 * running in script)
 */
char *python_action_remove_list = NULL;

/*
 * string used to execute action "autoload":
 * when signal "python_script_autoload" is received, name of string
 * is added to this string, to autoload or disable autoload later by a timer
 * (when nothing is running in script)
 */
char *python_action_autoload_list = NULL;


/*
 * Gets path to python 2.x interpreter.
 *
 * Note: result must be freed after use.
 */

char *
weechat_python_get_python2_bin ()
{
    const char *dir_separator;
    char *py2_bin, *path, **paths, bin[4096];
    char *versions[] = { "2.7", "2.6", "2.5", "2.4", "2.3", "2.2", "2", NULL };
    int num_paths, i, j, rc;
    struct stat stat_buf;

    py2_bin = NULL;

    dir_separator = weechat_info_get ("dir_separator", "");
    path = getenv ("PATH");

    if (dir_separator && path)
    {
        paths = weechat_string_split (path, ":", 0, 0, &num_paths);
        if (paths)
        {
            for (i = 0; i < num_paths; i++)
            {
                for (j = 0; versions[j]; j++)
                {
                    snprintf (bin, sizeof (bin), "%s%s%s%s",
                              paths[i], dir_separator, "python",
                              versions[j]);
                    rc = stat (bin, &stat_buf);
                    if ((rc == 0) && (S_ISREG(stat_buf.st_mode)))
                    {
                        py2_bin = strdup (bin);
                        break;
                    }
                }
                if (py2_bin)
                    break;
            }
            weechat_string_free_split (paths);
        }
    }

    if (!py2_bin)
        py2_bin = strdup ("python");

    return py2_bin;
}

/*
 * Converts a python unicode to a C UTF-8 string.
 *
 * Note: result must be freed after use.
 */

char *
weechat_python_unicode_to_string (PyObject *obj)
{
    PyObject *utf8string;
    char *str;

    str = NULL;

    utf8string = PyUnicode_AsUTF8String (obj);
    if (utf8string)
    {
        if (PyBytes_AsString (utf8string))
            str = strdup (PyBytes_AsString (utf8string));
        Py_XDECREF(utf8string);
    }

    return str;
}

/*
 * Callback called for each key/value in a hashtable.
 */

void
weechat_python_hashtable_map_cb (void *data,
                                 struct t_hashtable *hashtable,
                                 const char *key,
                                 const char *value)
{
    PyObject *dict, *dict_key, *dict_value;

    /* make C compiler happy */
    (void) hashtable;

    dict = (PyObject *)data;

    dict_key = Py_BuildValue ("s", key);
    dict_value = Py_BuildValue ("s", value);

    PyDict_SetItem (dict, dict_key, dict_value);

    Py_DECREF (dict_key);
    Py_DECREF (dict_value);
}

/*
 * Converts a WeeChat hashtable to a python dictionary.
 */

PyObject *
weechat_python_hashtable_to_dict (struct t_hashtable *hashtable)
{
    PyObject *dict;

    dict = PyDict_New ();
    if (!dict)
    {
        Py_INCREF(Py_None);
        return Py_None;
    }

    weechat_hashtable_map_string (hashtable, &weechat_python_hashtable_map_cb,
                                  dict);

    return dict;
}

/*
 * Converts a python dictionary to a WeeChat hashtable.
 *
 * Note: hashtable must be freed after use.
 */

struct t_hashtable *
weechat_python_dict_to_hashtable (PyObject *dict, int size,
                                  const char *type_keys,
                                  const char *type_values)
{
    struct t_hashtable *hashtable;
    PyObject *key, *value;
    Py_ssize_t pos;
    char *str_key, *str_value;

    hashtable = weechat_hashtable_new (size, type_keys, type_values,
                                       NULL, NULL);
    if (!hashtable)
        return NULL;

    pos = 0;
    while (PyDict_Next (dict, &pos, &key, &value))
    {
        str_key = NULL;
        str_value = NULL;
        if (PyBytes_Check (key))
        {
            if (PyBytes_AsString (key))
                str_key = strdup (PyBytes_AsString (key));
        }
        else
        {
            str_key = weechat_python_unicode_to_string (key);
        }
        if (PyBytes_Check (value))
        {
            if (PyBytes_AsString (value))
                str_value = strdup (PyBytes_AsString (value));
        }
        else
        {
            str_value = weechat_python_unicode_to_string (value);
        }

        if (str_key)
        {
            if (strcmp (type_values, WEECHAT_HASHTABLE_STRING) == 0)
            {
                weechat_hashtable_set (hashtable, str_key, str_value);
            }
            else if (strcmp (type_values, WEECHAT_HASHTABLE_POINTER) == 0)
            {
                weechat_hashtable_set (hashtable, str_key,
                                       plugin_script_str2ptr (weechat_python_plugin,
                                                              NULL, NULL,
                                                              str_value));
            }
        }

        if (str_key)
            free (str_key);
        if (str_value)
            free (str_value);
    }

    return hashtable;
}

/*
 * Flushes output.
 */

void
weechat_python_output_flush ()
{
    const char *ptr_command;
    char *temp_buffer, *command;
    int length;

    if (!*python_buffer_output[0])
        return;

    /* if there's no buffer, we catch the output, so there's no flush */
    if (python_eval_mode && !python_eval_buffer)
        return;

    temp_buffer = strdup (*python_buffer_output);
    if (!temp_buffer)
        return;

    weechat_string_dyn_copy (python_buffer_output, NULL);

    if (python_eval_mode)
    {
        if (python_eval_send_input)
        {
            if (python_eval_exec_commands)
                ptr_command = temp_buffer;
            else
                ptr_command = weechat_string_input_for_buffer (temp_buffer);
            if (ptr_command)
            {
                weechat_command (python_eval_buffer, temp_buffer);
            }
            else
            {
                length = 1 + strlen (temp_buffer) + 1;
                command = malloc (length);
                if (command)
                {
                    snprintf (command, length, "%c%s",
                              temp_buffer[0], temp_buffer);
                    weechat_command (python_eval_buffer,
                                     (command[0]) ? command : " ");
                    free (command);
                }
            }
        }
        else
        {
            weechat_printf (python_eval_buffer, "%s", temp_buffer);
        }
    }
    else
    {
        /* script (no eval mode) */
        weechat_printf (
            NULL,
            weechat_gettext ("%s: stdout/stderr (%s): %s"),
            PYTHON_PLUGIN_NAME,
            (python_current_script) ? python_current_script->name : "?",
            temp_buffer);
    }

    free (temp_buffer);
}

/*
 * Redirection for stdout and stderr.
 */

static PyObject *
weechat_python_output (PyObject *self, PyObject *args)
{
    char *msg, *ptr_msg, *ptr_newline;

    /* make C compiler happy */
    (void) self;

    msg = NULL;

    if (!PyArg_ParseTuple (args, "s", &msg))
    {
        weechat_python_output_flush ();
    }
    else
    {
        ptr_msg = msg;
        while ((ptr_newline = strchr (ptr_msg, '\n')) != NULL)
        {
            ptr_newline[0] = '\0';
            weechat_string_dyn_concat (python_buffer_output, ptr_msg);
            weechat_python_output_flush ();
            ptr_newline[0] = '\n';
            ptr_msg = ++ptr_newline;
        }
        weechat_string_dyn_concat (python_buffer_output, ptr_msg);
    }

    Py_INCREF(Py_None);
    return Py_None;
}

/*
 * Executes a python function.
 */

void *
weechat_python_exec (struct t_plugin_script *script,
                     int ret_type, const char *function,
                     char *format, void **argv)
{
    struct t_plugin_script *old_python_current_script;
    PyThreadState *old_interpreter;
    PyObject *evMain, *evDict, *evFunc, *rc;
    void *argv2[16], *ret_value, *ret_temp;
    int i, argc, *ret_int;

    ret_value = NULL;

    /* PyEval_AcquireLock (); */

    old_python_current_script = python_current_script;
    python_current_script = script;
    old_interpreter = NULL;
    if (script->interpreter)
    {
        old_interpreter = PyThreadState_Swap (NULL);
        PyThreadState_Swap (script->interpreter);
    }

    evMain = PyImport_AddModule ((char *) "__main__");
    evDict = PyModule_GetDict (evMain);
    evFunc = PyDict_GetItemString (evDict, function);

    if ( !(evFunc && PyCallable_Check (evFunc)) )
    {
        weechat_printf (NULL,
                        weechat_gettext ("%s%s: unable to run function \"%s\""),
                        weechat_prefix ("error"), PYTHON_PLUGIN_NAME, function);
        /* PyEval_ReleaseThread (python_current_script->interpreter); */
        goto end;
    }

    if (argv && argv[0])
    {
        argc = strlen (format);
        for (i = 0; i < 16; i++)
        {
            argv2[i] = (i < argc) ? argv[i] : NULL;
        }
        rc = PyObject_CallFunction (evFunc, format,
                                    argv2[0], argv2[1],
                                    argv2[2], argv2[3],
                                    argv2[4], argv2[5],
                                    argv2[6], argv2[7],
                                    argv2[8], argv2[9],
                                    argv2[10], argv2[11],
                                    argv2[12], argv2[13],
                                    argv2[14], argv2[15]);
    }
    else
    {
        rc = PyObject_CallFunction (evFunc, NULL);
    }

    weechat_python_output_flush ();

    /*
     * ugly hack : rc = NULL while 'return weechat.WEECHAT_RC_OK ....
     * because of '#define WEECHAT_RC_OK 0'
     */
    if (rc ==  NULL)
        rc = PyLong_FromLong ((long)0);

    if (PyErr_Occurred ())
    {
        PyErr_Print ();
        Py_XDECREF(rc);
    }
    else if ((ret_type == WEECHAT_SCRIPT_EXEC_STRING) && (PyUnicode_Check (rc)))
    {
        ret_value = weechat_python_unicode_to_string (rc);
        Py_XDECREF(rc);
    }
    else if ((ret_type == WEECHAT_SCRIPT_EXEC_STRING) && (PyBytes_Check (rc)))
    {
        if (PyBytes_AsString (rc))
            ret_value = strdup (PyBytes_AsString (rc));
        else
            ret_value = NULL;
        Py_XDECREF(rc);
    }
    else if ((ret_type == WEECHAT_SCRIPT_EXEC_POINTER) && (PyUnicode_Check (rc)))
    {
        ret_temp = weechat_python_unicode_to_string (rc);
        if (ret_temp)
        {
            ret_value = plugin_script_str2ptr (weechat_python_plugin,
                                               script->name, function,
                                               ret_temp);
            free (ret_temp);
        }
        else
        {
            ret_value = NULL;
        }
        Py_XDECREF(rc);
    }
    else if ((ret_type == WEECHAT_SCRIPT_EXEC_POINTER) && (PyBytes_Check (rc)))
    {
        if (PyBytes_AsString (rc))
        {
            ret_value = plugin_script_str2ptr (weechat_python_plugin,
                                               script->name, function,
                                               PyBytes_AsString (rc));
        }
        else
        {
            ret_value = NULL;
        }
        Py_XDECREF(rc);
    }
    else if ((ret_type == WEECHAT_SCRIPT_EXEC_INT) && (PY_INTEGER_CHECK(rc)))
    {
        ret_int = malloc (sizeof (*ret_int));
        if (ret_int)
            *ret_int = (int) PyLong_AsLong (rc);
        ret_value = ret_int;
        Py_XDECREF(rc);
    }
    else if (ret_type == WEECHAT_SCRIPT_EXEC_HASHTABLE)
    {
        ret_value = weechat_python_dict_to_hashtable (rc,
                                                      WEECHAT_SCRIPT_HASHTABLE_DEFAULT_SIZE,
                                                      WEECHAT_HASHTABLE_STRING,
                                                      WEECHAT_HASHTABLE_STRING);
        Py_XDECREF(rc);
    }
    else
    {
        if (ret_type != WEECHAT_SCRIPT_EXEC_IGNORE)
        {
            weechat_printf (NULL,
                            weechat_gettext ("%s%s: function \"%s\" must "
                                             "return a valid value"),
                            weechat_prefix ("error"), PYTHON_PLUGIN_NAME,
                            function);
        }
        Py_XDECREF(rc);
    }

    if ((ret_type != WEECHAT_SCRIPT_EXEC_IGNORE) && !ret_value)
    {
        weechat_printf (NULL,
                        weechat_gettext ("%s%s: error in function \"%s\""),
                        weechat_prefix ("error"), PYTHON_PLUGIN_NAME,
                        function);
    }

    /* PyEval_ReleaseThread (python_current_script->interpreter); */

end:
    python_current_script = old_python_current_script;

    if (old_interpreter)
        PyThreadState_Swap (old_interpreter);

    return ret_value;
}

/*
 * Initializes the "weechat" module.
 */

#if PY_MAJOR_VERSION >= 3
static PyObject *weechat_python_init_module_weechat ()
#else
void weechat_python_init_module_weechat ()
#endif /* PY_MAJOR_VERSION >= 3 */
{
    PyObject *weechat_module, *weechat_dict;

#if PY_MAJOR_VERSION >= 3
    /* python >= 3.x */
    weechat_module = PyModule_Create (&moduleDef);
#else
    /* python <= 2.x */
    weechat_module = Py_InitModule ("weechat", weechat_python_funcs);
#endif /* PY_MAJOR_VERSION >= 3 */

    if (!weechat_module)
    {
        weechat_printf (NULL,
                        weechat_gettext ("%s%s: unable to initialize WeeChat "
                                         "module"),
                        weechat_prefix ("error"), PYTHON_PLUGIN_NAME);
#if PY_MAJOR_VERSION >= 3
        return NULL;
#else
        return;
#endif /* PY_MAJOR_VERSION >= 3 */
    }

    /* define some constants */
    weechat_dict = PyModule_GetDict (weechat_module);
    PyDict_SetItemString (weechat_dict, "WEECHAT_RC_OK", PyLong_FromLong ((long)WEECHAT_RC_OK));
    PyDict_SetItemString (weechat_dict, "WEECHAT_RC_OK_EAT", PyLong_FromLong ((long)WEECHAT_RC_OK_EAT));
    PyDict_SetItemString (weechat_dict, "WEECHAT_RC_ERROR", PyLong_FromLong ((long)WEECHAT_RC_ERROR));

    PyDict_SetItemString (weechat_dict, "WEECHAT_CONFIG_READ_OK", PyLong_FromLong ((long)WEECHAT_CONFIG_READ_OK));
    PyDict_SetItemString (weechat_dict, "WEECHAT_CONFIG_READ_MEMORY_ERROR", PyLong_FromLong ((long)WEECHAT_CONFIG_READ_MEMORY_ERROR));
    PyDict_SetItemString (weechat_dict, "WEECHAT_CONFIG_READ_FILE_NOT_FOUND", PyLong_FromLong ((long)WEECHAT_CONFIG_READ_FILE_NOT_FOUND));
    PyDict_SetItemString (weechat_dict, "WEECHAT_CONFIG_WRITE_OK", PyLong_FromLong ((long)WEECHAT_CONFIG_WRITE_OK));
    PyDict_SetItemString (weechat_dict, "WEECHAT_CONFIG_WRITE_ERROR", PyLong_FromLong ((long)WEECHAT_CONFIG_WRITE_ERROR));
    PyDict_SetItemString (weechat_dict, "WEECHAT_CONFIG_WRITE_MEMORY_ERROR", PyLong_FromLong ((long)WEECHAT_CONFIG_WRITE_MEMORY_ERROR));
    PyDict_SetItemString (weechat_dict, "WEECHAT_CONFIG_OPTION_SET_OK_CHANGED", PyLong_FromLong ((long)WEECHAT_CONFIG_OPTION_SET_OK_CHANGED));
    PyDict_SetItemString (weechat_dict, "WEECHAT_CONFIG_OPTION_SET_OK_SAME_VALUE", PyLong_FromLong ((long)WEECHAT_CONFIG_OPTION_SET_OK_SAME_VALUE));
    PyDict_SetItemString (weechat_dict, "WEECHAT_CONFIG_OPTION_SET_ERROR", PyLong_FromLong ((long)WEECHAT_CONFIG_OPTION_SET_ERROR));
    PyDict_SetItemString (weechat_dict, "WEECHAT_CONFIG_OPTION_SET_OPTION_NOT_FOUND", PyLong_FromLong ((long)WEECHAT_CONFIG_OPTION_SET_OPTION_NOT_FOUND));
    PyDict_SetItemString (weechat_dict, "WEECHAT_CONFIG_OPTION_UNSET_OK_NO_RESET", PyLong_FromLong ((long)WEECHAT_CONFIG_OPTION_UNSET_OK_NO_RESET));
    PyDict_SetItemString (weechat_dict, "WEECHAT_CONFIG_OPTION_UNSET_OK_RESET", PyLong_FromLong ((long)WEECHAT_CONFIG_OPTION_UNSET_OK_RESET));
    PyDict_SetItemString (weechat_dict, "WEECHAT_CONFIG_OPTION_UNSET_OK_REMOVED", PyLong_FromLong ((long)WEECHAT_CONFIG_OPTION_UNSET_OK_REMOVED));
    PyDict_SetItemString (weechat_dict, "WEECHAT_CONFIG_OPTION_UNSET_ERROR", PyLong_FromLong ((long)WEECHAT_CONFIG_OPTION_UNSET_ERROR));

    PyDict_SetItemString (weechat_dict, "WEECHAT_LIST_POS_SORT", PyUnicode_FromString (WEECHAT_LIST_POS_SORT));
    PyDict_SetItemString (weechat_dict, "WEECHAT_LIST_POS_BEGINNING", PyUnicode_FromString (WEECHAT_LIST_POS_BEGINNING));
    PyDict_SetItemString (weechat_dict, "WEECHAT_LIST_POS_END", PyUnicode_FromString (WEECHAT_LIST_POS_END));

    PyDict_SetItemString (weechat_dict, "WEECHAT_HOTLIST_LOW", PyUnicode_FromString (WEECHAT_HOTLIST_LOW));
    PyDict_SetItemString (weechat_dict, "WEECHAT_HOTLIST_MESSAGE", PyUnicode_FromString (WEECHAT_HOTLIST_MESSAGE));
    PyDict_SetItemString (weechat_dict, "WEECHAT_HOTLIST_PRIVATE", PyUnicode_FromString (WEECHAT_HOTLIST_PRIVATE));
    PyDict_SetItemString (weechat_dict, "WEECHAT_HOTLIST_HIGHLIGHT", PyUnicode_FromString (WEECHAT_HOTLIST_HIGHLIGHT));

    PyDict_SetItemString (weechat_dict, "WEECHAT_HOOK_PROCESS_RUNNING", PyLong_FromLong ((long)WEECHAT_HOOK_PROCESS_RUNNING));
    PyDict_SetItemString (weechat_dict, "WEECHAT_HOOK_PROCESS_ERROR", PyLong_FromLong ((long)WEECHAT_HOOK_PROCESS_ERROR));

    PyDict_SetItemString (weechat_dict, "WEECHAT_HOOK_CONNECT_OK", PyLong_FromLong ((long)WEECHAT_HOOK_CONNECT_OK));
    PyDict_SetItemString (weechat_dict, "WEECHAT_HOOK_CONNECT_ADDRESS_NOT_FOUND", PyLong_FromLong ((long)WEECHAT_HOOK_CONNECT_ADDRESS_NOT_FOUND));
    PyDict_SetItemString (weechat_dict, "WEECHAT_HOOK_CONNECT_IP_ADDRESS_NOT_FOUND", PyLong_FromLong ((long)WEECHAT_HOOK_CONNECT_IP_ADDRESS_NOT_FOUND));
    PyDict_SetItemString (weechat_dict, "WEECHAT_HOOK_CONNECT_CONNECTION_REFUSED", PyLong_FromLong ((long)WEECHAT_HOOK_CONNECT_CONNECTION_REFUSED));
    PyDict_SetItemString (weechat_dict, "WEECHAT_HOOK_CONNECT_PROXY_ERROR", PyLong_FromLong ((long)WEECHAT_HOOK_CONNECT_PROXY_ERROR));
    PyDict_SetItemString (weechat_dict, "WEECHAT_HOOK_CONNECT_LOCAL_HOSTNAME_ERROR", PyLong_FromLong ((long)WEECHAT_HOOK_CONNECT_LOCAL_HOSTNAME_ERROR));
    PyDict_SetItemString (weechat_dict, "WEECHAT_HOOK_CONNECT_GNUTLS_INIT_ERROR", PyLong_FromLong ((long)WEECHAT_HOOK_CONNECT_GNUTLS_INIT_ERROR));
    PyDict_SetItemString (weechat_dict, "WEECHAT_HOOK_CONNECT_GNUTLS_HANDSHAKE_ERROR", PyLong_FromLong ((long)WEECHAT_HOOK_CONNECT_GNUTLS_HANDSHAKE_ERROR));
    PyDict_SetItemString (weechat_dict, "WEECHAT_HOOK_CONNECT_MEMORY_ERROR", PyLong_FromLong ((long)WEECHAT_HOOK_CONNECT_MEMORY_ERROR));
    PyDict_SetItemString (weechat_dict, "WEECHAT_HOOK_CONNECT_TIMEOUT", PyLong_FromLong ((long)WEECHAT_HOOK_CONNECT_TIMEOUT));
    PyDict_SetItemString (weechat_dict, "WEECHAT_HOOK_CONNECT_SOCKET_ERROR", PyLong_FromLong ((long)WEECHAT_HOOK_CONNECT_SOCKET_ERROR));

    PyDict_SetItemString (weechat_dict, "WEECHAT_HOOK_SIGNAL_STRING", PyUnicode_FromString (WEECHAT_HOOK_SIGNAL_STRING));
    PyDict_SetItemString (weechat_dict, "WEECHAT_HOOK_SIGNAL_INT", PyUnicode_FromString (WEECHAT_HOOK_SIGNAL_INT));
    PyDict_SetItemString (weechat_dict, "WEECHAT_HOOK_SIGNAL_POINTER", PyUnicode_FromString (WEECHAT_HOOK_SIGNAL_POINTER));

#if PY_MAJOR_VERSION >= 3
    return weechat_module;
#endif /* PY_MAJOR_VERSION >= 3 */
}

/*
 * Sets weechat output (to redirect stdout/stderr to WeeChat buffer).
 */

void
weechat_python_set_output ()
{
    PyObject *weechat_outputs;

#if PY_MAJOR_VERSION >= 3
    /* python >= 3.x */
    weechat_outputs = PyModule_Create (&moduleDefOutputs);
#else
    /* python <= 2.x */
    weechat_outputs = Py_InitModule ("weechatOutputs",
                                     weechat_python_output_funcs);
#endif /* PY_MAJOR_VERSION >= 3 */

    if (weechat_outputs)
    {
        if (PySys_SetObject ("stdout", weechat_outputs) == -1)
        {
            weechat_printf (NULL,
                            weechat_gettext ("%s%s: unable to redirect stdout"),
                            weechat_prefix ("error"), PYTHON_PLUGIN_NAME);
        }
        if (PySys_SetObject ("stderr", weechat_outputs) == -1)
        {
            weechat_printf (NULL,
                            weechat_gettext ("%s%s: unable to redirect stderr"),
                            weechat_prefix ("error"), PYTHON_PLUGIN_NAME);
        }
    }
    else
    {
        weechat_printf (NULL,
                        weechat_gettext ("%s%s: unable to redirect stdout and "
                                         "stderr"),
                        weechat_prefix ("error"), PYTHON_PLUGIN_NAME);
    }
}

/*
 * Loads a python script.
 *
 * If code is NULL, the content of filename is read and executed.
 * If code is not NULL, it is executed (the file is not read).
 *
 * Returns pointer to new registered script, NULL if error.
 */

struct t_plugin_script *
weechat_python_load (const char *filename, const char *code)
{
    char *argv[] = { "__weechat_plugin__" , NULL };
#if PY_MAJOR_VERSION >= 3
    wchar_t *wargv[] = { NULL, NULL };
#endif /* PY_MAJOR_VERSION >= 3 */
    FILE *fp;
    PyObject *python_path, *path, *module_main, *globals, *rc;
    const char *weechat_home;
    char *str_home;
    int len;

    fp = NULL;

    if (!code)
    {
        fp = fopen (filename, "r");
        if (!fp)
        {
            weechat_printf (NULL,
                            weechat_gettext ("%s%s: script \"%s\" not found"),
                            weechat_prefix ("error"), PYTHON_PLUGIN_NAME,
                            filename);
            return NULL;
        }
    }

    if ((weechat_python_plugin->debug >= 2) || !python_quiet)
    {
        weechat_printf (NULL,
                        weechat_gettext ("%s: loading script \"%s\""),
                        PYTHON_PLUGIN_NAME, filename);
    }

    python_current_script = NULL;
    python_registered_script = NULL;

    /* PyEval_AcquireLock (); */
    python_current_interpreter = Py_NewInterpreter ();
#if PY_MAJOR_VERSION >= 3
    /* python >= 3.x */
    len = mbstowcs (NULL, argv[0], 0) + 1;
    wargv[0] = malloc ((len + 1) * sizeof (wargv[0][0]));
    if (wargv[0])
    {
        if (mbstowcs (wargv[0], argv[0], len) == (size_t)(-1))
        {
            free (wargv[0]);
            wargv[0] = NULL;
        }
        PySys_SetArgv (1, wargv);
        if (wargv[0])
            free (wargv[0]);
    }
#else
    /* python <= 2.x */
    PySys_SetArgv (1, argv);
#endif /* PY_MAJOR_VERSION >= 3 */

    if (!python_current_interpreter)
    {
        weechat_printf (NULL,
                        weechat_gettext ("%s%s: unable to create new "
                                         "sub-interpreter"),
                        weechat_prefix ("error"), PYTHON_PLUGIN_NAME);
        if (fp)
            fclose (fp);
        /* PyEval_ReleaseLock (); */
        return NULL;
    }

    PyThreadState_Swap (python_current_interpreter);

    /* adding $weechat_dir/python in $PYTHONPATH */
    python_path = PySys_GetObject ("path");
    weechat_home = weechat_info_get ("weechat_dir", "");
    if (weechat_home)
    {
        len = strlen (weechat_home) + 1 + strlen (PYTHON_PLUGIN_NAME) + 1;
        str_home = malloc (len);
        if (str_home)
        {
            snprintf (str_home, len, "%s/python", weechat_home);
#if PY_MAJOR_VERSION >= 3
            /* python >= 3.x */
            path = PyUnicode_FromString (str_home);
#else
            /* python <= 2.x */
            path = PyBytes_FromString (str_home);
#endif /* PY_MAJOR_VERSION >= 3 */
            if (path != NULL)
            {
                PyList_Insert (python_path, 0, path);
                Py_DECREF (path);
            }
            free (str_home);
        }
    }

    weechat_python_set_output ();

    python_current_script_filename = filename;

    if (code)
    {
        /* execute code without reading file */
        module_main = PyImport_AddModule ("__main__");
        globals = PyModule_GetDict (module_main);
        rc = PyRun_String (code, Py_file_input, globals, NULL);
        if (PyErr_Occurred ())
        {
            weechat_printf (NULL,
                            weechat_gettext ("%s%s: unable to execute source "
                                             "code"),
                            weechat_prefix ("error"), PYTHON_PLUGIN_NAME);
            PyErr_Print ();
            if (rc)
                Py_XDECREF(rc);

            /* if script was registered, remove it from list */
            if (python_current_script)
            {
                plugin_script_remove (weechat_python_plugin,
                                      &python_scripts, &last_python_script,
                                      python_current_script);
                python_current_script = NULL;
            }

            Py_EndInterpreter (python_current_interpreter);
            /* PyEval_ReleaseLock (); */

            return NULL;
        }
        if (rc)
            Py_XDECREF(rc);
    }
    else
    {
        /* read and execute code from file */
        if (PyRun_SimpleFile (fp, filename) != 0)
        {
            weechat_printf (NULL,
                            weechat_gettext ("%s%s: unable to parse file \"%s\""),
                            weechat_prefix ("error"), PYTHON_PLUGIN_NAME, filename);
            fclose (fp);

            if (PyErr_Occurred ())
                PyErr_Print ();

            /* if script was registered, remove it from list */
            if (python_current_script)
            {
                plugin_script_remove (weechat_python_plugin,
                                      &python_scripts, &last_python_script,
                                      python_current_script);
                python_current_script = NULL;
            }

            Py_EndInterpreter (python_current_interpreter);
            /* PyEval_ReleaseLock (); */

            return NULL;
        }
        fclose (fp);
    }

    if (PyErr_Occurred ())
        PyErr_Print ();

    if (!python_registered_script)
    {
        weechat_printf (NULL,
                        weechat_gettext ("%s%s: function \"register\" not "
                                         "found (or failed) in file \"%s\""),
                        weechat_prefix ("error"), PYTHON_PLUGIN_NAME, filename);

        if (PyErr_Occurred ())
            PyErr_Print ();
        Py_EndInterpreter (python_current_interpreter);
        /* PyEval_ReleaseLock (); */

        return NULL;
    }
    python_current_script = python_registered_script;

    /* PyEval_ReleaseThread (python_current_script->interpreter); */

    /*
     * set input/close callbacks for buffers created by this script
     * (to restore callbacks after upgrade)
     */
    plugin_script_set_buffer_callbacks (weechat_python_plugin,
                                        python_scripts,
                                        python_current_script,
                                        &weechat_python_api_buffer_input_data_cb,
                                        &weechat_python_api_buffer_close_cb);

    (void) weechat_hook_signal_send ("python_script_loaded",
                                     WEECHAT_HOOK_SIGNAL_STRING,
                                     python_current_script->filename);

    return python_current_script;
}

/*
 * Callback for script_auto_load() function.
 */

void
weechat_python_load_cb (void *data, const char *filename)
{
    /* make C compiler happy */
    (void) data;

    weechat_python_load (filename, NULL);
}

/*
 * Unloads a python script.
 */

void
weechat_python_unload (struct t_plugin_script *script)
{
    int *rc;
    void *interpreter;
    char *filename;

    if ((weechat_python_plugin->debug >= 2) || !python_quiet)
    {
        weechat_printf (NULL,
                        weechat_gettext ("%s: unloading script \"%s\""),
                        PYTHON_PLUGIN_NAME, script->name);
    }

    if (script->shutdown_func && script->shutdown_func[0])
    {
        rc = (int *) weechat_python_exec (script, WEECHAT_SCRIPT_EXEC_INT,
                                          script->shutdown_func, NULL, NULL);
        if (rc)
            free (rc);
    }

    filename = strdup (script->filename);
    interpreter = script->interpreter;

    if (python_current_script == script)
    {
        python_current_script = (python_current_script->prev_script) ?
            python_current_script->prev_script : python_current_script->next_script;
    }

    plugin_script_remove (weechat_python_plugin, &python_scripts, &last_python_script,
                          script);

    if (interpreter)
    {
        PyThreadState_Swap (interpreter);
        Py_EndInterpreter (interpreter);
    }

    if (python_current_script)
        PyThreadState_Swap (python_current_script->interpreter);

    (void) weechat_hook_signal_send ("python_script_unloaded",
                                     WEECHAT_HOOK_SIGNAL_STRING, filename);
    if (filename)
        free (filename);
}

/*
 * Unloads a python script by name.
 */

void
weechat_python_unload_name (const char *name)
{
    struct t_plugin_script *ptr_script;

    ptr_script = plugin_script_search (weechat_python_plugin, python_scripts, name);
    if (ptr_script)
    {
        weechat_python_unload (ptr_script);
        if (!python_quiet)
        {
            weechat_printf (NULL,
                            weechat_gettext ("%s: script \"%s\" unloaded"),
                            PYTHON_PLUGIN_NAME, name);
        }
    }
    else
    {
        weechat_printf (NULL,
                        weechat_gettext ("%s%s: script \"%s\" not loaded"),
                        weechat_prefix ("error"), PYTHON_PLUGIN_NAME, name);
    }
}

/*
 * Unloads all python scripts.
 */

void
weechat_python_unload_all ()
{
    while (python_scripts)
    {
        weechat_python_unload (python_scripts);
    }
}

/*
 * Reloads a python script by name.
 */

void
weechat_python_reload_name (const char *name)
{
    struct t_plugin_script *ptr_script;
    char *filename;

    ptr_script = plugin_script_search (weechat_python_plugin, python_scripts, name);
    if (ptr_script)
    {
        filename = strdup (ptr_script->filename);
        if (filename)
        {
            weechat_python_unload (ptr_script);
            if (!python_quiet)
            {
                weechat_printf (NULL,
                                weechat_gettext ("%s: script \"%s\" unloaded"),
                                PYTHON_PLUGIN_NAME, name);
            }
            weechat_python_load (filename, NULL);
            free (filename);
        }
    }
    else
    {
        weechat_printf (NULL,
                        weechat_gettext ("%s%s: script \"%s\" not loaded"),
                        weechat_prefix ("error"), PYTHON_PLUGIN_NAME, name);
    }
}

/*
 * Evaluates python source code.
 *
 * Returns:
 *   1: OK
 *   0: error
 */

int
weechat_python_eval (struct t_gui_buffer *buffer, int send_to_buffer_as_input,
                     int exec_commands, const char *code)
{
    void *func_argv[1], *result;

    if (!python_script_eval)
    {
        python_quiet = 1;
        python_script_eval = weechat_python_load (WEECHAT_SCRIPT_EVAL_NAME,
                                                  PYTHON_EVAL_SCRIPT);
        python_quiet = 0;
        if (!python_script_eval)
            return 0;
    }

    weechat_python_output_flush ();

    python_eval_mode = 1;
    python_eval_send_input = send_to_buffer_as_input;
    python_eval_exec_commands = exec_commands;
    python_eval_buffer = buffer;

    func_argv[0] = (char *)code;
    result = weechat_python_exec (python_script_eval,
                                  WEECHAT_SCRIPT_EXEC_IGNORE,
                                  "script_python_eval",
                                  "s", func_argv);
    /* result is ignored */
    if (result)
        free (result);

    weechat_python_output_flush ();

    python_eval_mode = 0;
    python_eval_send_input = 0;
    python_eval_exec_commands = 0;
    python_eval_buffer = NULL;

    if (!weechat_config_boolean (python_config_look_eval_keep_context))
    {
        python_quiet = 1;
        weechat_python_unload (python_script_eval);
        python_quiet = 0;
        python_script_eval = NULL;
    }

    return 1;
}

/*
 * Callback for command "/python".
 */

int
weechat_python_command_cb (const void *pointer, void *data,
                           struct t_gui_buffer *buffer,
                           int argc, char **argv, char **argv_eol)
{
    char *ptr_name, *ptr_code, *path_script;
    int i, send_to_buffer_as_input, exec_commands;

    /* make C compiler happy */
    (void) pointer;
    (void) data;

    if (argc == 1)
    {
        plugin_script_display_list (weechat_python_plugin, python_scripts,
                                    NULL, 0);
    }
    else if (argc == 2)
    {
        if (weechat_strcasecmp (argv[1], "list") == 0)
        {
            plugin_script_display_list (weechat_python_plugin, python_scripts,
                                        NULL, 0);
        }
        else if (weechat_strcasecmp (argv[1], "listfull") == 0)
        {
            plugin_script_display_list (weechat_python_plugin, python_scripts,
                                        NULL, 1);
        }
        else if (weechat_strcasecmp (argv[1], "autoload") == 0)
        {
            plugin_script_auto_load (weechat_python_plugin, &weechat_python_load_cb);
        }
        else if (weechat_strcasecmp (argv[1], "reload") == 0)
        {
            weechat_python_unload_all ();
            plugin_script_auto_load (weechat_python_plugin, &weechat_python_load_cb);
        }
        else if (weechat_strcasecmp (argv[1], "unload") == 0)
        {
            weechat_python_unload_all ();
        }
        else if (weechat_strcasecmp (argv[1], "version") == 0)
        {
            plugin_script_display_interpreter (weechat_python_plugin, 0);
        }
        else
            WEECHAT_COMMAND_ERROR;
    }
    else
    {
        if (weechat_strcasecmp (argv[1], "list") == 0)
        {
            plugin_script_display_list (weechat_python_plugin, python_scripts,
                                        argv_eol[2], 0);
        }
        else if (weechat_strcasecmp (argv[1], "listfull") == 0)
        {
            plugin_script_display_list (weechat_python_plugin, python_scripts,
                                        argv_eol[2], 1);
        }
        else if ((weechat_strcasecmp (argv[1], "load") == 0)
                 || (weechat_strcasecmp (argv[1], "reload") == 0)
                 || (weechat_strcasecmp (argv[1], "unload") == 0))
        {
            ptr_name = argv_eol[2];
            if (strncmp (ptr_name, "-q ", 3) == 0)
            {
                python_quiet = 1;
                ptr_name += 3;
                while (ptr_name[0] == ' ')
                {
                    ptr_name++;
                }
            }
            if (weechat_strcasecmp (argv[1], "load") == 0)
            {
                /* load python script */
                path_script = plugin_script_search_path (weechat_python_plugin,
                                                         ptr_name);
                weechat_python_load ((path_script) ? path_script : ptr_name,
                                     NULL);
                if (path_script)
                    free (path_script);
            }
            else if (weechat_strcasecmp (argv[1], "reload") == 0)
            {
                /* reload one python script */
                weechat_python_reload_name (ptr_name);
            }
            else if (weechat_strcasecmp (argv[1], "unload") == 0)
            {
                /* unload python script */
                weechat_python_unload_name (ptr_name);
            }
            python_quiet = 0;
        }
        else if (weechat_strcasecmp (argv[1], "eval") == 0)
        {
            send_to_buffer_as_input = 0;
            exec_commands = 0;
            ptr_code = argv_eol[2];
            for (i = 2; i < argc; i++)
            {
                if (argv[i][0] == '-')
                {
                    if (strcmp (argv[i], "-o") == 0)
                    {
                        if (i + 1 >= argc)
                            WEECHAT_COMMAND_ERROR;
                        send_to_buffer_as_input = 1;
                        exec_commands = 0;
                        ptr_code = argv_eol[i + 1];
                    }
                    else if (strcmp (argv[i], "-oc") == 0)
                    {
                        if (i + 1 >= argc)
                            WEECHAT_COMMAND_ERROR;
                        send_to_buffer_as_input = 1;
                        exec_commands = 1;
                        ptr_code = argv_eol[i + 1];
                    }
                }
                else
                    break;
            }
            if (!weechat_python_eval (buffer, send_to_buffer_as_input,
                                      exec_commands, ptr_code))
                WEECHAT_COMMAND_ERROR;
        }
        else
            WEECHAT_COMMAND_ERROR;
    }

    return WEECHAT_RC_OK;
}

/*
 * Adds python scripts to completion list.
 */

int
weechat_python_completion_cb (const void *pointer, void *data,
                              const char *completion_item,
                              struct t_gui_buffer *buffer,
                              struct t_gui_completion *completion)
{
    /* make C compiler happy */
    (void) pointer;
    (void) data;
    (void) completion_item;
    (void) buffer;

    plugin_script_completion (weechat_python_plugin, completion, python_scripts);

    return WEECHAT_RC_OK;
}

/*
 * Returns hdata for python scripts.
 */

struct t_hdata *
weechat_python_hdata_cb (const void *pointer, void *data,
                         const char *hdata_name)
{
    /* make C compiler happy */
    (void) pointer;
    (void) data;

    return plugin_script_hdata_script (weechat_plugin,
                                       &python_scripts, &last_python_script,
                                       hdata_name);
}

/*
 * Returns python info "python2_bin".
 */

const char *
weechat_python_info_python2_bin_cb (const void *pointer, void *data,
                                    const char *info_name,
                                    const char *arguments)
{
    int rc;
    struct stat stat_buf;

    /* make C compiler happy */
    (void) pointer;
    (void) data;
    (void) info_name;
    (void) arguments;

    if (python2_bin && (strcmp (python2_bin, "python") != 0))
    {
        rc = stat (python2_bin, &stat_buf);
        if ((rc != 0) || (!S_ISREG(stat_buf.st_mode)))
        {
            free (python2_bin);
            python2_bin = weechat_python_get_python2_bin ();
        }
    }
    return python2_bin;
}

/*
 * Returns python info "python_eval".
 */

const char *
weechat_python_info_eval_cb (const void *pointer, void *data,
                             const char *info_name,
                             const char *arguments)
{
    /* make C compiler happy */
    (void) pointer;
    (void) data;
    (void) info_name;

    weechat_python_eval (NULL, 0, 0, (arguments) ? arguments : "");
    if (python_eval_output)
        free (python_eval_output);
    python_eval_output = strdup (*python_buffer_output);
    weechat_string_dyn_copy (python_buffer_output, NULL);

    return python_eval_output;
}

/*
 * Returns infolist with python scripts.
 */

struct t_infolist *
weechat_python_infolist_cb (const void *pointer, void *data,
                            const char *infolist_name,
                            void *obj_pointer, const char *arguments)
{
    /* make C compiler happy */
    (void) pointer;
    (void) data;

    if (!infolist_name || !infolist_name[0])
        return NULL;

    if (weechat_strcasecmp (infolist_name, "python_script") == 0)
    {
        return plugin_script_infolist_list_scripts (weechat_python_plugin,
                                                    python_scripts,
                                                    obj_pointer,
                                                    arguments);
    }

    return NULL;
}

/*
 * Dumps python plugin data in WeeChat log file.
 */

int
weechat_python_signal_debug_dump_cb (const void *pointer, void *data,
                                     const char *signal,
                                     const char *type_data, void *signal_data)
{
    /* make C compiler happy */
    (void) pointer;
    (void) data;
    (void) signal;
    (void) type_data;

    if (!signal_data
        || (weechat_strcasecmp ((char *)signal_data, PYTHON_PLUGIN_NAME) == 0))
    {
        plugin_script_print_log (weechat_python_plugin, python_scripts);
    }

    return WEECHAT_RC_OK;
}

/*
 * Timer for executing actions.
 */

int
weechat_python_timer_action_cb (const void *pointer, void *data,
                                int remaining_calls)
{
    /* make C compiler happy */
    (void) data;
    (void) remaining_calls;

    if (pointer)
    {
        if (pointer == &python_action_install_list)
        {
            plugin_script_action_install (weechat_python_plugin,
                                          python_scripts,
                                          &weechat_python_unload,
                                          &weechat_python_load,
                                          &python_quiet,
                                          &python_action_install_list);
        }
        else if (pointer == &python_action_remove_list)
        {
            plugin_script_action_remove (weechat_python_plugin,
                                         python_scripts,
                                         &weechat_python_unload,
                                         &python_quiet,
                                         &python_action_remove_list);
        }
        else if (pointer == &python_action_autoload_list)
        {
            plugin_script_action_autoload (weechat_python_plugin,
                                           &python_quiet,
                                           &python_action_autoload_list);
        }
    }

    return WEECHAT_RC_OK;
}

/*
 * Callback called when a script action is asked (install/remove/autoload a
 * script).
 */

int
weechat_python_signal_script_action_cb (const void *pointer, void *data,
                                        const char *signal,
                                        const char *type_data,
                                        void *signal_data)
{
    /* make C compiler happy */
    (void) pointer;
    (void) data;

    if (strcmp (type_data, WEECHAT_HOOK_SIGNAL_STRING) == 0)
    {
        if (strcmp (signal, "python_script_install") == 0)
        {
            plugin_script_action_add (&python_action_install_list,
                                      (const char *)signal_data);
            weechat_hook_timer (1, 0, 1,
                                &weechat_python_timer_action_cb,
                                &python_action_install_list, NULL);
        }
        else if (strcmp (signal, "python_script_remove") == 0)
        {
            plugin_script_action_add (&python_action_remove_list,
                                      (const char *)signal_data);
            weechat_hook_timer (1, 0, 1,
                                &weechat_python_timer_action_cb,
                                &python_action_remove_list, NULL);
        }
        else if (strcmp (signal, "python_script_autoload") == 0)
        {
            plugin_script_action_add (&python_action_autoload_list,
                                      (const char *)signal_data);
            weechat_hook_timer (1, 0, 1,
                                &weechat_python_timer_action_cb,
                                &python_action_autoload_list, NULL);
        }
    }

    return WEECHAT_RC_OK;
}

/*
 * Initializes python plugin.
 */

int
weechat_plugin_init (struct t_weechat_plugin *plugin, int argc, char *argv[])
{
    weechat_python_plugin = plugin;

    /* set interpreter name and version */
    weechat_hashtable_set (plugin->variables, "interpreter_name",
                           plugin->name);
#ifdef PY_VERSION
    weechat_hashtable_set (plugin->variables, "interpreter_version",
                           PY_VERSION);
#else
    weechat_hashtable_set (plugin->variables, "interpreter_version",
                           "");
#endif /* PY_VERSION */

    /* init stdout/stderr buffer */
    python_buffer_output = weechat_string_dyn_alloc (256);
    if (!python_buffer_output)
        return WEECHAT_RC_ERROR;

    /*
     * hook info to get path to python 2.x interpreter
     * (some scripts using hook_process need that)
     */
    python2_bin = weechat_python_get_python2_bin ();
    weechat_hook_info ("python2_bin",
                       N_("path to python 2.x interpreter"),
                       NULL,
                       &weechat_python_info_python2_bin_cb, NULL, NULL);

    PyImport_AppendInittab ("weechat",
                            &weechat_python_init_module_weechat);

    Py_Initialize ();
    if (Py_IsInitialized () == 0)
    {
        weechat_printf (NULL,
                        weechat_gettext ("%s%s: unable to launch global "
                                         "interpreter"),
                        weechat_prefix ("error"), PYTHON_PLUGIN_NAME);
        weechat_string_dyn_free (python_buffer_output, 1);
        return WEECHAT_RC_ERROR;
    }

    /* PyEval_InitThreads(); */
    /* python_mainThreadState = PyThreadState_Swap(NULL); */
#if PY_VERSION_HEX >= 0x03070000
    python_mainThreadState = PyThreadState_Get();
#else
    python_mainThreadState = PyEval_SaveThread();
#endif
    /* PyEval_ReleaseLock (); */

    if (!python_mainThreadState)
    {
        weechat_printf (NULL,
                        weechat_gettext ("%s%s: unable to get current "
                                         "interpreter state"),
                        weechat_prefix ("error"), PYTHON_PLUGIN_NAME);
        weechat_string_dyn_free (python_buffer_output, 1);
        return WEECHAT_RC_ERROR;
    }

    python_data.config_file = &python_config_file;
    python_data.config_look_check_license = &python_config_look_check_license;
    python_data.config_look_eval_keep_context = &python_config_look_eval_keep_context;
    python_data.scripts = &python_scripts;
    python_data.last_script = &last_python_script;
    python_data.callback_command = &weechat_python_command_cb;
    python_data.callback_completion = &weechat_python_completion_cb;
    python_data.callback_hdata = &weechat_python_hdata_cb;
    python_data.callback_info_eval = &weechat_python_info_eval_cb;
    python_data.callback_infolist = &weechat_python_infolist_cb;
    python_data.callback_signal_debug_dump = &weechat_python_signal_debug_dump_cb;
    python_data.callback_signal_script_action = &weechat_python_signal_script_action_cb;
    python_data.callback_load_file = &weechat_python_load_cb;
    python_data.unload_all = &weechat_python_unload_all;

    python_quiet = 1;
    plugin_script_init (weechat_python_plugin, argc, argv, &python_data);
    python_quiet = 0;

    plugin_script_display_short_list (weechat_python_plugin,
                                      python_scripts);

    /* init OK */
    return WEECHAT_RC_OK;
}

/*
 * Ends python plugin.
 */

int
weechat_plugin_end (struct t_weechat_plugin *plugin)
{
    /* unload all scripts */
    python_quiet = 1;
    if (python_script_eval)
    {
        weechat_python_unload (python_script_eval);
        python_script_eval = NULL;
    }
    plugin_script_end (plugin, &python_data);
    python_quiet = 0;

    /* free python interpreter */
    if (python_mainThreadState != NULL)
    {
        /* PyEval_AcquireLock (); */
        PyThreadState_Swap (python_mainThreadState);
        /* PyEval_ReleaseLock (); */
        python_mainThreadState = NULL;
    }

    Py_Finalize ();
    if (Py_IsInitialized () != 0)
    {
        weechat_printf (NULL,
                        weechat_gettext ("%s%s: unable to free interpreter"),
                        weechat_prefix ("error"), PYTHON_PLUGIN_NAME);
    }

    /* free some data */
    if (python2_bin)
        free (python2_bin);
    if (python_action_install_list)
        free (python_action_install_list);
    if (python_action_remove_list)
        free (python_action_remove_list);
    if (python_action_autoload_list)
        free (python_action_autoload_list);
    weechat_string_dyn_free (python_buffer_output, 1);
    if (python_eval_output)
        free (python_eval_output);

    return WEECHAT_RC_OK;
}
