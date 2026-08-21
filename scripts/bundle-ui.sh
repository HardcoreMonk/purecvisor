#!/bin/bash
                                                                
 
                                                    
                                                       
                                                         
                                       
 
                                                                        
 
                          
                                                                 
                                                           
                                                            
                                                             
                                           
                                                
 
                      
                                                  
                                                                
set -e
cd "$(dirname "$0")/.."

make ui-bundle

OUT="ui/app.bundle.js"
SW="ui/sw.js"

                                                  
                                            
INSTALL_DIR="/usr/local/share/purecvisor/ui"
if [ "${PCV_NO_DEPLOY:-0}" = "1" ]; then
  echo "[bundle] PCV_NO_DEPLOY=1 — skipping install to $INSTALL_DIR"
elif [ -d "$INSTALL_DIR" ]; then
  if sudo -n true 2>/dev/null; then
    sudo cp "$OUT" "$INSTALL_DIR/app.bundle.js"
    [ -f "$SW" ] && sudo cp "$SW" "$INSTALL_DIR/sw.js"
    echo "[bundle] installed → $INSTALL_DIR"
  else
    echo "[bundle] WARN: sudo password 필요 — 수동 배포: sudo cp $OUT $SW $INSTALL_DIR/"
  fi
else
  echo "[bundle] INFO: $INSTALL_DIR 미존재 — 데몬 미설치 환경, 배포 생략"
fi
