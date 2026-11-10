/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#include "xenia/base/managed_volume_authority.h"
#include "xenia/base/content_mutation_owner.h"
#include <algorithm>
#include <array>
#include <string_view>
#include "third_party/mbedtls/include/mbedtls/sha256.h"
#include "xenia/base/mutex.h"
namespace xe::filesystem {
namespace {
std::error_code Denied() {
  return std::make_error_code(std::errc::permission_denied);
}
std::string Key(std::string_view id, const std::filesystem::path& path,
                std::error_code& error) {
  const auto text = std::string(id) + "\n" + path.generic_string();
  std::array<uint8_t, 32> digest{};
  if (mbedtls_sha256(reinterpret_cast<const uint8_t*>(text.data()), text.size(),
                     digest.data(), 0)) {
    error = std::make_error_code(std::errc::io_error);
    return {};
  }
  constexpr char hex[] = "0123456789abcdef";
  std::string key;
  for (auto byte : digest) {
    key += hex[byte >> 4];
    key += hex[byte & 15];
  }
  return key;
}
std::shared_ptr<HostStorageRoot> Child(
    const std::shared_ptr<HostStorageRoot>& parent,
    const std::filesystem::path& name, bool& created, std::error_code& error,
    bool* denied_missing_creation = nullptr) {
  if (denied_missing_creation) {
    *denied_missing_creation = false;
  }
  auto root = parent->Subroot(name, false, error);
  if (!root && error == std::errc::no_such_file_or_directory) {
    error = parent->Create(name, true);
    if (denied_missing_creation &&
        (error == std::errc::permission_denied ||
         error == std::errc::read_only_file_system)) {
      *denied_missing_creation = true;
    }
    created = !error;
    if (!error || error == std::errc::file_exists) {
      root = parent->Subroot(name, false, error);
    }
  }
  return root;
}
std::span<const uint8_t> Bytes(const std::string& text) {
  return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}
struct Binding {
  std::string source, content, parent, public_root;
  uint64_t content_directory = 0, parent_directory = 0, private_directory = 0,
           public_directory = 0;
};
void Number(std::vector<uint8_t>& bytes, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    bytes.push_back(uint8_t(value >> shift));
  }
}
void Text(std::vector<uint8_t>& bytes, std::string_view value) {
  Number(bytes, value.size());
  bytes.insert(bytes.end(), value.begin(), value.end());
}
std::vector<uint8_t> EncodeBinding(const Binding& b, std::error_code& error) {
  std::vector<uint8_t> out{'X', 'M', 'V', 'O', 'L', 'B', '2', '\n'};
  Text(out, b.source);
  Text(out, b.content);
  Number(out, b.content_directory);
  Text(out, b.parent);
  Number(out, b.parent_directory);
  Number(out, b.private_directory);
  Text(out, b.public_root);
  Number(out, b.public_directory);
  std::array<uint8_t, 32> hash{};
  if (mbedtls_sha256(out.data(), out.size(), hash.data(), 0)) {
    error = std::make_error_code(std::errc::io_error);
    return {};
  }
  out.insert(out.end(), hash.begin(), hash.end());
  return out;
}
Binding DecodeBinding(std::span<const uint8_t> bytes, std::error_code& error) {
  Binding b;
  constexpr std::array<uint8_t, 8> magic{'X', 'M', 'V', 'O',
                                         'L', 'B', '2', '\n'};
  if (bytes.size() < 8 + 8 * 8 + 32 || bytes.size() > 16384 ||
      !std::equal(magic.begin(), magic.end(), bytes.begin())) {
    error = std::make_error_code(std::errc::bad_message);
    return b;
  }
  std::array<uint8_t, 32> hash{};
  if (mbedtls_sha256(bytes.data(), bytes.size() - 32, hash.data(), 0) ||
      !std::equal(hash.begin(), hash.end(), bytes.end() - 32)) {
    error = std::make_error_code(std::errc::bad_message);
    return b;
  }
  auto remaining = bytes.subspan(8, bytes.size() - 40);
  auto number = [&]() {
    if (remaining.size() < 8) {
      error = std::make_error_code(std::errc::bad_message);
      return uint64_t(0);
    }
    uint64_t value = 0;
    for (auto byte : remaining.first(8)) {
      value = (value << 8) | byte;
    }
    remaining = remaining.subspan(8);
    return value;
  };
  auto text = [&](size_t maximum) {
    const auto size = number();
    if (error || !size || size > maximum || size > remaining.size()) {
      error = std::make_error_code(std::errc::bad_message);
      return std::string{};
    }
    const auto value = remaining.first(size_t(size));
    remaining = remaining.subspan(size_t(size));
    if (std::find(value.begin(), value.end(), 0) != value.end()) {
      error = std::make_error_code(std::errc::bad_message);
      return std::string{};
    }
    return std::string(value.begin(), value.end());
  };
  b.source = text(256);
  b.content = text(4096);
  b.content_directory = number();
  b.parent = text(4096);
  b.parent_directory = number();
  b.private_directory = number();
  b.public_root = text(4096);
  b.public_directory = number();
  if (!remaining.empty() || !b.content_directory || !b.parent_directory ||
      !b.private_directory || !b.public_directory ||
      !std::filesystem::path(b.parent).is_absolute() ||
      !std::filesystem::path(b.public_root).is_absolute()) {
    error = std::make_error_code(std::errc::bad_message);
  }
  return b;
}
}  // namespace
std::shared_ptr<const ManagedStorageContext> ManagedStorageContext::Capture(
    std::shared_ptr<HostStorageRoot> data_parent,
    std::shared_ptr<const SnapshotRecoveryOwner> legacy_recovery,
    std::shared_ptr<HostStorageRoot> state_parent_override,
    std::error_code& error) {
  error.clear();
  if (xe::global_critical_region::is_held_by_current_thread()) {
    error = std::make_error_code(std::errc::operation_not_permitted);
    return {};
  }
  if (!data_parent || data_parent->read_only() ||
      data_parent->is_guest_private() ||
      !data_parent->native_filesystem_authority() || !legacy_recovery ||
      legacy_recovery->error() || !legacy_recovery->root() ||
      legacy_recovery->root()->path() !=
          data_parent->path() / SnapshotRecoveryOwner::kDirectory) {
    error = Denied();
    return {};
  }
  auto result =
      std::shared_ptr<ManagedStorageContext>(new ManagedStorageContext);
  result->data_parent_ = std::move(data_parent);
  result->legacy_recovery_ = std::move(legacy_recovery);
  result->state_parent_override_ = std::move(state_parent_override);
  error = result->CheckCurrent();
  return error ? nullptr : result;
}
std::error_code ManagedStorageContext::CheckCurrent() const {
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return std::make_error_code(std::errc::operation_not_permitted);
  }
  if (!data_parent_ || !legacy_recovery_ || !legacy_recovery_->root()) {
    return Denied();
  }
  if (auto error = data_parent_->Validate()) {
    return error;
  }
  if (auto error = legacy_recovery_->CheckTarget(*data_parent_)) {
    return error;
  }
  if (state_parent_override_) {
    if (state_parent_override_->read_only() ||
        state_parent_override_->is_guest_private() ||
        !state_parent_override_->native_filesystem_authority()) {
      return Denied();
    }
    if (auto error = state_parent_override_->Validate()) {
      return error;
    }
  }
  return {};
}
std::shared_ptr<const ManagedVolumeAuthority> ManagedVolumeAuthority::Open(
    Options options) {
  return OpenInternal(std::move(options), false);
}
std::shared_ptr<const ManagedVolumeAuthority>
ManagedVolumeAuthority::OpenInternal(Options options, bool inspect) {
  auto result =
      std::shared_ptr<ManagedVolumeAuthority>(new ManagedVolumeAuthority);
  auto& error = result->error_;
  if (xe::global_critical_region::is_held_by_current_thread()) {
    error = std::make_error_code(std::errc::operation_not_permitted);
    return result;
  }
  auto content_anchor = std::move(options.content);
  result->content_ =
      content_anchor ? content_anchor->Subroot({}, false, error) : nullptr;
  if (!result->content_ || options.source_id.empty() ||
      options.source_id.size() > 256 ||
      options.source_id.find('\0') != std::string::npos || !options.device_id ||
      result->content_->read_only() || result->content_->is_guest_private()) {
    error = Denied();
    return result;
  }
  // One native cooperating-owner lease covers the entire initialization,
  // including all refusal/partial results. No Space guard crosses these calls.
  if (!inspect) {
    result->source_lease_ = result->content_->AcquireIntentLease(error, true);
    if (!result->source_lease_) { return result; }
  }
  const auto source_identity = result->content_->IntentRootIdentity(error);
  if (error) {
    return result;
  }
  std::shared_ptr<const ManagedDeviceIdentity> inspected_identity;
  bool existing_marker = false;
  if (inspect) {
    // Explicit recovery resolves only an existing marker, without a guest
    // mount or an initial source lease that would self-conflict with drain.
    inspected_identity = ManagedDeviceIdentity::OpenInternal(
        result->content_, options.source_id, options.device_id, error,
        false, false);
    if (!inspected_identity ||
        (error && error != std::errc::operation_in_progress)) { return result; }
    existing_marker = true;
    error.clear();
  } else {
    existing_marker = ManagedDeviceIdentity::CheckExisting(
        result->content_, options.source_id, options.device_id, error);
    if (error) { return result; }
  }
  const auto key = Key(options.source_id, result->content_->path(), error);
  if (error) {
    return result;
  }
  const auto entries = content_anchor->ManagedVolumeEntries(key, error);
  if (error) {
    return result;
  }
  if ((entries && !(entries & 1)) || (entries && !existing_marker) ||
      (options.preserve_legacy_layout && entries)) {
    error = std::make_error_code(std::errc::state_not_recoverable);
    return result;
  }
  const bool reuse = (entries & 1) != 0;
  auto same_fs = [&](const std::shared_ptr<HostStorageRoot>& root) {
    if (!root) {
      error = Denied();
      return false;
    }
    const auto identity = root->IntentRootIdentity(error);
    if (!error && identity.filesystem != source_identity.filesystem) {
      error = std::make_error_code(std::errc::cross_device_link);
    }
    return !error;
  };
  auto mounted = result->content_->MountedFilesystemRoot(error);
  if (!mounted) {
    return result;
  }
  const auto mounted_identity = mounted->IntentRootIdentity(error);
  if (error) {
    return result;
  }
  const bool folder = source_identity.directory != mounted_identity.directory;
  bool known_legacy = false;
  std::shared_ptr<HostStorageRoot> binding_parent;
  if (!options.preserve_legacy_layout && existing_marker && !reuse) {
    // A pre-V1 source is not fresh ownership. Only an explicit known producer
    // bundle can preserve its existing redo/recovery/public locations.
    if (!options.existing_legacy) {
      error = Denied();
      return result;
    }
    const auto& old = *options.existing_legacy;
    if (!same_fs(old.private_parent) || !same_fs(old.public_root) ||
        !old.recovery || old.recovery->error() ||
        old.recovery->CheckTarget(*result->content_) ||
        (options.state_parent_override &&
         options.state_parent_override->path() != old.private_parent->path())) {
      if (!error) {
        error = Denied();
      }
      return result;
    }
    result->private_parent_ = old.private_parent;
    result->public_root_ = old.public_root;
    result->recovery_ = SnapshotRecoveryOwner::OpenExisting(old.private_parent);
    if (!result->recovery_ || result->recovery_->error() ||
        result->recovery_->root()->path() != old.recovery->root()->path()) {
      error = result->recovery_ ? result->recovery_->error() : Denied();
      if (!error) {
        error = Denied();
      }
      return result;
    }
    if (!result->public_root_->Subroot("Content", false, error)) {
      return result;
    }
    known_legacy = true;
  }
  if (options.preserve_legacy_layout || known_legacy) {
    if (!known_legacy) {
      result->private_parent_ = options.legacy_private_parent;
      result->public_root_ = options.legacy_public_root;
      result->recovery_ = options.legacy_recovery;
    }
    if (!same_fs(result->private_parent_) || !same_fs(result->public_root_) ||
        !result->recovery_ || result->recovery_->error()) {
      if (!error) {
        error = Denied();
      }
      return result;
    }
    error = result->recovery_->CheckTarget(*result->content_);
    if (error) {
      return result;
    }
    result->state_parent_ = result->private_parent_;
    result->content_->SetFormatEligibility(folder);
  } else {
    if (options.state_parent_override &&
        !same_fs(options.state_parent_override)) {
      return result;
    }
    auto parent =
        options.state_parent_override ? options.state_parent_override : mounted;
    if (!reuse) {
      // Absence at the explicit preferred parent is checked without creating
      // anything. Existing/unknown partial owners never become a fresh bundle.
      for (const auto& leaf : {".xenia-device-state-v1-" + key,
                               ".xenia-device-volume-v1-" + key}) {
        if (parent->Subroot(leaf, false, error)) {
          error = std::make_error_code(std::errc::state_not_recoverable);
          return result;
        }
        if (error != std::errc::no_such_file_or_directory) {
          return result;
        }
        error.clear();
      }
    }
    bool binding_created = false;
    const auto binding_leaf = ".xenia-device-binding-v1-" + key;
    auto binding_raw =
        reuse ? content_anchor->Subroot(binding_leaf, false, error)
              : Child(content_anchor, binding_leaf, binding_created, error);
    if (!binding_raw || (!reuse && !binding_created)) {
      if (!error) {
        error = std::make_error_code(std::errc::state_not_recoverable);
      }
      return result;
    }
    error = binding_raw->CheckStateContainer(false);
    if (error) {
      return result;
    }
    result->binding_root_ = binding_raw->ProtectFlat(error);
    if (!result->binding_root_) {
      return result;
    }
    Binding binding;
    if (reuse) {
      const auto stored =
          result->binding_root_->CaptureSnapshot("owner.v1", 16384, error);
      if (error || !stored || !stored->exists()) {
        if (!error) {
          error = std::make_error_code(std::errc::bad_message);
        }
        return result;
      }
      binding = DecodeBinding(stored->bytes(), error);
      if (error || binding.source != options.source_id ||
          binding.content != result->content_->path().generic_string() ||
          binding.content_directory != source_identity.directory) {
        if (!error) {
          error = std::make_error_code(std::errc::state_not_recoverable);
        }
        return result;
      }
      parent = HostStorageRoot::Open(binding.parent, false, error, 0);
      if (!parent || !same_fs(parent)) {
        return result;
      }
      const auto id = parent->IntentRootIdentity(error);
      if (error || parent->path().generic_string() != binding.parent ||
          id.directory != binding.parent_directory ||
          (options.state_parent_override &&
           options.state_parent_override->path() != parent->path())) {
        if (!error) {
          error = std::make_error_code(std::errc::state_not_recoverable);
        }
        return result;
      }
    }
    if (!same_fs(parent)) {
      return result;
    }
    const auto private_leaf = ".xenia-device-state-v1-" + key;
    bool created = false, denied_missing_creation = false;
    result->private_parent_ = reuse
                                  ? parent->Subroot(private_leaf, false, error)
                                  : Child(parent, private_leaf, created, error,
                                          &denied_missing_creation);
    if (!result->private_parent_ && !reuse && !options.state_parent_override &&
        denied_missing_creation) {
      parent = content_anchor;
      created = false;
      result->private_parent_ = Child(parent, private_leaf, created, error);
    }
    binding_parent = parent;
    result->state_parent_ = parent;
    if (!result->private_parent_ || (!reuse && !created)) {
      if (!error) {
        error = std::make_error_code(std::errc::state_not_recoverable);
      }
      return result;
    }
    error = result->private_parent_->CheckStateContainer(false);
    if (error) {
      return result;
    }
    const auto private_identity =
        result->private_parent_->IntentRootIdentity(error);
    if (error) {
      return result;
    }
    if (reuse && private_identity.directory != binding.private_directory) {
      error = std::make_error_code(std::errc::state_not_recoverable);
      return result;
    }
    bool public_created = false;
    const auto public_leaf = ".xenia-device-volume-v1-" + key;
    result->public_root_ =
        options.legacy_public_root ? options.legacy_public_root
        : reuse ? parent->Subroot(public_leaf, false, error)
                : Child(parent, public_leaf, public_created, error);
    if (!result->public_root_ || !same_fs(result->public_root_)) {
      return result;
    }
    if (!options.legacy_public_root) {
      error = result->public_root_->CheckStateContainer(true);
      if (error) {
        return result;
      }
      if (!reuse && !public_created) {
        error = std::make_error_code(std::errc::state_not_recoverable);
        return result;
      }
    }
    const auto public_identity =
        result->public_root_->IntentRootIdentity(error);
    if (error ||
        (reuse && (public_identity.directory != binding.public_directory ||
                   result->public_root_->path().generic_string() !=
                       binding.public_root))) {
      if (!error) {
        error = std::make_error_code(std::errc::state_not_recoverable);
      }
      return result;
    }
    std::vector<std::shared_ptr<const HostStorageRoot>> namespaces;
    if (!options.legacy_public_root) {
      namespaces.push_back(result->public_root_);
    }
    result->projection_ = result->content_->InstallProjection(
        {result->binding_root_, result->private_parent_}, std::move(namespaces),
        folder, error);
    if (!result->projection_) {
      return result;
    }
    const std::string marker =
        "Xenia managed-volume state\nversion=1\nsource=" + options.source_id +
        "\ncontent=" + result->content_->path().generic_string() +
        "\ndirectory=" + std::to_string(source_identity.directory) +
        "\nstate-directory=" + std::to_string(private_identity.directory) +
        "\n";
    if (!reuse) {
      error = parent->FlushDirectory();
      if (error) {
        return result;
      }
      const auto written =
          result->private_parent_->WriteNewFile("owner.v1", Bytes(marker));
      error = written.error;
      if (!error && written.bytes != marker.size()) {
        error = std::make_error_code(std::errc::io_error);
      }
      if (error) {
        return result;
      }
    }
    const auto observed =
        result->private_parent_->CaptureSnapshot("owner.v1", 8192, error);
    if (error || !observed || !observed->exists() ||
        !std::equal(observed->bytes().begin(), observed->bytes().end(),
                    Bytes(marker).begin(), Bytes(marker).end())) {
      if (!error) {
        error = std::make_error_code(std::errc::bad_message);
      }
      return result;
    }
    auto content_shell = result->public_root_->Subroot("Content", false, error);
    if (!content_shell && !reuse &&
        error == std::errc::no_such_file_or_directory) {
      error = result->public_root_->Create("Content", true);
      if (!error) {
        content_shell = result->public_root_->Subroot("Content", false, error);
      }
    }
    if (!content_shell) {
      return result;
    }
    result->recovery_ =
        reuse ? SnapshotRecoveryOwner::OpenExisting(result->private_parent_)
              : SnapshotRecoveryOwner::Open(result->private_parent_);
    if (!result->recovery_ || result->recovery_->error()) {
      error = result->recovery_ ? result->recovery_->error() : Denied();
      return result;
    }
  }
  // Public guest generation is separately disposable; its actual authority
  // must survive an owned format without an arbitrary path reopen.
  result->public_authority_ = result->public_root_;
  if (inspect) {
    result->identity_ = std::move(inspected_identity);
    error = std::make_error_code(std::errc::operation_in_progress);
    return result;  // No intents, new epoch, public guest view or readiness.
  }
  result->public_root_ = result->public_root_->IndependentNamespaceView(error);
  if (!result->public_root_) {
    return result;
  }
  if (result->projection_) {
    error = result->public_root_->AttachPublicProjection(*result->content_);
    if (error) {
      return result;
    }
  }
  result->public_root_ = result->public_root_->GuestView(error);
  if (!result->public_root_) {
    return result;
  }
  result->identity_ =
      existing_marker
          ? ManagedDeviceIdentity::OpenExisting(
                result->content_, options.source_id, options.device_id, error)
          : ManagedDeviceIdentity::Open(result->content_, options.source_id,
                                        options.device_id, error);
  if (!result->identity_ || error) {
    return result;
  }
  result->intents_ = ProfileIntentOwner::OpenInternal(
      result->private_parent_, result->identity_,
      !existing_marker && !reuse && !known_legacy, result->source_lease_);
  if (!result->intents_ || result->intents_->error()) {
    error = result->intents_ ? result->intents_->error() : Denied();
    return result;
  }
  result->mutations_ = ContentMutationOwner::OpenInternal(
      result->content_, result->identity_, result->private_parent_,
      result->recovery_, result->source_lease_, true);
  if (!result->mutations_ || result->mutations_->error()) {
    error = result->mutations_ ? result->mutations_->error() : Denied();
    return result;
  }
  if (!options.preserve_legacy_layout && !known_legacy && !reuse) {
    const auto private_identity =
        result->private_parent_->IntentRootIdentity(error);
    if (error) {
      return result;
    }
    const auto public_identity =
        result->public_root_->IntentRootIdentity(error);
    if (error) {
      return result;
    }
    const auto parent_identity = binding_parent->IntentRootIdentity(error);
    if (error) {
      return result;
    }
    const auto wire = EncodeBinding(
        {options.source_id, result->content_->path().generic_string(),
         binding_parent->path().generic_string(),
         result->public_root_->path().generic_string(),
         source_identity.directory, parent_identity.directory,
         private_identity.directory, public_identity.directory},
        error);
    if (error) {
      return result;
    }
    error = content_anchor->FlushDirectory();
    if (error) {
      return result;
    }
    // Final commit: every identity, source lease, recovery, intent and public
    // view is already ready. A partial directory never counts as committed.
    const auto written = result->binding_root_->WriteNewFile("owner.v1", wire);
    error = written.error;
    if (!error && written.bytes != wire.size()) {
      error = std::make_error_code(std::errc::io_error);
    }
  }
  return result;
}
std::array<uint8_t, 32> ManagedVolumeAuthority::FormatBindingDigest(
    const ManagedVolumeAuthority& volume, std::error_code& error) {
  error.clear();
  std::array<uint8_t, 32> digest{};
  if (!volume.content_ || !volume.private_parent_ || !volume.public_authority_ ||
      !volume.state_parent_ || !volume.identity_ || !volume.recovery_ ||
      volume.recovery_->error() || !volume.recovery_->root() ||
      volume.identity_->source() != volume.content_) {
    error = Denied(); return digest;
  }
  std::vector<uint8_t> wire{'X','M','V','F','B','1','\n'};
  Text(wire, volume.identity_->source_id());
  Number(wire, volume.identity_->device_id());
  wire.insert(wire.end(), volume.identity_->uuid().begin(),
              volume.identity_->uuid().end());
  std::vector<HostStorageRootIdentity> roots;
  for (const auto& root : {volume.content_, volume.private_parent_,
                           volume.public_authority_, volume.state_parent_,
                           volume.binding_root_, volume.recovery_->root()}) {
    if (!root) {
      Text(wire, ""); Number(wire, 0); roots.push_back({}); continue;
    }
    const auto identity = root->IntentRootIdentity(error);
    if (error) { return digest; }
    Text(wire, root->path().generic_string());
    Number(wire, identity.directory);  // Reboot-stable ID, not live mount/st_dev.
    roots.push_back(identity);
  }
  if (volume.binding_root_) {
    const auto stored = volume.binding_root_->CaptureSnapshot("owner.v1", 16384,
                                                              error);
    if (error || !stored || !stored->exists()) {
      if (!error) { error = std::make_error_code(std::errc::bad_message); }
      return digest;
    }
    const auto binding = DecodeBinding(stored->bytes(), error);
    if (error || binding.source != volume.identity_->source_id() ||
        binding.content != volume.content_->path().generic_string() ||
        binding.content_directory != roots[0].directory ||
        binding.private_directory != roots[1].directory ||
        binding.public_directory != roots[2].directory ||
        binding.public_root != volume.public_authority_->path().generic_string() ||
        binding.parent != volume.state_parent_->path().generic_string() ||
        binding.parent_directory != roots[3].directory) {
      if (!error) { error = std::make_error_code(std::errc::state_not_recoverable); }
      return digest;
    }
    Number(wire, stored->bytes().size());
    wire.insert(wire.end(), stored->bytes().begin(), stored->bytes().end());
  } else {
    Number(wire, 0);  // Explicit existing primary/legacy owner configuration.
  }
  if (mbedtls_sha256(wire.data(), wire.size(), digest.data(), 0)) {
    error = std::make_error_code(std::errc::io_error);
  }
  return digest;
}
std::unique_ptr<ManagedVolumeFormatHandoff>
ManagedVolumeAuthority::CaptureFormatHandoff() const {
  auto ticket = std::unique_ptr<ManagedVolumeFormatHandoff>(
      new ManagedVolumeFormatHandoff);
  ticket->identity_ = identity_;
  ticket->source_id_ = identity_ ? identity_->source_id() : std::string{};
  if (xe::global_critical_region::is_held_by_current_thread()) {
    ticket->error_ = std::make_error_code(std::errc::operation_not_permitted);
    return ticket;
  }
  if (!format_eligible()) {
    ticket->error_ = std::make_error_code(std::errc::operation_not_supported);
    return ticket;
  }
  ticket->binding_ = FormatBindingDigest(*this, ticket->error_);
  if (ticket->error_) { return ticket; }
  ticket->content_ = content_;
  ticket->private_parent_ = private_parent_;
  ticket->public_authority_ = public_authority_;
  ticket->state_parent_ = state_parent_;
  ticket->binding_root_ = binding_root_;
  ticket->old_public_view_ = public_root_;
  ticket->recovery_ = recovery_;
  ticket->projection_ = projection_;
  ticket->previous_intents_ = intents_;
  ticket->previous_mutations_ = mutations_;  // Weak; never blocks actual drain.
  return ticket;
}
std::unique_ptr<ManagedVolumeFormatHandoff>
ManagedVolumeAuthority::InspectFormatRecovery(Options options) {
  auto ticket = std::unique_ptr<ManagedVolumeFormatHandoff>(
      new ManagedVolumeFormatHandoff);
  const auto inspected = OpenInternal(std::move(options), true);
  if (!inspected || inspected->error_ != std::errc::operation_in_progress ||
      !inspected->identity_ || !inspected->content_->format_eligible()) {
    ticket->error_ = inspected && inspected->error_ ? inspected->error_
        : std::make_error_code(std::errc::operation_not_permitted);
    return ticket;
  }
  ticket->binding_ = FormatBindingDigest(*inspected, ticket->error_);
  if (ticket->error_) { return ticket; }
  ticket->identity_ = inspected->identity_;
  ticket->source_id_ = ticket->identity_->source_id();
  ticket->content_ = inspected->content_;
  ticket->private_parent_ = inspected->private_parent_;
  ticket->public_authority_ = inspected->public_authority_;
  ticket->state_parent_ = inspected->state_parent_;
  ticket->binding_root_ = inspected->binding_root_;
  ticket->recovery_ = inspected->recovery_;
  ticket->projection_ = inspected->projection_;
  ticket->operation_ = ManagedDeviceIdentity::InspectHandoff(
      ticket->identity_, ticket->binding_);
  ticket->error_ = ticket->operation_->error;
  return ticket;
}
ManagedDeviceFormatReceipt ManagedVolumeAuthority::ExecuteFormat(
    ManagedVolumeFormatHandoff& ticket, const std::atomic<bool>* cancellation) {
  ManagedDeviceFormatReceipt receipt;
  if (xe::global_critical_region::is_held_by_current_thread()) {
    receipt.error = std::make_error_code(std::errc::operation_not_permitted);
    return receipt;
  }
  if (ticket.error_ || !ticket.identity_ || !ticket.content_) {
    receipt.error = ticket.error_ ? ticket.error_ : Denied(); return receipt;
  }
  if (!ticket.operation_ && ticket.old_public_view_ &&
      !ticket.old_public_view_->retiring()) {
    receipt.error = std::make_error_code(std::errc::device_or_resource_busy);
    ticket.error_ = receipt.error; return receipt;
  }
  if (!ticket.operation_ &&
      (ticket.canceled_.load(std::memory_order_acquire) ||
       (cancellation && cancellation->load(std::memory_order_acquire)))) {
    receipt.error = std::make_error_code(std::errc::operation_canceled);
    ticket.error_ = receipt.error; return receipt;
  }
  std::error_code binding_error;
  const auto observed_binding = FormatTicketDigest(ticket, binding_error);
  if (binding_error || observed_binding != ticket.binding_) {
    receipt.error = binding_error ? binding_error
        : std::make_error_code(std::errc::state_not_recoverable);
    ticket.error_ = receipt.error; return receipt;
  }
  if (!ticket.operation_) {
    // Real source exclusion is acquired here, after coordinator drops all old
    // MVA/intents holders. No weak-owner observation claims drain.
    ticket.operation_ = PrepareFormatHandoff(ticket, cancellation);
  }
  receipt = ManagedDeviceIdentity::ExecuteHandoff(*ticket.operation_);
  ticket.error_ = receipt.error;
  // Private new-epoch creator must not escape before actual owners and both
  // durable handoff phases exist. FromCompletedFormat is the sole exposure.
  receipt.current.reset();
  return receipt;
}
std::unique_ptr<ManagedDeviceIdentity::Handoff>
ManagedVolumeAuthority::PrepareFormatHandoff(
    ManagedVolumeFormatHandoff& ticket, const std::atomic<bool>* external) {
  return ManagedDeviceIdentity::PrepareHandoff(
      ticket.identity_, ticket.binding_, &ticket.canceled_, external);
}
std::array<uint8_t, 32> ManagedVolumeAuthority::FormatTicketDigest(
    const ManagedVolumeFormatHandoff& ticket, std::error_code& error) {
  ManagedVolumeAuthority view;
  view.identity_ = ticket.operation_ ? ticket.operation_->identity : ticket.identity_;
  view.content_ = view.identity_
      ? std::const_pointer_cast<HostStorageRoot>(view.identity_->source()) : nullptr;
  view.private_parent_ = ticket.private_parent_;
  view.public_authority_ = ticket.public_authority_;
  view.state_parent_ = ticket.state_parent_;
  view.binding_root_ = ticket.binding_root_;
  view.recovery_ = ticket.recovery_;
  return FormatBindingDigest(view, error);
}
std::shared_ptr<ManagedVolumeAuthority> ManagedVolumeAuthority::BuildFormatVolume(
    ManagedVolumeFormatHandoff& ticket, bool completed) {
  auto result = std::shared_ptr<ManagedVolumeAuthority>(new ManagedVolumeAuthority);
  auto& error = result->error_;
  if (xe::global_critical_region::is_held_by_current_thread()) {
    error = std::make_error_code(std::errc::operation_not_permitted); return result;
  }
  if (!ticket.identity_ || (completed && (!ticket.operation_ ||
      !ticket.operation_->creator_ready || ticket.operation_->error))) {
    error = Denied(); return result;
  }
  if (!completed && ticket.operation_ && ticket.operation_->phase) {
    error = std::make_error_code(std::errc::operation_in_progress); return result;
  }
  if (!completed && (!ticket.old_public_view_ ||
                     !ticket.old_public_view_->retiring())) {
    error = std::make_error_code(std::errc::device_or_resource_busy); return result;
  }
  result->identity_ = completed ? ticket.operation_->identity : ticket.identity_;
  error = result->identity_->CheckCurrent();
  if (error) { return result; }
  result->content_ = std::const_pointer_cast<HostStorageRoot>(
      result->identity_->source());
  result->private_parent_ = ticket.private_parent_;
  result->public_authority_ = ticket.public_authority_;
  result->state_parent_ = ticket.state_parent_;
  result->binding_root_ = ticket.binding_root_;
  result->recovery_ = ticket.recovery_;
  result->projection_ = completed && ticket.operation_->projection
                           ? ticket.operation_->projection : ticket.projection_;
  const auto current_binding = FormatBindingDigest(*result, error);
  if (error || current_binding != ticket.binding_) {
    if (!error) { error = std::make_error_code(std::errc::state_not_recoverable); }
    return result;
  }
  std::shared_ptr<const ProfileIntentOwner> previous;
  if (completed) {
    result->source_lease_ = ticket.operation_->source_lease;
  } else {
    // This is restoration only, not drain admission. A genuine existing
    // intent owner may share its unchanged-epoch source lease after refusal.
    previous = ticket.previous_intents_.lock();
    if (previous && (!previous->error() &&
        previous->managed_identity() == result->identity_ &&
        previous->source() == result->content_ &&
        previous->private_parent() == result->private_parent_)) {
      result->source_lease_ = previous->source_lease_;
    } else {
      previous.reset();
      if (ticket.operation_ && ticket.operation_->source_lease) {
        result->source_lease_ = ticket.operation_->source_lease;
      } else {
        result->source_lease_ = result->content_->AcquireIntentLease(error, true);
      }
    }
  }
  if (!result->source_lease_ || error ||
      !result->content_->IntentLeaseMatches(*result->source_lease_)) {
    if (!error) { error = Denied(); } return result;
  }
  error = result->recovery_->CheckTarget(*result->content_);
  if (error) { return result; }
  result->public_root_ = result->public_authority_->IndependentNamespaceView(error);
  if (!result->public_root_) { return result; }
  if (result->projection_) {
    error = result->public_root_->AttachRetainedPublicProjection(
        *result->content_, *result->projection_);
    if (error) { return result; }
  }
  result->public_root_ = result->public_root_->GuestView(error);
  if (!result->public_root_) { return result; }
  if (previous) {
    result->intents_ = std::move(previous);
  } else {
    // Later durable phases certify that owners already existed. They can be
    // validated/reopened, never recreated from marker bytes alone.
    const bool create = completed && ticket.operation_->phase == 1;
    result->intents_ = ProfileIntentOwner::OpenInternal(
        result->private_parent_, result->identity_, create, result->source_lease_);
  }
  if (!result->intents_ || result->intents_->error()) {
    error = result->intents_ ? result->intents_->error() : Denied();
  }
  if (!error) {
    auto previous_mutations = completed ? nullptr : ticket.previous_mutations_.lock();
    if (previous_mutations && previous_mutations->identity_ == result->identity_ &&
        previous_mutations->content_ == result->content_ &&
        previous_mutations->parent_ == result->private_parent_) {
      result->mutations_ = std::move(previous_mutations);
    } else {
      // Format preserves private mutation history. Never invent a new owner
      // during later recovery phases or replay an unfinished older epoch.
      result->mutations_ = ContentMutationOwner::OpenInternal(
          result->content_, result->identity_, result->private_parent_,
          result->recovery_, result->source_lease_, false);
    }
    if (!result->mutations_ || !result->mutations_->ready()) {
      error = result->mutations_ ? result->mutations_->error() : Denied();
      if (!error) { error = std::make_error_code(std::errc::operation_in_progress); }
    }
  }
  return result;
}
std::shared_ptr<const ManagedVolumeAuthority>
ManagedVolumeAuthority::RestoreUnchangedFormat(ManagedVolumeFormatHandoff& ticket) {
  // Caller has not published Prepared; all data/identity bytes must still be
  // the original epoch. Restoration makes new views, never reactivates old.
  ticket.partial_.reset();
  auto restored = BuildFormatVolume(ticket, false);
  ticket.error_ = restored->error_;
  if (ticket.error_) {
    if (restored->public_root_) { restored->public_root_->Retire(); }
    ticket.partial_ = restored;
  } else {
    ticket.operation_.reset();
    ticket.previous_intents_ = restored->intents_;
    ticket.previous_mutations_ = restored->mutations_;
  }
  return restored;
}
std::shared_ptr<const ManagedVolumeAuthority>
ManagedVolumeAuthority::FromCompletedFormat(ManagedVolumeFormatHandoff& ticket) {
  auto result = BuildFormatVolume(ticket, true);
  if (!result->error_) {
    if (ticket.operation_->phase == 1) {
      result->error_ = ManagedDeviceIdentity::AdvanceHandoff(*ticket.operation_,
                                                            false);
    }
    if (!result->error_ && ticket.operation_->phase == 2) {
      result->error_ = ManagedDeviceIdentity::AdvanceHandoff(*ticket.operation_,
                                                            true);
    }
    if (!result->error_ && ticket.operation_->phase != 3) {
      result->error_ = std::make_error_code(std::errc::state_not_recoverable);
    }
  }
  ticket.error_ = result->error_;
  if (ticket.error_) {
    if (result->public_root_) { result->public_root_->Retire(); }
    ticket.partial_ = result;
  } else {
    ticket.partial_.reset();
    ticket.previous_intents_ = result->intents_;
    ticket.previous_mutations_ = result->mutations_;
    ticket.identity_ = result->identity_;
    ticket.content_ = result->content_;
    ticket.projection_ = result->projection_;
    // The ready bundle now owns the same source lease. Completed operation
    // metadata does not keep an extra format reservation alive indefinitely.
    ticket.operation_.reset();
  }
  return result;
}
std::shared_ptr<const ManagedVolumeAuthority> ManagedVolumeAuthority::ResumeFormat(
    ManagedVolumeFormatHandoff& ticket) {
  if (!ticket.operation_ ||
      xe::global_critical_region::is_held_by_current_thread()) {
    auto failed = std::shared_ptr<ManagedVolumeAuthority>(new ManagedVolumeAuthority);
    failed->error_ = std::make_error_code(std::errc::operation_not_permitted);
    return failed;
  }
  if (ticket.partial_) {
    if (ticket.partial_->public_root_) { ticket.partial_->public_root_->Retire(); }
    ticket.partial_.reset();  // Native/private lease destruction outside locks.
  }
  ticket.error_ = ManagedDeviceIdentity::ReopenHandoffForResume(*ticket.operation_);
  if (!ticket.error_) {
    std::error_code error;
    const auto observed = FormatTicketDigest(ticket, error);
    if (error || observed != ticket.binding_) {
      ticket.error_ = error ? error
          : std::make_error_code(std::errc::state_not_recoverable);
    }
  }
  if (!ticket.error_) {
    const auto receipt = ExecuteFormat(ticket);
    ticket.error_ = receipt.error;
  }
  if (!ticket.error_) { return FromCompletedFormat(ticket); }
  auto failed = std::shared_ptr<ManagedVolumeAuthority>(new ManagedVolumeAuthority);
  failed->error_ = ticket.error_;
  return failed;
}
bool ManagedVolumeAuthority::profile_writable() const {
  return !error_ && identity_ && intents_ && recovery_ && mutations_ &&
         mutations_->ready() &&
         !identity_->retiring() && !intents_->error() && !recovery_->error();
}
bool ManagedVolumeAuthority::format_eligible() const {
  return profile_writable() && mutations_->idle() && content_->format_eligible();
}
}  // namespace xe::filesystem
