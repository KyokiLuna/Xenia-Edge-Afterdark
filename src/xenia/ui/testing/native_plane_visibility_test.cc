/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for details.
 */
#include <atomic>
#include <future>
#include <limits>
#include <thread>
#include "third_party/catch/include/catch.hpp"
#include "xenia/ui/native_ui_plane_handoff.h"
#include "xenia/ui/presenter.h"

namespace xe::ui::test {
namespace {
// Exercises real publication/selection ownership without a Window or GPU.
class MetadataPresenter final : public Presenter {
 public:
  MetadataPresenter() : Presenter({}) {}
  using Presenter::SnapshotNativeUiPlaneVisibility;
  using Presenter::SealNativeUiPlanePublication;
  Surface::TypeFlags GetSupportedSurfaceTypes() const override { return 0; }
  bool CaptureGuestOutput(RawImage&) override { return false; }
 protected:
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      Surface&, uint32_t, uint32_t, bool, bool&) override {
    return SurfacePaintConnectResult::kFailure;
  }
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override {}
  bool RefreshGuestOutputImpl(uint32_t, uint32_t, uint32_t,
                              std::function<bool(GuestOutputRefreshContext&)>,
                              bool&) override {
    return false;
  }
  PaintResult PaintAndPresentImpl(bool) override {
    return PaintResult::kNotPresented;
  }
};
class MetadataAuthority final : public NativeUiPlaneAuthority {
 public:
  explicit MetadataAuthority(uint64_t foreground = 3)
      : owner_{{1, 2, 3, 4}, 5, foreground} {}
  NativeUiPlaneOwner owner() const noexcept override { return owner_; }
  bool IsLive() const noexcept override { return live.load(); }
  std::atomic<bool> live{true};

 private:
  NativeUiPlaneOwner owner_;
};
// The real producer may advance before host delivery. This isolated authority
// models that timing only; it is never a native event or GPU completion.
class MetadataEpoch final : public NativeUiPlaneImageEpoch {
 public:
  MetadataEpoch(std::shared_ptr<std::atomic<uint64_t>> producer, uint64_t epoch)
      : producer_(std::move(producer)), epoch_(epoch) {}
  bool IsLive() const noexcept override {
    return !IsRevoked() && producer_->load() == epoch_;
  }
  bool IsRevoked() const noexcept override { return revoked.load(); }
  std::atomic<bool> revoked{false};

 private:
  std::shared_ptr<std::atomic<uint64_t>> producer_;
  uint64_t epoch_;
};
gpu::NativeFrameSubmissionIdentity Image(const MetadataAuthority& authority,
                                         uint64_t serial) {
  return {authority.owner().allocation_owner, serial};
}
struct Fixture {
  std::shared_ptr<MetadataAuthority> authority =
      std::make_shared<MetadataAuthority>();
  std::shared_ptr<std::atomic<uint64_t>> producer =
      std::make_shared<std::atomic<uint64_t>>(1);
  std::shared_ptr<MetadataEpoch> epoch =
      std::make_shared<MetadataEpoch>(producer, 1);
  std::shared_ptr<const NativeUiPlaneImagePermission> Permission(
      uint64_t serial,
      std::optional<gpu::NativeOverlayPresentation> presentation = std::nullopt) {
    return std::make_shared<NativeUiPlaneImagePermission>(
        Image(*authority, serial), epoch, std::move(presentation));
  }
  NativeUiPlaneImageReference Reference(
      uint64_t serial,
      std::shared_ptr<void> payload = std::make_shared<int>(1)) {
    return {Image(*authority, serial), authority,
            authority->owner().backend_epoch, std::move(payload)};
  }
  NativeUiPlaneVisibility Visible(
      uint64_t revision,
      std::shared_ptr<const NativeUiPlaneImagePermission> permission) {
    return {authority, revision, true, std::move(permission)};
  }
};
NativeUiPlaneImageSelection::Selection Paint(
    NativeUiPlaneImageSelection& selector,
    const NativeUiPlaneVisibility& visible,
    const NativeUiPlaneImageReference& desired = {},
    const NativeUiPlaneImageReference& received = {}) {
  auto probe = selector.Prepare(visible);
  probe.CheckLiveness();  // Production performs this outside its backend lock.
  return selector.Select(probe, visible, desired, received);
}
TEST_CASE("Nonmodal permission names an exact source and immutable epoch",
          "[native_plane][visibility]") {
  Fixture f;
  MetadataPresenter presenter;
  const auto first = f.Permission(11);
  REQUIRE(presenter.BindNativeUiPlaneAuthority(f.authority));
  REQUIRE(presenter.SetNativeUiPlaneVisibility(f.authority, 1, true, first));
  CHECK(presenter.SnapshotNativeUiPlaneVisibility().image_permission == first);
  // Another token cannot change an existing revision, even for the same source.
  CHECK_FALSE(presenter.SetNativeUiPlaneVisibility(f.authority, 1, true,
                                                   f.Permission(11)));
  REQUIRE(presenter.SetNativeUiPlaneVisibility(f.authority, 2, false, first));
  CHECK_FALSE(presenter.SnapshotNativeUiPlaneVisibility().image_permission);
  auto foreign = Image(*f.authority, 12);
  ++foreign.owner.kernel_epoch;
  CHECK_FALSE(presenter.SetNativeUiPlaneVisibility(
      f.authority, 3, true,
      std::make_shared<NativeUiPlaneImagePermission>(foreign, f.epoch)));
  CHECK_FALSE(presenter.SetNativeUiPlaneVisibility(
      f.authority, 3, true,
      std::make_shared<NativeUiPlaneImagePermission>(
          gpu::NativeFrameSubmissionIdentity{}, f.epoch)));
}
TEST_CASE("Native clear wins a delayed permission publication",
          "[native_plane][visibility][lifetime]") {
  Fixture f;
  MetadataPresenter presenter;
  const auto permission = f.Permission(9);
  REQUIRE(presenter.BindNativeUiPlaneAuthority(f.authority));
  REQUIRE(
      presenter.SetNativeUiPlaneVisibility(f.authority, 1, true, permission));
  std::promise<void> ready;
  auto cleared = ready.get_future();
  bool accepted = true;
  std::thread delayed([&] {
    cleared.wait();
    accepted =
        presenter.SetNativeUiPlaneVisibility(f.authority, 1, true, permission);
  });
  f.epoch->revoked = true;
  const bool hidden =
      presenter.SetNativeUiPlaneVisibility(f.authority, 2, false);
  ready.set_value();
  delayed.join();
  REQUIRE(hidden);
  CHECK_FALSE(accepted);
  CHECK_FALSE(presenter.SnapshotNativeUiPlaneVisibility().visible);
}
TEST_CASE("Withdrawn and sealed plane authority cannot regain permission",
          "[native_plane][visibility][lifetime]") {
  Fixture f;
  MetadataPresenter presenter;
  REQUIRE(presenter.BindNativeUiPlaneAuthority(f.authority));
  REQUIRE(presenter.SetNativeUiPlaneVisibility(f.authority, 1, true,
                                               f.Permission(1)));
  REQUIRE(presenter.WithdrawNativeUiPlaneAuthority(f.authority));
  f.authority->live = false;
  auto next = std::make_shared<MetadataAuthority>(4);
  REQUIRE(presenter.BindNativeUiPlaneAuthority(next));
  CHECK_FALSE(presenter.SetNativeUiPlaneVisibility(f.authority, 2, true,
                                                   f.Permission(2)));
  REQUIRE(presenter.SetNativeUiPlaneVisibility(next, 1, true));
  presenter.SealNativeUiPlanePublication();
  CHECK_FALSE(presenter.SetNativeUiPlaneVisibility(next, 2, true));
  CHECK_FALSE(presenter.BindNativeUiPlaneAuthority(next));
  CHECK_FALSE(presenter.SnapshotNativeUiPlaneVisibility().authority);
}
TEST_CASE("Producer advance before host delivery revokes publication and paint",
          "[native_plane][selection][epoch]") {
  Fixture f;
  MetadataPresenter presenter;
  NativeUiPlaneImageSelection selector;
  const auto token = f.Permission(10);
  const auto frame = f.Reference(10);
  REQUIRE(presenter.BindNativeUiPlaneAuthority(f.authority));
  REQUIRE(presenter.SetNativeUiPlaneVisibility(f.authority, 1, true, token));
  const auto before = presenter.SnapshotNativeUiPlaneVisibility();
  REQUIRE(Paint(selector, before, frame));
  f.producer->store(2);  // No host delivery or visibility revision change.
  const auto undelivered = presenter.SnapshotNativeUiPlaneVisibility();
  REQUIRE(undelivered.revision == before.revision);
  CHECK_FALSE(Paint(selector, undelivered, frame));
  CHECK_FALSE(
      presenter.SetNativeUiPlaneVisibility(f.authority, 2, true, token));
  CHECK_FALSE(selector.retained_publication());
}
TEST_CASE(
    "New received image waits for qualification without blanking old toast",
    "[native_plane][selection][ordering]") {
  Fixture f;
  NativeUiPlaneImageSelection selector;
  const auto n = f.Reference(10), next = f.Reference(11),
             guide = f.Reference(2);
  const auto current = f.Visible(1, f.Permission(10));
  REQUIRE(Paint(selector, current, n).reference.publication == n.publication);
  // Receipt/status does not promote the next frame or expose a resident Guide.
  const auto waiting = Paint(selector, current, {}, next);
  REQUIRE(waiting.reference.publication == n.publication);
  auto status_probe = selector.Prepare(current);
  status_probe.CheckLiveness();
  CHECK(selector.Current(status_probe, current, next).reference.publication ==
        n.publication);
  NativeUiPlaneImageSelection empty;
  CHECK_FALSE(Paint(empty, current, {}, guide));
  const auto approved = f.Visible(2, f.Permission(11));
  auto approved_status = selector.Prepare(approved);
  approved_status.CheckLiveness();
  // Status alone cannot promote the available but unselected next source.
  CHECK(
      selector.Current(approved_status, approved, next).reference.publication ==
      n.publication);
  CHECK(Paint(selector, approved, next).reference.publication ==
        next.publication);
}
TEST_CASE(
    "Desired image may arrive later and intermediate sources may be skipped",
    "[native_plane][selection][ordering]") {
  Fixture f;
  NativeUiPlaneImageSelection selector;
  const auto n = f.Reference(10), last = f.Reference(12);
  REQUIRE(Paint(selector, f.Visible(1, f.Permission(10)), n));
  const auto desired11 = f.Visible(2, f.Permission(11));
  CHECK(Paint(selector, desired11).reference.publication == n.publication);
  const auto desired12 = f.Visible(3, f.Permission(12));
  CHECK(Paint(selector, desired12).reference.publication == n.publication);
  CHECK(Paint(selector, desired12, last).reference.publication ==
        last.publication);
}
TEST_CASE("Clear new epoch and modal mode cannot reuse old nonmodal pixels",
          "[native_plane][selection][epoch]") {
  Fixture f;
  NativeUiPlaneImageSelection selector;
  const auto n = f.Reference(10), guide = f.Reference(20);
  REQUIRE(Paint(selector, f.Visible(1, f.Permission(10)), n));
  f.epoch->revoked = true;
  // A rapid new request may replace the hidden revision before any paint.
  // The still-retained old selection must never serve the new epoch.
  f.producer->store(2);
  f.epoch = std::make_shared<MetadataEpoch>(f.producer, 2);
  const auto fresh = f.Visible(3, f.Permission(11));
  CHECK_FALSE(Paint(selector, fresh, {}, n));
  const auto modal = f.Visible(4, {});
  CHECK(Paint(selector, modal, {}, guide).reference.publication ==
        guide.publication);
  // A later nonmodal request cannot fallback to the modal Guide image.
  CHECK_FALSE(Paint(selector, f.Visible(5, f.Permission(11)), {}, guide));
}
TEST_CASE("Locked selection rejects stale preflight and atomic revocation",
          "[native_plane][selection][ordering]") {
  Fixture f;
  NativeUiPlaneImageSelection selector;
  const auto frame = f.Reference(10);
  const auto visible = f.Visible(1, f.Permission(10));
  auto probe = selector.Prepare(visible);
  probe.CheckLiveness();
  CHECK_FALSE(selector.Select(probe, {f.authority, 2, false, {}}, frame, {}));
  f.epoch->revoked = true;
  CHECK_FALSE(selector.Select(probe, visible, frame, {}));
  CHECK_FALSE(selector.retained_publication());
}
TEST_CASE(
    "Qualified payload survives pending receipt and an admitted paint lease",
    "[native_plane][selection][lifetime]") {
  struct Payload {
    explicit Payload(std::atomic<unsigned>& count) : destroyed(count) {}
    ~Payload() { ++destroyed; }
    std::atomic<unsigned>& destroyed;
  };
  Fixture f;
  std::atomic<unsigned> destroyed{0};
  NativeUiPlaneImageSelection selector;
  auto frame = f.Reference(10, std::make_shared<Payload>(destroyed));
  auto sampling_lease = Paint(selector, f.Visible(1, f.Permission(10)), frame);
  REQUIRE(sampling_lease);
  frame = {};
  // The exact production selector, not a fixture-side cache, retains payload.
  REQUIRE(destroyed == 0);
  {
    const auto pending = f.Reference(11);
    CHECK(Paint(selector, f.Visible(2, f.Permission(11)), {}, pending));
  }
  REQUIRE(destroyed == 0);
  f.epoch->revoked = true;
  CHECK_FALSE(Paint(selector, {f.authority, 3, false, {}}));
  CHECK_FALSE(selector.retained_publication());
  CHECK(destroyed == 0);  // Actual backend owns its analogous sampling fence.
  sampling_lease = {};
  CHECK(destroyed == 1);
}
gpu::NativeOverlayPresentation OverlayPresentation() {
  gpu::NativeOverlayPresentation value;
  value.canvas_width = 1280;
  value.canvas_height = 720;
  value.x = 256;
  value.y = 576;
  value.width = 729;
  value.height = 90;
  return value;
}
TEST_CASE("Native window is placed within its fitted canvas, not fitted alone",
          "[native_plane][overlay][projection]") {
  auto value = OverlayPresentation();
  value.canvas_x = 100;
  value.canvas_y = -20;
  value.x += value.canvas_x;
  value.y += value.canvas_y;
  const auto projected = ProjectNativeUiPlanePresentation(value, 1280, 800);
  REQUIRE(projected);
  REQUIRE(projected->drawable());
  CHECK(projected->x == 256);
  CHECK(projected->y == 616);
  CHECK(projected->width == 729);
  CHECK(projected->height == 90);
  CHECK(projected->scissor_x == 256);
  CHECK(projected->scissor_y == 616);
  CHECK(projected->scissor_width == 729);
  CHECK(projected->scissor_height == 90);
  CHECK(projected->opacity == 1);
}
TEST_CASE("Native clipping keeps signed unclipped texture coordinates",
          "[native_plane][overlay][projection][clipping]") {
  auto value = OverlayPresentation();
  value.canvas_x = -100;
  value.canvas_y = -50;
  value.x = -120;
  value.y = -60;
  value.width = 100;
  value.height = 40;
  const auto projected = ProjectNativeUiPlanePresentation(value, 1280, 800);
  REQUIRE(projected);
  REQUIRE(projected->drawable());
  CHECK(projected->x == -20);
  CHECK(projected->y == 30);
  CHECK(projected->width == 100);
  CHECK(projected->height == 40);
  CHECK(projected->scissor_x == 0);
  CHECK(projected->scissor_y == 40);
  CHECK(projected->scissor_width == 80);
  CHECK(projected->scissor_height == 30);
}
TEST_CASE("Native fractional placement and UV size are not rounded to scissor",
          "[native_plane][overlay][projection]") {
  auto value = OverlayPresentation();
  ++value.x;
  ++value.y;
  const auto projected = ProjectNativeUiPlanePresentation(value, 1920, 1200);
  REQUIRE(projected);
  CHECK(projected->x == 385.5f);
  CHECK(projected->y == 925.5f);
  CHECK(projected->width == 1093.5f);
  CHECK(projected->height == 135);
  CHECK(projected->scissor_x == 385);
  CHECK(projected->scissor_y == 925);
  CHECK(projected->scissor_width == 1094);
  CHECK(projected->scissor_height == 135);
}
TEST_CASE("Fractional native canvas clipping excludes host letterbox pixels",
          "[native_plane][overlay][projection][clipping]") {
  auto value = OverlayPresentation();
  value.x = -20;
  value.y = -20;
  value.width = 1320;
  value.height = 40;
  const auto projected = ProjectNativeUiPlanePresentation(value, 1000, 700);
  REQUIRE(projected);
  CHECK(projected->x == -15.625f);
  CHECK(projected->y == 53.125f);
  CHECK(projected->width == 1031.25f);
  CHECK(projected->height == 31.25f);
  CHECK(projected->scissor_x == 0);
  CHECK(projected->scissor_width == 1000);
  // The fitted canvas starts at 68.75. Pixel center 68.5 is outside it;
  // center 69.5 is inside. Keep the original quad for texture coordinates.
  CHECK(projected->scissor_y == 69);
  CHECK(projected->scissor_height == 15);
}
TEST_CASE("Valid offscreen and transparent native windows produce no paint",
          "[native_plane][overlay][projection][clipping]") {
  auto value = OverlayPresentation();
  value.x = 1280;
  auto projected = ProjectNativeUiPlanePresentation(value, 1280, 800);
  REQUIRE(projected);
  CHECK_FALSE(projected->drawable());
  value.x = -int32_t(value.width);
  projected = ProjectNativeUiPlanePresentation(value, 1280, 800);
  REQUIRE(projected);
  CHECK_FALSE(projected->drawable());
  value.x = 256;
  value.opacity = 0;
  projected = ProjectNativeUiPlanePresentation(value, 1280, 800);
  REQUIRE(projected);
  CHECK(projected->opacity == 0);
  CHECK_FALSE(projected->drawable());
}
TEST_CASE("Native projection rejects invalid shapes and unrepresentable targets",
          "[native_plane][overlay][projection][validation]") {
  const auto value = OverlayPresentation();
  CHECK_FALSE(ProjectNativeUiPlanePresentation(value, 0, 720));
  CHECK_FALSE(ProjectNativeUiPlanePresentation(value, 1280, 0));
  CHECK_FALSE(ProjectNativeUiPlanePresentation(
      value, std::numeric_limits<uint32_t>::max(), 720));
  auto bad = value;
  bad.canvas_width = 0;
  CHECK_FALSE(ProjectNativeUiPlanePresentation(bad, 1280, 720));
  bad = value;
  bad.height = 0;
  CHECK_FALSE(ProjectNativeUiPlanePresentation(bad, 1280, 720));
  bad = value;
  bad.canvas_height = 16385;
  CHECK_FALSE(ProjectNativeUiPlanePresentation(bad, 1280, 720));
  bad = value;
  bad.x = std::numeric_limits<int32_t>::max();
  CHECK_FALSE(ProjectNativeUiPlanePresentation(bad, 1280, 720));
}
TEST_CASE("Native normalized opacity preserves both ends and partial values",
          "[native_plane][overlay][projection][opacity]") {
  auto value = OverlayPresentation();
  value.opacity = 1;
  auto projected = ProjectNativeUiPlanePresentation(value, 1280, 720);
  REQUIRE(projected);
  CHECK(projected->opacity == Approx(0.0039215686f));
  value.opacity = 128;
  projected = ProjectNativeUiPlanePresentation(value, 1280, 720);
  REQUIRE(projected);
  CHECK(projected->opacity == Approx(0.5019607843f));
  value.opacity = 255;
  projected = ProjectNativeUiPlanePresentation(value, 1280, 720);
  REQUIRE(projected);
  CHECK(projected->opacity == 1);
}
TEST_CASE("Native image permission owns validated immutable presentation",
          "[native_plane][overlay][visibility]") {
  Fixture f;
  MetadataPresenter presenter;
  auto value = OverlayPresentation();
  const auto original = value;
  const auto permission = f.Permission(10, value);
  value.x = 3;
  value.opacity = 9;
  REQUIRE(permission->presentation());
  CHECK(*permission->presentation() == original);
  REQUIRE(permission->IsLive());
  REQUIRE(presenter.BindNativeUiPlaneAuthority(f.authority));
  REQUIRE(presenter.SetNativeUiPlaneVisibility(f.authority, 1, true, permission));
  value.canvas_width = 0;
  const auto invalid = f.Permission(11, value);
  CHECK(invalid->IsRevoked());
  CHECK_FALSE(invalid->IsLive());
  CHECK_FALSE(presenter.SetNativeUiPlaneVisibility(f.authority, 2, true, invalid));
  CHECK(presenter.SnapshotNativeUiPlaneVisibility().image_permission == permission);
  CHECK_FALSE(f.Permission(12)->presentation());  // Legacy profile unchanged.
}
TEST_CASE("Pending native image retains selected bounds and opacity together",
          "[native_plane][overlay][selection][ordering]") {
  Fixture f;
  NativeUiPlaneImageSelection selector;
  auto first = OverlayPresentation();
  first.opacity = 128;
  auto next = first;
  next.x = 400;
  next.opacity = 64;
  const auto a = f.Permission(10, first), b = f.Permission(11, next);
  const auto image_a = f.Reference(10), image_b = f.Reference(11);
  REQUIRE(Paint(selector, f.Visible(1, a), image_a));
  const auto pending = Paint(selector, f.Visible(2, b));
  REQUIRE(pending);
  CHECK(pending.reference.publication == image_a.publication);
  CHECK(pending.permission == a);
  REQUIRE(pending.permission->presentation());
  CHECK(*pending.permission->presentation() == first);
  const auto old_geometry = ProjectNativeUiPlanePresentation(
      *pending.permission->presentation(), 1280, 800);
  REQUIRE(old_geometry);
  CHECK(old_geometry->x == 256);
  CHECK(old_geometry->opacity == Approx(0.5019607843f));
  const auto ready = Paint(selector, f.Visible(2, b), image_b);
  REQUIRE(ready);
  CHECK(ready.reference.publication == image_b.publication);
  CHECK(ready.permission == b);
  CHECK(*ready.permission->presentation() == next);
}
TEST_CASE("Transparent native advance cannot reuse an older opaque image",
          "[native_plane][overlay][selection][visibility]") {
  Fixture f;
  NativeUiPlaneImageSelection selector;
  auto value = OverlayPresentation();
  const auto old_image = f.Reference(10);
  REQUIRE(Paint(selector, f.Visible(1, f.Permission(10, value)), old_image));
  value.opacity = 0;
  const auto transparent = f.Visible(2, f.Permission(11, value));
  CHECK_FALSE(Paint(selector, transparent));
  CHECK_FALSE(selector.retained_publication());
  value.opacity = 255;
  const auto visible = f.Visible(3, f.Permission(12, value));
  CHECK_FALSE(Paint(selector, visible, {}, old_image));
  CHECK(Paint(selector, visible, f.Reference(12)));
}
TEST_CASE("Native placement revision and paint lease retain exact permission",
          "[native_plane][overlay][selection][lifetime]") {
  Fixture f;
  NativeUiPlaneImageSelection selector;
  const auto frame = f.Reference(10);
  auto value = OverlayPresentation();
  auto permission = f.Permission(10, value);
  std::weak_ptr<const NativeUiPlaneImagePermission> retained = permission;
  auto paint_lease = Paint(selector, f.Visible(1, permission), frame);
  REQUIRE(paint_lease);
  value.x = 400;
  const auto moved = f.Permission(10, value);
  auto relocated = Paint(selector, f.Visible(2, moved), frame);
  REQUIRE(relocated);
  CHECK(relocated.reference.publication == paint_lease.reference.publication);
  CHECK(relocated.permission != paint_lease.permission);
  CHECK(relocated.permission->presentation()->x == 400);
  CHECK(paint_lease.permission->presentation()->x == 256);
  permission.reset();
  f.epoch->revoked = true;
  CHECK_FALSE(Paint(selector, {f.authority, 3, false, {}}));
  relocated = {};
  CHECK_FALSE(retained.expired());  // Already admitted paint keeps its owner.
  paint_lease = {};
  CHECK(retained.expired());
}

TEST_CASE("Native Source advance during paint preflight retains a coherent image",
          "[native_plane][selection][handoff]") {
  Fixture f;
  NativeUiPlaneImageSelection selector;
  auto first = OverlayPresentation();
  first.opacity = 128;
  auto next = first;
  next.x = 400;
  next.opacity = 64;
  const auto a = f.Permission(10, first), b = f.Permission(11, next);
  const auto image_a = f.Reference(10), image_b = f.Reference(11);
  const auto old_visibility = f.Visible(1, a);
  REQUIRE(Paint(selector, old_visibility, image_a));
  auto probe = selector.Prepare(old_visibility);
  probe.CheckLiveness();  // Before the production sampling-fence wait.
  const auto advanced = f.Visible(2, b);  // Source completes during that wait.
  // Status stays strict and cannot promote or mutate a paint selection.
  CHECK_FALSE(selector.Current(probe, advanced, image_b));
  const auto pending = selector.Select(probe, advanced, {}, {});
  REQUIRE(pending);
  CHECK(pending.reference.publication == image_a.publication);
  CHECK(pending.permission == a);
  CHECK(*pending.permission->presentation() == first);
  CHECK(pending.visibility_revision == 2);
  CHECK(NativeUiPlaneImageSelection::CanRecord(advanced, pending));

  auto later_probe = selector.Prepare(advanced);
  later_probe.CheckLiveness();
  const auto c = f.Permission(12, next);
  const auto image_c = f.Reference(12);
  const auto ready = selector.Select(later_probe, f.Visible(3, c), image_c, {});
  REQUIRE(ready);
  CHECK(ready.reference.publication == image_c.publication);
  CHECK(ready.permission == c);
  CHECK(*ready.permission->presentation() == next);
  CHECK(pending.reference.publication == image_a.publication);
  CHECK(*pending.permission->presentation() == first);

  // A first image can also become ready while another same-epoch Source
  // advances. There is no qualified image to invent or fall back to here.
  NativeUiPlaneImageSelection empty;
  auto initial_probe = empty.Prepare(old_visibility);
  initial_probe.CheckLiveness();
  const auto initial = empty.Select(initial_probe, advanced, image_b, {});
  REQUIRE(initial);
  CHECK(initial.reference.publication == image_b.publication);
  CHECK(initial.permission == b);
}
TEST_CASE("Concurrent paint qualification never resurrects the stale probe payload",
          "[native_plane][selection][handoff][lifetime]") {
  Fixture f;
  NativeUiPlaneImageSelection selector;
  const auto a = f.Permission(10, OverlayPresentation());
  const auto b = f.Permission(11, OverlayPresentation());
  const auto image_a = f.Reference(10), image_b = f.Reference(11);
  REQUIRE(Paint(selector, f.Visible(1, a), image_a));
  auto probe = selector.Prepare(f.Visible(2, b));
  probe.CheckLiveness();
  // Another actual paint changes qualified_ while the first paint has dropped
  // the backend lock. The next selection must keep that current owned pair.
  REQUIRE(Paint(selector, f.Visible(2, b), image_b));
  SECTION("The visibility revision itself stays the same") {
    const auto selected = selector.Select(probe, f.Visible(2, b), {}, {});
    REQUIRE(selected);
    CHECK(selected.reference.publication == image_b.publication);
    CHECK(selected.permission == b);
    CHECK(selected.reference.publication != image_a.publication);
  }
  SECTION("A further Source is pending too") {
    const auto c = f.Permission(12, OverlayPresentation());
    const auto selected = selector.Select(probe, f.Visible(3, c), {}, {});
    REQUIRE(selected);
    CHECK(selected.reference.publication == image_b.publication);
    CHECK(selected.permission == b);
    CHECK(selected.visibility_revision == 3);
  }
}
TEST_CASE("Paint handoff never crosses hide epoch owner or transparent barriers",
          "[native_plane][selection][handoff][epoch]") {
  Fixture f;
  NativeUiPlaneImageSelection selector;
  auto placement = OverlayPresentation();
  const auto a = f.Permission(10, placement);
  const auto image_a = f.Reference(10);
  const auto visible = f.Visible(1, a);
  REQUIRE(Paint(selector, visible, image_a));
  auto probe = selector.Prepare(visible);
  probe.CheckLiveness();
  NativeUiPlaneVisibility changed;
  SECTION("Hide wins after liveness preflight") {
    changed = {f.authority, 2, false, {}};
  }
  SECTION("A revoked epoch cannot be retained") {
    f.epoch->revoked = true;
    changed = f.Visible(2, f.Permission(11, placement));
  }
  SECTION("A fresh epoch is not authorized by the old preflight") {
    f.producer->store(2);
    f.epoch = std::make_shared<MetadataEpoch>(f.producer, 2);
    changed = f.Visible(2, f.Permission(11, placement));
  }
  SECTION("A new foreground authority is not the old owner") {
    f.authority = std::make_shared<MetadataAuthority>(4);
    changed = f.Visible(2, f.Permission(11, placement));
  }
  SECTION("Transparent zero drops the opaque retained selection") {
    placement.opacity = 0;
    changed = f.Visible(2, f.Permission(11, placement));
  }
  SECTION("Modal handoff cannot reuse a nonmodal image") {
    changed = f.Visible(2, {});
  }
  SECTION("A different permission cannot rewrite the same revision") {
    changed = f.Visible(1, f.Permission(11, placement));
  }
  SECTION("Regressing revisions cannot revive an older image") {
    changed = f.Visible(0, f.Permission(11, placement));
  }
  CHECK_FALSE(selector.Select(probe, changed, {}, image_a));
  CHECK_FALSE(selector.retained_publication());
  // Even a later visible request cannot reuse the image that crossed the
  // barrier. Only an independently admitted exact new image may appear.
  placement.opacity = 255;
  CHECK_FALSE(Paint(selector, f.Visible(3, f.Permission(12, placement)), {},
                    image_a));
}
TEST_CASE("Continuous Source advances keep the last qualified immutable pair",
          "[native_plane][selection][handoff][ordering]") {
  Fixture f;
  NativeUiPlaneImageSelection selector;
  auto placement = OverlayPresentation();
  auto permission = f.Permission(10, placement);
  auto reference = f.Reference(10);
  auto visibility = f.Visible(1, permission);
  REQUIRE(Paint(selector, visibility, reference));
  for (uint64_t step = 1; step <= 256; ++step) {
    auto probe = selector.Prepare(visibility);
    probe.CheckLiveness();
    auto moved = placement;
    moved.x = int32_t(256 + step);
    moved.opacity = uint8_t(64 + step % 128);
    const auto next_permission = f.Permission(10 + step, moved);
    const auto next_reference = f.Reference(10 + step);
    const auto next_visibility = f.Visible(1 + step, next_permission);
    NativeUiPlaneImageReference desired;
    if (step % 3 == 0) {
      desired = next_reference;
      reference = next_reference;
      permission = next_permission;
    } else if (step % 5 == 0) {
      REQUIRE(Paint(selector, next_visibility, next_reference));
      reference = next_reference;
      permission = next_permission;
    }
    // Every outer paint sees a producer advance after its full check. No
    // sleeps, retry limit or race timing can turn this into an accidental pass.
    const auto selected = selector.Select(probe, next_visibility, desired, {});
    REQUIRE(selected);
    CHECK(selected.reference.publication == reference.publication);
    CHECK(selected.permission == permission);
    CHECK(selected.reference.image == selected.permission->image());
    CHECK(selected.visibility_revision == next_visibility.revision);
    CHECK(NativeUiPlaneImageSelection::CanRecord(next_visibility, selected));
    visibility = next_visibility;
  }
  f.epoch->revoked = true;
  CHECK_FALSE(Paint(selector, {f.authority, 258, false, {}}));
  CHECK_FALSE(selector.retained_publication());
}
TEST_CASE("Reconciled paint performs no full liveness callback under backend ownership",
          "[native_plane][selection][handoff][locking]") {
  bool backend_owned = false;
  unsigned full_checks = 0, forbidden_checks = 0;
  class CheckedAuthority final : public NativeUiPlaneAuthority {
   public:
    CheckedAuthority(bool& locked, unsigned& checks, unsigned& forbidden)
        : locked_(locked), checks_(checks), forbidden_(forbidden) {}
    NativeUiPlaneOwner owner() const noexcept override {
      return {{1, 2, 3, 4}, 5, 3};
    }
    bool IsLive() const noexcept override {
      ++checks_;
      forbidden_ += locked_;
      return true;
    }
   private:
    bool& locked_;
    unsigned& checks_;
    unsigned& forbidden_;
  };
  class CheckedEpoch final : public NativeUiPlaneImageEpoch {
   public:
    CheckedEpoch(bool& locked, unsigned& checks, unsigned& forbidden)
        : locked_(locked), checks_(checks), forbidden_(forbidden) {}
    bool IsLive() const noexcept override {
      ++checks_;
      forbidden_ += locked_;
      return true;
    }
    bool IsRevoked() const noexcept override { return false; }
   private:
    bool& locked_;
    unsigned& checks_;
    unsigned& forbidden_;
  };
  const auto authority = std::make_shared<CheckedAuthority>(
      backend_owned, full_checks, forbidden_checks);
  const auto epoch = std::make_shared<CheckedEpoch>(
      backend_owned, full_checks, forbidden_checks);
  const gpu::NativeFrameSubmissionIdentity image_a{
      authority->owner().allocation_owner, 10};
  const gpu::NativeFrameSubmissionIdentity image_b{
      authority->owner().allocation_owner, 11};
  const auto a = std::make_shared<const NativeUiPlaneImagePermission>(
      image_a, epoch, OverlayPresentation());
  const auto b = std::make_shared<const NativeUiPlaneImagePermission>(
      image_b, epoch, OverlayPresentation());
  const NativeUiPlaneImageReference reference{
      image_a, authority, 5, std::make_shared<int>(1)};
  const NativeUiPlaneVisibility old_visibility{authority, 1, true, a};
  const NativeUiPlaneVisibility new_visibility{authority, 2, true, b};
  NativeUiPlaneImageSelection selector;
  REQUIRE(Paint(selector, old_visibility, reference));
  auto probe = selector.Prepare(old_visibility);
  probe.CheckLiveness();
  const unsigned checked_before_lock = full_checks;
  backend_owned = true;  // Model the production metadata-only locked section.
  const auto selected = selector.Select(probe, new_visibility, {}, {});
  const bool recordable =
      NativeUiPlaneImageSelection::CanRecord(new_visibility, selected);
  const auto further = std::make_shared<const NativeUiPlaneImagePermission>(
      gpu::NativeFrameSubmissionIdentity{authority->owner().allocation_owner, 12},
      epoch, OverlayPresentation());
  const NativeUiPlaneVisibility record_visibility{authority, 3, true, further};
  const bool recordable_after_handoff =
      CanRecordNativeUiPlaneAfterHandoff(
          new_visibility, record_visibility, selected);
  backend_owned = false;
  REQUIRE(selected);
  CHECK(recordable);
  CHECK(recordable_after_handoff);
  CHECK(full_checks == checked_before_lock);
  CHECK(forbidden_checks == 0);
}

TEST_CASE("Source advance after selection keeps the admitted paint tuple",
          "[native_plane][selection][handoff][recording]") {
  Fixture f;
  NativeUiPlaneImageSelection selector;
  auto placement = OverlayPresentation();
  placement.opacity = 128;
  const auto first = f.Permission(10, placement);
  const auto admitted = f.Visible(1, first);
  const auto image = f.Reference(10);
  const auto selected = Paint(selector, admitted, image);
  REQUIRE(selected);
  CHECK(CanRecordNativeUiPlaneAfterHandoff(
      admitted, admitted, selected));
  auto moved = placement;
  moved.x = 32;
  moved.opacity = 64;
  // Model a Source arriving during swapchain acquisition/base-title drawing,
  // after actual Select but before the record-time visibility snapshot.
  for (uint64_t step = 1; step <= 256; ++step) {
    const auto current = f.Visible(1 + step, f.Permission(10 + step, moved));
    CHECK_FALSE(NativeUiPlaneImageSelection::CanRecord(current, selected));
    REQUIRE(CanRecordNativeUiPlaneAfterHandoff(
        admitted, current, selected));
    CHECK(selected.reference.publication == image.publication);
    CHECK(selected.permission == first);
    CHECK(*selected.permission->presentation() == placement);
    CHECK(selected.visibility_revision == admitted.revision);
    CHECK(selector.retained_publication() == image.publication);
  }
}
TEST_CASE("Record-time Source handoff respects every lifecycle barrier",
          "[native_plane][selection][handoff][recording][epoch]") {
  Fixture f;
  NativeUiPlaneImageSelection selector;
  auto placement = OverlayPresentation();
  const auto admitted = f.Visible(1, f.Permission(10, placement));
  const auto selected = Paint(selector, admitted, f.Reference(10));
  REQUIRE(selected);
  NativeUiPlaneVisibility current;
  SECTION("A native hide retires an admitted paint") {
    current = {f.authority, 2, false, {}};
  }
  SECTION("A revoked epoch retires an admitted paint") {
    f.epoch->revoked = true;
    current = f.Visible(2, f.Permission(11, placement));
  }
  SECTION("A new epoch is not the admitted lifetime") {
    f.producer->store(2);
    f.epoch = std::make_shared<MetadataEpoch>(f.producer, 2);
    current = f.Visible(2, f.Permission(11, placement));
  }
  SECTION("A different foreground authority is not the admitted owner") {
    f.authority = std::make_shared<MetadataAuthority>(4);
    current = f.Visible(2, f.Permission(11, placement));
  }
  SECTION("Transparent zero cannot replay earlier opaque pixels") {
    placement.opacity = 0;
    current = f.Visible(2, f.Permission(11, placement));
  }
  SECTION("A modal handoff cannot reuse the nonmodal image") {
    current = f.Visible(2, {});
  }
  SECTION("The same revision cannot be rewritten") {
    current = f.Visible(1, f.Permission(11, placement));
  }
  SECTION("Regressing revisions cannot authorize earlier pixels") {
    current = f.Visible(0, f.Permission(11, placement));
  }
  SECTION("Foreign allocation ownership is rejected even in the same epoch") {
    auto identity = Image(*f.authority, 11);
    ++identity.owner.kernel_epoch;
    current = f.Visible(
        2, std::make_shared<const NativeUiPlaneImagePermission>(
               identity, f.epoch, placement));
  }
  SECTION("Invalid presentation cannot authorize a retained image") {
    placement.canvas_width = 0;
    current = f.Visible(2, f.Permission(11, placement));
  }
  CHECK_FALSE(CanRecordNativeUiPlaneAfterHandoff(
      admitted, current, selected));
}
TEST_CASE("Modal record-time permission retains exact snapshot semantics",
          "[native_plane][selection][handoff][recording][modal]") {
  Fixture f;
  NativeUiPlaneImageSelection selector;
  const auto modal = f.Visible(1, {});
  const auto selected = Paint(selector, modal, {}, f.Reference(10));
  REQUIRE(selected);
  CHECK(CanRecordNativeUiPlaneAfterHandoff(
      modal, modal, selected));
  CHECK_FALSE(CanRecordNativeUiPlaneAfterHandoff(
      modal, f.Visible(2, {}), selected));
  CHECK_FALSE(CanRecordNativeUiPlaneAfterHandoff(
      modal, f.Visible(2, f.Permission(11, OverlayPresentation())), selected));
  // A tuple that was never admitted by the old snapshot cannot gain record
  // permission merely because it happens to match a later Source.
  auto unadmitted = selected;
  unadmitted.visibility_revision = 2;
  CHECK_FALSE(CanRecordNativeUiPlaneAfterHandoff(
      modal, f.Visible(2, {}), unadmitted));
}
}  // namespace
}  // namespace xe::ui::test
