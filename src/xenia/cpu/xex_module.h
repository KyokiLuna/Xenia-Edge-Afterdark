/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_XEX_MODULE_H_
#define XENIA_CPU_XEX_MODULE_H_

#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "xenia/base/mapped_memory.h"
#include "xenia/cpu/guest_import_provider.h"
#include "xenia/cpu/module.h"
#include "xenia/kernel/util/xex2_info.h"

namespace xe {
namespace kernel {
class KernelState;
}  // namespace kernel
}  // namespace xe

namespace xe {
namespace cpu {

constexpr fourcc_t kXEX0Signature = make_fourcc("XEX0");
constexpr fourcc_t kXEXQSignature = make_fourcc("XEX?");
constexpr fourcc_t kXEXHSignature = make_fourcc("XEX-");
constexpr fourcc_t kXEX25Signature = make_fourcc("XEX%");
constexpr fourcc_t kXEX1Signature = make_fourcc("XEX1");
constexpr fourcc_t kXEX2Signature = make_fourcc("XEX2");
constexpr fourcc_t kElfSignature = make_fourcc(0x7F, 'E', 'L', 'F');
constexpr fourcc_t kXBESignature = make_fourcc("XBEH");

class Runtime;
struct InfoCacheFlags {
  uint32_t was_resolved : 1;  // has this address ever been called/requested
                              // via resolvefunction?
  uint32_t accessed_mmio : 1;
  uint32_t is_syscall_func : 1;
  uint32_t is_return_site : 1;  // address can be reached from another function
                                // by returning
  uint32_t reserved : 28;
};
static_assert(sizeof(InfoCacheFlags) == 4,
              "InfoCacheFlags size should be equal to sizeof ppc instruction.");

// The flags share one 32-bit word in a shared mmap and are written by several
// threads. Set bits atomically so concurrent writes don't lose updates.
inline void AtomicSetInfoCacheFlags(InfoCacheFlags* slot, InfoCacheFlags bits) {
  uint32_t mask;
  std::memcpy(&mask, &bits, sizeof(mask));
  std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(slot))
      .fetch_or(mask, std::memory_order_relaxed);
}

struct XexInfoCache {
  // increment this to invalidate all user infocaches
  static constexpr uint32_t CURRENT_INFOCACHE_VERSION = 4;

  struct InfoCacheFlagsHeader {
    uint32_t version;

    unsigned char reserved[252];

    InfoCacheFlags* LookupFlags(unsigned offset) {
      return &reinterpret_cast<InfoCacheFlags*>(&this[1])[offset];
    }
  };
  /*
        for every 4-byte aligned address, records a 4 byte set of flags.
  */
  std::unique_ptr<MappedMemory> executable_addr_flags_;

  void Init(class XexModule*);
  InfoCacheFlagsHeader* GetHeader() {
    if (!executable_addr_flags_) {
      return nullptr;
    }
    uint8_t* data = executable_addr_flags_->data();

    if (!data) {
      return nullptr;
    }
    return reinterpret_cast<InfoCacheFlagsHeader*>(data);
  }

  InfoCacheFlags* LookupFlags(unsigned offset) {
    offset /= 4;
    if (!executable_addr_flags_) {
      return nullptr;
    }
    uint8_t* data = executable_addr_flags_->data();

    if (!data) {
      return nullptr;
    }
    return reinterpret_cast<InfoCacheFlagsHeader*>(data)->LookupFlags(offset);
  }
};

class XexModule : public xe::cpu::Module {
 public:
  struct ImportLibraryFn {
   public:
    uint32_t ordinal;
    uint32_t value_address;
    uint32_t thunk_address;
  };
  struct ImportLibrary {
   public:
    std::string name;
    uint32_t id;
    xe_xex2_version_t version;
    xe_xex2_version_t min_version;
    std::vector<ImportLibraryFn> imports;
  };
  struct SecurityInfoContext {
    const char* rsa_signature;
    const char* aes_key;
    uint32_t image_size;
    uint32_t image_flags;
    uint32_t export_table;
    uint32_t load_address;
    uint32_t page_descriptor_count;
    const xex2_page_descriptor* page_descriptors;
  };
  enum XexFormat {
    kFormatUnknown,
    kFormatXex0,
    kFormatXexQ,   // ?
    kFormatXexH,   // -
    kFormatXex25,  // %
    kFormatXex1,
    kFormatXex2,
  };

  XexModule(Processor* processor, kernel::KernelState* kernel_state);
  virtual ~XexModule();

  // Bind a real guest export entry to an implemented host export, including
  // intra-image direct calls. Only a quiescent, not-yet-executed module runtime
  // may install these. A previously compiled target is rejected: compiled
  // callers may contain literal native addresses that invalidation cannot
  // repair. The owner must prevent execution/precompilation while binding.
  // Translation uses an owned syscall overlay; guest bytes remain unchanged.
  bool BindGuestExportToHost(uint16_t ordinal, std::string_view host_library,
                             uint16_t host_ordinal);
  // Bind a genuine export with a checked complete original extent/hash. Unlike
  // the legacy overload, this also enforces terminal and interior-entry checks.
  // The export table is the entry witness; an intra-image BL is not required.
  bool BindGuestExportToHost(uint16_t ordinal, std::string_view host_library,
                             uint16_t host_ordinal, uint32_t full_size,
                             std::string_view full_sha256);
  // Bind a signature-verified internal entry (at least two instructions).
  // Requires a concrete BL target in this image's mapped code and a complete
  // terminal extent, with no interior entry or overlapping function. Only
  // static trusted code may supply a handler; never load host pointers from
  // guest data or configuration. Same quiescence rules as export binding.
  bool BindGuestFunctionToHost(uint32_t address, uint32_t size,
                               std::string_view expected_sha256,
                               std::string_view host_library,
                               uint16_t host_ordinal);
  bool BindGuestFunctionToHost(uint32_t address, uint32_t size,
                               std::string_view expected_sha256,
                               GuestFunction::ExternHandler host_handler,
                               std::string_view host_name);
  // A version-verified, same-image native TaskSchedule registration. This
  // accepts only the checked nine-instruction registration template described
  // below, not an arbitrary address-taken pointer or a caller-supplied bypass.
  // The callback and scheduler must have exact full-extent PE .pdata entries.
  // Only trusted native runtime setup may supply this metadata and handler,
  // retaining this exact private module generation and keeping it quiescent.
  struct GuestTaskCallbackRegistration {
    uint32_t registration_address = 0;
    uint32_t registration_size = 0;
    std::string_view registration_sha256;
    uint32_t schedule_address = 0;
    uint32_t schedule_size = 0;
    std::string_view schedule_sha256;
  };
  bool BindGuestTaskCallbackToHost(
      uint32_t address, uint32_t size, std::string_view expected_sha256,
      const GuestTaskCallbackRegistration& registration,
      GuestFunction::ExternHandler host_handler, std::string_view host_name);
  const uint8_t* TranslateCode(uint32_t address) const override;
  Symbol::Status DeclareFunction(uint32_t address,
                                 Function** out_function) override;

  bool loaded() const { return loaded_; }
  const std::string& image_code_sha1() const { return image_sha_str_; }
  const xex2_header* xex_header() const {
    return reinterpret_cast<const xex2_header*>(xex_header_mem_.data());
  }
  const SecurityInfoContext* xex_security_info() const {
    return &security_info_;
  }

  uint32_t image_size() const {
    assert_not_zero(base_address_);

    // Calculate the new total size of the XEX image from its headers.
    auto heap = memory()->LookupHeap(base_address_);
    uint32_t total_size = 0;
    for (uint32_t i = 0; i < xex_security_info()->page_descriptor_count; i++) {
      // Byteswap the bitfield manually.
      xex2_page_descriptor desc;
      desc.value =
          xe::byte_swap(xex_security_info()->page_descriptors[i].value);

      total_size += desc.page_count * heap->page_size();
    }
    return total_size;
  }

  const std::vector<ImportLibrary>* import_libraries() const {
    return &import_libs_;
  }
  // The modules it imports from that failed to load, whose imports are left
  // unresolved.
  const std::vector<std::string>& missing_import_libraries() const {
    return missing_import_libs_;
  }

  const xex2_opt_execution_info* opt_execution_info() const {
    xex2_opt_execution_info* retval = nullptr;
    GetOptHeader(XEX_HEADER_EXECUTION_INFO, &retval);
    return retval;
  }

  const xex2_opt_file_format_info* opt_file_format_info() const {
    xex2_opt_file_format_info* retval = nullptr;
    GetOptHeader(XEX_HEADER_FILE_FORMAT_INFO, &retval);
    return retval;
  }

  std::vector<uint32_t> opt_alternate_title_ids() const {
    return opt_alternate_title_ids_;
  }

  const uint32_t base_address() const { return base_address_; }
  const bool is_dev_kit() const { return is_dev_kit_; }

  // Section type of the image page containing `address`.
  bool GetPageSectionType(uint32_t address, xex2_section_type* out_type) const;

  // Gets an optional header. Returns NULL if not found.
  // Special case: if key & 0xFF == 0x00, this function will return the value,
  // not a pointer! This assumes out_ptr points to uint32_t.
  static bool GetOptHeader(const xex2_header* header, xex2_header_keys key,
                           void** out_ptr);
  bool GetOptHeader(xex2_header_keys key, void** out_ptr) const;

  // Ultra-cool templated version
  // Special case: if key & 0xFF == 0x00, this function will return the value,
  // not a pointer! This assumes out_ptr points to uint32_t.
  template <typename T>
  static bool GetOptHeader(const xex2_header* header, xex2_header_keys key,
                           T* out_ptr) {
    return GetOptHeader(header, key, reinterpret_cast<void**>(out_ptr));
  }

  template <typename T>
  bool GetOptHeader(xex2_header_keys key, T* out_ptr) const {
    return GetOptHeader(key, reinterpret_cast<void**>(out_ptr));
  }

  static const void* GetSecurityInfo(const xex2_header* header);

  const PESection* GetPESection(const char* name);

  uint32_t GetProcAddress(uint16_t ordinal) const;
  uint32_t GetProcAddress(const std::string_view name) const;

  int ApplyPatch(XexModule* module);
  bool Load(const std::string_view name, const std::string_view path,
            const void* xex_addr, size_t xex_length);
  bool LoadContinue();
  // Install only before loading a fresh module. One immutable provider covers
  // its complete import list; missing claimed exports fail before slot writes.
  void SetGuestImportProvider(
      std::shared_ptr<const GuestImportProvider> provider) {
    assert_false(loaded_);
    guest_import_provider_ = std::move(provider);
  }
  const std::shared_ptr<const GuestImportProvider>& guest_import_provider()
      const {
    return guest_import_provider_;
  }
  bool Unload();

  bool ContainsAddress(uint32_t address) override;

  const std::string& name() const override { return name_; }
  bool is_executable() const override {
    return (xex_header()->module_flags & XEX_MODULE_TITLE) != 0;
  }

  bool is_valid_executable() const;
  // A module of resources alone has no code, nor a PE image.
  bool has_code() const;

  bool is_patch() const {
    assert_not_null(xex_header());
    if (!xex_header()) {
      return false;
    }
    return (xex_header()->module_flags &
            (XEX_MODULE_MODULE_PATCH | XEX_MODULE_PATCH_DELTA |
             XEX_MODULE_PATCH_FULL));
  }

  InfoCacheFlags* GetInstructionAddressFlags(uint32_t guest_addr);

  virtual void Precompile() override;
  // The no-argument virtual entry preserves normal module loading. Explicit
  // fresh native runtimes may defer compilation until bridges are installed;
  // hashing, save/restore metadata and info-cache setup still occur.
  void Precompile(bool compile_discovered);
  // Compiles everything early precompilation discovers, if enabled.
  void PrecompileDiscoveredFunctions();
  // Compiles the CRT static initializers and their callees. Call after all
  // code patching and before the entry point runs.
  void PrecompileStaticInitializers();

 protected:
  std::unique_ptr<Function> CreateFunction(uint32_t address) override;

 private:
  struct GuestHostBridge {
    uint32_t original_size;
    std::array<uint8_t, 8> translation;
    // Native Task callback admission depends on these original native bytes.
    // Protect them against all subsequent bridge installations too.
    struct Dependency {
      uint32_t address = 0;
      uint32_t size = 0;
    };
    std::array<Dependency, 2> dependencies{};
  };
  enum class EntryWitness {
    kExport,
    kExportFullExtent,
    kDirectCall,
    kNativeTaskRegistration
  };
  bool InstallGuestFunctionBridge(
      uint32_t address, uint32_t size, std::string_view expected_sha256,
      GuestFunction::ExternHandler handler, Export* export_data,
      std::string_view host_name, EntryWitness entry_witness,
      const GuestTaskCallbackRegistration* task_registration = nullptr);
  std::map<uint32_t, GuestHostBridge> guest_host_bridges_;
  void PrecompileKnownFunctions();
  std::vector<uint32_t> FindStaticInitializers() const;
  bool IsCodeAddress(uint32_t address) const {
    return !(address & 3) && address >= low_address_ && address < high_address_;
  }
  std::vector<uint32_t> PreanalyzeCode();
  friend struct XexInfoCache;
  void ReadSecurityInfo();

  int ReadImage(const void* xex_addr, size_t xex_length, const uint8_t* key);
  bool AllocImage(uint32_t size);
  void ReleaseImage();
  int ReadImageUncompressed(const void* xex_addr, size_t xex_length);
  int ReadImageBasicCompressed(const void* xex_addr, size_t xex_length);
  int ReadImageCompressed(const void* xex_addr, size_t xex_length);

  int ReadPEHeaders();

  bool SetupLibraryImports(const std::string_view name,
                           const xex2_import_library* library);
  bool FindSaveRest();

  kernel::KernelState* kernel_state_ = nullptr;
  std::string name_;
  std::string path_;
  std::vector<uint8_t> xex_header_mem_;  // Holds the xex header
  std::vector<uint8_t> xexp_data_mem_;   // Holds XEXP patch data

  std::vector<ImportLibrary>
      import_libs_;  // pre-loaded import libraries for ease of use
  std::vector<std::string> missing_import_libs_;
  std::vector<PESection> pe_sections_;

  // XEX_HEADER_ALTERNATE_TITLE_IDS loaded into a safe std::vector
  std::vector<uint32_t> opt_alternate_title_ids_;

  uint8_t session_key_[0x10];
  bool is_dev_kit_ = false;
  bool image_allocated_ = false;  // Image memory at base_address_ is ours?

  bool loaded_ = false;         // Loaded into memory?
  bool finished_load_ = false;  // PE/imports/symbols/etc all loaded?
  std::shared_ptr<const GuestImportProvider> guest_import_provider_;

  uint32_t base_address_ = 0;
  uint32_t low_address_ = 0;
  uint32_t high_address_ = 0;

  XexFormat xex_format_ = kFormatUnknown;
  SecurityInfoContext security_info_ = {};

  uint8_t image_sha_bytes_[20];
  std::string image_sha_str_;
  XexInfoCache info_cache_;
};

// Decrypts a retail XEX2 image key, which only the hypervisor can on a console.
void DecryptXex2ImageKey(const uint8_t* encrypted_key, uint8_t* key);

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_XEX_MODULE_H_
