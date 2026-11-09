   
                          
                                                               
  
                                                                                          
                                                                                          
                                                                                            
                                                                                                          
                                                                                                     
                                      
                                                                                     
   
#include "cli_secret_echo.h"

#include <glib.h>
#include <string.h>

                                                                                                  
static const char *const SECRET_OPTION_WORDS[] = { "key", "password", "passwd", "passphrase", "secret", "token", "webhook" };

                                                          
static const char *const AUTH_SAFE_ACTIONS[] = { "list", "delete", "role", "tenant", "help", "--help", "-h" };

static bool
_option_names_secret(const char *token)
{
    if (token[0] != '-') return false;
    const char *equals = strchr(token, '=');                                      
    gchar *lower = g_ascii_strdown(token, equals ? (gssize)(equals - token) : -1);
    bool found = false;
    for (gsize i = 0; i < G_N_ELEMENTS(SECRET_OPTION_WORDS) && !found; i++)
        found = strstr(lower, SECRET_OPTION_WORDS[i]) != NULL;
    g_free(lower);
    return found;
}

                         
static bool
_command_takes_secret(const char *object, const char *action)
{
    if (g_ascii_strcasecmp(object, "auth") == 0) {
        for (gsize i = 0; i < G_N_ELEMENTS(AUTH_SAFE_ACTIONS); i++)
            if (strcmp(action, AUTH_SAFE_ACTIONS[i]) == 0) return false;
        return true;
    }
                                                                                                              
    return g_ascii_strcasecmp(object, "container") == 0 && g_ascii_strncasecmp(action, "env", 3) == 0
        && strcmp(action, "env-list") != 0 && strcmp(action, "env-delete") != 0;
}

                                                                                                     
                                                                                                 
                                                           
static bool
_leading_command_takes_secret(int argc, char **argv)
{
    int i = 0;
    while (i < argc && argv[i][0] == '-') {
        if (strchr(argv[i], '=') || i + 1 >= argc || argv[i + 1][0] == '-') { i++; continue; }
        if (i + 2 < argc && _command_takes_secret(argv[i + 1], argv[i + 2])) return true;
        i += 2;
    }
    return i + 1 < argc && _command_takes_secret(argv[i], argv[i + 1]);
}

bool
pcv_cli_argv_has_secret(int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        if (_option_names_secret(argv[i])) return true;
    }
    return _leading_command_takes_secret(argc, argv);
}

                                                                                     
static GPtrArray *
_split_tokens(const char *line)
{
    GPtrArray *tokens = g_ptr_array_new_with_free_func(g_free);
    GString *cur = NULL;
    char quote = '\0';
    for (const char *p = line; *p; p++) {
        char c = *p;
        if (quote) {
            if (c == quote) quote = '\0';
            else if (c == '\\' && quote == '"' && p[1]) g_string_append_c(cur, *++p);
            else g_string_append_c(cur, c);
            continue;
        }
        if (g_ascii_isspace(c)) {
            if (cur) { g_ptr_array_add(tokens, g_string_free(cur, FALSE)); cur = NULL; }
            continue;
        }
        if (!cur) cur = g_string_new(NULL);
        if (c == '\'' || c == '"') quote = c;
        else if (c == '\\' && p[1]) g_string_append_c(cur, *++p);
        else g_string_append_c(cur, c);
    }
    if (cur) g_ptr_array_add(tokens, g_string_free(cur, FALSE));
    return tokens;
}

bool
pcv_cli_line_has_secret(const char *line)
{
    GPtrArray *tokens = _split_tokens(line);
    bool found = pcv_cli_argv_has_secret((int)tokens->len, (char **)tokens->pdata);
    g_ptr_array_free(tokens, TRUE);
    return found;
}

const char *
pcv_cli_echo_arg(int argc, char **argv, int index)
{
    return pcv_cli_argv_has_secret(argc, argv) ? "(secret argument)" : argv[index];
}

const char *
pcv_cli_echo_action(int argc, char **argv, bool known_object)
{
    if (!pcv_cli_argv_has_secret(argc, argv)) return argv[1];
    return known_object ? "(hidden)" : "";
}
