/*
 * Copyright © Microsoft Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "d3d12_screen.h"
#include "d3d12_public.h"

#include "util/u_debug.h"
#include "util/u_memory.h"
#include "util/u_dl.h"

#include <directx/dxcore.h>
#include <dxguids/dxguids.h>
#include <stdio.h>

#define XV6_DXCORE_DIAG(...) do { \
      fprintf(stderr, "xv6-mesa-dxcore: " __VA_ARGS__); \
      fflush(stderr); \
      fprintf(stdout, "xv6-mesa-dxcore: " __VA_ARGS__); \
      fflush(stdout); \
   } while (0)

struct xv6_d3dkmt_handle {
   uint32_t v;
};

struct xv6_winluid {
   uint32_t a;
   uint32_t b;
};

struct xv6_d3dkmt_adapterinfo {
   struct xv6_d3dkmt_handle adapter_handle;
   struct xv6_winluid adapter_luid;
   uint32_t num_sources;
   uint32_t present_move_regions_preferred;
};

struct xv6_d3dkmt_enumadapters2 {
   uint32_t num_adapters;
   uint32_t reserved;
   uint64_t *adapters;
};

struct xv6_d3dkmt_queryadapterinfo {
   struct xv6_d3dkmt_handle adapter;
   uint32_t type;
   uint64_t private_data;
   uint32_t private_data_size;
};

struct xv6_d3dkmt_device_ids {
   uint32_t vendor_id;
   uint32_t device_id;
   uint32_t sub_vendor_id;
   uint32_t sub_system_id;
   uint32_t revision_id;
   uint32_t bus_type;
};

#define XV6_KMTQAITYPE_PHYSICALADAPTERDEVICEIDS 31

static const char *
xv6_format_guid(REFGUID guid, char *buf, size_t buf_size)
{
   snprintf(buf, buf_size,
            "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
            guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
            guid.Data4[6], guid.Data4[7]);
   return buf;
}

static IDXCoreAdapterFactory *
get_dxcore_factory()
{
   typedef HRESULT(WINAPI *PFN_CREATE_DXCORE_ADAPTER_FACTORY)(REFIID riid, void **ppFactory);
   const char *candidates[] = {
      UTIL_DL_PREFIX "dxcore" UTIL_DL_EXT,
      "/lib/" UTIL_DL_PREFIX "dxcore" UTIL_DL_EXT,
      "/usr/lib/" UTIL_DL_PREFIX "dxcore" UTIL_DL_EXT,
      "/usr/lib/x86_64-linux-gnu/" UTIL_DL_PREFIX "dxcore" UTIL_DL_EXT,
   };
   char last_error[512] = "no candidates tried";

   XV6_DXCORE_DIAG("candidate diagnostics begin version=20260519 candidates=%u\n",
                   (unsigned)ARRAY_SIZE(candidates));
   for (unsigned i = 0; i < ARRAY_SIZE(candidates); i++) {
      util_dl_library *dxcore_mod = util_dl_open(candidates[i]);
      if (!dxcore_mod) {
         const char *err = util_dl_error();
         snprintf(last_error, sizeof(last_error),
                  "candidate='%s' dlopen failed: %s",
                  candidates[i], err ? err : "unknown error");
         XV6_DXCORE_DIAG("%s\n", last_error);
         continue;
      }

      XV6_DXCORE_DIAG("candidate='%s' dlopen ok\n", candidates[i]);

      PFN_CREATE_DXCORE_ADAPTER_FACTORY DXCoreCreateAdapterFactory =
         (PFN_CREATE_DXCORE_ADAPTER_FACTORY)util_dl_get_proc_address(dxcore_mod, "DXCoreCreateAdapterFactory");
      if (!DXCoreCreateAdapterFactory) {
         const char *err = util_dl_error();
         snprintf(last_error, sizeof(last_error),
                  "candidate='%s' dlsym DXCoreCreateAdapterFactory failed: %s",
                  candidates[i], err ? err : "unknown error");
         XV6_DXCORE_DIAG("%s\n", last_error);
         util_dl_close(dxcore_mod);
         continue;
      }

      XV6_DXCORE_DIAG("candidate='%s' DXCoreCreateAdapterFactory=%p\n",
                      candidates[i], (void *)DXCoreCreateAdapterFactory);

      static const GUID factory1_iid = __uuidof(IDXCoreAdapterFactory1);
      static const GUID factory_iid = __uuidof(IDXCoreAdapterFactory);
      static const struct {
         const char *name;
         const GUID *iid;
      } factory_iids[] = {
         { "IID_IDXCoreAdapterFactory1", &factory1_iid },
         { "IID_IDXCoreAdapterFactory", &factory_iid },
      };

      for (unsigned j = 0; j < ARRAY_SIZE(factory_iids); j++) {
         char iid_buf[64];
         IDXCoreAdapterFactory *factory = NULL;
         HRESULT hr = DXCoreCreateAdapterFactory(*factory_iids[j].iid,
                                                 (void **)&factory);
         if (SUCCEEDED(hr) && factory) {
            XV6_DXCORE_DIAG("candidate='%s' factory ok interface=%s iid=%s factory=%p\n",
                            candidates[i], factory_iids[j].name,
                            xv6_format_guid(*factory_iids[j].iid, iid_buf,
                                            sizeof(iid_buf)),
                            (void *)factory);
            return factory;
         }

         snprintf(last_error, sizeof(last_error),
                  "candidate='%s' DXCoreCreateAdapterFactory(%s iid=%s) failed hr=0x%08x factory=%p",
                  candidates[i], factory_iids[j].name,
                  xv6_format_guid(*factory_iids[j].iid, iid_buf,
                                  sizeof(iid_buf)),
                  (unsigned)hr, (void *)factory);
         XV6_DXCORE_DIAG("%s\n", last_error);
         if (factory)
            factory->Release();
      }

      util_dl_close(dxcore_mod);
   }

   debug_printf("D3D12: failed to load/create DXCore.DLL/libdxcore.so factory: %s\n",
                last_error);
   XV6_DXCORE_DIAG("failed to create DXCore factory: %s\n", last_error);
   return NULL;
}

static util_dl_library *
open_dxcore_for_d3dkmt(const char *symbol_name)
{
   const char *candidates[] = {
      UTIL_DL_PREFIX "dxcore" UTIL_DL_EXT,
      "/lib/" UTIL_DL_PREFIX "dxcore" UTIL_DL_EXT,
      "/usr/lib/" UTIL_DL_PREFIX "dxcore" UTIL_DL_EXT,
      "/usr/lib/x86_64-linux-gnu/" UTIL_DL_PREFIX "dxcore" UTIL_DL_EXT,
   };

   for (unsigned i = 0; i < ARRAY_SIZE(candidates); i++) {
      util_dl_library *dxcore_mod = util_dl_open(candidates[i]);
      if (!dxcore_mod) {
         const char *err = util_dl_error();
         fprintf(stderr, "D3D12: DXCore D3DKMT candidate='%s' dlopen failed: %s\n",
                 candidates[i], err ? err : "unknown error");
         fflush(stderr);
         continue;
      }
      if (util_dl_get_proc_address(dxcore_mod, symbol_name)) {
         fprintf(stderr,
                 "D3D12: DXCore D3DKMT candidate='%s' provides %s\n",
                 candidates[i], symbol_name);
         fflush(stderr);
         return dxcore_mod;
      }
      const char *err = util_dl_error();
      fprintf(stderr,
              "D3D12: DXCore D3DKMT candidate='%s' missing %s: %s\n",
              candidates[i], symbol_name, err ? err : "unknown error");
      fflush(stderr);
      util_dl_close(dxcore_mod);
   }

   return NULL;
}

static bool
get_first_d3dkmt_luid(LUID *adapter_luid)
{
   typedef int32_t(WINAPI *PFN_D3DKMT_ENUM_ADAPTERS2)(xv6_d3dkmt_enumadapters2 *args);

   util_dl_library *dxcore_mod = open_dxcore_for_d3dkmt("D3DKMTEnumAdapters2");
   if (!dxcore_mod)
      return false;

   PFN_D3DKMT_ENUM_ADAPTERS2 enum_adapters =
      (PFN_D3DKMT_ENUM_ADAPTERS2)util_dl_get_proc_address(dxcore_mod, "D3DKMTEnumAdapters2");
   if (!enum_adapters)
      return false;

   xv6_d3dkmt_enumadapters2 query = {};
   if (enum_adapters(&query) != 0 || query.num_adapters == 0)
      return false;

   xv6_d3dkmt_adapterinfo adapters[8] = {};
   if (query.num_adapters > ARRAY_SIZE(adapters))
      query.num_adapters = ARRAY_SIZE(adapters);
   query.adapters = (uint64_t *)adapters;
   if (enum_adapters(&query) != 0 || query.num_adapters == 0 ||
       adapters[0].adapter_handle.v == 0)
      return false;

   adapter_luid->LowPart = adapters[0].adapter_luid.a;
   adapter_luid->HighPart = (LONG)adapters[0].adapter_luid.b;
   fprintf(stderr, "D3D12: D3DKMT fallback adapter handle=0x%x luid=%08x:%08x\n",
           adapters[0].adapter_handle.v, adapters[0].adapter_luid.b,
           adapters[0].adapter_luid.a);
   return true;
}

static bool
dxcore_luid_is_zero(const LUID &luid)
{
   return luid.HighPart == 0 && luid.LowPart == 0;
}

static bool
dxcore_luid_equal(const LUID &a, const LUID &b)
{
   return a.HighPart == b.HighPart && a.LowPart == b.LowPart;
}

static bool
dxcore_get_adapter_luid(IDXCoreAdapter *adapter, LUID *luid)
{
   if (!adapter || !luid)
      return false;

   LUID adapter_luid = {};
   HRESULT hr =
      adapter->GetProperty(DXCoreAdapterProperty::InstanceLuid,
                           sizeof(adapter_luid), &adapter_luid);
   if (FAILED(hr)) {
      fprintf(stderr, "D3D12: DXCore adapter InstanceLuid failed hr=0x%08x\n",
              (unsigned)hr);
      return false;
   }

   *luid = adapter_luid;
   return true;
}

static bool
dxcore_hardware_id_is_sane(uint32_t vendor_id, uint32_t device_id)
{
   return vendor_id != 0 && vendor_id <= 0xffff && device_id != 0;
}

static bool
dxcore_vendor_id_is_known(uint32_t vendor_id)
{
   switch (vendor_id) {
   case 0x1002: /* AMD */
   case 0x1022: /* AMD */
   case 0x10de: /* NVIDIA */
   case 0x1414: /* Microsoft */
   case 0x1af4: /* VirtIO */
   case 0x8086: /* Intel */
      return true;
   default:
      return false;
   }
}

static bool
get_d3dkmt_adapter_handle_by_luid(util_dl_library *dxcore_mod,
                                  const LUID &luid,
                                  xv6_d3dkmt_handle *adapter_handle)
{
   typedef int32_t(WINAPI *PFN_D3DKMT_ENUM_ADAPTERS2)(xv6_d3dkmt_enumadapters2 *args);

   if (!dxcore_mod || !adapter_handle)
      return false;

   PFN_D3DKMT_ENUM_ADAPTERS2 enum_adapters =
      (PFN_D3DKMT_ENUM_ADAPTERS2)util_dl_get_proc_address(dxcore_mod, "D3DKMTEnumAdapters2");
   if (!enum_adapters)
      return false;

   xv6_d3dkmt_enumadapters2 query = {};
   if (enum_adapters(&query) != 0 || query.num_adapters == 0)
      return false;

   xv6_d3dkmt_adapterinfo adapters[8] = {};
   if (query.num_adapters > ARRAY_SIZE(adapters))
      query.num_adapters = ARRAY_SIZE(adapters);
   query.adapters = (uint64_t *)adapters;
   if (enum_adapters(&query) != 0 || query.num_adapters == 0)
      return false;

   for (uint32_t i = 0; i < query.num_adapters; i++) {
      if (adapters[i].adapter_handle.v == 0)
         continue;
      if (adapters[i].adapter_luid.a == (uint32_t)luid.LowPart &&
          adapters[i].adapter_luid.b == (uint32_t)luid.HighPart) {
         *adapter_handle = adapters[i].adapter_handle;
         return true;
      }
   }

   return false;
}

static bool
dxcore_get_d3dkmt_hardware_id(IDXCoreAdapter *adapter,
                              DXCoreHardwareID *hardware_id)
{
   typedef int32_t(WINAPI *PFN_D3DKMT_QUERY_ADAPTER_INFO)(
      const xv6_d3dkmt_queryadapterinfo *args);

   util_dl_library *dxcore_mod = NULL;
   PFN_D3DKMT_QUERY_ADAPTER_INFO query_adapter_info = NULL;
   uint32_t query_device_ids_raw[7] = {};
   xv6_d3dkmt_device_ids direct_device_ids = {};
   xv6_d3dkmt_device_ids indexed_device_ids = {};
   const xv6_d3dkmt_device_ids *device_ids = NULL;
   xv6_d3dkmt_queryadapterinfo query = {};
   xv6_d3dkmt_handle adapter_handle = {};
   LUID luid = {};
   HRESULT luid_hr;
   int32_t status;
   bool ok = false;

   if (!adapter || !hardware_id)
      return false;

   luid_hr =
      adapter->GetProperty(DXCoreAdapterProperty::InstanceLuid,
                           sizeof(luid), &luid);
   if (FAILED(luid_hr)) {
      fprintf(stderr,
              "D3D12: DXCore D3DKMT HardwareID fallback no InstanceLuid hr=0x%08x\n",
              (unsigned)luid_hr);
      return false;
   }

   dxcore_mod = open_dxcore_for_d3dkmt("D3DKMTQueryAdapterInfo");
   if (!dxcore_mod)
      return false;

   query_adapter_info =
      (PFN_D3DKMT_QUERY_ADAPTER_INFO)util_dl_get_proc_address(
         dxcore_mod, "D3DKMTQueryAdapterInfo");
   if (!query_adapter_info)
      return false;

   if (!get_d3dkmt_adapter_handle_by_luid(dxcore_mod, luid, &adapter_handle)) {
      fprintf(stderr,
              "D3D12: DXCore D3DKMT HardwareID fallback no D3DKMT handle for LUID %08x:%08x\n",
              (unsigned)luid.HighPart, (unsigned)luid.LowPart);
      return false;
   }

   query_device_ids_raw[0] = 0;
   query.adapter = adapter_handle;
   query.type = XV6_KMTQAITYPE_PHYSICALADAPTERDEVICEIDS;
   query.private_data = (uint64_t)(uintptr_t)query_device_ids_raw;
   query.private_data_size = sizeof(query_device_ids_raw);
   status = query_adapter_info(&query);
   direct_device_ids.vendor_id = query_device_ids_raw[0];
   direct_device_ids.device_id = query_device_ids_raw[1];
   direct_device_ids.sub_vendor_id = query_device_ids_raw[2];
   direct_device_ids.sub_system_id = query_device_ids_raw[3];
   direct_device_ids.revision_id = query_device_ids_raw[4];
   direct_device_ids.bus_type = query_device_ids_raw[5];
   indexed_device_ids.vendor_id = query_device_ids_raw[1];
   indexed_device_ids.device_id = query_device_ids_raw[2];
   indexed_device_ids.sub_vendor_id = query_device_ids_raw[3];
   indexed_device_ids.sub_system_id = query_device_ids_raw[4];
   indexed_device_ids.revision_id = query_device_ids_raw[5];
   indexed_device_ids.bus_type = query_device_ids_raw[6];
   if (status >= 0 &&
       dxcore_hardware_id_is_sane(direct_device_ids.vendor_id,
                                  direct_device_ids.device_id))
      device_ids = &direct_device_ids;
   else if (status >= 0 &&
            dxcore_hardware_id_is_sane(indexed_device_ids.vendor_id,
                                       indexed_device_ids.device_id))
      device_ids = &indexed_device_ids;
   ok = status >= 0 &&
      device_ids != NULL;
   fprintf(stderr,
           "D3D12: DXCore D3DKMT HardwareID fallback status=%d handle=0x%x luid=%08x:%08x raw=%x,%x,%x,%x,%x,%x,%x layout=%s vendor=0x%04x device=0x%04x subvendor=0x%04x subsystem=0x%04x rev=0x%x bus=0x%x ok=%u\n",
           status, adapter_handle.v, (unsigned)luid.HighPart,
           (unsigned)luid.LowPart, query_device_ids_raw[0],
           query_device_ids_raw[1], query_device_ids_raw[2],
           query_device_ids_raw[3], query_device_ids_raw[4],
           query_device_ids_raw[5], query_device_ids_raw[6],
           device_ids == &direct_device_ids ? "direct" :
              device_ids == &indexed_device_ids ? "indexed" : "none",
           device_ids ? device_ids->vendor_id : 0,
           device_ids ? device_ids->device_id : 0,
           device_ids ? device_ids->sub_vendor_id : 0,
           device_ids ? device_ids->sub_system_id : 0,
           device_ids ? device_ids->revision_id : 0,
           device_ids ? device_ids->bus_type : 0, ok);
   if (!ok)
      return false;

   hardware_id->vendorID = device_ids->vendor_id;
   hardware_id->deviceID = device_ids->device_id;
   hardware_id->subSysID =
      ((device_ids->sub_vendor_id & 0xffff) << 16) |
      (device_ids->sub_system_id & 0xffff);
   hardware_id->revision = device_ids->revision_id;
   return true;
}

static DXCoreHardwareID
dxcore_get_normalized_hardware_id(IDXCoreAdapter *adapter,
                                  const DXCoreHardwareID &raw_hardware_id)
{
   DXCoreHardwareID hardware_id = raw_hardware_id;
   DXCoreHardwareIDParts hardware_id_parts = {};
   DXCoreHardwareID d3dkmt_hardware_id = {};
   HRESULT parts_hr = E_FAIL;
   bool raw_sane =
      dxcore_hardware_id_is_sane(raw_hardware_id.vendorID,
                                 raw_hardware_id.deviceID);
   bool parts_sane = false;
   bool d3dkmt_sane = false;
   const char *source = "HardwareID";

   if (adapter) {
      parts_hr =
         adapter->GetProperty(DXCoreAdapterProperty::HardwareIDParts,
                              sizeof(hardware_id_parts), &hardware_id_parts);
      parts_sane =
         SUCCEEDED(parts_hr) &&
         dxcore_hardware_id_is_sane(hardware_id_parts.vendorID,
                                    hardware_id_parts.deviceID);
   }

   if (parts_sane) {
      hardware_id.vendorID = hardware_id_parts.vendorID;
      hardware_id.deviceID = hardware_id_parts.deviceID;
      hardware_id.subSysID =
         ((hardware_id_parts.subVendorID & 0xffff) << 16) |
         (hardware_id_parts.subSystemID & 0xffff);
      hardware_id.revision = hardware_id_parts.revisionID;
      source = "HardwareIDParts";
   }

   if (!dxcore_vendor_id_is_known(hardware_id.vendorID)) {
      d3dkmt_sane =
         dxcore_get_d3dkmt_hardware_id(adapter, &d3dkmt_hardware_id);
      if (d3dkmt_sane) {
         hardware_id = d3dkmt_hardware_id;
         source = "D3DKMT";
      }
   }

   fprintf(stderr,
           "D3D12: DXCore HardwareID raw vendor=0x%08x device=0x%08x subsys=0x%08x rev=0x%x raw_sane=%u parts_hr=0x%08x parts vendor=0x%08x device=0x%08x subsystem=0x%08x subvendor=0x%08x rev=0x%x parts_sane=%u d3dkmt_sane=%u normalized vendor=0x%04x device=0x%04x subsys=0x%08x rev=0x%x source=%s\n",
           raw_hardware_id.vendorID, raw_hardware_id.deviceID,
           raw_hardware_id.subSysID, raw_hardware_id.revision,
           raw_sane, (unsigned)parts_hr, hardware_id_parts.vendorID,
           hardware_id_parts.deviceID, hardware_id_parts.subSystemID,
           hardware_id_parts.subVendorID, hardware_id_parts.revisionID,
           parts_sane, d3dkmt_sane, hardware_id.vendorID, hardware_id.deviceID,
           hardware_id.subSysID, hardware_id.revision,
           source);

   return hardware_id;
}

static IDXCoreAdapter *
get_dxcore_adapter_by_luid(IDXCoreAdapterFactory *factory, const LUID &luid,
                           const char *label)
{
   IDXCoreAdapter *adapter = nullptr;
   HRESULT hr = factory->GetAdapterByLuid(luid, &adapter);
   if (SUCCEEDED(hr) && adapter) {
      fprintf(stderr,
              "D3D12: selected DXCore adapter by %s LUID %08x:%08x\n",
              label, (unsigned)luid.HighPart, (unsigned)luid.LowPart);
      return adapter;
   }

   fprintf(stderr,
           "D3D12: DXCore GetAdapterByLuid %s LUID %08x:%08x failed hr=0x%08x adapter=%p\n",
           label, (unsigned)luid.HighPart, (unsigned)luid.LowPart,
           (unsigned)hr, (void *)adapter);
   if (adapter)
      adapter->Release();
   return nullptr;
}

static IDXCoreAdapter *
choose_from_dxcore_list(IDXCoreAdapterList *list, const char *label,
                        const LUID *required_luid)
{
   IDXCoreAdapter *adapter = nullptr;
   unsigned adapter_count = list->GetAdapterCount();
   fprintf(stderr, "D3D12: DXCore %s adapter list count=%u\n",
           label, adapter_count);

   if (required_luid) {
      for (unsigned i = 0; i < adapter_count; ++i) {
         if (FAILED(list->GetAdapter(i, &adapter))) {
            fprintf(stderr, "D3D12: DXCore %s GetAdapter(%u) failed\n",
                    label, i);
            continue;
         }

         LUID list_luid = {};
         if (dxcore_get_adapter_luid(adapter, &list_luid)) {
            fprintf(stderr,
                    "D3D12: DXCore %s adapter %u luid=%08x:%08x required=%08x:%08x\n",
                    label, i, (unsigned)list_luid.HighPart,
                    (unsigned)list_luid.LowPart,
                    (unsigned)required_luid->HighPart,
                    (unsigned)required_luid->LowPart);
            if (dxcore_luid_equal(list_luid, *required_luid)) {
               fprintf(stderr,
                       "D3D12: selected DXCore %s adapter by D3DKMT LUID match\n",
                       label);
               return adapter;
            }
         }

         adapter->Release();
         adapter = nullptr;
      }

      fprintf(stderr,
              "D3D12: DXCore %s has no adapter matching required D3DKMT LUID %08x:%08x\n",
              label, (unsigned)required_luid->HighPart,
              (unsigned)required_luid->LowPart);
      return nullptr;
   }

#ifndef _WIN32
   // Pick the user selected adapter if any
   const char *adapter_name = os_get_option("MESA_D3D12_DEFAULT_ADAPTER_NAME");
   if (adapter_name) {
      for (unsigned i=0; i<adapter_count; i++) {
         if (SUCCEEDED(list->GetAdapter(i, &adapter))) {

            size_t desc_size;
            if (!SUCCEEDED(adapter->GetPropertySize(DXCoreAdapterProperty::DriverDescription, &desc_size))) {
               adapter->Release();
               adapter = nullptr;
               continue;
            }

            char *desc = (char*)malloc(desc_size);
            if (!desc) {
               adapter->Release();
               adapter = nullptr;
               continue;
            }

            if (!SUCCEEDED(adapter->GetProperty(DXCoreAdapterProperty::DriverDescription, desc_size, desc))) {
               free(desc);
               adapter->Release();
               adapter = nullptr;
               continue;
            }

            if (strcasestr(desc, adapter_name)) {
               free(desc);
               return adapter;
            } else {
               free(desc);
               adapter->Release();
               adapter = nullptr;
            }
         }
      }
      debug_printf("D3D12: Couldn't find an adapter containing the substring (%s)\n", adapter_name);
   }
#endif

   // Adapter not specified or not found, so pick an integrated adapter if possible
   for (unsigned i = 0; i < adapter_count; ++i) {
      if (SUCCEEDED(list->GetAdapter(i, &adapter))) {
         bool is_integrated;
         HRESULT integrated_hr =
            adapter->GetProperty(DXCoreAdapterProperty::IsIntegrated, &is_integrated);
         fprintf(stderr, "D3D12: DXCore %s adapter %u IsIntegrated hr=0x%08x value=%u\n",
                 label, i, (unsigned)integrated_hr,
                 SUCCEEDED(integrated_hr) ? (unsigned)is_integrated : 0);
         if (SUCCEEDED(integrated_hr) && is_integrated)
            return adapter;
         adapter->Release();
         adapter = nullptr;
      } else {
         fprintf(stderr, "D3D12: DXCore %s GetAdapter(%u) failed\n",
                 label, i);
      }
   }

   // No integrated GPUs, so pick the first valid one
   if (adapter_count > 0 && SUCCEEDED(list->GetAdapter(0, &adapter))) {
      fprintf(stderr, "D3D12: selected DXCore %s adapter 0 fallback\n",
              label);
      return adapter;
   }

   return nullptr;
}

static IDXCoreAdapter *
choose_from_dxcore_create_adapter_list(IDXCoreAdapterFactory *factory,
                                       const char *label,
                                       uint32_t num_attributes,
                                       const GUID *attributes,
                                       const LUID *required_luid)
{
   IDXCoreAdapterList *list = nullptr;
   HRESULT hr = factory->CreateAdapterList(num_attributes, attributes, &list);
   if (FAILED(hr) || !list) {
      fprintf(stderr, "D3D12: DXCore CreateAdapterList(%s) failed hr=0x%08x\n",
              label, (unsigned)hr);
      return nullptr;
   }

   IDXCoreAdapter *adapter = choose_from_dxcore_list(list, label, required_luid);
   list->Release();
   return adapter;
}

static IDXCoreAdapter *
choose_from_dxcore_workload_list(IDXCoreAdapterFactory *factory,
                                 const LUID *required_luid)
{
   IDXCoreAdapterFactory1 *factory1 = nullptr;
   HRESULT qi_hr = factory->QueryInterface(IID_PPV_ARGS(&factory1));
   if (FAILED(qi_hr) || !factory1) {
      fprintf(stderr,
              "D3D12: DXCore Factory1 unavailable for workload adapter list hr=0x%08x\n",
              (unsigned)qi_hr);
      return nullptr;
   }

   IDXCoreAdapterList *list = nullptr;
   HRESULT hr = factory1->CreateAdapterListByWorkload(
      DXCoreWorkload::Graphics,
      DXCoreRuntimeFilterFlags::D3D12,
      DXCoreHardwareTypeFilterFlags::GPU,
      IID_PPV_ARGS(&list));
   factory1->Release();
   if (FAILED(hr) || !list) {
      fprintf(stderr,
              "D3D12: DXCore CreateAdapterListByWorkload(Graphics,D3D12,GPU) failed hr=0x%08x\n",
              (unsigned)hr);
      return nullptr;
   }

   IDXCoreAdapter *adapter =
      choose_from_dxcore_list(list, "workload Graphics/D3D12/GPU",
                              required_luid);
   list->Release();
   return adapter;
}

static void
log_dxcore_attribute_counts(IDXCoreAdapterFactory *factory)
{
   static const struct {
      const char *name;
      const GUID *attr;
   } attrs[] = {
      { "D3D12_GRAPHICS", &DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS },
      { "D3D12_CORE_COMPUTE", &DXCORE_ADAPTER_ATTRIBUTE_D3D12_CORE_COMPUTE },
      { "D3D11_GRAPHICS", &DXCORE_ADAPTER_ATTRIBUTE_D3D11_GRAPHICS },
      { "D3D12_GENERIC_ML", &DXCORE_ADAPTER_ATTRIBUTE_D3D12_GENERIC_ML },
      { "D3D12_GENERIC_MEDIA", &DXCORE_ADAPTER_ATTRIBUTE_D3D12_GENERIC_MEDIA },
      { "HARDWARE_GPU", &DXCORE_HARDWARE_TYPE_ATTRIBUTE_GPU },
   };

   for (unsigned i = 0; i < ARRAY_SIZE(attrs); i++) {
      IDXCoreAdapterList *list = nullptr;
      HRESULT hr = factory->CreateAdapterList(1, attrs[i].attr, &list);
      unsigned count = SUCCEEDED(hr) && list ? list->GetAdapterCount() : 0;

      fprintf(stderr, "D3D12: DXCore attr %s hr=0x%08x count=%u\n",
              attrs[i].name, (unsigned)hr, count);
      if (list)
         list->Release();
   }
}

static IDXCoreAdapter *
choose_dxcore_adapter(IDXCoreAdapterFactory *factory, LUID *adapter_luid)
{
   IDXCoreAdapter *adapter = nullptr;
   log_dxcore_attribute_counts(factory);
   if (adapter_luid) {
      adapter = get_dxcore_adapter_by_luid(factory, *adapter_luid,
                                           "requested");
      if (adapter)
         return adapter;
      debug_printf("D3D12: requested adapter missing, falling back to auto-detection...\n");
   }

   adapter = choose_from_dxcore_create_adapter_list(
      factory, "attr D3D12_GRAPHICS", 1,
      &DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS, nullptr);
   if (adapter)
      return adapter;

   LUID d3dkmt_luid = {};
   if (get_first_d3dkmt_luid(&d3dkmt_luid) && !dxcore_luid_is_zero(d3dkmt_luid)) {
      adapter = get_dxcore_adapter_by_luid(factory, d3dkmt_luid, "D3DKMT");
      if (adapter)
         return adapter;

      adapter = choose_from_dxcore_workload_list(factory, &d3dkmt_luid);
      if (adapter)
         return adapter;
      adapter = get_dxcore_adapter_by_luid(factory, d3dkmt_luid,
                                           "D3DKMT after workload");
      if (adapter)
         return adapter;

      fprintf(stderr, "D3D12: D3DKMT LUID fallback failed\n");
   }

   return NULL;
}

static const char *
dxcore_get_name(struct pipe_screen *screen)
{
   struct d3d12_dxcore_screen *dxcore_screen = d3d12_dxcore_screen(d3d12_screen(screen));
   static char buf[1000];
   if (dxcore_screen->description[0] == '\0')
      return "D3D12 (Unknown)";

   snprintf(buf, sizeof(buf), "D3D12 (%s)", dxcore_screen->description);
   return buf;
}

static void
dxcore_get_memory_info(struct d3d12_screen *screen, struct d3d12_memory_info *output)
{
   struct d3d12_dxcore_screen *dxcore_screen = d3d12_dxcore_screen(screen);
   if (!dxcore_screen->adapter) {
      output->budget_local = screen->memory_device_size_megabytes << 20;
      output->budget_nonlocal = screen->memory_system_size_megabytes << 20;
      output->budget = output->budget_local + output->budget_nonlocal;
      output->usage_local = 0;
      output->usage_nonlocal = 0;
      output->usage = 0;
      return;
   }
   DXCoreAdapterMemoryBudget local_info, nonlocal_info;
   DXCoreAdapterMemoryBudgetNodeSegmentGroup local_node_segment = { 0, DXCoreSegmentGroup::Local };
   DXCoreAdapterMemoryBudgetNodeSegmentGroup nonlocal_node_segment = { 0, DXCoreSegmentGroup::NonLocal };
   dxcore_screen->adapter->QueryState(DXCoreAdapterState::AdapterMemoryBudget, &local_node_segment, &local_info);
   dxcore_screen->adapter->QueryState(DXCoreAdapterState::AdapterMemoryBudget, &nonlocal_node_segment, &nonlocal_info);

   output->budget_local = local_info.budget;
   output->budget_nonlocal = nonlocal_info.budget;
   output->budget = local_info.budget + nonlocal_info.budget;
   output->usage_local = local_info.currentUsage;
   output->usage_nonlocal = nonlocal_info.currentUsage;
   output->usage = local_info.currentUsage + nonlocal_info.currentUsage;
}

static void
d3d12_deinit_dxcore_screen(struct d3d12_screen *dscreen)
{
   d3d12_deinit_screen(dscreen);
   struct d3d12_dxcore_screen *screen = d3d12_dxcore_screen(dscreen);
   if (screen->adapter) {
      screen->adapter->Release();
      screen->adapter = nullptr;
   }
   if (screen->factory) {
      screen->factory->Release();
      screen->factory = nullptr;
   }
}

static void
d3d12_destroy_dxcore_screen(struct pipe_screen *pscreen)
{
   struct d3d12_screen *screen = d3d12_screen(pscreen);
   d3d12_deinit_dxcore_screen(screen);
   d3d12_destroy_screen(screen);
}

static bool
d3d12_init_dxcore_screen(struct d3d12_screen *dscreen)
{
   struct d3d12_dxcore_screen *screen = d3d12_dxcore_screen(dscreen);

   screen->factory = get_dxcore_factory();
   if (!screen->factory) {
      XV6_DXCORE_DIAG("failed to create DXCore factory during screen init; "
                      "candidate diagnostics should be visible above\n");
      return false;
   }

   LUID *adapter_luid = &dscreen->adapter_luid;
   if (adapter_luid->HighPart == 0 && adapter_luid->LowPart == 0)
      adapter_luid = nullptr;

   screen->adapter = choose_dxcore_adapter(screen->factory, adapter_luid);
   if (!screen->adapter) {
      LUID d3dkmt_luid = {};
      if (!get_first_d3dkmt_luid(&d3dkmt_luid)) {
         debug_printf("D3D12: no suitable adapter\n");
         fprintf(stderr, "D3D12: no suitable DXCore adapter\n");
         return false;
      }

      debug_printf("D3D12: no suitable adapter\n");
      fprintf(stderr, "D3D12: DXCore could not map D3DKMT LUID %08x:%08x\n",
              (unsigned)d3dkmt_luid.HighPart, (unsigned)d3dkmt_luid.LowPart);
      return false;
   }

   DXCoreHardwareID hardware_ids = {};
   DXCoreHardwareID normalized_hardware_ids = {};
   uint64_t dedicated_video_memory, dedicated_system_memory, shared_system_memory;
   if (FAILED(screen->adapter->GetProperty(DXCoreAdapterProperty::HardwareID, &hardware_ids)) ||
       FAILED(screen->adapter->GetProperty(DXCoreAdapterProperty::DedicatedAdapterMemory, &dedicated_video_memory)) ||
       FAILED(screen->adapter->GetProperty(DXCoreAdapterProperty::DedicatedSystemMemory, &dedicated_system_memory)) ||
       FAILED(screen->adapter->GetProperty(DXCoreAdapterProperty::SharedSystemMemory, &shared_system_memory)) ||
       FAILED(screen->adapter->GetProperty(DXCoreAdapterProperty::DriverVersion, &screen->base.driver_version)) ||
       FAILED(screen->adapter->GetProperty(DXCoreAdapterProperty::DriverDescription,
                                           sizeof(screen->description),
                                           screen->description))) {
      debug_printf("D3D12: failed to retrieve adapter description\n");
      fprintf(stderr, "D3D12: failed to retrieve DXCore adapter properties\n");
      return false;
   }

   normalized_hardware_ids =
      dxcore_get_normalized_hardware_id(screen->adapter, hardware_ids);
   screen->base.vendor_id = normalized_hardware_ids.vendorID;
   screen->base.device_id = normalized_hardware_ids.deviceID;
   screen->base.subsys_id = normalized_hardware_ids.subSysID;
   screen->base.revision = normalized_hardware_ids.revision;
   screen->base.memory_device_size_megabytes = dedicated_video_memory >> 20;
   screen->base.memory_system_size_megabytes = (dedicated_system_memory + shared_system_memory) >> 20;
   screen->base.base.get_name = dxcore_get_name;
   screen->base.get_memory_info = dxcore_get_memory_info;
   fprintf(stderr,
           "D3D12: adapter properties vendor=0x%04x device=0x%04x subsys=0x%08x rev=0x%x dedicated=%llu system=%llu shared=%llu driver=0x%llx desc='%s'\n",
           screen->base.vendor_id, screen->base.device_id,
           screen->base.subsys_id, screen->base.revision,
           (unsigned long long)dedicated_video_memory,
           (unsigned long long)dedicated_system_memory,
           (unsigned long long)shared_system_memory,
           (unsigned long long)screen->base.driver_version,
           screen->description);

   if (!d3d12_init_screen(&screen->base, screen->adapter)) {
      debug_printf("D3D12: failed to initialize DXCore screen\n");
      fprintf(stderr, "D3D12: failed to initialize DXCore screen\n");
      return false;
   }

   return true;
}

struct pipe_screen *
d3d12_create_dxcore_screen(struct sw_winsys *winsys, LUID *adapter_luid)
{
   struct d3d12_dxcore_screen *screen = CALLOC_STRUCT(d3d12_dxcore_screen);
   if (!screen)
      return nullptr;

   if (!d3d12_init_screen_base(&screen->base, winsys, adapter_luid)) {
      d3d12_destroy_screen(&screen->base);
      return nullptr;
   }
   screen->base.base.destroy = d3d12_destroy_dxcore_screen;
   screen->base.init = d3d12_init_dxcore_screen;
   screen->base.deinit = d3d12_deinit_dxcore_screen;

   if (!d3d12_init_dxcore_screen(&screen->base)) {
      d3d12_destroy_dxcore_screen(&screen->base.base);
      return nullptr;
   }

   return &screen->base.base;
}

struct pipe_screen *
d3d12_create_dxcore_screen_from_d3d12_device(struct sw_winsys *winsys, IUnknown* pDevUnk, LUID **out_adapter_luid)
{
   struct d3d12_dxcore_screen *screen = CALLOC_STRUCT(d3d12_dxcore_screen);
   if (!screen)
      return nullptr;

   if (FAILED(pDevUnk->QueryInterface(IID_PPV_ARGS(&screen->base.dev)))) {
      d3d12_destroy_screen(&screen->base);
      return nullptr;
   }

   LUID adapter_luid = GetAdapterLuid(screen->base.dev);
   if (!d3d12_init_screen_base(&screen->base, winsys, &adapter_luid)) {
      d3d12_destroy_screen(&screen->base);
      return nullptr;
   }
   screen->base.base.destroy = d3d12_destroy_dxcore_screen;
   screen->base.init = d3d12_init_dxcore_screen;
   screen->base.deinit = d3d12_deinit_dxcore_screen;

   if (!d3d12_init_dxcore_screen(&screen->base)) {
      d3d12_destroy_dxcore_screen(&screen->base.base);
      return nullptr;
   }

   *out_adapter_luid = &screen->base.adapter_luid;
   return &screen->base.base;
}
