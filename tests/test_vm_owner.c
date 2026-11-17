   
                        
                                                                                                 
  
                           
                                                                                                   
                                                                                           
                                                                         
                                                                                                                 
                                                                                                  
                                                                    
                                                                                                                   
                                                          
  
                                                                                 
                                                                      
   
#include <glib.h>
#include <string.h>

#include "modules/resource/resource_domain_refs.h"
#include "modules/virt/vm_owner.h"

                                                       
gchar *pcv_vm_xml_with_owner_metadata(const gchar *xml, const gchar *owner);

#define NS "urn:purecvisor:metadata"
#define DEV "<devices/>"

                                    
static const struct { const gchar *label; const gchar *xml; const gchar *want; } k_cases[] = {
    { "pcv 접두 직속",
      "<domain><name>v</name><metadata><pcv:owner xmlns:pcv='" NS "'>alice</pcv:owner></metadata>" DEV "</domain>", "alice" },
    { "기본 이름공간 꼴 직속",
      "<domain><metadata><owner xmlns='" NS "'>alice</owner></metadata>" DEV "</domain>", "alice" },
    { "다른 이름공간 앞에 있고 pcv 직속 뒤(SetMetadata 꼴)",
      "<domain><metadata><x:info xmlns:x='urn:other'><x:owner>mallory</x:owner></x:info>"
      "<pcv:owner xmlns:pcv='" NS "'>alice</pcv:owner></metadata>" DEV "</domain>", "alice" },
    { "이름공간 없는 owner 직속",
      "<domain><metadata><owner>alice</owner></metadata>" DEV "</domain>", NULL },
    { "다른 이름공간 최상위 owner",
      "<domain><metadata><y:owner xmlns:y='urn:other'>mallory</y:owner></metadata>" DEV "</domain>", NULL },
    { "PCV URI 다른 이름 요소 속 owner 후손",
      "<domain><metadata><pcv:wrap xmlns:pcv='" NS "'><pcv:owner>mallory</pcv:owner></pcv:wrap></metadata>" DEV "</domain>", NULL },
    { "PCV 소유자 둘",
      "<domain><metadata><pcv:owner xmlns:pcv='" NS "'>alice</pcv:owner>"
      "<q:owner xmlns:q='" NS "'>bob</q:owner></metadata>" DEV "</domain>", NULL },
    { "앞 공백", "<domain><metadata><pcv:owner xmlns:pcv='" NS "'> alice</pcv:owner></metadata>" DEV "</domain>", NULL },
    { "뒤 줄바꿈", "<domain><metadata><pcv:owner xmlns:pcv='" NS "'>alice\n</pcv:owner></metadata>" DEV "</domain>", NULL },
    { "빈 값", "<domain><metadata><pcv:owner xmlns:pcv='" NS "'></pcv:owner></metadata>" DEV "</domain>", NULL },
    { "자식 요소", "<domain><metadata><pcv:owner xmlns:pcv='" NS "'>al<b/>ice</pcv:owner></metadata>" DEV "</domain>", NULL },
    { "DTD 엔티티",
      "<!DOCTYPE domain [<!ENTITY who 'alice'>]><domain><metadata><pcv:owner xmlns:pcv='" NS "'>&who;</pcv:owner>"
      "</metadata>" DEV "</domain>", NULL },
    { "metadata 없음", "<domain><name>v</name>" DEV "</domain>", NULL },
    { "metadata 밖 PCV owner",
      "<domain><description><pcv:owner xmlns:pcv='" NS "'>mallory</pcv:owner></description>" DEV "</domain>", NULL },
    { "뿌리가 domain 아님", "<other><metadata><pcv:owner xmlns:pcv='" NS "'>alice</pcv:owner></metadata></other>", NULL },
    { "이스케이프 값",
      "<domain><metadata><pcv:owner xmlns:pcv='" NS "'>a&amp;b</pcv:owner></metadata>" DEV "</domain>", "a&b" },
};

                                                                  
static void
test_strict_rule_table(void)
{
    for (guint i = 0; i < G_N_ELEMENTS(k_cases); i++) {
        g_autofree gchar *got = pcv_vm_owner_from_domain_xml(k_cases[i].xml);
        if (g_strcmp0(got, k_cases[i].want) != 0)
            g_error("%s: got %s want %s", k_cases[i].label, got ? got : "(null)", k_cases[i].want ? k_cases[i].want : "(null)");
    }
    g_assert_null(pcv_vm_owner_from_domain_xml(NULL));
    g_assert_null(pcv_vm_owner_from_domain_xml(""));
    g_assert_null(pcv_vm_owner_from_domain_xml("not xml"));
    g_assert_null(pcv_vm_owner_from_domain_node(NULL));
}

                                                                                     
                                  
static void
test_writer_matches_reader(void)
{
    static const gchar *const bases[] = {
        "<domain type='kvm'>\n  <name>v</name>\n  <devices/>\n</domain>\n",
        "<domain type='kvm'>\n  <name>v</name>\n  <metadata>\n    <x:info xmlns:x='urn:other'/>\n  </metadata>\n"
        "  <devices/>\n</domain>\n",
        "<domain type='kvm'>\n  <devices/>\n</domain>\n",
    };
    static const gchar *const owners[] = { "alice", "a&b<c>'\"", "grpc" };
    for (guint b = 0; b < G_N_ELEMENTS(bases); b++) {
        for (guint o = 0; o < G_N_ELEMENTS(owners); o++) {
            g_autofree gchar *xml = pcv_vm_xml_with_owner_metadata(bases[b], owners[o]);
            g_autofree gchar *got = pcv_vm_owner_from_domain_xml(xml);
            if (g_strcmp0(got, owners[o]) != 0)
                g_error("writer base %u owner %u: got %s\n%s", b, o, got ? got : "(null)", xml);
        }
    }
}

                                                                                       
static void
test_paths_agree(void)
{
    for (guint i = 0; i < G_N_ELEMENTS(k_cases); i++) {
        PcvResDomainRefs refs = { 0 };
        g_autoptr(GError) err = NULL;
        gboolean ok = pcv_res_domain_refs_parse(k_cases[i].xml, &refs, &err);
        g_autofree gchar *shared = pcv_vm_owner_from_domain_xml(k_cases[i].xml);
        if (ok && g_strcmp0(refs.owner, shared) != 0)
            g_error("%s: refs %s shared %s", k_cases[i].label, refs.owner ? refs.owner : "(null)", shared ? shared : "(null)");
        if (!ok)
            g_assert_null(shared);                            
        pcv_res_domain_refs_clear(&refs);
    }
}

void
test_vm_owner_register(void)
{
    g_test_add_func("/vm_owner/strict_rule_table", test_strict_rule_table);
    g_test_add_func("/vm_owner/writer_matches_reader", test_writer_matches_reader);
    g_test_add_func("/vm_owner/paths_agree", test_paths_agree);
}
