                                                                                                         
                                                                                              
                                        
                                                                          
#include <stdio.h>
#include <glib.h>

#include "modules/network/network_firewall_nft.h"

int
main(int argc, char **argv)
{
    g_autoptr(PcvGuestFwSnapshot) snap = pcv_guest_fw_snapshot_new();
    for (int i = 1; i < argc; i++) {
        g_auto(GStrv) parts = g_strsplit(argv[i], ":", 3);                                       
        if (g_strv_length(parts) != 3) {
            fprintf(stderr, "usage: guest_fw_emit mode:bridge:cidr ...\n");
            return 2;
        }
        pcv_guest_fw_snapshot_add(snap, parts[1], parts[0], parts[2]);
    }
    g_autofree gchar *script = pcv_guest_fw_build_script(snap);
    fputs(script, stdout);
    return 0;
}
