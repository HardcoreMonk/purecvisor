#!/usr/bin/env python3
                          
                                                             
                                                                                 
                                                                       
                                                                   
                                                   
 
                      
                                                                           
                                        
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from rpc_extract import extract_consumer_sent, CLI_RE


def test_sent_keys_same_scope():
    src = '''
    static int cmd_net(char **argv){
      JsonObject *params = json_object_new();
      json_object_set_string_member(params,"bridge_name",argv[2]);
      json_object_set_string_member(params,"mode",argv[3]);
      return purectl_send_request("network.mode_set", params, &e);
    }'''
    sent = extract_consumer_sent(src, CLI_RE)
    assert sent["network.mode_set"] == {"bridge_name", "mode"}


def test_sent_array_key_same_scope():
    src = '''
    static int cmd_publish(void){
      JsonObject *params = json_object_new();
      JsonArray *sources = json_array_new();
      json_object_set_array_member(params,"allowed_sources",sources);
      return purectl_send_request("vpc.service.publish", params, &e);
    }'''
    sent = extract_consumer_sent(src, CLI_RE)
    assert sent["vpc.service.publish"] == {"allowed_sources"}


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for t in tests:
        t()
        print(f"  ok  {t.__name__}")
    print("OK")
