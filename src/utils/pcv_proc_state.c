                                                                             
                             
                                                                               
  
                           
                                                                                  
                       
  
                                        
                                                                                
#include "pcv_proc_state.h"

#include <string.h>

                                                               
                                                         
gchar
pcv_proc_stat_state(const gchar *stat_text)
{
    if (!stat_text)
        return '\0';
    const gchar *rp = strrchr(stat_text, ')');
    if (!rp || rp[1] != ' ')
        return '\0';
    gchar c = rp[2];
    return g_ascii_isalpha(c) ? c : '\0';
}

                                                              
                                             
gboolean
pcv_proc_pid_alive(GPid pid)
{
    if (pid <= 0)
        return FALSE;
    gchar *path = g_strdup_printf("/proc/%d/stat", (gint)pid);
    gchar *text = NULL;
    gboolean ok = g_file_get_contents(path, &text, NULL, NULL);
    gchar st = ok ? pcv_proc_stat_state(text) : '\0';
    g_free(text);
    g_free(path);
    return st != '\0' && st != 'Z' && st != 'X';
}
