/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include "xenia/gpu/native_frame_command_plan.h"
#include "xenia/kernel/native_identity_image_journal.h"
#include "xenia/kernel/util/native_wait_snapshot.h"
namespace xe::kernel::identity_detail {
inline bool NativeImageFullKeyMatches(Memory& memory,uint32_t at,uint32_t address,
    uint32_t bytes,uint64_t version) noexcept try {
 std::array<uint16_t,64> expected{};size_t count=0;
 constexpr char prefix[]="memorykey://";for(size_t i=0;i<sizeof(prefix)-1;++i)expected[count++]=prefix[i];
 const auto append=[&](uint64_t value,uint32_t minimum,bool upper){
  constexpr char lower[]="0123456789abcdef",caps[]="0123456789ABCDEF";
  std::array<uint16_t,16> reversed{};uint32_t n=0;
  do{reversed[n++]=(upper?caps:lower)[value&15];value>>=4;}while(value && n<16);
  while(n<minimum)reversed[n++]='0';while(n)expected[count++]=reversed[--n];
 };
 append(address,1,true);expected[count++]=',';append(bytes,1,true);
 expected[count++]='_';append(version,16,false);expected[count]=0;
 if(at<0x1000 || (at&1) || uint64_t(at)+(count+1)*2>0x100000000ull)return false;
 // Bounded current key metadata only; no encoded PNG/source byte snapshot,
 // resource owner or future translated pointer is created by this predicate.
 std::array<uint8_t,128> actual{};
 if(!internal::ReadNativeWaitSnapshot(memory,at,std::span(actual.data(),(count+1)*2)))return false;
 for(size_t i=0;i<=count;++i)
  if(uint16_t((uint16_t(actual[i*2])<<8)|actual[i*2+1])!=expected[i])return false;
 return true;
}catch(...){return false;}

// Original CPU use metadata only, supplied by the SAME Owner at genuine
// native call/store/own-return cuts. No native pointer is dereferenced here.
struct NativeImageDraw {
 NativeImageJournal::Draw draw;
 uint32_t packet_address=0,packet_header=0,viz=0,initiator=0;
 std::array<uint32_t,6> fetch{};
};
// Value history for one ACTUAL render iteration only. Neutral Task/control
// Work has no image-history role and must not reserve these 64 inline rows.
// A missing history is unavailable, never proof of a completed empty draw.
struct NativeImageDrawHistory {
 std::array<std::optional<NativeImageDraw>,64> draws;
 void RevokeTexture(uint32_t texture) noexcept {
  for(auto& draw:draws)if(draw && draw->draw.copied.texture==texture)draw.reset();
 }
};
inline bool ImageDrawMatchesPlan(const NativeImageDraw& actual,uint32_t primary,
    const gpu::NativeFrameCommandPlan& plan) noexcept {
 if(!actual.draw.copied.valid() || !actual.draw.render || !actual.draw.call ||
    !actual.draw.selection || !actual.draw.device || !actual.draw.command_tail ||
    actual.draw.dc.kind!=NativeImageJournal::Kind::kDc || !actual.draw.dc.birth ||
    actual.draw.dc.root!=actual.draw.copied.input.root() ||
    !primary || primary>=0x20000000 || actual.packet_address>=0x20000000 ||
    actual.packet_address<primary || (actual.packet_address-primary)%4 ||
    actual.packet_header!=0xC0012200)return false;
 const auto offset=(actual.packet_address-primary)/4;
 const auto& words=plan.words();
 if(uint64_t(offset)+3>words.size() || words[offset]!=actual.packet_header ||
    words[offset+1]!=actual.viz || words[offset+2]!=actual.initiator)return false;
 bool packet=false;
 for(const auto& parsed:plan.packets())if(parsed.dword_offset==offset &&
     parsed.kind==gpu::NativeFrameCommandPlan::PacketKind::kDraw && parsed.dword_count==3)packet=true;
 if(!packet)return false;
 for(const auto& parsed:plan.inventory().draws)if(parsed.packet_dword_offset==offset &&
     parsed.opcode==0x22 && parsed.viz_token==actual.viz && parsed.initiator==actual.initiator){
  for(size_t n=0;n<actual.fetch.size();++n)
   if(!parsed.fetch_known.test(n) || parsed.fetch_words[n]!=actual.fetch[n])return false;
  return true;
 }
 return false;
}
} // namespace xe::kernel::identity_detail
