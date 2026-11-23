/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include "xenia/kernel/native_identity_image_observation.h"
namespace xe::kernel::identity_detail {
// Native section bytes are a DISTINCT source; no Provider allocation epoch is
// invented for an original module resource. The issuer must observe the exact
// live original module/section/entry/default-output tuple under its real owner.
struct NativeSectionImageSource {
 NativeProfileIdentityTasks::Root root;
 uint64_t source=0,provider=0,version=0,module_generation=0;
 uint32_t module=0,section=0,section_size=0,address=0,size=0;
 bool valid() const noexcept {
  return root.valid() && source && provider && module && module_generation==root.generation &&
      section && section_size && address>=section && size &&
      uint64_t(section)+section_size<=0x100000000ull &&
      uint64_t(address)+size<=uint64_t(section)+section_size;
 }
 bool operator==(const NativeSectionImageSource&) const=default;
};
struct NativeImageInput {
 std::optional<EncodedImageSource> provider;
 std::optional<NativeSectionImageSource> section;
 bool valid() const noexcept {
  if(bool(provider)==bool(section))return false;
  return provider?provider->root.valid() && provider->source && provider->provider &&
      provider->allocation && provider->publication && provider->address && provider->size &&
      uint64_t(provider->address)+provider->size<=0x100000000ull:
      section->valid();
 }
 NativeProfileIdentityTasks::Root root() const noexcept {
  return provider?provider->root:section?section->root:NativeProfileIdentityTasks::Root{};
 }
 uint64_t source() const noexcept {return provider?provider->source:section?section->source:0;}
 uint32_t address() const noexcept {return provider?provider->address:section?section->address:0;}
 uint32_t size() const noexcept {return provider?provider->size:section?section->size:0;}
 bool operator==(const NativeImageInput&) const=default;
};
struct NativeDecodedImageTransfer {
 NativeImageInput input;
 uint64_t decoder_invocation=0;
 uint32_t texture=0;
 bool valid() const noexcept {return input.valid() && decoder_invocation && texture;}
 bool operator==(const NativeDecodedImageTransfer&) const=default;
};
// Bounded VALUE metadata owned by the existing identity State. It owns no
// native references, module, bytes, mutex, callback, cache or renderer. Only
// authenticated original cuts may call it; this is not an authority issuer.
class NativeImageJournal final {
 public:
 using Root=NativeProfileIdentityTasks::Root;
 enum class Kind:uint32_t {kInterface,kBrush,kWrapper,kDc};
 struct Object {
  Root root;Kind kind=Kind::kInterface;uint32_t address=0;uint64_t birth=0;
  bool operator==(const Object&) const=default;
 };
 struct Selection {Object dc;uint64_t serial=0;};
 struct Draw {
  Object dc;NativeDecodedImageTransfer copied;
  uint64_t selection=0,render=0,call=0;
  uint32_t device=0,command_tail=0;
  bool operator==(const Draw&) const=default;
 };
 explicit NativeImageJournal(Root root):root_(root){}
 std::optional<Object> BeginNativeObject(Kind kind,uint32_t address) noexcept {
  if(!root_.valid() || unavailable_ || !address || (address&3) || !Next(next_birth_))return {};
  for(auto& r:records_)if(r.used && r.object.address==address)r.used=false;
  for(auto& r:records_)if(!r.used){r={};r.used=true;r.constructing=true;
   r.object={root_,kind,address,next_birth_};return r.object;}
  unavailable_=true;return {};
 }
 bool FinishNativeObject(Object object,bool actual_normal_output) noexcept {
  auto* r=Find(object);if(!r || !r->constructing)return false;
  if(!actual_normal_output){r->used=false;return false;}r->constructing=false;return true;
 }
 std::optional<Object> Current(Kind kind,uint32_t address) const noexcept {
  if(unavailable_)return {};
  for(const auto& r:records_)if(r.used && !r.constructing && r.object.kind==kind &&
      r.object.address==address)return r.object;
  return {};
 }
 void RetireObject(Object object) noexcept {if(auto* r=Find(object))r->used=false;}
 void RevokeNativeTexture(uint32_t address) noexcept {
  if(!address)return;
  for(auto& r:records_)if(r.used){
   if(r.copied && r.copied->texture==address)r.copied.reset();
   if(r.previous && r.previous->texture==address)r.previous.reset();
  }
 }

 void WithdrawResource(Object object) noexcept {
  if(auto* r=Ready(object)){r->copied.reset();r->linked={};
   if(object.kind==Kind::kDc){r->previous.reset();r->previous_device=r->previous_texture=0;
    r->device=r->texture=0;if(r->pending_selections)r->selection_uncertain=true;}
  }
 }
 void BeforePathChange(Object object) noexcept {
  if(auto* r=Ready(object)){r->path.reset();r->copied.reset();r->linked={};}
 }
 bool CopyOwnedPath(Object object,const NativeImageInput& input,bool actual_copy_returned) noexcept {
  auto* r=Ready(object);if(!r || object.kind!=Kind::kInterface || !actual_copy_returned ||
     !Valid(input))return false;r->path=input;return true;
 }
 std::optional<NativeImageInput> OwnedPath(Object object) const noexcept {
  const auto* r=Ready(object);return r?r->path:std::optional<NativeImageInput>{};
 }
 bool FinishWrapperAssignment(Object object,const NativeDecodedImageTransfer& actual,
     bool actual_assignment_returned) noexcept {
  auto* r=Ready(object);if(!r || object.kind!=Kind::kWrapper || !actual_assignment_returned ||
     !actual.valid() || !Valid(actual.input))return false;r->copied=actual;return true;
 }
 std::optional<NativeDecodedImageTransfer> Copied(Object object) const noexcept {
  const auto* r=Ready(object);return r?r->copied:std::optional<NativeDecodedImageTransfer>{};
 }
 bool AdoptKnownWinner(Object brush,Object wrapper,const NativeImageInput& desired,
     bool actual_brush_assignment_returned) noexcept {
  auto* b=Ready(brush);const auto* w=Ready(wrapper);
  if(!b || !w || brush.kind!=Kind::kBrush || wrapper.kind!=Kind::kWrapper ||
     !actual_brush_assignment_returned || !Valid(desired) || !w->copied ||
     w->copied->input!=desired)return false;
  b->copied=w->copied;b->linked=wrapper;return true;
 }
 std::optional<Selection> BeginDcSelection(Object dc) noexcept {
  auto* r=Ready(dc);if(!r || dc.kind!=Kind::kDc || !Next(next_selection_))return {};
  if(r->pending_selections==UINT32_MAX){r->copied.reset();r->selection_uncertain=true;return {};}
  ++r->pending_selections;r->previous=r->copied;r->previous_device=r->device;
  r->previous_texture=r->texture;r->copied.reset();r->selection=next_selection_;
  if(r->pending_selections!=1)r->selection_uncertain=true;
  return Selection{dc,r->selection};
 }
 bool SelectDcTexture(Selection selection,Object wrapper,uint32_t actual_device,
     uint32_t actual_texture,bool actual_native_store_returned,
     bool actual_same_texture_returned) noexcept {
  auto* d=Ready(selection.dc);const auto* w=Ready(wrapper);
  if(!d || !d->pending_selections)return false;
  --d->pending_selections;
  const bool serialized=d->selection==selection.serial && !d->selection_uncertain;
  if(!serialized){d->copied.reset();if(!d->pending_selections)d->selection_uncertain=false;return false;}
  if(!w || !w->copied || wrapper.kind!=Kind::kWrapper || !actual_device ||
     !actual_texture || w->copied->texture!=actual_texture){d->copied.reset();return false;}
  const bool same=actual_same_texture_returned && d->previous &&
      d->previous_device==actual_device && d->previous_texture==actual_texture &&
      *d->previous==*w->copied;
  if(!actual_native_store_returned && !same)return false;
  d->device=actual_device;d->texture=actual_texture;d->copied=w->copied;
  d->previous.reset();return true;
 }
 std::optional<Draw> ObserveDrawCall(Object dc,uint64_t selection,uint64_t render,
     uint64_t call,uint32_t actual_device,uint32_t actual_tail,
     bool actual_command_store_returned) const noexcept {
  const auto* d=Ready(dc);
  if(!d || dc.kind!=Kind::kDc || d->pending_selections || d->selection_uncertain ||
     !d->copied || selection!=d->selection || !render || !call || !actual_tail ||
     d->device!=actual_device || !actual_command_store_returned)return {};
  return Draw{dc,*d->copied,selection,render,call,actual_device,actual_tail};
 }
 std::optional<Draw> ObserveRetainedDrawCall(Object dc,uint64_t actual_selection,
     const NativeDecodedImageTransfer& actual_copied,uint64_t render,uint64_t call,
     uint32_t actual_device,uint32_t actual_tail,bool actual_store_returned) const noexcept {
  const auto value=ObserveDrawCall(dc,actual_selection,render,call,actual_device,actual_tail,actual_store_returned);
  if(!value || value->copied!=actual_copied)return {};return value;
 }
 bool NativeTextureOwned(const NativeDecodedImageTransfer& actual) const noexcept {
  if(unavailable_ || !actual.valid())return false;
  for(const auto& r:records_)if(r.used && !r.constructing && r.copied && *r.copied==actual &&
      (r.object.kind==Kind::kWrapper || r.object.kind==Kind::kDc))return true;
  return false;
 }
 uint64_t SelectionSerial(Object dc) const noexcept {const auto* d=Ready(dc);return d?d->selection:0;}
 bool unavailable() const noexcept {return unavailable_;}
 void RetireMetadataAfterActualStop() noexcept {for(auto& r:records_)r={};unavailable_=true;}
 private:
 struct Record {
  Object object,linked;bool used=false,constructing=false,selection_uncertain=false;
  uint32_t pending_selections=0,device=0,texture=0,previous_device=0,previous_texture=0;
  uint64_t selection=0;
  std::optional<NativeImageInput> path;
  std::optional<NativeDecodedImageTransfer> copied,previous;
 };
 bool Next(uint64_t& value) noexcept {if(value==UINT64_MAX){unavailable_=true;return false;}++value;return true;}
 bool Valid(const NativeImageInput& value) const noexcept {return !unavailable_ && value.valid() && value.root()==root_;}
 Record* Find(Object object) noexcept {
  if(unavailable_ || object.root!=root_ || !object.birth)return nullptr;
  for(auto& r:records_)if(r.used && r.object==object)return &r;return nullptr;
 }
 const Record* Find(Object object) const noexcept {
  return const_cast<NativeImageJournal*>(this)->Find(object);
 }
 Record* Ready(Object object) noexcept {auto* r=Find(object);return r && !r->constructing?r:nullptr;}
 const Record* Ready(Object object) const noexcept {return const_cast<NativeImageJournal*>(this)->Ready(object);}
 Root root_;uint64_t next_birth_=0,next_selection_=0;bool unavailable_=false;
 std::array<Record,256> records_{};
};
} // namespace xe::kernel::identity_detail
