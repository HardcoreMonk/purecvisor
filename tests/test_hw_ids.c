   
                      
                                                      
  
                           
                                                                                    
                                                                                       
                                                                                       
                                                                                                         
                                                                             
                                 
  
                                                                          
                         
                                                                       
   
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <unistd.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "modules/dispatcher/hw_ids.h"

                                                                           
static void ids_write(const gchar *root, const gchar *relative, const gchar *value)
{
    g_autofree gchar *path = g_build_filename(root, relative, NULL);
    g_autofree gchar *parent = g_path_get_dirname(path);
    g_assert_cmpint(g_mkdir_with_parents(parent, 0700), ==, 0);
    g_assert_true(g_file_set_contents(path, value, -1, NULL));
}

                                                                                                  
static int ids_rm_cb(const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
    (void)sb; (void)typeflag; (void)ftwbuf;
    return remove(path);
}

                                                      
                                    
static void ids_remove_tree(const gchar *root)
{
    g_assert_cmpint(nftw(root, ids_rm_cb, 16, FTW_DEPTH | FTW_PHYS), ==, 0);
    g_assert_false(g_file_test(root, G_FILE_TEST_EXISTS));
}

                                                                    
static gchar *ids_tmp_root(void)
{
    g_autoptr(GError) error = NULL;
    gchar *root = g_dir_make_tmp("pcv-hw-ids-XXXXXX", &error);
    g_assert_no_error(error);
    return root;
}

                                                                             
static void test_pci_vendor_device(void)
{
    g_autofree gchar *root = ids_tmp_root();
    ids_write(root, "usr/share/misc/pci.ids",
              "# PCI ID 목록\n"
              "\n"
              "10de  NVIDIA Corporation\n"
              "\t2482  GA104 [GeForce RTX 3070 Ti]\n"
              "\t\t10de 2482  Subsystem Should Not Win\n"
              "\t\t1462 2482  MSI board\n"
              "\t228b  GA104 High Definition Audio Controller\n"
              "8086  Intel Corporation\n"
              "\t9a49  TigerLake-LP GT2 [Iris Xe Graphics]\n");
    PcvHwIds *ids = pcv_hw_ids_load(root, PCV_HW_IDS_PCI);
    g_assert_nonnull(ids);
    g_assert_cmpstr(pcv_hw_ids_vendor(ids, "10de"), ==, "NVIDIA Corporation");
    g_assert_cmpstr(pcv_hw_ids_device(ids, "10de", "2482"), ==, "GA104 [GeForce RTX 3070 Ti]");
    g_assert_cmpstr(pcv_hw_ids_device(ids, "10de", "228b"), ==, "GA104 High Definition Audio Controller");
    g_assert_cmpstr(pcv_hw_ids_vendor(ids, "8086"), ==, "Intel Corporation");
    g_assert_cmpstr(pcv_hw_ids_device(ids, "8086", "9a49"), ==, "TigerLake-LP GT2 [Iris Xe Graphics]");
    g_assert_null(pcv_hw_ids_device(ids, "10de", "1462"));                             
    g_assert_null(pcv_hw_ids_device(ids, "8086", "2482"));                               
    g_assert_null(pcv_hw_ids_vendor(ids, "1462"));
    pcv_hw_ids_free(ids);

                                 
    g_autofree gchar *usb_only = ids_tmp_root();
    ids_write(usb_only, "usr/share/misc/usb.ids", "8087  Intel Corp.\n");
    ids = pcv_hw_ids_load(usb_only, PCV_HW_IDS_PCI);
    g_assert_null(pcv_hw_ids_vendor(ids, "8087"));
    pcv_hw_ids_free(ids);
    ids_remove_tree(usb_only);
    ids_remove_tree(root);
}

                                                                                            
                                                                       
static void test_usb_vendor_section_only(void)
{
    g_autofree gchar *root = ids_tmp_root();
    ids_write(root, "usr/share/misc/usb.ids",
              "# USB ID 목록\n"
              "1d6b  Linux Foundation\n"
              "\t0002  2.0 root hub\n"
              "8087  Intel Corp.\n"
              "\t0029  AX200 Bluetooth\n"
              "\t0a2b  Bluetooth wireless interface\n"
              "C 03  Human Interface Device\n"
              "\t01  Boot Interface Subclass\n"
              "\t\t01  Keyboard\n"
              "\t0029  X\n"
              "0029  Y\n"
              "HID 01  Keyboard\n");
    PcvHwIds *ids = pcv_hw_ids_load(root, PCV_HW_IDS_USB);
    g_assert_nonnull(ids);
    g_assert_cmpstr(pcv_hw_ids_vendor(ids, "8087"), ==, "Intel Corp.");
    g_assert_cmpstr(pcv_hw_ids_device(ids, "8087", "0029"), ==, "AX200 Bluetooth");
    g_assert_cmpstr(pcv_hw_ids_device(ids, "8087", "0a2b"), ==, "Bluetooth wireless interface");
    g_assert_cmpstr(pcv_hw_ids_device(ids, "1d6b", "0002"), ==, "2.0 root hub");
    g_assert_null(pcv_hw_ids_vendor(ids, "0029"));
    pcv_hw_ids_free(ids);

                                 
    g_autofree gchar *pci_only = ids_tmp_root();
    ids_write(pci_only, "usr/share/misc/pci.ids", "10de  NVIDIA Corporation\n");
    ids = pcv_hw_ids_load(pci_only, PCV_HW_IDS_USB);
    g_assert_null(pcv_hw_ids_vendor(ids, "10de"));
    pcv_hw_ids_free(ids);
    ids_remove_tree(pci_only);
    ids_remove_tree(root);
}

                                                                                         
static void ids_write_oversize(const gchar *root, const gchar *relative)
{
    ids_write(root, relative, "8087  Oversize Corp.\n\t0029  Oversize Device\n");
    g_autofree gchar *path = g_build_filename(root, relative, NULL);
    int fd = g_open(path, O_WRONLY, 0);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, (off_t)PCV_HW_IDS_MAX_BYTES + 1), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
}

                                                                                         
static void test_missing_oversize_fallback(void)
{
    g_autofree gchar *hwdata = ids_tmp_root();
    ids_write(hwdata, "usr/share/hwdata/usb.ids", "8087  Intel Corp.\n\t0029  AX200 Bluetooth\n");
    PcvHwIds *ids = pcv_hw_ids_load(hwdata, PCV_HW_IDS_USB);
    g_assert_cmpstr(pcv_hw_ids_vendor(ids, "8087"), ==, "Intel Corp.");
    g_assert_cmpstr(pcv_hw_ids_device(ids, "8087", "0029"), ==, "AX200 Bluetooth");
    pcv_hw_ids_free(ids);

    g_autofree gchar *empty = ids_tmp_root();
    for (guint k = 0; k < 2; k++) {
        ids = pcv_hw_ids_load(empty, k == 0 ? PCV_HW_IDS_PCI : PCV_HW_IDS_USB);
        g_assert_nonnull(ids);                                                        
        g_assert_null(pcv_hw_ids_vendor(ids, "8087"));
        g_assert_null(pcv_hw_ids_device(ids, "8087", "0029"));
        pcv_hw_ids_free(ids);
    }

    g_autofree gchar *oversize = ids_tmp_root();
    ids_write_oversize(oversize, "usr/share/misc/usb.ids");
    ids = pcv_hw_ids_load(oversize, PCV_HW_IDS_USB);
    g_assert_nonnull(ids);
    g_assert_null(pcv_hw_ids_vendor(ids, "8087"));
    g_assert_null(pcv_hw_ids_device(ids, "8087", "0029"));
    pcv_hw_ids_free(ids);
    ids_write(oversize, "usr/share/hwdata/usb.ids", "8087  Intel Corp.\n");                       
    ids = pcv_hw_ids_load(oversize, PCV_HW_IDS_USB);
    g_assert_cmpstr(pcv_hw_ids_vendor(ids, "8087"), ==, "Intel Corp.");
    pcv_hw_ids_free(ids);

    pcv_hw_ids_free(NULL);                                                    
    ids_remove_tree(oversize);
    ids_remove_tree(empty);
    ids_remove_tree(hwdata);
}

void test_hw_ids_register(void)
{
    g_test_add_func("/hw_ids/pci_vendor_device", test_pci_vendor_device);
    g_test_add_func("/hw_ids/usb_vendor_section_only", test_usb_vendor_section_only);
    g_test_add_func("/hw_ids/missing_oversize_fallback", test_missing_oversize_fallback);
}
