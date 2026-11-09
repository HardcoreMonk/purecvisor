#!/usr/bin/env bash
                          
                                                                                
                                                                                                        
                                                                      
                                                                                     
                                                                               
                                                        
 
                      
                                                                           
                                                                                       
set -euo pipefail
umask 022

                                                             
                                                                         
                                                                      
 
     
                                                 
                                                                  
     
                                        
     
                                                           
 
                
                                                                                    
                                                                           
                                                                          
                                                                      

usage() { printf 'usage: %s --src DIR [--root PATH]\n' "$0" >&2; }

SRC=""
ROOT="/"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --src) [[ $# -ge 2 ]] || { usage; exit 2; }; SRC="$2"; shift 2 ;;
        --root) [[ $# -ge 2 ]] || { usage; exit 2; }; ROOT="$2"; shift 2 ;;
        *) usage; exit 2 ;;
    esac
done
if [[ -z "$SRC" || ! -d "$SRC" ]]; then
    echo "error: --src must name an existing directory" >&2
    usage
    exit 2
fi

                                                                             
DROPIN_DIR="$ROOT/etc/systemd/system/purecvisorsd.service.d"
SERVICE_TARGET="$ROOT/etc/systemd/system/proc-hidepid.service"
DROPINS=(95-coredump-hardening.conf 96-hidepid.conf 97-sandbox.conf)

                                                     
for name in "${DROPINS[@]}" proc-hidepid.service; do
    if [[ ! -f "$SRC/$name" ]]; then
        echo "error: canonical drop-in missing from --src: $name" >&2
        exit 2
    fi
done

                                                                   
                                                                   
                                                                                   
                                                                           
                                          
INSTALL_OWNER=()
if [[ "$ROOT" == "/" ]]; then
    INSTALL_OWNER=(-o root -g root)
fi

for name in "${DROPINS[@]}"; do
    install -D -m644 "${INSTALL_OWNER[@]}" "$SRC/$name" "$DROPIN_DIR/$name"
done
install -D -m644 "${INSTALL_OWNER[@]}" "$SRC/proc-hidepid.service" "$SERVICE_TARGET"

if [[ "$ROOT" == "/" ]]; then
                                                                                         
    systemctl daemon-reload
                                                                                      
                                                                                            
    systemctl enable proc-hidepid.service || true
fi
echo "daemon-dropins: installed"
