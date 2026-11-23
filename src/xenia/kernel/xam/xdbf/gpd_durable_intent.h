/** Xenia : Xbox 360 Emulator Research Project; BSD license, see LICENSE. */
#ifndef XENIA_KERNEL_XAM_XDBF_GPD_DURABLE_INTENT_H_
#define XENIA_KERNEL_XAM_XDBF_GPD_DURABLE_INTENT_H_
#include "xenia/base/profile_publication_intent.h"
#include "xenia/kernel/xam/xdbf/gpd_snapshot.h"
namespace xe::kernel::xam {
// Compatibility names retain the existing kernel producers/consumers while
// the storage-only wire/redo owner can run before managed guest admission.
using GpdIntentAction = filesystem::GpdIntentAction;
using GpdManagedSourceIdentity = filesystem::GpdManagedSourceIdentity;
using GpdIntentBinding = filesystem::GpdIntentBinding;
using GpdIntentMutation = filesystem::GpdIntentMutation;
using GpdIntentSummary = filesystem::GpdIntentSummary;
using GpdIntentFileReceipt = filesystem::GpdIntentFileReceipt;
using GpdIntentAttempt = filesystem::GpdIntentAttempt;
using GpdDurableIntentStore = filesystem::ProfilePublicationIntentStore;
}  // namespace xe::kernel::xam
#endif
