   
                          
                                                                                 
  
                                                                                         
                                                                                  
                                                                                   
                                                                                 
                                       
   
#ifndef PURECVISOR_CLI_SECRET_ECHO_H
#define PURECVISOR_CLI_SECRET_ECHO_H

#include <stdbool.h>

                                                                                  
bool pcv_cli_line_has_secret(const char *line);

                                                                                              
bool pcv_cli_argv_has_secret(int argc, char **argv);

                                                                                    
const char *pcv_cli_echo_arg(int argc, char **argv, int index);

                                                                                                                     
const char *pcv_cli_echo_action(int argc, char **argv, bool known_object);

#endif                                   
