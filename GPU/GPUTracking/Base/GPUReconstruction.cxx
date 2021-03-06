//**************************************************************************\
//* This file is property of and copyright by the ALICE Project            *\
//* ALICE Experiment at CERN, All rights reserved.                         *\
//*                                                                        *\
//* Primary Authors: Matthias Richter <Matthias.Richter@ift.uib.no>        *\
//*                  for The ALICE HLT Project.                            *\
//*                                                                        *\
//* Permission to use, copy, modify and distribute this software and its   *\
//* documentation strictly for non-commercial purposes is hereby granted   *\
//* without fee, provided that the above copyright notice appears in all   *\
//* copies and that both the copyright notice and this permission notice   *\
//* appear in the supporting documentation. The authors make no claims     *\
//* about the suitability of this software for any purpose. It is          *\
//* provided "as is" without express or implied warranty.                  *\
//**************************************************************************

/// \file GPUReconstruction.cxx
/// \author David Rohr

#include <cstring>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>
#include <map>
#include <queue>
#include <mutex>
#include <condition_variable>

#ifdef _WIN32
#include <windows.h>
#include <winbase.h>
#include <conio.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#endif

#ifdef WITH_OPENMP
#include <omp.h>
#endif

#include "GPUReconstruction.h"
#include "GPUReconstructionIncludes.h"

#include "GPUMemoryResource.h"
#include "GPUChain.h"
#include "GPUMemorySizeScalers.h"

#define GPUCA_LOGGING_PRINTF
#include "GPULogging.h"

namespace GPUCA_NAMESPACE
{
namespace gpu
{
struct GPUReconstructionPipelineQueue {
  unsigned int op = 0; // For now, 0 = process, 1 = terminate
  GPUChain* chain = nullptr;
  std::mutex m;
  std::condition_variable c;
  bool done = false;
  int retVal = 0;
};

struct GPUReconstructionPipelineContext {
  std::queue<GPUReconstructionPipelineQueue*> queue;
  std::mutex mutex;
  std::condition_variable cond;
  bool terminate = false;
};
} // namespace gpu
} // namespace GPUCA_NAMESPACE

using namespace GPUCA_NAMESPACE::gpu;

constexpr const char* const GPUReconstruction::DEVICE_TYPE_NAMES[];
constexpr const char* const GPUReconstruction::GEOMETRY_TYPE_NAMES[];
constexpr const char* const GPUReconstruction::IOTYPENAMES[];
constexpr GPUReconstruction::GeometryType GPUReconstruction::geometryType;

static long long int ptrDiff(void* a, void* b) { return (long long int)((char*)a - (char*)b); }

GPUReconstruction::GPUReconstruction(const GPUSettingsProcessing& cfg) : mHostConstantMem(new GPUConstantMem), mProcessingSettings(cfg)
{
  if (cfg.master) {
    if (cfg.master->mProcessingSettings.deviceType != cfg.deviceType) {
      throw std::invalid_argument("device type of master and slave GPUReconstruction does not match");
    }
    if (cfg.master->mMaster) {
      throw std::invalid_argument("Cannot be slave to a slave");
    }
    mMaster = cfg.master;
    cfg.master->mSlaves.emplace_back(this);
  }
  mDeviceProcessingSettings.SetDefaults();
  mEventSettings.SetDefaults();
  param().SetDefaults(&mEventSettings);
  mMemoryScalers.reset(new GPUMemorySizeScalers);
  for (unsigned int i = 0; i < NSLICES; i++) {
    processors()->tpcTrackers[i].SetSlice(i); // TODO: Move to a better place
#ifdef HAVE_O2HEADERS
    processors()->tpcClusterer[i].mISlice = i;
#endif
  }
}

GPUReconstruction::~GPUReconstruction()
{
  if (mInitialized) {
    GPUError("GPU Reconstruction not properly deinitialized!");
  }
}

void GPUReconstruction::GetITSTraits(std::unique_ptr<o2::its::TrackerTraits>* trackerTraits, std::unique_ptr<o2::its::VertexerTraits>* vertexerTraits)
{
  if (trackerTraits) {
    trackerTraits->reset(new o2::its::TrackerTraitsCPU);
  }
  if (vertexerTraits) {
    vertexerTraits->reset(new o2::its::VertexerTraits);
  }
}

int GPUReconstruction::Init()
{
  if (mMaster) {
    throw std::runtime_error("Must not call init on slave!");
  }
  int retVal = InitPhaseBeforeDevice();
  if (retVal) {
    return retVal;
  }
  for (unsigned int i = 0; i < mSlaves.size(); i++) {
    retVal = mSlaves[i]->InitPhaseBeforeDevice();
    if (retVal) {
      GPUError("Error initialization slave (before deviceinit)");
      return retVal;
    }
    mNStreams = std::max(mNStreams, mSlaves[i]->mNStreams);
    mHostMemorySize = std::max(mHostMemorySize, mSlaves[i]->mHostMemorySize);
    mDeviceMemorySize = std::max(mDeviceMemorySize, mSlaves[i]->mDeviceMemorySize);
  }
  if (InitDevice()) {
    return 1;
  }
  mHostMemoryPoolEnd = (char*)mHostMemoryBase + mHostMemorySize;
  mDeviceMemoryPoolEnd = (char*)mDeviceMemoryBase + mDeviceMemorySize;
  if (InitPhasePermanentMemory()) {
    return 1;
  }
  for (unsigned int i = 0; i < mSlaves.size(); i++) {
    mSlaves[i]->mDeviceMemoryBase = mDeviceMemoryPermanent;
    mSlaves[i]->mHostMemoryBase = mHostMemoryPermanent;
    mSlaves[i]->mDeviceMemorySize = mDeviceMemorySize - ((char*)mSlaves[i]->mDeviceMemoryBase - (char*)mDeviceMemoryBase);
    mSlaves[i]->mHostMemorySize = mHostMemorySize - ((char*)mSlaves[i]->mHostMemoryBase - (char*)mHostMemoryBase);
    mSlaves[i]->mHostMemoryPoolEnd = mHostMemoryPoolEnd;
    mSlaves[i]->mDeviceMemoryPoolEnd = mDeviceMemoryPoolEnd;
    if (mSlaves[i]->InitDevice()) {
      GPUError("Error initialization slave (deviceinit)");
      return 1;
    }
    if (mSlaves[i]->InitPhasePermanentMemory()) {
      GPUError("Error initialization slave (permanent memory)");
      return 1;
    }
    mDeviceMemoryPermanent = mSlaves[i]->mDeviceMemoryPermanent;
    mHostMemoryPermanent = mSlaves[i]->mHostMemoryPermanent;
  }
  retVal = InitPhaseAfterDevice();
  if (retVal) {
    return retVal;
  }
  for (unsigned int i = 0; i < mSlaves.size(); i++) {
    mSlaves[i]->mDeviceMemoryPermanent = mDeviceMemoryPermanent;
    mSlaves[i]->mHostMemoryPermanent = mHostMemoryPermanent;
    retVal = mSlaves[i]->InitPhaseAfterDevice();
    if (retVal) {
      GPUError("Error initialization slave (after device init)");
      return retVal;
    }
  }
  return 0;
}

int GPUReconstruction::InitPhaseBeforeDevice()
{
#ifndef HAVE_O2HEADERS
  mRecoSteps.setBits(RecoStep::ITSTracking, false);
  mRecoSteps.setBits(RecoStep::TRDTracking, false);
  mRecoSteps.setBits(RecoStep::TPCConversion, false);
  mRecoSteps.setBits(RecoStep::TPCCompression, false);
  mRecoSteps.setBits(RecoStep::TPCdEdx, false);
#endif
  mRecoStepsGPU &= mRecoSteps;
  mRecoStepsGPU &= AvailableRecoSteps();
  if (!IsGPU()) {
    mRecoStepsGPU.set((unsigned char)0);
  }

  if (mDeviceProcessingSettings.memoryAllocationStrategy == GPUMemoryResource::ALLOCATION_AUTO) {
    mDeviceProcessingSettings.memoryAllocationStrategy = IsGPU() ? GPUMemoryResource::ALLOCATION_GLOBAL : GPUMemoryResource::ALLOCATION_INDIVIDUAL;
  }
  if (mDeviceProcessingSettings.debugLevel >= 4) {
    mDeviceProcessingSettings.keepAllMemory = true;
  }
  if (mDeviceProcessingSettings.debugLevel >= 5 && mDeviceProcessingSettings.allocDebugLevel < 2) {
    mDeviceProcessingSettings.allocDebugLevel = 2;
  }
  if (mDeviceProcessingSettings.eventDisplay || mDeviceProcessingSettings.keepAllMemory) {
    mDeviceProcessingSettings.keepDisplayMemory = true;
  }
  if (mDeviceProcessingSettings.debugLevel < 6) {
    mDeviceProcessingSettings.debugMask = 0;
  }
  if (mDeviceProcessingSettings.debugLevel < 1) {
    mDeviceProcessingSettings.deviceTimers = false;
  }
  if (mDeviceProcessingSettings.debugLevel >= 6 && mDeviceProcessingSettings.comparableDebutOutput) {
    mDeviceProcessingSettings.nTPCClustererLanes = 1;
    if (mDeviceProcessingSettings.trackletConstructorInPipeline < 0) {
      mDeviceProcessingSettings.trackletConstructorInPipeline = 1;
    }
    if (mDeviceProcessingSettings.trackletSelectorInPipeline < 0) {
      mDeviceProcessingSettings.trackletSelectorInPipeline = 1;
    }
    if (mDeviceProcessingSettings.trackletSelectorSlices < 0) {
      mDeviceProcessingSettings.trackletSelectorSlices = 1;
    }
  }
  if (mDeviceProcessingSettings.tpcCompressionGatherMode < 0) {
    mDeviceProcessingSettings.tpcCompressionGatherMode = (mRecoStepsGPU & GPUDataTypes::RecoStep::TPCCompression) ? 2 : 0;
  }
  if (!(mRecoStepsGPU & GPUDataTypes::RecoStep::TPCMerging)) {
    mDeviceProcessingSettings.mergerSortTracks = false;
  }
  if (!IsGPU()) {
    mDeviceProcessingSettings.nDeviceHelperThreads = 0;
  }

  if (param().rec.NonConsecutiveIDs) {
    param().rec.DisableRefitAttachment = 0xFF;
  }
  if (!(mRecoStepsGPU & RecoStep::TPCMerging)) {
    mDeviceProcessingSettings.fullMergerOnGPU = false;
  }
  if (mDeviceProcessingSettings.debugLevel || !mDeviceProcessingSettings.fullMergerOnGPU) {
    mDeviceProcessingSettings.delayedOutput = false;
  }

  UpdateSettings();
  GPUCA_GPUReconstructionUpdateDefailts();
  if (!mDeviceProcessingSettings.trackletConstructorInPipeline) {
    mDeviceProcessingSettings.trackletSelectorInPipeline = false;
  }

  mMemoryScalers->factor = mDeviceProcessingSettings.memoryScalingFactor;

#ifdef WITH_OPENMP
  if (mDeviceProcessingSettings.nThreads <= 0) {
    mDeviceProcessingSettings.nThreads = omp_get_max_threads();
  } else {
    omp_set_num_threads(mDeviceProcessingSettings.nThreads);
  }
#else
  mDeviceProcessingSettings.nThreads = 1;
#endif
  mMaxThreads = std::max(mMaxThreads, mDeviceProcessingSettings.nThreads);
  if (IsGPU()) {
    mNStreams = std::max(mDeviceProcessingSettings.nStreams, 3);
  }

  if (mDeviceProcessingSettings.doublePipeline && (mChains.size() != 1 || mChains[0]->SupportsDoublePipeline() == false || !IsGPU() || mDeviceProcessingSettings.memoryAllocationStrategy != GPUMemoryResource::ALLOCATION_GLOBAL)) {
    GPUError("Must use double pipeline mode only with exactly one chain that must support it");
    return 1;
  }

  if (mMaster == nullptr && mDeviceProcessingSettings.doublePipeline) {
    mPipelineContext.reset(new GPUReconstructionPipelineContext);
  }

  mDeviceMemorySize = mHostMemorySize = 0;
  for (unsigned int i = 0; i < mChains.size(); i++) {
    mChains[i]->RegisterPermanentMemoryAndProcessors();
    size_t memGpu, memHost;
    mChains[i]->MemorySize(memGpu, memHost);
    mDeviceMemorySize += memGpu;
    mHostMemorySize += memHost;
  }
  if (mDeviceProcessingSettings.forceMemoryPoolSize && mDeviceProcessingSettings.forceMemoryPoolSize <= 2 && CanQueryMaxMemory()) {
    mDeviceMemorySize = mDeviceProcessingSettings.forceMemoryPoolSize;
  } else if (mDeviceProcessingSettings.forceMemoryPoolSize > 2) {
    mDeviceMemorySize = mHostMemorySize = mDeviceProcessingSettings.forceMemoryPoolSize;
  }

  for (unsigned int i = 0; i < mProcessors.size(); i++) {
    (mProcessors[i].proc->*(mProcessors[i].RegisterMemoryAllocation))();
  }

  return 0;
}

int GPUReconstruction::InitPhasePermanentMemory()
{
  if (IsGPU()) {
    for (unsigned int i = 0; i < mChains.size(); i++) {
      mChains[i]->RegisterGPUProcessors();
    }
  }
  AllocateRegisteredPermanentMemory();
  return 0;
}

int GPUReconstruction::InitPhaseAfterDevice()
{
  for (unsigned int i = 0; i < mChains.size(); i++) {
    if (mChains[i]->Init()) {
      return 1;
    }
  }
  for (unsigned int i = 0; i < mProcessors.size(); i++) {
    (mProcessors[i].proc->*(mProcessors[i].InitializeProcessor))();
  }

  WriteConstantParams(); // First initialization, if the user doesn't use RunChains

  mInitialized = true;
  return 0;
}

void GPUReconstruction::WriteConstantParams()
{
  if (IsGPU()) {
    const auto threadContext = GetThreadContext();
    WriteToConstantMemory((char*)&processors()->param - (char*)processors(), &param(), sizeof(param()), -1);
  }
}

int GPUReconstruction::Finalize()
{
  for (unsigned int i = 0; i < mChains.size(); i++) {
    mChains[i]->Finalize();
  }
  return 0;
}

int GPUReconstruction::Exit()
{
  if (!mInitialized) {
    return 1;
  }
  for (unsigned int i = 0; i < mSlaves.size(); i++) {
    if (mSlaves[i]->Exit()) {
      GPUError("Error exiting slave");
    }
  }

  mChains.clear();          // Make sure we destroy a possible ITS GPU tracker before we call the destructors
  mHostConstantMem.reset(); // Reset these explicitly before the destruction of other members unloads the library
  if (mDeviceProcessingSettings.memoryAllocationStrategy == GPUMemoryResource::ALLOCATION_INDIVIDUAL) {
    for (unsigned int i = 0; i < mMemoryResources.size(); i++) {
      if (mMemoryResources[i].mReuse >= 0) {
        continue;
      }
      operator delete(mMemoryResources[i].mPtrDevice GPUCA_OPERATOR_NEW_ALIGNMENT);
      mMemoryResources[i].mPtr = mMemoryResources[i].mPtrDevice = nullptr;
    }
  }
  mMemoryResources.clear();
  if (mInitialized) {
    ExitDevice();
  }
  mInitialized = false;
  return 0;
}

void GPUReconstruction::RegisterGPUDeviceProcessor(GPUProcessor* proc, GPUProcessor* slaveProcessor) { proc->InitGPUProcessor(this, GPUProcessor::PROCESSOR_TYPE_DEVICE, slaveProcessor); }
void GPUReconstruction::ConstructGPUProcessor(GPUProcessor* proc) { proc->mConstantMem = proc->mGPUProcessorType == GPUProcessor::PROCESSOR_TYPE_DEVICE ? mDeviceConstantMem : mHostConstantMem.get(); }

void GPUReconstruction::ComputeReuseMax(GPUProcessor* proc)
{
  for (auto it = mMemoryReuse1to1.begin(); it != mMemoryReuse1to1.end(); it++) {
    auto& re = it->second;
    if (proc == nullptr || re.proc == proc) {
      GPUMemoryResource& resMain = mMemoryResources[re.res[0]];
      resMain.mOverrideSize = 0;
      for (unsigned int i = 0; i < re.res.size(); i++) {
        GPUMemoryResource& res = mMemoryResources[re.res[i]];
        resMain.mOverrideSize = std::max<size_t>(resMain.mOverrideSize, (char*)res.SetPointers((void*)1) - (char*)1);
      }
    }
  }
}

size_t GPUReconstruction::AllocateRegisteredMemory(GPUProcessor* proc)
{
  if (mDeviceProcessingSettings.debugLevel >= 5) {
    GPUInfo("Allocating memory %p", (void*)proc);
  }
  size_t total = 0;
  for (unsigned int i = 0; i < mMemoryResources.size(); i++) {
    if ((proc == nullptr ? !mMemoryResources[i].mProcessor->mAllocateAndInitializeLate : mMemoryResources[i].mProcessor == proc) && !(mMemoryResources[i].mType & GPUMemoryResource::MEMORY_CUSTOM)) {
      total += AllocateRegisteredMemory(i);
    }
  }
  if (mDeviceProcessingSettings.debugLevel >= 5) {
    GPUInfo("Allocating memory done");
  }
  return total;
}

size_t GPUReconstruction::AllocateRegisteredPermanentMemory()
{
  if (mDeviceProcessingSettings.debugLevel >= 5) {
    GPUInfo("Allocating Permanent Memory");
  }
  int total = 0;
  for (unsigned int i = 0; i < mMemoryResources.size(); i++) {
    if ((mMemoryResources[i].mType & GPUMemoryResource::MEMORY_PERMANENT) && mMemoryResources[i].mPtr == nullptr) {
      total += AllocateRegisteredMemory(i);
    }
  }
  mHostMemoryPermanent = mHostMemoryPool;
  mDeviceMemoryPermanent = mDeviceMemoryPool;
  if (mDeviceProcessingSettings.debugLevel >= 5) {
    GPUInfo("Permanent Memory Done");
  }
  return total;
}

size_t GPUReconstruction::AllocateRegisteredMemoryHelper(GPUMemoryResource* res, void*& ptr, void*& memorypool, void* memorybase, size_t memorysize, void* (GPUMemoryResource::*setPtr)(void*), void*& memorypoolend)
{
  if (res->mReuse >= 0) {
    ptr = (&ptr == &res->mPtrDevice) ? mMemoryResources[res->mReuse].mPtrDevice : mMemoryResources[res->mReuse].mPtr;
    size_t retVal = (char*)((res->*setPtr)(ptr)) - (char*)(ptr);
    if (retVal > mMemoryResources[res->mReuse].mSize) {
      throw std::bad_alloc();
    }
    if (mDeviceProcessingSettings.allocDebugLevel >= 2) {
      std::cout << "Reused " << res->mName << ": " << retVal << "\n";
    }
    return retVal;
  }
  if (memorypool == nullptr) {
    GPUInfo("Memory pool uninitialized");
    throw std::bad_alloc();
  }
  size_t retVal;
  size_t minSize = res->mOverrideSize;
  if (IsGPU() && minSize < GPUCA_BUFFER_ALIGNMENT) {
    minSize = GPUCA_BUFFER_ALIGNMENT;
  }
  if ((res->mType & GPUMemoryResource::MEMORY_STACK) && memorypoolend) {
    retVal = (char*)((res->*setPtr)((char*)1)) - (char*)(1);
    memorypoolend = (void*)((char*)memorypoolend - GPUProcessor::getAlignmentMod<GPUCA_MEMALIGN>(memorypoolend));
    if (retVal < minSize) {
      retVal = minSize;
    }
    retVal += GPUProcessor::getAlignment<GPUCA_MEMALIGN>(retVal);
    memorypoolend = (char*)memorypoolend - retVal;
    ptr = memorypoolend;
    (res->*setPtr)(ptr);
  } else {
    ptr = memorypool;
    memorypool = (char*)((res->*setPtr)(ptr));
    retVal = (char*)memorypool - (char*)ptr;
    if (retVal < minSize) {
      retVal = minSize;
      memorypool = (char*)ptr + minSize;
    }
    memorypool = (void*)((char*)memorypool + GPUProcessor::getAlignment<GPUCA_MEMALIGN>(memorypool));
  }
  if (memorypoolend ? (memorypool > memorypoolend) : ((size_t)((char*)memorypool - (char*)memorybase) > memorysize)) {
    std::cout << "Memory pool size exceeded (" << res->mName << ": " << (memorypoolend ? (memorysize + ((char*)memorypool - (char*)memorypoolend)) : (char*)memorypool - (char*)memorybase) << " < " << memorysize << "\n";
    throw std::bad_alloc();
  }
  if (mDeviceProcessingSettings.allocDebugLevel >= 2) {
    std::cout << "Allocated " << res->mName << ": " << retVal << " - available: " << (memorypoolend ? ((char*)memorypoolend - (char*)memorypool) : (memorysize - ((char*)memorypool - (char*)memorybase))) << "\n";
  }
  return retVal;
}

size_t GPUReconstruction::AllocateRegisteredMemory(short ires, GPUOutputControl* control)
{
  GPUMemoryResource* res = &mMemoryResources[ires];
  if ((res->mType & GPUMemoryResource::MEMORY_PERMANENT) && res->mPtr != nullptr) {
    ResetRegisteredMemoryPointers(ires);
  } else if (mDeviceProcessingSettings.memoryAllocationStrategy == GPUMemoryResource::ALLOCATION_INDIVIDUAL && (control == nullptr || control->OutputType == GPUOutputControl::AllocateInternal)) {
    if (!(res->mType & GPUMemoryResource::MEMORY_EXTERNAL)) {
      if (res->mPtrDevice && res->mReuse < 0) {
        operator delete(res->mPtrDevice GPUCA_OPERATOR_NEW_ALIGNMENT);
      }
      res->mSize = std::max((size_t)res->SetPointers((void*)1) - 1, res->mOverrideSize);
      if (res->mReuse >= 0) {
        if (res->mSize > mMemoryResources[res->mReuse].mSize) {
          GPUError("Invalid reuse, insufficient size: %lld < %lld", (long long int)mMemoryResources[res->mReuse].mSize, (long long int)res->mSize);
          throw std::bad_alloc();
        }
        res->mPtrDevice = mMemoryResources[res->mReuse].mPtrDevice;
      } else {
        res->mPtrDevice = operator new(res->mSize + GPUCA_BUFFER_ALIGNMENT GPUCA_OPERATOR_NEW_ALIGNMENT);
      }
      res->mPtr = GPUProcessor::alignPointer<GPUCA_BUFFER_ALIGNMENT>(res->mPtrDevice);
      res->SetPointers(res->mPtr);
      if (mDeviceProcessingSettings.allocDebugLevel >= 2) {
        std::cout << (res->mReuse >= 0 ? "Reused " : "Allocated ") << res->mName << ": " << res->mSize << "\n";
      }
    }
  } else {
    if (res->mPtr != nullptr) {
      GPUError("Double allocation! (%s)", res->mName);
      throw std::bad_alloc();
    }
    if ((!IsGPU() || (res->mType & GPUMemoryResource::MEMORY_HOST) || mDeviceProcessingSettings.keepDisplayMemory) && !(res->mType & GPUMemoryResource::MEMORY_EXTERNAL)) { // keepAllMemory --> keepDisplayMemory
      if (control && control->OutputType == GPUOutputControl::UseExternalBuffer) {
        if (control->OutputAllocator) {
          res->mSize = std::max((size_t)res->SetPointers((void*)1) - 1, res->mOverrideSize);
          res->mPtr = control->OutputAllocator(res->mSize);
          res->SetPointers(res->mPtr);
        } else {
          void* dummy = nullptr;
          res->mSize = AllocateRegisteredMemoryHelper(res, res->mPtr, control->OutputPtr, control->OutputBase, control->OutputMaxSize, &GPUMemoryResource::SetPointers, dummy);
        }
      } else {
        res->mSize = AllocateRegisteredMemoryHelper(res, res->mPtr, mHostMemoryPool, mHostMemoryBase, mHostMemorySize, &GPUMemoryResource::SetPointers, mHostMemoryPoolEnd);
      }
    }
    if (IsGPU() && (res->mType & GPUMemoryResource::MEMORY_GPU)) {
      if (res->mProcessor->mDeviceProcessor == nullptr) {
        GPUError("Device Processor not set (%s)", res->mName);
        throw std::bad_alloc();
      }
      size_t size = AllocateRegisteredMemoryHelper(res, res->mPtrDevice, mDeviceMemoryPool, mDeviceMemoryBase, mDeviceMemorySize, &GPUMemoryResource::SetDevicePointers, mDeviceMemoryPoolEnd);

      if (!(res->mType & GPUMemoryResource::MEMORY_HOST) || (res->mType & GPUMemoryResource::MEMORY_EXTERNAL)) {
        res->mSize = size;
      } else if (size != res->mSize) {
        GPUError("Inconsistent device memory allocation (%s)", res->mName);
        throw std::bad_alloc();
      }
    }
    UpdateMaxMemoryUsed();
  }
  return res->mReuse >= 0 ? 0 : res->mSize;
}

void* GPUReconstruction::AllocateUnmanagedMemory(size_t size, int type)
{
  if (type != GPUMemoryResource::MEMORY_HOST && (!IsGPU() || type != GPUMemoryResource::MEMORY_GPU)) {
    throw std::bad_alloc();
  }
  if (mDeviceProcessingSettings.memoryAllocationStrategy == GPUMemoryResource::ALLOCATION_INDIVIDUAL) {
    mUnmanagedChunks.emplace_back(new char[size + GPUCA_BUFFER_ALIGNMENT]);
    return GPUProcessor::alignPointer<GPUCA_BUFFER_ALIGNMENT>(mUnmanagedChunks.back().get());
  } else {
    void* pool = type == GPUMemoryResource::MEMORY_GPU ? mDeviceMemoryPool : mHostMemoryPool;
    void* poolend = type == GPUMemoryResource::MEMORY_GPU ? mDeviceMemoryPoolEnd : mHostMemoryPoolEnd;
    char* retVal;
    GPUProcessor::computePointerWithAlignment(pool, retVal, size);
    if (pool > poolend) {
      throw std::bad_alloc();
    }
    UpdateMaxMemoryUsed();
    return retVal;
  }
}

void* GPUReconstruction::AllocateVolatileDeviceMemory(size_t size)
{
  if (mVolatileMemoryStart == nullptr) {
    mVolatileMemoryStart = mDeviceMemoryPool;
  }
  char* retVal;
  GPUProcessor::computePointerWithAlignment(mDeviceMemoryPool, retVal, size);
  if (mDeviceMemoryPool > mDeviceMemoryPoolEnd) {
    throw std::bad_alloc();
  }
  UpdateMaxMemoryUsed();
  return retVal;
}

void GPUReconstruction::ResetRegisteredMemoryPointers(GPUProcessor* proc)
{
  for (unsigned int i = 0; i < mMemoryResources.size(); i++) {
    if (proc == nullptr || mMemoryResources[i].mProcessor == proc) {
      ResetRegisteredMemoryPointers(i);
    }
  }
}

void GPUReconstruction::ResetRegisteredMemoryPointers(short ires)
{
  GPUMemoryResource* res = &mMemoryResources[ires];
  if (!(res->mType & GPUMemoryResource::MEMORY_EXTERNAL)) {
    res->SetPointers(res->mPtr);
  }
  if (IsGPU() && (res->mType & GPUMemoryResource::MEMORY_GPU)) {
    res->SetDevicePointers(res->mPtrDevice);
  }
}

void GPUReconstruction::FreeRegisteredMemory(GPUProcessor* proc, bool freeCustom, bool freePermanent)
{
  for (unsigned int i = 0; i < mMemoryResources.size(); i++) {
    if ((proc == nullptr || mMemoryResources[i].mProcessor == proc) && (freeCustom || !(mMemoryResources[i].mType & GPUMemoryResource::MEMORY_CUSTOM)) && (freePermanent || !(mMemoryResources[i].mType & GPUMemoryResource::MEMORY_PERMANENT))) {
      FreeRegisteredMemory(i);
    }
  }
}

void GPUReconstruction::FreeRegisteredMemory(short ires)
{
  GPUMemoryResource* res = &mMemoryResources[ires];
  if (mDeviceProcessingSettings.allocDebugLevel >= 2 && (res->mPtr || res->mPtrDevice)) {
    std::cout << "Freeing " << res->mName << ": size " << res->mSize << " (reused " << res->mReuse << ")\n";
  }
  if (mDeviceProcessingSettings.memoryAllocationStrategy == GPUMemoryResource::ALLOCATION_INDIVIDUAL && res->mReuse < 0) {
    operator delete(res->mPtrDevice GPUCA_OPERATOR_NEW_ALIGNMENT);
  }
  res->mPtr = nullptr;
  res->mPtrDevice = nullptr;
}

void GPUReconstruction::ReturnVolatileDeviceMemory()
{
  if (mVolatileMemoryStart) {
    mDeviceMemoryPool = mVolatileMemoryStart;
    mVolatileMemoryStart = nullptr;
  }
}

void GPUReconstruction::PushNonPersistentMemory()
{
  mNonPersistentMemoryStack.emplace_back(mHostMemoryPoolEnd, mDeviceMemoryPoolEnd);
}

void GPUReconstruction::PopNonPersistentMemory(RecoStep step)
{
  if (mDeviceProcessingSettings.debugLevel >= 3 || mDeviceProcessingSettings.allocDebugLevel) {
    printf("Allocated memory after %30s: %'13lld (non temporary %'13lld, blocked %'13lld)\n", GPUDataTypes::RECO_STEP_NAMES[getRecoStepNum(step, true)], ptrDiff(mDeviceMemoryPool, mDeviceMemoryBase) + ptrDiff((char*)mDeviceMemoryBase + mDeviceMemorySize, mDeviceMemoryPoolEnd), ptrDiff(mDeviceMemoryPool, mDeviceMemoryBase), mDeviceMemoryPoolBlocked == nullptr ? 0ll : ptrDiff((char*)mDeviceMemoryBase + mDeviceMemorySize, mDeviceMemoryPoolBlocked));
  }
  if (mDeviceProcessingSettings.keepDisplayMemory || mDeviceProcessingSettings.disableMemoryReuse) {
    return;
  }
  mHostMemoryPoolEnd = mNonPersistentMemoryStack.back().first;
  mDeviceMemoryPoolEnd = mNonPersistentMemoryStack.back().second;
  mNonPersistentMemoryStack.pop_back();
}

void GPUReconstruction::BlockStackedMemory(GPUReconstruction* rec)
{
  if (mHostMemoryPoolBlocked || mDeviceMemoryPoolBlocked) {
    throw std::runtime_error("temporary memory stack already blocked");
  }
  mHostMemoryPoolBlocked = rec->mHostMemoryPoolEnd;
  mDeviceMemoryPoolBlocked = rec->mDeviceMemoryPoolEnd;
}

void GPUReconstruction::UnblockStackedMemory()
{
  if (mNonPersistentMemoryStack.size()) {
    throw std::runtime_error("cannot unblock while there is stacked memory");
  }
  mHostMemoryPoolEnd = (char*)mHostMemoryBase + mHostMemorySize;
  mDeviceMemoryPoolEnd = (char*)mDeviceMemoryBase + mDeviceMemorySize;
  mHostMemoryPoolBlocked = nullptr;
  mDeviceMemoryPoolBlocked = nullptr;
}

void GPUReconstruction::SetMemoryExternalInput(short res, void* ptr)
{
  mMemoryResources[res].mPtr = ptr;
}

void GPUReconstruction::ClearAllocatedMemory(bool clearOutputs)
{
  for (unsigned int i = 0; i < mMemoryResources.size(); i++) {
    if (!(mMemoryResources[i].mType & GPUMemoryResource::MEMORY_PERMANENT) && (clearOutputs || !(mMemoryResources[i].mType & GPUMemoryResource::MEMORY_OUTPUT))) {
      FreeRegisteredMemory(i);
    }
  }
  mHostMemoryPool = GPUProcessor::alignPointer<GPUCA_MEMALIGN>(mHostMemoryPermanent);
  mDeviceMemoryPool = GPUProcessor::alignPointer<GPUCA_MEMALIGN>(mDeviceMemoryPermanent);
  mUnmanagedChunks.clear();
  mVolatileMemoryStart = nullptr;
  mNonPersistentMemoryStack.clear();
  mHostMemoryPoolEnd = mHostMemoryPoolBlocked ? mHostMemoryPoolBlocked : ((char*)mHostMemoryBase + mHostMemorySize);
  mDeviceMemoryPoolEnd = mDeviceMemoryPoolBlocked ? mDeviceMemoryPoolBlocked : ((char*)mDeviceMemoryBase + mDeviceMemorySize);
}

void GPUReconstruction::UpdateMaxMemoryUsed()
{
  mHostMemoryUsedMax = std::max<size_t>(mHostMemoryUsedMax, ptrDiff(mHostMemoryPool, mHostMemoryBase) + ptrDiff((char*)mHostMemoryBase + mHostMemorySize, mHostMemoryPoolEnd));
  mDeviceMemoryUsedMax = std::max<size_t>(mDeviceMemoryUsedMax, ptrDiff(mDeviceMemoryPool, mDeviceMemoryBase) + ptrDiff((char*)mDeviceMemoryBase + mDeviceMemorySize, mDeviceMemoryPoolEnd));
}

void GPUReconstruction::PrintMemoryMax()
{
  printf("Maximum Memory Allocation: Host %'lld / Device %'lld\n", (long long int)mHostMemoryUsedMax, (long long int)mDeviceMemoryUsedMax);
}

void GPUReconstruction::PrintMemoryOverview()
{
  if (mDeviceProcessingSettings.memoryAllocationStrategy == GPUMemoryResource::ALLOCATION_GLOBAL) {
    printf("Memory Allocation: Host %'lld / %'lld (Permanent %'lld), Device %'lld / %'lld, (Permanent %'lld) %d chunks\n",
           ptrDiff(mHostMemoryPool, mHostMemoryBase) + ptrDiff((char*)mHostMemoryBase + mHostMemorySize, mHostMemoryPoolEnd), (long long int)mHostMemorySize, ptrDiff(mHostMemoryPermanent, mHostMemoryBase),
           ptrDiff(mDeviceMemoryPool, mDeviceMemoryBase) + ptrDiff((char*)mDeviceMemoryBase + mDeviceMemorySize, mDeviceMemoryPoolEnd), (long long int)mDeviceMemorySize, ptrDiff(mDeviceMemoryPermanent, mDeviceMemoryBase), (int)mMemoryResources.size());
  }
}

void GPUReconstruction::PrintMemoryStatistics()
{
  std::map<std::string, std::array<size_t, 3>> sizes;
  for (unsigned int i = 0; i < mMemoryResources.size(); i++) {
    auto& res = mMemoryResources[i];
    if (res.mReuse >= 0) {
      continue;
    }
    auto& x = sizes[res.mName];
    if (res.mPtr) {
      x[0] += res.mSize;
    }
    if (res.mPtrDevice) {
      x[1] += res.mSize;
    }
    if (res.mType & GPUMemoryResource::MemoryType::MEMORY_PERMANENT) {
      x[2] = 1;
    }
  }
  printf("%59s CPU / %9s GPU\n", "", "");
  for (auto it = sizes.begin(); it != sizes.end(); it++) {
    printf("Allocation %30s %s: Size %'13lld / %'13lld\n", it->first.c_str(), it->second[2] ? "P" : " ", (long long int)it->second[0], (long long int)it->second[1]);
  }
  PrintMemoryOverview();
  for (unsigned int i = 0; i < mChains.size(); i++) {
    mChains[i]->PrintMemoryStatistics();
  }
}

template <class T>
static inline int getStepNum(T step, bool validCheck, int N, const char* err = "Invalid step num")
{
  static_assert(sizeof(step) == sizeof(unsigned int), "Invalid step enum size");
  int retVal = 8 * sizeof(unsigned int) - 1 - CAMath::Clz((unsigned int)step);
  if ((unsigned int)step == 0 || retVal >= N) {
    if (!validCheck) {
      return -1;
    }
    throw std::runtime_error("Invalid General Step");
  }
  return retVal;
}

int GPUReconstruction::getRecoStepNum(RecoStep step, bool validCheck) { return getStepNum(step, validCheck, GPUDataTypes::N_RECO_STEPS, "Invalid Reco Step"); }
int GPUReconstruction::getGeneralStepNum(GeneralStep step, bool validCheck) { return getStepNum(step, validCheck, GPUDataTypes::N_GENERAL_STEPS, "Invalid General Step"); }

void GPUReconstruction::RunPipelineWorker()
{
  if (!mInitialized || !mDeviceProcessingSettings.doublePipeline || mMaster != nullptr || !mSlaves.size()) {
    throw std::invalid_argument("Cannot start double pipeline mode");
  }
  if (mDeviceProcessingSettings.debugLevel >= 3) {
    GPUInfo("Pipeline worker started");
  }
  bool terminate = false;
  while (!terminate) {
    {
      std::unique_lock<std::mutex> lk(mPipelineContext->mutex);
      mPipelineContext->cond.wait(lk, [this] { return this->mPipelineContext->queue.size() > 0; });
    }
    GPUReconstructionPipelineQueue* q;
    {
      std::lock_guard<std::mutex> lk(mPipelineContext->mutex);
      q = mPipelineContext->queue.front();
      mPipelineContext->queue.pop();
    }
    if (q->op == 1) {
      terminate = 1;
    } else {
      q->retVal = q->chain->RunChain();
    }
    std::lock_guard<std::mutex> lk(q->m);
    q->done = true;
    q->c.notify_one();
  }
  if (mDeviceProcessingSettings.debugLevel >= 3) {
    GPUInfo("Pipeline worker ended");
  }
}

void GPUReconstruction::TerminatePipelineWorker()
{
  EnqueuePipeline(true);
}

int GPUReconstruction::EnqueuePipeline(bool terminate)
{
  GPUReconstruction* rec = mMaster ? mMaster : this;
  std::unique_ptr<GPUReconstructionPipelineQueue> qu(new GPUReconstructionPipelineQueue);
  GPUReconstructionPipelineQueue* q = qu.get();
  q->chain = terminate ? nullptr : mChains[0].get();
  q->op = terminate ? 1 : 0;
  std::unique_lock<std::mutex> lkdone(q->m);
  {
    std::lock_guard<std::mutex> lkpipe(rec->mPipelineContext->mutex);
    if (rec->mPipelineContext->terminate) {
      throw std::runtime_error("Must not enqueue work after termination request");
    }
    rec->mPipelineContext->queue.push(q);
    rec->mPipelineContext->terminate = terminate;
    rec->mPipelineContext->cond.notify_one();
  }
  q->c.wait(lkdone, [&q]() { return q->done; });
  if (q->retVal) {
    return q->retVal;
  }
  return mChains[0]->FinalizePipelinedProcessing();
}

GPUChain* GPUReconstruction::GetNextChainInQueue()
{
  GPUReconstruction* rec = mMaster ? mMaster : this;
  std::lock_guard<std::mutex> lk(rec->mPipelineContext->mutex);
  return rec->mPipelineContext->queue.size() && rec->mPipelineContext->queue.front()->op == 0 ? rec->mPipelineContext->queue.front()->chain : nullptr;
}

void GPUReconstruction::PrepareEvent()
{
  ClearAllocatedMemory(true);
  for (unsigned int i = 0; i < mChains.size(); i++) {
    mChains[i]->PrepareEvent();
  }
  for (unsigned int i = 0; i < mProcessors.size(); i++) {
    if (mProcessors[i].proc->mAllocateAndInitializeLate) {
      continue;
    }
    (mProcessors[i].proc->*(mProcessors[i].SetMaxData))(mHostConstantMem->ioPtrs);
    if (mProcessors[i].proc->mDeviceProcessor) {
      (mProcessors[i].proc->mDeviceProcessor->*(mProcessors[i].SetMaxData))(mHostConstantMem->ioPtrs);
    }
  }
  ComputeReuseMax(nullptr);
  AllocateRegisteredMemory(nullptr);
}

int GPUReconstruction::CheckErrorCodes()
{
  int retVal = 0;
  for (unsigned int i = 0; i < mChains.size(); i++) {
    if (mChains[i]->CheckErrorCodes()) {
      retVal++;
    }
  }
  return retVal;
}

void GPUReconstruction::DumpSettings(const char* dir)
{
  std::string f;
  f = dir;
  f += "settings.dump";
  DumpStructToFile(&mEventSettings, f.c_str());
  for (unsigned int i = 0; i < mChains.size(); i++) {
    mChains[i]->DumpSettings(dir);
  }
}

void GPUReconstruction::UpdateEventSettings(const GPUSettingsEvent* e, const GPUSettingsDeviceProcessing* p)
{
  param().UpdateEventSettings(e, p);
  if (mInitialized) {
    WriteConstantParams();
  }
}

int GPUReconstruction::ReadSettings(const char* dir)
{
  std::string f;
  f = dir;
  f += "settings.dump";
  mEventSettings.SetDefaults();
  if (ReadStructFromFile(f.c_str(), &mEventSettings)) {
    return 1;
  }
  param().UpdateEventSettings(&mEventSettings);
  for (unsigned int i = 0; i < mChains.size(); i++) {
    mChains[i]->ReadSettings(dir);
  }
  return 0;
}

void GPUReconstruction::SetSettings(float solenoidBz)
{
  GPUSettingsEvent ev;
  ev.SetDefaults();
  ev.solenoidBz = solenoidBz;
  SetSettings(&ev, nullptr, nullptr);
}

void GPUReconstruction::SetSettings(const GPUSettingsEvent* settings, const GPUSettingsRec* rec, const GPUSettingsDeviceProcessing* proc, const GPURecoStepConfiguration* workflow)
{
  if (mInitialized) {
    GPUError("Cannot update settings while initialized");
    throw std::runtime_error("Settings updated while initialized");
  }
  mEventSettings = *settings;
  if (proc) {
    mDeviceProcessingSettings = *proc;
  }
  if (workflow) {
    mRecoSteps = workflow->steps;
    mRecoStepsGPU &= workflow->stepsGPUMask;
    mRecoStepsInputs = workflow->inputs;
    mRecoStepsOutputs = workflow->outputs;
  }
  param().SetDefaults(&mEventSettings, rec, proc, workflow);
}

void GPUReconstruction::SetOutputControl(void* ptr, size_t size)
{
  GPUOutputControl outputControl;
  outputControl.set(ptr, size);
  SetOutputControl(outputControl);
}

std::unique_ptr<GPUReconstruction::GPUThreadContext> GPUReconstruction::GetThreadContext() { return std::unique_ptr<GPUReconstruction::GPUThreadContext>(new GPUThreadContext); }

GPUReconstruction* GPUReconstruction::CreateInstance(DeviceType type, bool forceType, GPUReconstruction* master)
{
  GPUSettingsProcessing cfg;
  cfg.SetDefaults();
  cfg.deviceType = type;
  cfg.forceDeviceType = forceType;
  cfg.master = master;
  return CreateInstance(cfg);
}

GPUReconstruction* GPUReconstruction::CreateInstance(const GPUSettingsProcessing& cfg)
{
  GPUReconstruction* retVal = nullptr;
  unsigned int type = cfg.deviceType;
  if (type == DeviceType::CPU) {
    retVal = GPUReconstruction_Create_CPU(cfg);
  } else if (type == DeviceType::CUDA) {
    if ((retVal = sLibCUDA->GetPtr(cfg))) {
      retVal->mMyLib = sLibCUDA;
    }
  } else if (type == DeviceType::HIP) {
    if ((retVal = sLibHIP->GetPtr(cfg))) {
      retVal->mMyLib = sLibHIP;
    }
  } else if (type == DeviceType::OCL) {
    if ((retVal = sLibOCL->GetPtr(cfg))) {
      retVal->mMyLib = sLibOCL;
    }
  } else if (type == DeviceType::OCL2) {
    if ((retVal = sLibOCL2->GetPtr(cfg))) {
      retVal->mMyLib = sLibOCL2;
    }
  } else {
    GPUError("Error: Invalid device type %u", type);
    return nullptr;
  }

  if (retVal == nullptr) {
    if (cfg.forceDeviceType) {
      GPUError("Error: Could not load GPUReconstruction for specified device: %s (%u)", DEVICE_TYPE_NAMES[type], type);
    } else {
      GPUError("Could not load GPUReconstruction for device type %s (%u), falling back to CPU version", DEVICE_TYPE_NAMES[type], type);
      GPUSettingsProcessing cfg2 = cfg;
      cfg2.deviceType = DeviceType::CPU;
      retVal = CreateInstance(cfg2);
    }
  } else {
    GPUInfo("Created GPUReconstruction instance for device type %s (%u) %s", DEVICE_TYPE_NAMES[type], type, cfg.master ? " (slave)" : "");
  }

  return retVal;
}

GPUReconstruction* GPUReconstruction::CreateInstance(const char* type, bool forceType, GPUReconstruction* master)
{
  DeviceType t = GetDeviceType(type);
  if (t == DeviceType::INVALID_DEVICE) {
    GPUError("Invalid device type: %s", type);
    return nullptr;
  }
  return CreateInstance(t, forceType, master);
}

GPUReconstruction::DeviceType GPUReconstruction::GetDeviceType(const char* type)
{
  for (unsigned int i = 1; i < sizeof(DEVICE_TYPE_NAMES) / sizeof(DEVICE_TYPE_NAMES[0]); i++) {
    if (strcmp(DEVICE_TYPE_NAMES[i], type) == 0) {
      return (DeviceType)i;
    }
  }
  return DeviceType::INVALID_DEVICE;
}

#ifdef _WIN32
#define LIBRARY_EXTENSION ".dll"
#define LIBRARY_TYPE HMODULE
#define LIBRARY_LOAD(name) LoadLibraryEx(name, nullptr, nullptr)
#define LIBRARY_CLOSE FreeLibrary
#define LIBRARY_FUNCTION GetProcAddress
#else
#define LIBRARY_EXTENSION ".so"
#define LIBRARY_TYPE void*
#define LIBRARY_LOAD(name) dlopen(name, RTLD_NOW)
#define LIBRARY_CLOSE dlclose
#define LIBRARY_FUNCTION dlsym
#endif

#if defined(GPUCA_ALIROOT_LIB)
#define LIBRARY_PREFIX "Ali"
#elif defined(GPUCA_O2_LIB)
#define LIBRARY_PREFIX "O2"
#else
#define LIBRARY_PREFIX ""
#endif

std::shared_ptr<GPUReconstruction::LibraryLoader> GPUReconstruction::sLibCUDA(new GPUReconstruction::LibraryLoader("lib" LIBRARY_PREFIX "GPUTracking"
                                                                                                                   "CUDA" LIBRARY_EXTENSION,
                                                                                                                   "GPUReconstruction_Create_"
                                                                                                                   "CUDA"));
std::shared_ptr<GPUReconstruction::LibraryLoader> GPUReconstruction::sLibHIP(new GPUReconstruction::LibraryLoader("lib" LIBRARY_PREFIX "GPUTracking"
                                                                                                                  "HIP" LIBRARY_EXTENSION,
                                                                                                                  "GPUReconstruction_Create_"
                                                                                                                  "HIP"));
std::shared_ptr<GPUReconstruction::LibraryLoader> GPUReconstruction::sLibOCL(new GPUReconstruction::LibraryLoader("lib" LIBRARY_PREFIX "GPUTracking"
                                                                                                                  "OCL" LIBRARY_EXTENSION,
                                                                                                                  "GPUReconstruction_Create_"
                                                                                                                  "OCL"));

std::shared_ptr<GPUReconstruction::LibraryLoader> GPUReconstruction::sLibOCL2(new GPUReconstruction::LibraryLoader("lib" LIBRARY_PREFIX "GPUTracking"
                                                                                                                   "OCL2" LIBRARY_EXTENSION,
                                                                                                                   "GPUReconstruction_Create_"
                                                                                                                   "OCL2"));

GPUReconstruction::LibraryLoader::LibraryLoader(const char* lib, const char* func) : mLibName(lib), mFuncName(func), mGPULib(nullptr), mGPUEntry(nullptr) {}

GPUReconstruction::LibraryLoader::~LibraryLoader() { CloseLibrary(); }

int GPUReconstruction::LibraryLoader::LoadLibrary()
{
  static std::mutex mut;
  std::lock_guard<std::mutex> lock(mut);

  if (mGPUEntry) {
    return 0;
  }

  LIBRARY_TYPE hGPULib;
  hGPULib = LIBRARY_LOAD(mLibName);
  if (hGPULib == nullptr) {
#ifndef _WIN32
    GPUImportant("The following error occured during dlopen: %s", dlerror());
#endif
    GPUError("Error Opening cagpu library for GPU Tracker (%s)", mLibName);
    return 1;
  } else {
    void* createFunc = LIBRARY_FUNCTION(hGPULib, mFuncName);
    if (createFunc == nullptr) {
      GPUError("Error fetching entry function in GPU library\n");
      LIBRARY_CLOSE(hGPULib);
      return 1;
    } else {
      mGPULib = (void*)(size_t)hGPULib;
      mGPUEntry = createFunc;
      GPUInfo("GPU Tracker library loaded and GPU tracker object created sucessfully");
    }
  }
  return 0;
}

GPUReconstruction* GPUReconstruction::LibraryLoader::GetPtr(const GPUSettingsProcessing& cfg)
{
  if (LoadLibrary()) {
    return nullptr;
  }
  if (mGPUEntry == nullptr) {
    return nullptr;
  }
  GPUReconstruction* (*tmp)(const GPUSettingsProcessing& cfg) = (GPUReconstruction * (*)(const GPUSettingsProcessing& cfg)) mGPUEntry;
  return tmp(cfg);
}

int GPUReconstruction::LibraryLoader::CloseLibrary()
{
  if (mGPUEntry == nullptr) {
    return 1;
  }
  LIBRARY_CLOSE((LIBRARY_TYPE)(size_t)mGPULib);
  mGPULib = nullptr;
  mGPUEntry = nullptr;
  return 0;
}
