/*
 * Copyright (C) 2010 Robert Ancell.
 * Author: Robert Ancell <robert.ancell@canonical.com>
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2 or version 3 of the License.
 * See http://www.gnu.org/copyleft/lgpl.html the full text of the license.
 */

#include <config.h>

#include <stdlib.h>
#include <string.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <security/pam_appl.h>

#include "lightdm/greeter.h"

enum {
    PROP_0,
    PROP_DEFAULT_SESSION_HINT,
    PROP_HIDE_USERS_HINT,
    PROP_SHOW_MANUAL_LOGIN_HINT,
    PROP_SHOW_REMOTE_LOGIN_HINT,
    PROP_LOCK_HINT,
    PROP_HAS_GUEST_ACCOUNT_HINT,
    PROP_SELECT_USER_HINT,
    PROP_SELECT_GUEST_HINT,
    PROP_AUTOLOGIN_USER_HINT,
    PROP_AUTOLOGIN_GUEST_HINT,
    PROP_AUTOLOGIN_TIMEOUT_HINT,
    PROP_AUTHENTICATION_USER,
    PROP_IN_AUTHENTICATION,
    PROP_IS_AUTHENTICATED,
};

enum {
    SHOW_PROMPT,
    SHOW_MESSAGE,
    AUTHENTICATION_COMPLETE,
    AUTOLOGIN_TIMER_EXPIRED,
    IDLE,
    RESET,
    LAST_SIGNAL
};
static guint signals[LAST_SIGNAL] = { 0 };

typedef struct
{
    /* TRUE if the daemon can reuse this greeter */
    gboolean resettable;

    /* Socket connection to daemon */
    GSocket *socket;

    /* Channel to write to daemon */
    GIOChannel *to_server_channel;

    /* Channel to read from daemon */
    GIOChannel *from_server_channel;
    guint from_server_watch;

    /* Data read from the daemon */
    guint8 *read_buffer;
    gsize n_read;

    gsize n_responses_waiting;
    GList *responses_received;

    /* TRUE if have got a connect response */
    gboolean connected;

    /* Pending connect requests */
    GList *connect_requests;

    /* Pending start session requests */
    GList *start_session_requests;

    /* Pending ensure shared data dir requests */
    GList *ensure_shared_data_dir_requests;

    /* Hints provided by the daemon */
    GHashTable *hints;

    /* Timeout source to notify greeter to autologin */
    guint autologin_timeout;

    gchar *authentication_user;
    gboolean in_authentication;
    gboolean is_authenticated;
    guint32 authenticate_sequence_number;
    gboolean cancelling_authentication;
} LightDMGreeterPrivate;

G_DEFINE_TYPE (LightDMGreeter, lightdm_greeter, G_TYPE_OBJECT);

#define GET_PRIVATE(obj) G_TYPE_INSTANCE_GET_PRIVATE ((obj), LIGHTDM_TYPE_GREETER, LightDMGreeterPrivate)

#define HEADER_SIZE 8
#define MAX_MESSAGE_LENGTH 1024

/* Messages from the greeter to the server */
typedef enum
{
    GREETER_MESSAGE_CONNECT = 0,
    GREETER_MESSAGE_AUTHENTICATE,
    GREETER_MESSAGE_AUTHENTICATE_AS_GUEST,
    GREETER_MESSAGE_CONTINUE_AUTHENTICATION,
    GREETER_MESSAGE_START_SESSION,
    GREETER_MESSAGE_CANCEL_AUTHENTICATION,
    GREETER_MESSAGE_SET_LANGUAGE,
    GREETER_MESSAGE_AUTHENTICATE_REMOTE,
    GREETER_MESSAGE_ENSURE_SHARED_DIR,
} GreeterMessage;

/* Messages from the server to the greeter */
typedef enum
{
    SERVER_MESSAGE_CONNECTED = 0,
    SERVER_MESSAGE_PROMPT_AUTHENTICATION,
    SERVER_MESSAGE_END_AUTHENTICATION,
    SERVER_MESSAGE_SESSION_RESULT,
    SERVER_MESSAGE_SHARED_DIR_RESULT,
    SERVER_MESSAGE_IDLE,
    SERVER_MESSAGE_RESET,
} ServerMessage;

/* Request sent to server */
typedef struct
{
    GObject parent_instance;
    GCancellable *cancellable;
    GAsyncReadyCallback callback;
    gpointer user_data;
    gboolean complete;
    guint32 return_code;
    gchar *dir;
} Request;
typedef struct
{
    GObjectClass parent_class;
} RequestClass;
GType request_get_type (void);
static void request_iface_init (GAsyncResultIface *iface);
#define REQUEST(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), request_get_type (), Request))
G_DEFINE_TYPE_WITH_CODE (Request, request, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE (G_TYPE_ASYNC_RESULT, request_iface_init));

static gboolean from_server_cb (GIOChannel *source, GIOCondition condition, gpointer data);

GType
lightdm_prompt_type_get_type (void)
{
    static GType enum_type = 0;
  
    if (G_UNLIKELY(enum_type == 0)) {
        static const GEnumValue values[] = {
            { LIGHTDM_PROMPT_TYPE_QUESTION, "LIGHTDM_PROMPT_TYPE_QUESTION", "question" },
            { LIGHTDM_PROMPT_TYPE_SECRET, "LIGHTDM_PROMPT_TYPE_SECRET", "secret" },
            { 0, NULL, NULL }
        };
        enum_type = g_enum_register_static (g_intern_static_string ("LightDMPromptType"), values);
    }

    return enum_type;
}

GType
lightdm_message_type_get_type (void)
{
    static GType enum_type = 0;
  
    if (G_UNLIKELY(enum_type == 0)) {
        static const GEnumValue values[] = {
            { LIGHTDM_MESSAGE_TYPE_INFO, "LIGHTDM_MESSAGE_TYPE_INFO", "info" },
            { LIGHTDM_MESSAGE_TYPE_ERROR, "LIGHTDM_MESSAGE_TYPE_ERROR", "error" },
            { 0, NULL, NULL }
        };
        enum_type = g_enum_register_static (g_intern_static_string ("LightDMMessageType"), values);
    }

    return enum_type;
}


/**
 * lightdm_greeter_new:
 *
 * Create a new greeter.
 *
 * Return value: the new #LightDMGreeter
 **/
LightDMGreeter *
lightdm_greeter_new (void)
{
    return g_object_new (LIGHTDM_TYPE_GREETER, NULL);
}

/**
 * lightdm_greeter_set_resettable:
 * @greeter: A #LightDMGreeter
 * @resettable: Whether the greeter wants to be reset instead of killed after the user logs in
 *
 * Set whether the greeter will be reset instead of killed after the user logs in.
 * This must be called before lightdm_greeter_connect is called.
 **/
void
lightdm_greeter_set_resettable (LightDMGreeter *greeter, gboolean resettable)
{
    LightDMGreeterPrivate *priv;

    g_return_if_fail (LIGHTDM_IS_GREETER (greeter));

    priv = GET_PRIVATE (greeter);

    g_return_if_fail (!priv->connected);
    priv->resettable = resettable;
}

static Request *
request_new (GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    Request *request;

    request = g_object_new (request_get_type (), NULL);
    if (cancellable)
        request->cancellable = g_object_ref (cancellable);
    request->callback = callback;
    request->user_data = user_data;

    return request;
}

static void
request_complete (Request *request, GObject *object)
{
    request->complete = TRUE;

    if (!request->callback)
        return;

    if (request->cancellable && g_cancellable_is_cancelled (request->cancellable))
        return;

    request->callback (object, G_ASYNC_RESULT (request), request->user_data);
}

static gboolean
timed_login_cb (gpointer data)
{
    LightDMGreeter *greeter = data;
    LightDMGreeterPrivate *priv = GET_PRIVATE (greeter);

    priv->autologin_timeout = 0;
    g_signal_emit (G_OBJECT (greeter), signals[AUTOLOGIN_TIMER_EXPIRED], 0);

    return FALSE;
}

static guint32
int_length (void)
{
    return 4;
}

static void
write_int (guint8 *buffer, gint buffer_length, guint32 value, gsize *offset)
{
    if (*offset + 4 >= buffer_length)
        return;
    buffer[*offset] = value >> 24;
    buffer[*offset+1] = (value >> 16) & 0xFF;
    buffer[*offset+2] = (value >> 8) & 0xFF;
    buffer[*offset+3] = value & 0xFF;
    *offset += 4;
}

static void
write_string (guint8 *buffer, gint buffer_length, const gchar *value, gsize *offset)
{
    gint length = 0;

    if (value)
        length = strlen (value);
    write_int (buffer, buffer_length, length, offset);
    if (*offset + length >= buffer_length)
        return;
    if (value)
        memcpy (buffer + *offset, value, length);
    *offset += length;
}

static guint32
read_int (guint8 *message, gsize message_length, gsize *offset)
{
    guint32 value;
    guint8 *buffer;

    if (message_length - *offset < int_length ())
    {
        g_warning ("Not enough space for int, need %i, got %zi", int_length (), message_length - *offset);
        return 0;
    }

    buffer = message + *offset;
    value = buffer[0] << 24 | buffer[1] << 16 | buffer[2] << 8 | buffer[3];
    *offset += int_length ();

    return value;
}

static gchar *
read_string (guint8 *message, gsize message_length, gsize *offset)
{
    guint32 length;
    gchar *value;

    length = read_int (message, message_length, offset);
    if (message_length - *offset < length)
    {
        g_warning ("Not enough space for string, need %u, got %zu", length, message_length - *offset);
        return g_strdup ("");
    }

    value = g_malloc (sizeof (gchar) * (length + 1));
    memcpy (value, message + *offset, length);
    value[length] = '\0';
    *offset += length;

    return value;
}

static guint32
string_length (const gchar *value)
{
    if (value)
        return int_length () + strlen (value);
    else
        return int_length ();
}

static void
write_header (guint8 *buffer, gint buffer_length, guint32 id, guint32 length, gsize *offset)
{
    write_int (buffer, buffer_length, id, offset);
    write_int (buffer, buffer_length, length, offset);
}

static guint32
get_message_length (guint8 *message, gsize message_length)
{
    gsize offset = 4;
    return read_int (message, message_length, &offset);
}

static gboolean
connect_to_daemon (LightDMGreeter *greeter)
{
    LightDMGreeterPrivate *priv = GET_PRIVATE (greeter);
    const gchar *to_server_fd, *from_server_fd, *pipe_path;
    GError *error = NULL;

    if (priv->to_server_channel || priv->from_server_channel)
        return TRUE;

    /* Use private connection if one exists */  
    to_server_fd = g_getenv ("LIGHTDM_TO_SERVER_FD");
    from_server_fd = g_getenv ("LIGHTDM_FROM_SERVER_FD");
    pipe_path = g_getenv ("LIGHTDM_GREETER_PIPE");
    if (to_server_fd && from_server_fd)
    {
        priv->to_server_channel = g_io_channel_unix_new (atoi (to_server_fd));
        priv->from_server_channel = g_io_channel_unix_new (atoi (from_server_fd));
    }
    else if (pipe_path)
    {
        GSocketAddress *address;
        gboolean result;

        priv->socket = g_socket_new (G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, &error);
        if (!priv->socket)
            return FALSE;

        address = g_unix_socket_address_new (pipe_path);
        result = g_socket_connect (priv->socket, address, NULL, &error);
        g_object_unref (address);
        if (!result)
        {
            g_warning ("Failed to connect to greeter socket %s: %s", pipe_path, error->message);
            g_clear_error (&error);
            return FALSE;
        }

        priv->from_server_channel = g_io_channel_unix_new (g_socket_get_fd (priv->socket));
        priv->to_server_channel = g_io_channel_ref (priv->from_server_channel);
    }
    else
    {
        g_warning ("Unable to determine socket to daemon");
        return FALSE;
    }

    priv->from_server_watch = g_io_add_watch (priv->from_server_channel, G_IO_IN, from_server_cb, greeter);

    if (!g_io_channel_set_encoding (priv->to_server_channel, NULL, &error) ||
        !g_io_channel_set_encoding (priv->from_server_channel, NULL, &error))
    {
        g_warning ("Failed to set encoding on from server channel to binary: %s", error->message);
        g_clear_error (&error);
        return FALSE;
    }

    return TRUE;
}

static gboolean
send_message (LightDMGreeter *greeter, guint8 *message, gsize message_length)
{
    LightDMGreeterPrivate *priv = GET_PRIVATE (greeter);
    gchar *data;
    gsize data_length;
    GError *error = NULL;
    guint32 stated_length;

    if (!connect_to_daemon (greeter))
        return FALSE;

    /* Double check that we're sending well-formed messages.  If we say we're
       sending more than we do, we end up DOS'ing lightdm as it waits for the
       rest.  If we say we're sending less than we do, we confuse the heck out
       of lightdm, as it starts reading headers from the middle of our
       messages. */
    stated_length = HEADER_SIZE + get_message_length (message, message_length);
    if (stated_length != message_length)
    {
        g_warning ("Refusing to write malformed packet to daemon: declared size is %u, but actual size is %zu", stated_length, message_length);
        return FALSE;
    }

    data = (gchar *) message;
    data_length = message_length;
    while (data_length > 0)
    {
        GIOStatus status;
        gsize n_written;

        status = g_io_channel_write_chars (priv->to_server_channel, data, data_length, &n_written, &error);
        if (error)
            g_warning ("Error writing to daemon: %s", error->message);
        g_clear_error (&error);
        if (status == G_IO_STATUS_AGAIN)
            continue;
        if (status != G_IO_STATUS_NORMAL)
            return FALSE;
        data_length -= n_written;
        data += n_written;
    }

    g_debug ("Wrote %zi bytes to daemon", message_length);
    g_io_channel_flush (priv->to_server_channel, &error);
    if (error)
        g_warning ("Failed to flush data to daemon: %s", error->message);
    g_clear_error (&error);

    return TRUE;
}

static void
handle_connected (LightDMGreeter *greeter, guint8 *message, gsize message_length, gsize *offset)
{
    LightDMGreeterPrivate *priv = GET_PRIVATE (greeter);
    gchar *version;
    GString *hint_string;
    int timeout;
    Request *request;

    version = read_string (message, message_length, offset);
    hint_string = g_string_new ("");
    while (*offset < message_length)
    {
        gchar *name, *value;

        name = read_string (message, message_length, offset);
        value = read_string (message, message_length, offset);
        g_hash_table_insert (priv->hints, name, value);
        g_string_append_printf (hint_string, " %s=%s", name, value);
    }

    priv->connected = TRUE;
    g_debug ("Connected version=%s%s", version, hint_string->str);
    g_free (version);
    g_string_free (hint_string, TRUE);

    /* Set timeout for default login */
    timeout = lightdm_greeter_get_autologin_timeout_hint (greeter);
    if (timeout)
    {
        g_debug ("Setting autologin timer for %d seconds", timeout);
        priv->autologin_timeout = g_timeout_add (timeout * 1000, timed_login_cb, greeter);
    }

    /* Notify asynchronous caller */
    request = g_list_nth_data (priv->connect_requests, 0);
    if (request)
    {
        request_complete (request, G_OBJECT (greeter));
        priv->connect_requests = g_list_remove (priv->connect_requests, request);
        g_object_unref (request);
    }
}

static void
handle_prompt_authentication (LightDMGreeter *greeter, guint8 *message, gsize message_length, gsize *offset)
{
    LightDMGreeterPrivate *priv = GET_PRIVATE (greeter);
    guint32 sequence_number, n_messages, i;
    gchar *username;

    sequence_number = read_int (message, message_length, offset);
    if (sequence_number != priv->authenticate_sequence_number)
    {
        g_debug ("Ignoring prompt authentication with invalid sequence number %d", sequence_number);
        return;
    }

    if (priv->cancelling_authentication)
    {
        g_debug ("Ignoring prompt authentication as waiting for it to cancel");
        return;
    }

    /* Update username */
    username = read_string (message, message_length, offset);
    if (strcmp (username, "") == 0)
    {
        g_free (username);
        username = NULL;
    }
    g_free (priv->authentication_user);
    priv->authentication_user = username;

    g_list_free_full (priv->responses_received, g_free);
    priv->responses_received = NULL;
    priv->n_responses_waiting = 0;

    n_messages = read_int (message, message_length, offset);
    g_debug ("Prompt user with %d message(s)", n_messages);

    for (i = 0; i < n_messages; i++)
    {
        int style;
        gchar *text;

        style = read_int (message, message_length, offset);
        text = read_string (message, message_length, offset);

        // FIXME: Should stop on prompts?
        switch (style)
        {
        case PAM_PROMPT_ECHO_OFF:
            priv->n_responses_waiting++;
            g_signal_emit (G_OBJECT (greeter), signals[SHOW_PROMPT], 0, text, LIGHTDM_PROMPT_TYPE_SECRET);
            break;
        case PAM_PROMPT_ECHO_ON:
            priv->n_responses_waiting++;
            g_signal_emit (G_OBJECT (greeter), signals[SHOW_PROMPT], 0, text, LIGHTDM_PROMPT_TYPE_QUESTION);
            break;
        case PAM_ERROR_MSG:
            g_signal_emit (G_OBJECT (greeter), signals[SHOW_MESSAGE], 0, text, LIGHTDM_MESSAGE_TYPE_ERROR);
            break;
        case PAM_TEXT_INFO:
            g_signal_emit (G_OBJECT (greeter), signals[SHOW_MESSAGE], 0, text, LIGHTDM_MESSAGE_TYPE_INFO);
            break;
        }

        g_free (text);
    }
}

static void
handle_end_authentication (LightDMGreeter *greeter, guint8 *message, gsize message_length, gsize *offset)
{
    LightDMGreeterPrivate *priv = GET_PRIVATE (greeter);
    guint32 sequence_number, return_code;
    gchar *username;

    sequence_number = read_int (message, message_length, offset);

    if (sequence_number != priv->authenticate_sequence_number)
    {
        g_debug ("Ignoring end authentication with invalid sequence number %d", sequence_number);
        return;
    }

    username = read_string (message, message_length, offset);
    return_code = read_int (message, message_length, offset);

    g_debug ("Authentication complete for user %s with return code %d", username, return_code);

    /* Update username */
    if (strcmp (username, "") == 0)
    {
        g_free (username);
        username = NULL;
    }
    g_free (priv->authentication_user);
    priv->authentication_user = username;

    priv->cancelling_authentication = FALSE;
    priv->is_authenticated = (return_code == 0);

    priv->in_authentication = FALSE;
    g_signal_emit (G_OBJECT (greeter), signals[AUTHENTICATION_COMPLETE], 0);
}

static void
handle_idle (LightDMGreeter *greeter, guint8 *message, gsize message_length, gsize *offset)
{
    g_signal_emit (G_OBJECT (greeter), signals[IDLE], 0);
}

static void
handle_reset (LightDMGreeter *greeter, guint8 *message, gsize message_length, gsize *offset)
{
    LightDMGreeterPrivate *priv = GET_PRIVATE (greeter);
    GString *hint_string;

    g_hash_table_remove_all (priv->hints);

    hint_string = g_string_new ("");
    while (*offset < message_length)
    {
        gchar *name, *value;

        name = read_string (message, message_length, offset);
        value = read_string (message, message_length, offset);
        g_hash_table_insert (priv->hints, name, value);
        g_string_append_printf (hint_string, " %s=%s", name, value);
    }

    g_debug ("Reset%s", hint_string->str);
    g_string_free (hint_string, TRUE);

    g_signal_emit (G_OBJECT (greeter), signals[RESET], 0);
}

static void
handle_session_result (LightDMGreeter *greeter, guint8 *message, gsize message_length, gsize *offset)
{
    LightDMGreeterPrivate *priv = GET_PRIVATE (greeter);
    Request *request;

    /* Notify asynchronous caller */
    request = g_list_nth_data (priv->start_session_requests, 0);
    if (request)
    {
        request->return_code = read_int (message, message_length, offset);
        request_complete (request, G_OBJECT (greeter));
        priv->start_session_requests = g_list_remove (priv->start_session_requests, request);
        g_object_unref (request);
    }
}

static void
handle_shared_dir_result (LightDMGreeter *greeter, guint8 *message, gsize message_length, gsize *offset)
{
    LightDMGreeterPrivate *priv = GET_PRIVATE (greeter);
    Request *request;

    /* Notify asynchronous caller */
    request = g_list_nth_data (priv->ensure_shared_data_dir_requests, 0);
    if (request)
    {
        request->dir = read_string (message, message_length, offset);
        /* Blank data dir means invalid user */
        if (g_strcmp0 (request->dir, "") == 0)
        {
            g_free (request->dir);
            request->dir = NULL;
        }
        request_complete (request, G_OBJECT (greeter));
        priv->ensure_shared_data_dir_requests = g_list_remove (priv->ensure_shared_data_dir_requests, request);
        g_object_unref (request);
    }
}

static void
handle_message (LightDMGreeter *greeter, guint8 *message, gsize message_length)
{
    gsize offset = 0;
    guint32 id;

    id = read_int (message, message_length, &offset);
    read_int (message, message_length, &offset);
    switch (id)
    {
    case SERVER_MESSAGE_CONNECTED:
        handle_connected (greeter, message, message_length, &offset);
        break;
    case SERVER_MESSAGE_PROMPT_AUTHENTICATION:
        handle_prompt_authentication (greeter, message, message_length, &offset);
        break;
    case SERVER_MESSAGE_END_AUTHENTICATION:
        handle_end_authentication (greeter, message, message_length, &offset);
        break;
    case SERVER_MESSAGE_SESSION_RESULT:
        handle_session_result (greeter, message, message_length, &offset);
        break;
    case SERVER_MESSAGE_SHARED_DIR_RESULT:
        handle_shared_dir_result (greeter, message, message_length, &offset);
        break;
    case SERVER_MESSAGE_IDLE:
        handle_idle (greeter, message, message_length, &offset);
        break;
    case SERVER_MESSAGE_RESET:
        handle_reset (greeter, message, message_length, &offset);
        break;
    default:
        g_warning ("Unknown message from server: %d", id);
        break;
    }
}

static guint8 *
recv_message (LightDMGreeter *greeter, gsize *length, gboolean block)
{
    LightDMGreeterPrivate *priv = GET_PRIVATE (greeter);
    gsize n_to_read, n_read;
    guint8 *buffer;
    GError *error = NULL;

    if (!connect_to_daemon (greeter))
        return FALSE;

    /* Read the header, or the whole message if we already have that */
    n_to_read = HEADER_SIZE;
    if (priv->n_read >= HEADER_SIZE)
        n_to_read += get_message_length (priv->read_buffer, priv->n_read);

    do
    {
        GIOStatus status;
        status = g_io_channel_read_chars (priv->from_server_channel,
                                          (gchar *) priv->read_buffer + priv->n_read,
                                          n_to_read - priv->n_read,
                                          &n_read,
                                          &error);
        if (error)
            g_warning ("Error reading from server: %s", error->message);
        g_clear_error (&error);
        if (status == G_IO_STATUS_AGAIN)
        {
            if (block)
                continue;
        }
        else if (status != G_IO_STATUS_NORMAL)
            break;

        g_debug ("Read %zi bytes from daemon", n_read);

        priv->n_read += n_read;
    } while (priv->n_read < n_to_read && block);

    /* Stop if haven't got all the data we want */
    if (priv->n_read != n_to_read)
        return NULL;

    /* If have header, rerun for content */
    if (priv->n_read == HEADER_SIZE)
    {
        n_to_read = get_message_length (priv->read_buffer, priv->n_read);
        if (n_to_read > 0)
        {
            priv->read_buffer = g_realloc (priv->read_buffer, HEADER_SIZE + n_to_read);
            return recv_message (greeter, length, block);
        }
    }

    buffer = priv->read_buffer;
    *length = priv->n_read;

    priv->read_buffer = g_malloc (priv->n_read);
    priv->n_read = 0;

    return buffer;
}

static gboolean
from_server_cb (GIOChannel *source, GIOCondition condition, gpointer data)
{
    LightDMGreeter *greeter = data;
    guint8 *message;
    gsize message_length;

    /* Read one message and process it */
    message = recv_message (greeter, &message_length, FALSE);
    if (message)
    {
        handle_message (greeter, message, message_length);
        g_free (message);
    }

    return TRUE;
}

static gboolean
send_connect (LightDMGreeter *greeter, gboolean resettable)
{
    guint8 message[MAX_MESSAGE_LENGTH];
    gsize offset = 0;

    g_debug ("Connecting to display manager...");
    write_header (message, MAX_MESSAGE_LENGTH, GREETER_MESSAGE_CONNECT, string_length (VERSION) + int_length (), &offset);
    write_string (message, MAX_MESSAGE_LENGTH, VERSION, &offset);
    write_int (message, MAX_MESSAGE_LENGTH, resettable ? 1 : 0, &offset);

    return send_message (greeter, message, offset);
}

static gboolean
send_start_session (LightDMGreeter *greeter, const gchar *session)
{
    guint8 message[MAX_MESSAGE_LENGTH];
    gsize offset = 0;

    if (session)
        g_debug ("Starting session %s", session);
    else
        g_debug ("Starting default session");

    write_header (message, MAX_MESSAGE_LENGTH, GREETER_MESSAGE_START_SESSION, string_length (session), &offset);
    write_string (message, MAX_MESSAGE_LENGTH, session, &offset);
    return send_message (greeter, message, offset);
}

static gboolean
send_ensure_shared_data_dir (LightDMGreeter *greeter, const gchar *username)
{
    guint8 message[MAX_MESSAGE_LENGTH];
    gsize offset = 0;

    g_debug ("Ensuring data directory for user %s", username);

    write_header (message, MAX_MESSAGE_LENGTH, GREETER_MESSAGE_ENSURE_SHARED_DIR, string_length (username), &offset);
    write_string (message, MAX_MESSAGE_LENGTH, username, &offset);
    return send_message (greeter, message, offset);
}

/**
 * lightdm_greeter_connect_to_daemon:
 * @greeter: The greeter to connect
 * @cancellable: (allow-none): A #GCancellable or %NULL.
 * @callback: (allow-none): A #GAsyncReadyCallback to call when completed or %NULL.
 * @user_data: (allow-none): data to pass to the @callback or %NULL.
 *
 * Asynchronously connects the greeter to the display manager.
 *
 * When the operation is finished, @callback will be invoked. You can then call lightdm_greeter_connect_to_daemon_finish() to get the result of the operation.
 *
 * See lightdm_greeter_connect_to_daemon_sync() for the synchronous version.
 **/
void
lightdm_greeter_connect_to_daemon (LightDMGreeter *greeter, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    LightDMGreeterPrivate *priv;
    Request *request;

    g_return_if_fail (LIGHTDM_IS_GREETER (greeter));

    priv = GET_PRIVATE (greeter);

    request = request_new (cancellable, callback, user_data);
    priv->connect_requests = g_list_append (priv->connect_requests, request);
    send_connect (greeter, priv->resettable);
}

/**
 * lightdm_greeter_connect_to_daemon_finish:
 * @greeter: The greeter the the request was done with
 * @result: A #GAsyncResult.
 * @error: return location for a #GError, or %NULL
 *
 * Finishes an operation started with lightdm_greeter_connect_to_daemon().
 *
 * Return value: #TRUE if successfully connected
 **/
gboolean
lightdm_greeter_connect_to_daemon_finish (LightDMGreeter *greeter, GAsyncResult *result, GError **error)
{
    g_return_val_if_fail (LIGHTDM_IS_GREETER (greeter), FALSE);
    return REQUEST (result)->complete;
}

/**
 * lightdm_greeter_connect_to_daemon_sync:
 * @greeter: The greeter to connect
 * @error: return location for a #GError, or %NULL
 *
 * Connects the greeter to the display manager.  Will block until connected.
 *
 * Return value: #TRUE if successfully connected
 **/
gboolean
lightdm_greeter_connect_to_daemon_sync (LightDMGreeter *greeter, GError **error)
{
    LightDMGreeterPrivate *priv;
    Request *request;
    gboolean result;

    g_return_val_if_fail (LIGHTDM_IS_GREETER (greeter), FALSE);

    priv = GET_PRIVATE (greeter);

    /* Read until we are connected */
    send_connect (greeter, priv->resettable);
    request = request_new (NULL, NULL, NULL);
    priv->connect_requests = g_list_append (priv->connect_requests, g_object_ref (request));
    do
    {
        guint8 *message;
        gsize message_length;

        message = recv_message (greeter, &message_length, TRUE);
        if (!message)
            break;
        handle_message (greeter, message, message_length);
        g_free (message);
    } while (!request->complete);

    result = request->complete;
    g_object_unref (request);

    return result;
}

/**
 * lightdm_greeter_connect_sync:
 * @greeter: The greeter to connect
 * @error: return location for a #GError, or %NULL
 *
 * Connects the greeter to the display manager.  Will block until connected.
 *
 * Return value: #TRUE if successfully connected
 *
 * Deprecated: 1.11.1: Use lightdm_greeter_connect_to_daemon_sync() instead
 **/
gboolean
lightdm_greeter_connect_sync (LightDMGreeter *greeter, GError **error)
{
    return lightdm_greeter_connect_to_daemon_sync (greeter, error);
}

/**
 * lightdm_greeter_get_hint:
 * @greeter: A #LightDMGreeter
 * @name: The hint name to query.
 *
 * Get a hint.
 *
 * Return value: (nullable): The value for this hint or #NULL if not set.
 **/
const gchar *
lightdm_greeter_get_hint (LightDMGreeter *greeter, const gchar *name)
{
    g_return_val_if_fail (LIGHTDM_IS_GREETER (greeter), NULL);
    return g_hash_table_lookup (GET_PRIVATE (greeter)->hints, name);
}

/**
 * lightdm_greeter_get_default_session_hint:
 * @greeter: A #LightDMGreeter
 *
 * Get the default session to use.
 *
 * Return value: The session name
 **/
const gchar *
lightdm_greeter_get_default_session_hint (LightDMGreeter *greeter)
{
    g_return_val_if_fail (LIGHTDM_IS_GREETER (greeter), NULL);
    return lightdm_greeter_get_hint (greeter, "default-session");
}

/**
 * lightdm_greeter_get_hide_users_hint:
 * @greeter: A #LightDMGreeter
 *
 * Check if user accounts should be shown.  If this is TRUE then the list of
 * accounts should be taken from #LightDMUserList and displayed in the greeter
 * for the user to choose from.  Note that this list can be empty and it is
 * recommended you show a method for the user to enter a username manually.
 *
 * If this option is shown the greeter should only allow these users to be
 * chosen for login unless the manual login hint is set.
 *
 * Return value: #TRUE if the available users should not be shown.
 */
gboolean
lightdm_greeter_get_hide_users_hint (LightDMGreeter *greeter)
{
    const gchar *value;

    g_return_val_if_fail (LIGHTDM_IS_GREETER (greeter), FALSE);
    value = lightdm_greeter_get_hint (greeter, "hide-users");

    return g_strcmp0 (value, "true") == 0;
}

/**
 * lightdm_greeter_get_show_manual_login_hint:
 * @greeter: A #LightDMGreeter
 *
 * Check if a manual login option should be shown.  If set the GUI
 * should provide a way for a username to be entered manually.
 * Without this hint a greeter which is showing a user list can
 * limit logins to only those users.
 *
 * Return value: #TRUE if a manual login option should be shown.
 */
gboolean
lightdm_greeter_get_show_manual_login_hint (LightDMGreeter *greeter)
{
    const gchar *value;

    g_return_val_if_fail (LIGHTDM_IS_GREETER (greeter), FALSE);
    value = lightdm_greeter_get_hint (greeter, "show-manual-login");

    return g_strcmp0 (value, "true") == 0;
}

/**
 * lightdm_greeter_get_show_remote_login_hint:
 * @greeter: A #LightDMGreeter
 *
 * Check if a remote login option should be shown.  If set the GUI
 * should provide a way for a user to log into a remote desktop server.
 *
 * Return value: #TRUE if a remote login option should be shown.
 */
gboolean
lightdm_greeter_get_show_remote_login_hint (LightDMGreeter *greeter)
{
    const gchar *value;

    g_return_val_if_fail (LIGHTDM_IS_GREETER (greeter), FALSE);
    value = lightdm_greeter_get_hint (greeter, "show-remote-login");

    return g_strcmp0 (value, "true") == 0;
}

/**
 * lightdm_greeter_get_lock_hint:
 * @greeter: A #LightDMGreeter
 *
 * Check if the greeter is acting as a lock screen.
 *
 * Return value: #TRUE if the greeter was triggered by locking the seat.
 */
gboolean
lightdm_greeter_get_lock_hint (LightDMGreeter *greeter)
{
    const gchar *value;

    g_return_val_if_fail (LIGHTDM_IS_GREETER (greeter), FALSE);
    value = lightdm_greeter_get_hint (greeter, "lock-screen");

    return g_strcmp0 (value, "true") == 0;
}

/**
 * lightdm_greeter_get_has_guest_account_hint:
 * @greeter: A #LightDMGreeter
 *
 * Check if guest sessions are supported.
 *
 * Return value: #TRUE if guest sessions are supported.
 */
gboolean
lightdm_greeter_get_has_guest_account_hint (LightDMGreeter *greeter)
{
    const gchar *value;

    g_return_val_if_fail (LIGHTDM_IS_GREETER (greeter), FALSE);
    value = lightdm_greeter_get_hint (greeter, "has-guest-account");

    return g_strcmp0 (value, "true") == 0;
}

/**
 * lightdm_greeter_get_select_user_hint:
 * @greeter: A #LightDMGreeter
 *
 * Get the user to select by default.
 *
 * Return value: (nullable): A username or %NULL if no particular user should be selected.
 */
const gchar *
lightdm_greeter_get_select_user_hint (LightDMGreeter *greeter)
{
    g_return_val_if_fail (LIGHTDM_IS_GREETER (greeter), NULL);
    return lightdm_greeter_get_hint (greeter, "select-user");
}

/**
 * lightdm_greeter_get_select_guest_hint:
 * @greeter: A #LightDMGreeter
 *
 * Check if the guest account should be selected by default.
 *
 * Return value: #TRUE if the guest account should be selected by default.
 */
gboolean
lightdm_greeter_get_select_guest_hint (LightDMGreeter *greeter)
{
    const gchar *value;

    g_return_val_if_fail (LIGHTDM_IS_GREETER (greeter), FALSE);
    value = lightdm_greeter_get_hint (greeter, "select-guest");

    return g_strcmp0 (value, "true") == 0;
}

/**
 * lightdm_greeter_get_autologin_user_hint:
 * @greeter: A #LightDMGreeter
 *
 * Get the user account to automatically log into when the timer expires.
 *
 * Return value: (nullable): The user account to automatically log into or %NULL if none configured.
 */
const gchar *
lightdm_greeter_get_autologin_user_hint (LightDMGreeter *greeter)
{
    g_return_val_if_fail (LIGHTDM_IS_GREETER (greeter), NULL);
    return lightdm_greeter_get_hint (greeter, "autologin-user");
}

/**
 * lightdm_greeter_get_autologin_guest_hint:
 * @greeter: A #LightDMGreeter
 *
 * Check if the guest account should be automatically logged into when the timer expires.
 *
 * Return value: #TRUE if the guest account should be automatically logged into.
 */
gboolean
lightdm_greeter_get_autologin_guest_hint (LightDMGreeter *greeter)
{
    const gchar *value;

    g_return_val_if_fail (LIGHTDM_IS_GREETER (greeter), FALSE);
    value = lightdm_greeter_get_hint (greeter, "autologin-guest");

    return g_strcmp0 (value, "true") == 0;
}

/**
 * lightdm_greeter_get_autologin_timeout_hint:
 * @greeter: A #LightDMGreeter
 *
 * Get the number of seconds to wait before automaitcally logging in.
 *
 * Return value: The number of seconds to wait before automatically logging in or 0 for no timeout.
 */
gint
lightdm_greeter_get_autologin_timeout_hint (LightDMGreeter *greeter)
{
    const gchar *value;
    gint timeout = 0;

    g_return_val_if_fail (LIGHTDM_IS_GREETER (greeter), FALSE);
    value = lightdm_greeter_get_hint (greeter, "autologin-timeout");
    if (value)
        timeout = atoi (value);
    if (timeout < 0)
        timeout = 0;

    return timeout;
}

/**
 * lightdm_greeter_cancel_autologin:
 * @greeter: A #LightDMGreeter
 *
 * Cancel the automatic login.
 */
void
lightdm_greeter_cancel_autologin (LightDMGreeter *greeter)
{
    LightDMGreeterPrivate *priv;

    g_return_if_fail (LIGHTDM_IS_GREETER (greeter));

    priv = GET_PRIVATE (greeter);

    if (priv->autologin_timeout)
       g_source_remove (priv->autologin_timeout);
    priv->autologin_timeout = 0;
}

/**
 * lightdm_greeter_authenticate:
 * @greeter: A #LightDMGreeter
 * @username: (allow-none): A username or #NULL to prompt for a username.
 *
 * Starts the authentication procedure for a user.
 **/
void
lightdm_greeter_authenticate (LightDMGreeter *greeter, const gchar *username)
{
    LightDMGreeterPrivate *priv;
    guint8 message[MAX_MESSAGE_LENGTH];
    gsize offset = 0;

    g_return_if_fail (LIGHTDM_IS_GREETER (greeter));

    priv = GET_PRIVATE (greeter);

    g_return_if_fail (priv->connected);

    priv->cancelling_authentication = FALSE;
    priv->authenticate_sequence_number++;
    priv->in_authentication = TRUE;
    priv->is_authenticated = FALSE;
    if (username != priv->authentication_user)
    {
        g_free (priv->authentication_user);
        priv->authentication_user = g_strdup (username);
    }

    g_debug ("Starting authentication for user %s...", username);
    write_header (message, MAX_MESSAGE_LENGTH, GREETER_MESSAGE_AUTHENTICATE, int_length () + string_length (username), &offset);
    write_int (message, MAX_MESSAGE_LENGTH, priv->authenticate_sequence_number, &offset);
    write_string (message, MAX_MESSAGE_LENGTH, username, &offset);
    send_message (greeter, message, offset);
}

/**
 * lightdm_greeter_authenticate_as_guest:
 * @greeter: A #LightDMGreeter
 *
 * Starts the authentication procedure for the guest user.
 **/
void
lightdm_greeter_authenticate_as_guest (LightDMGreeter *greeter)
{
    LightDMGreeterPrivate *priv;
    guint8 message[MAX_MESSAGE_LENGTH];
    gsize offset = 0;

    g_return_if_fail (LIGHTDM_IS_GREETER (greeter));

    priv = GET_PRIVATE (greeter);

    g_return_if_fail (priv->connected);

    priv->cancelling_authentication = FALSE;
    priv->authenticate_sequence_number++;
    priv->in_authentication = TRUE;
    priv->is_authenticated = FALSE;
    g_free (priv->authentication_user);
    priv->authentication_user = NULL;

    g_debug ("Starting authentication for guest account...");
    write_header (message, MAX_MESSAGE_LENGTH, GREETER_MESSAGE_AUTHENTICATE_AS_GUEST, int_length (), &offset);
    write_int (message, MAX_MESSAGE_LENGTH, priv->authenticate_sequence_number, &offset);
    send_message (greeter, message, offset);
}

/**
 * lightdm_greeter_authenticate_autologin:
 * @greeter: A #LightDMGreeter
 *
 * Starts the authentication procedure for the automatic login user.
 **/
void
lightdm_greeter_authenticate_autologin (LightDMGreeter *greeter)
{
    const gchar *user;

    user = lightdm_greeter_get_autologin_user_hint (greeter);
    if (lightdm_greeter_get_autologin_guest_hint (greeter))
        lightdm_greeter_authenticate_as_guest (greeter);
    else if (user)
        lightdm_greeter_authenticate (greeter, user);
}

/**
 * lightdm_greeter_authenticate_remote:
 * @greeter: A #LightDMGreeter
 * @session: The name of a remote session
 * @username: (allow-none): A username of #NULL to prompt for a username.
 *
 * Start authentication for a remote session type.
 **/
void
lightdm_greeter_authenticate_remote (LightDMGreeter *greeter, const gchar *session, const gchar *username)
{
    LightDMGreeterPrivate *priv;
    guint8 message[MAX_MESSAGE_LENGTH];
    gsize offset = 0;

    g_return_if_fail (LIGHTDM_IS_GREETER (greeter));

    priv = GET_PRIVATE (greeter);

    g_return_if_fail (priv->connected);

    priv->cancelling_authentication = FALSE;
    priv->authenticate_sequence_number++;
    priv->in_authentication = TRUE;
    priv->is_authenticated = FALSE;
    g_free (priv->authentication_user);
    priv->authentication_user = NULL;

    if (username)
        g_debug ("Starting authentication for remote session %s as user %s...", session, username);
    else
        g_debug ("Starting authentication for remote session %s...", session);
    write_header (message, MAX_MESSAGE_LENGTH, GREETER_MESSAGE_AUTHENTICATE_REMOTE, int_length () + string_length (session) + string_length (username), &offset);
    write_int (message, MAX_MESSAGE_LENGTH, priv->authenticate_sequence_number, &offset);
    write_string (message, MAX_MESSAGE_LENGTH, session, &offset);
    write_string (message, MAX_MESSAGE_LENGTH, username, &offset);
    send_message (greeter, message, offset);
}

/**
 * lightdm_greeter_respond:
 * @greeter: A #LightDMGreeter
 * @response: Response to a prompt
 *
 * Provide response to a prompt.  May be one in a series.
 **/
void
lightdm_greeter_respond (LightDMGreeter *greeter, const gchar *response)
{
    LightDMGreeterPrivate *priv;
    guint8 message[MAX_MESSAGE_LENGTH];
    gsize offset = 0;

    g_return_if_fail (LIGHTDM_IS_GREETER (greeter));
    g_return_if_fail (response != NULL);

    priv = GET_PRIVATE (greeter);

    g_return_if_fail (priv->connected);
    g_return_if_fail (priv->n_responses_waiting > 0);

    priv->n_responses_waiting--;
    priv->responses_received = g_list_append (priv->responses_received, g_strdup (response));

    if (priv->n_responses_waiting == 0)
    {
        guint32 msg_length;
        GList *iter;

        g_debug ("Providing response to display manager");

        msg_length = int_length ();
        for (iter = priv->responses_received; iter; iter = iter->next)
            msg_length += string_length ((gchar *)iter->data);

        write_header (message, MAX_MESSAGE_LENGTH, GREETER_MESSAGE_CONTINUE_AUTHENTICATION, msg_length, &offset);
        write_int (message, MAX_MESSAGE_LENGTH, g_list_length (priv->responses_received), &offset);
        for (iter = priv->responses_received; iter; iter = iter->next)
            write_string (message, MAX_MESSAGE_LENGTH, (gchar *)iter->data, &offset);
        send_message (greeter, message, offset);

        g_list_free_full (priv->responses_received, g_free);
        priv->responses_received = NULL;
    }
}

/**
 * lightdm_greeter_cancel_authentication:
 * @greeter: A #LightDMGreeter
 *
 * Cancel the current user authentication.
 **/
void
lightdm_greeter_cancel_authentication (LightDMGreeter *greeter)
{
    LightDMGreeterPrivate *priv;
    guint8 message[MAX_MESSAGE_LENGTH];
    gsize offset = 0;

    g_return_if_fail (LIGHTDM_IS_GREETER (greeter));

    priv = GET_PRIVATE (greeter);

    g_return_if_fail (priv->connected);

    priv->cancelling_authentication = TRUE;
    write_header (message, MAX_MESSAGE_LENGTH, GREETER_MESSAGE_CANCEL_AUTHENTICATION, 0, &offset);
    send_message (greeter, message, offset);
}

/**
 * lightdm_greeter_get_in_authentication:
 * @greeter: A #LightDMGreeter
 *
 * Checks if the greeter is in the process of authenticating.
 *
 * Return value: #TRUE if the greeter is authenticating a user.
 **/
gboolean
lightdm_greeter_get_in_authentication (LightDMGreeter *greeter)
{
    g_return_val_if_fail (LIGHTDM_IS_GREETER (greeter), FALSE);
    return GET_PRIVATE (greeter)->in_authentication;
}

/**
 * lightdm_greeter_get_is_authenticated:
 * @greeter: A #LightDMGreeter
 *
 * Checks if the greeter has successfully authenticated.
 *
 * Return value: #TRUE if the greeter is authenticated for login.
 **/
gboolean
lightdm_greeter_get_is_authenticated (LightDMGreeter *greeter)
{
    g_return_val_if_fail (LIGHTDM_IS_GREETER (greeter), FALSE);
    return GET_PRIVATE (greeter)->is_authenticated;
}

/**
 * lightdm_greeter_get_authentication_user:
 * @greeter: A #LightDMGreeter
 *
 * Get the user that is being authenticated.
 *
 * Return value: (nullable): The username of the authentication user being authenticated or #NULL if no authentication in progress.
 */
const gchar *
lightdm_greeter_get_authentication_user (LightDMGreeter *greeter)
{
    g_return_val_if_fail (LIGHTDM_IS_GREETER (greeter), NULL);
    return GET_PRIVATE (greeter)->authentication_user;
}

/**
 * lightdm_greeter_set_language:
 * @greeter: A #LightDMGreeter
 * @language: The language to use for this user in the form of a locale specification (e.g. "de_DE.UTF-8").
 *
 * Set the language for the currently authenticated user.
 **/
void
lightdm_greeter_set_language (LightDMGreeter *greeter, const gchar *language)
{
    LightDMGreeterPrivate *priv;
    guint8 message[MAX_MESSAGE_LENGTH];
    gsize offset = 0;

    g_return_if_fail (LIGHTDM_IS_GREETER (greeter));

    priv = GET_PRIVATE (greeter);

    g_return_if_fail (priv->connected);

    write_header (message, MAX_MESSAGE_LENGTH, GREETER_MESSAGE_SET_LANGUAGE, string_length (language), &offset);
    write_string (message, MAX_MESSAGE_LENGTH, language, &offset);
    send_message (greeter, message, offset);
}

/**
 * lightdm_greeter_start_session:
 * @greeter: A #LightDMGreeter
 * @session: (allow-none): The session to log into or #NULL to use the default.
 * @cancellable: (allow-none): A #GCancellable or %NULL.
 * @callback: (allow-none): A #GAsyncReadyCallback to call when completed or %NULL.
 * @user_data: (allow-none): data to pass to the @callback or %NULL.
 *
 * Asynchronously start a session for the authenticated user.
 *
 * When the operation is finished, @callback will be invoked. You can then call lightdm_greeter_start_session_finish() to get the result of the operation.
 *
 * See lightdm_greeter_start_session_sync() for the synchronous version.
 **/
void
lightdm_greeter_start_session (LightDMGreeter *greeter, const gchar *session, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    LightDMGreeterPrivate *priv;
    Request *request;

    g_return_if_fail (LIGHTDM_IS_GREETER (greeter));

    priv = GET_PRIVATE (greeter);

    send_start_session (greeter, session);
    request = request_new (cancellable, callback, user_data);
    priv->start_session_requests = g_list_append (priv->start_session_requests, request);
}

/**
 * lightdm_greeter_start_session_finish:
 * @greeter: A #LightDMGreeter
 * @result: A #GAsyncResult.
 * @error: return location for a #GError, or %NULL
 *
 * Start a session for the authenticated user.
 *
 * Return value: TRUE if the session was started.
 **/
gboolean
lightdm_greeter_start_session_finish (LightDMGreeter *greeter, GAsyncResult *result, GError **error)
{
    g_return_val_if_fail (LIGHTDM_IS_GREETER (greeter), FALSE);
    return REQUEST (result)->return_code == 0;
}

/**
 * lightdm_greeter_start_session_sync:
 * @greeter: A #LightDMGreeter
 * @session: (allow-none): The session to log into or #NULL to use the default.
 * @error: return location for a #GError, or %NULL
 *
 * Start a session for the authenticated user.
 *
 * Return value: TRUE if the session was started.
 **/
gboolean
lightdm_greeter_start_session_sync (LightDMGreeter *greeter, const gchar *session, GError **error)
{
    LightDMGreeterPrivate *priv;
    Request *request;
    guint32 return_code;

    g_return_val_if_fail (LIGHTDM_IS_GREETER (greeter), FALSE);

    priv = GET_PRIVATE (greeter);

    g_return_val_if_fail (priv->connected, FALSE);
    g_return_val_if_fail (priv->is_authenticated, FALSE);

    /* Read until the session is started */
    send_start_session (greeter, session);
    request = request_new (NULL, NULL, NULL);
    priv->start_session_requests = g_list_append (priv->start_session_requests, g_object_ref (request));
    do
    {
        guint8 *message;
        gsize message_length;

        message = recv_message (greeter, &message_length, TRUE);
        if (!message)
            break;
        handle_message (greeter, message, message_length);
        g_free (message);
    } while (!request->complete);

    return_code = request->return_code;
    g_object_unref (request);

    return return_code == 0;
}

/**
 * lightdm_greeter_ensure_shared_data_dir:
 * @greeter: A #LightDMGreeter
 * @username: A username
 * @cancellable: (allow-none): A #GCancellable or %NULL.
 * @callback: (allow-none): A #GAsyncReadyCallback to call when completed or %NULL.
 * @user_data: (allow-none): data to pass to the @callback or %NULL.
 *
 * Ensure that a shared data dir for the given user is available.  Both the
 * greeter user and @username will have write access to that folder.  The
 * intention is that larger pieces of shared data would be stored there (files
 * that the greeter creates but wants to give to a user -- like camera
 * photos -- or files that the user creates but wants the greeter to
 * see -- like contact avatars).
 *
 * LightDM will automatically create these if the user actually logs in, so
 * greeters only need to call this method if they want to store something in
 * the directory themselves.
 **/
void
lightdm_greeter_ensure_shared_data_dir (LightDMGreeter *greeter, const gchar *username, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    LightDMGreeterPrivate *priv;
    Request *request;

    g_return_if_fail (LIGHTDM_IS_GREETER (greeter));

    priv = GET_PRIVATE (greeter);

    send_ensure_shared_data_dir (greeter, username);
    request = request_new (cancellable, callback, user_data);
    priv->ensure_shared_data_dir_requests = g_list_append (priv->ensure_shared_data_dir_requests, request);
}

/**
 * lightdm_greeter_ensure_shared_data_dir_finish:
 * @result: A #GAsyncResult.
 * @greeter: A #LightDMGreeter
 *
 * Function to call from lightdm_greeter_ensure_shared_data_dir callback.
 *
 * Return value: The path to the shared directory, free with g_free.
 **/
gchar *
lightdm_greeter_ensure_shared_data_dir_finish (LightDMGreeter *greeter, GAsyncResult *result)
{
    g_return_val_if_fail (LIGHTDM_IS_GREETER (greeter), NULL);
    return g_strdup (REQUEST (result)->dir);
}

/**
 * lightdm_greeter_ensure_shared_data_dir_sync:
 * @greeter: A #LightDMGreeter
 * @username: A username
 *
 * Ensure that a shared data dir for the given user is available.  Both the
 * greeter user and @username will have write access to that folder.  The
 * intention is that larger pieces of shared data would be stored there (files
 * that the greeter creates but wants to give to a user -- like camera
 * photos -- or files that the user creates but wants the greeter to
 * see -- like contact avatars).
 *
 * LightDM will automatically create these if the user actually logs in, so
 * greeters only need to call this method if they want to store something in
 * the directory themselves.
 *
 * Return value: The path to the shared directory, free with g_free.
 **/
gchar *
lightdm_greeter_ensure_shared_data_dir_sync (LightDMGreeter *greeter, const gchar *username)
{
    LightDMGreeterPrivate *priv;
    Request *request;
    gchar *data_dir;

    g_return_val_if_fail (LIGHTDM_IS_GREETER (greeter), NULL);

    priv = GET_PRIVATE (greeter);

    g_return_val_if_fail (priv->connected, NULL);

    /* Read until a response */
    send_ensure_shared_data_dir (greeter, username);
    request = request_new (NULL, NULL, NULL);
    priv->ensure_shared_data_dir_requests = g_list_append (priv->ensure_shared_data_dir_requests, g_object_ref (request));
    do
    {
        guint8 *message;
        gsize message_length;

        message = recv_message (greeter, &message_length, TRUE);
        if (!message)
            break;
        handle_message (greeter, message, message_length);
        g_free (message);
    } while (!request->complete);

    data_dir = g_strdup (request->dir);
    g_object_unref (request);

    return data_dir;
}

static void
lightdm_greeter_init (LightDMGreeter *greeter)
{
    LightDMGreeterPrivate *priv = GET_PRIVATE (greeter);

    priv->read_buffer = g_malloc (HEADER_SIZE);
    priv->hints = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
}

static void
lightdm_greeter_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

static void
lightdm_greeter_get_property (GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
    LightDMGreeter *self;

    self = LIGHTDM_GREETER (object);

    switch (prop_id) {
    case PROP_DEFAULT_SESSION_HINT:
        g_value_set_string (value, lightdm_greeter_get_default_session_hint (self));
        break;
    case PROP_HIDE_USERS_HINT:
        g_value_set_boolean (value, lightdm_greeter_get_hide_users_hint (self));
        break;
    case PROP_SHOW_MANUAL_LOGIN_HINT:
        g_value_set_boolean (value, lightdm_greeter_get_show_manual_login_hint (self));
        break;
    case PROP_SHOW_REMOTE_LOGIN_HINT:
        g_value_set_boolean (value, lightdm_greeter_get_show_remote_login_hint (self));
        break;
    case PROP_LOCK_HINT:
        g_value_set_boolean (value, lightdm_greeter_get_lock_hint (self));
        break;
    case PROP_HAS_GUEST_ACCOUNT_HINT:
        g_value_set_boolean (value, lightdm_greeter_get_has_guest_account_hint (self));
        break;
    case PROP_SELECT_USER_HINT:
        g_value_set_string (value, lightdm_greeter_get_select_user_hint (self));
        break;
    case PROP_SELECT_GUEST_HINT:
        g_value_set_boolean (value, lightdm_greeter_get_select_guest_hint (self));
        break;
    case PROP_AUTOLOGIN_USER_HINT:
        g_value_set_string (value, lightdm_greeter_get_autologin_user_hint (self));
        break;
    case PROP_AUTOLOGIN_GUEST_HINT:
        g_value_set_boolean (value, lightdm_greeter_get_autologin_guest_hint (self));
        break;
    case PROP_AUTOLOGIN_TIMEOUT_HINT:
        g_value_set_int (value, lightdm_greeter_get_autologin_timeout_hint (self));
        break;
    case PROP_AUTHENTICATION_USER:
        g_value_set_string (value, lightdm_greeter_get_authentication_user (self));
        break;
    case PROP_IN_AUTHENTICATION:
        g_value_set_boolean (value, lightdm_greeter_get_in_authentication (self));
        break;
    case PROP_IS_AUTHENTICATED:
        g_value_set_boolean (value, lightdm_greeter_get_is_authenticated (self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
lightdm_greeter_finalize (GObject *object)
{
    LightDMGreeter *self = LIGHTDM_GREETER (object);
    LightDMGreeterPrivate *priv = GET_PRIVATE (self);

    g_clear_object (&priv->socket);
    if (priv->to_server_channel)
        g_io_channel_unref (priv->to_server_channel);
    if (priv->from_server_channel)
        g_io_channel_unref (priv->from_server_channel);
    if (priv->from_server_watch)
        g_source_remove (priv->from_server_watch);
    priv->from_server_watch = 0;
    g_clear_pointer (&priv->read_buffer, g_free);
    g_list_free_full (priv->responses_received, g_free);
    priv->responses_received = NULL;
    g_list_free_full (priv->connect_requests, g_object_unref);
    priv->connect_requests = NULL;
    g_list_free_full (priv->start_session_requests, g_object_unref);
    priv->start_session_requests = NULL;
    g_list_free_full (priv->ensure_shared_data_dir_requests, g_object_unref);
    priv->ensure_shared_data_dir_requests = NULL;
    g_clear_pointer (&priv->authentication_user, g_free);
    g_hash_table_unref (priv->hints);
    priv->hints = NULL;

    G_OBJECT_CLASS (lightdm_greeter_parent_class)->finalize (object);
}

static void
lightdm_greeter_class_init (LightDMGreeterClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (klass, sizeof (LightDMGreeterPrivate));

    object_class->set_property = lightdm_greeter_set_property;
    object_class->get_property = lightdm_greeter_get_property;
    object_class->finalize = lightdm_greeter_finalize;

    g_object_class_install_property (object_class,
                                     PROP_DEFAULT_SESSION_HINT,
                                     g_param_spec_string ("default-session-hint",
                                                          "default-session-hint",
                                                          "Default session hint",
                                                          NULL,
                                                          G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_HIDE_USERS_HINT,
                                     g_param_spec_boolean ("hide-users-hint",
                                                           "hide-users-hint",
                                                           "Hide users hint",
                                                           FALSE,
                                                           G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_SHOW_MANUAL_LOGIN_HINT,
                                     g_param_spec_boolean ("show-manual-login-hint",
                                                           "show-manual-login-hint",
                                                           "Show manual login hint",
                                                           FALSE,
                                                           G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_SHOW_REMOTE_LOGIN_HINT,
                                     g_param_spec_boolean ("show-remote-login-hint",
                                                           "show-remote-login-hint",
                                                           "Show remote login hint",
                                                           FALSE,
                                                           G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_LOCK_HINT,
                                     g_param_spec_boolean ("lock-hint",
                                                           "lock-hint",
                                                           "Lock hint",
                                                           FALSE,
                                                           G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_HAS_GUEST_ACCOUNT_HINT,
                                     g_param_spec_boolean ("has-guest-account-hint",
                                                           "has-guest-account-hint",
                                                           "Has guest account hint",
                                                           FALSE,
                                                           G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_SELECT_USER_HINT,
                                     g_param_spec_string ("select-user-hint",
                                                          "select-user-hint",
                                                          "Select user hint",
                                                          NULL,
                                                          G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_SELECT_GUEST_HINT,
                                     g_param_spec_boolean ("select-guest-hint",
                                                           "select-guest-hint",
                                                           "Select guest account hint",
                                                           FALSE,
                                                           G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_AUTOLOGIN_USER_HINT,
                                     g_param_spec_string ("autologin-user-hint",
                                                          "autologin-user-hint",
                                                          "Autologin user hint",
                                                          NULL,
                                                          G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_AUTOLOGIN_GUEST_HINT,
                                     g_param_spec_boolean ("autologin-guest-hint",
                                                           "autologin-guest-hint",
                                                           "Autologin guest account hint",
                                                           FALSE,
                                                           G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_AUTOLOGIN_TIMEOUT_HINT,
                                     g_param_spec_int ("autologin-timeout-hint",
                                                       "autologin-timeout-hint",
                                                       "Autologin timeout hint",
                                                       0, G_MAXINT, 0,
                                                       G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_AUTHENTICATION_USER,
                                     g_param_spec_string ("authentication-user",
                                                          "authentication-user",
                                                          "The user being authenticated",
                                                          NULL,
                                                          G_PARAM_READABLE));
    g_object_class_install_property (object_class,
                                     PROP_IN_AUTHENTICATION,
                                     g_param_spec_boolean ("in-authentication",
                                                           "in-authentication",
                                                           "TRUE if a user is being authenticated",
                                                           FALSE,
                                                           G_PARAM_READABLE));
    g_object_class_install_property (object_class,
                                     PROP_IS_AUTHENTICATED,
                                     g_param_spec_boolean ("is-authenticated",
                                                           "is-authenticated",
                                                           "TRUE if the selected user is authenticated",
                                                           FALSE,
                                                           G_PARAM_READABLE));

    /**
     * LightDMGreeter::show-prompt:
     * @greeter: A #LightDMGreeter
     * @text: Prompt text
     * @type: Prompt type
     *
     * The ::show-prompt signal gets emitted when the greeter should show a
     * prompt to the user.  The given text should be displayed and an input
     * field for the user to provide a response.
     *
     * Call lightdm_greeter_respond() with the resultant input or
     * lightdm_greeter_cancel_authentication() to abort the authentication.
     **/
    signals[SHOW_PROMPT] =
        g_signal_new (LIGHTDM_GREETER_SIGNAL_SHOW_PROMPT,
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (LightDMGreeterClass, show_prompt),
                      NULL, NULL,
                      NULL,
                      G_TYPE_NONE, 2, G_TYPE_STRING, lightdm_prompt_type_get_type ());

    /**
     * LightDMGreeter::show-message:
     * @greeter: A #LightDMGreeter
     * @text: Message text
     * @type: Message type
     *
     * The ::show-message signal gets emitted when the greeter
     * should show a message to the user.
     **/
    signals[SHOW_MESSAGE] =
        g_signal_new (LIGHTDM_GREETER_SIGNAL_SHOW_MESSAGE,
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (LightDMGreeterClass, show_message),
                      NULL, NULL,
                      NULL,
                      G_TYPE_NONE, 2, G_TYPE_STRING, lightdm_message_type_get_type ());

    /**
     * LightDMGreeter::authentication-complete:
     * @greeter: A #LightDMGreeter
     *
     * The ::authentication-complete signal gets emitted when the greeter
     * has completed authentication.
     *
     * Call lightdm_greeter_get_is_authenticated() to check if the authentication
     * was successful.
     **/
    signals[AUTHENTICATION_COMPLETE] =
        g_signal_new (LIGHTDM_GREETER_SIGNAL_AUTHENTICATION_COMPLETE,
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (LightDMGreeterClass, authentication_complete),
                      NULL, NULL,
                      NULL,
                      G_TYPE_NONE, 0);

    /**
     * LightDMGreeter::autologin-timer-expired:
     * @greeter: A #LightDMGreeter
     *
     * The ::timed-login signal gets emitted when the automatic login timer has expired.
     * The application should then call lightdm_greeter_login().
     **/
    signals[AUTOLOGIN_TIMER_EXPIRED] =
        g_signal_new (LIGHTDM_GREETER_SIGNAL_AUTOLOGIN_TIMER_EXPIRED,
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (LightDMGreeterClass, autologin_timer_expired),
                      NULL, NULL,
                      NULL,
                      G_TYPE_NONE, 0);

    /**
     * LightDMGreeter::idle:
     * @greeter: A #LightDMGreeter
     *
     * The ::idle signal gets emitted when the user has logged in and the
     * greeter is no longer needed.
     *
     * This signal only matters if the greeter has marked itself as
     * resettable using lightdm_greeter_set_resettable().
     **/
    signals[IDLE] =
        g_signal_new (LIGHTDM_GREETER_SIGNAL_IDLE,
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (LightDMGreeterClass, idle),
                      NULL, NULL,
                      NULL,
                      G_TYPE_NONE, 0);

    /**
     * LightDMGreeter::reset:
     * @greeter: A #LightDMGreeter
     *
     * The ::reset signal gets emitted when the user is returning to a greeter
     * that was previously marked idle.
     *
     * This signal only matters if the greeter has marked itself as
     * resettable using lightdm_greeter_set_resettable().
     **/
    signals[RESET] =
        g_signal_new (LIGHTDM_GREETER_SIGNAL_RESET,
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (LightDMGreeterClass, reset),
                      NULL, NULL,
                      NULL,
                      G_TYPE_NONE, 0);
}

static void
request_init (Request *request)
{
}

static void
request_finalize (GObject *object)
{
    Request *request = REQUEST (object);

    g_free (request->dir);
    g_clear_object (&request->cancellable);

    G_OBJECT_CLASS (request_parent_class)->finalize (object);
}

static void
request_class_init (RequestClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->finalize = request_finalize;
}

static gpointer
request_get_user_data (GAsyncResult *result)
{
    return REQUEST (result)->user_data;
}

static GObject *
request_get_source_object (GAsyncResult *res)
{
    return NULL;
}

static void
request_iface_init (GAsyncResultIface *iface)
{
    iface->get_user_data = request_get_user_data;
    iface->get_source_object = request_get_source_object;
}
