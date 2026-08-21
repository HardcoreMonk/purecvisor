#!/usr/bin/env bash
                          
                                                                     
                                     
                                                                                                                   
 
                      
                                                                          
set -euo pipefail
umask 022

                                                              
 
     
                                             
                                                                   
     
                                     
     
                                                     
 
                
                                                                             
                                                        
                                            

usage() { printf 'usage: %s --unit PATH [--root PATH]\n' "$0" >&2; }

UNIT_SRC=""
ROOT="/"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --unit) [[ $# -ge 2 ]] || { usage; exit 2; }; UNIT_SRC="$2"; shift 2 ;;
        --root) [[ $# -ge 2 ]] || { usage; exit 2; }; ROOT="$2"; shift 2 ;;
        *) usage; exit 2 ;;
    esac
done
if [[ -z "$UNIT_SRC" || ! -f "$UNIT_SRC" ]]; then
    echo "error: --unit must name an existing unit file" >&2
    usage
    exit 2
fi

TARGET_DIR="$ROOT/etc/systemd/system"
TARGET="$TARGET_DIR/purecvisor-host-tuning.service"
                                                       
                                                  
[[ "$ROOT" == "/" ]] || install -d "$TARGET_DIR"
install -m644 "$UNIT_SRC" "$TARGET"

if [[ "$ROOT" == "/" ]]; then
    systemctl daemon-reload
    systemctl enable purecvisor-host-tuning.service
fi
echo "host-tuning: installed"
