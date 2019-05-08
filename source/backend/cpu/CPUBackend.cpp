//
//  CPUBackend.cpp
//  MNN
//
//  Created by MNN on 2018/07/06.
//  Copyright © 2018, Alibaba Group Holding Limited
//

#include "CPUBackend.hpp"
#include <mutex>
#include "BufferAllocator.hpp"
#include "CPUConcat.hpp"
#include "CPUTensorConvert.hpp"
#include "CommonOptFunction.h"
#include "TensorUtils.hpp"
#ifdef _OPENMP
#include <omp.h>
#endif // _OPENMP
#include "CPURuntime.hpp"
#include "CPUOPRegister.hpp"

#define MAX_THREAD_NUMBER 32

//#define MNN_DUMP_MEMORY_USAGE
namespace MNN {
static inline std::map<OpType, CPUBackend::Creator*>* getCreatorMap() {
    static std::once_flag of;
    static std::map<OpType, CPUBackend::Creator*>* ret = nullptr;
    std::call_once(of, [&]() { ret = new std::map<OpType, CPUBackend::Creator*>; });
    return ret;
}

bool CPUBackend::addCreator(OpType t, Creator* c) {
    auto map = getCreatorMap();
    if (map->find(t) != map->end()) {
        MNN_PRINT("Error: %d type has be added\n", t);
        return false;
    }
    map->insert(std::make_pair(t, c));
    return true;
}

CPUBackend::CPUBackend(int numberThread, BackendConfig::MemoryMode memory, BackendConfig::PowerMode power)
    : Backend(MNN_FORWARD_CPU), mThreadNumber(numberThread), mMemory(memory), mPower(power) {
    mThreadNumber = std::max(1, mThreadNumber);
    mThreadNumber = std::min(mThreadNumber, MAX_THREAD_NUMBER);
    mDynamicAllocator.reset(new BufferAllocator);
    mStaticAllocator.reset(new BufferAllocator);
    switch (power) {
        case BackendConfig::Power_Low:
            MNNSetCPUThreadsMode(MNN_CPU_MODE_LITTLE);
            break;
        case BackendConfig::Power_High:
            MNNSetCPUThreadsMode(MNN_CPU_MODE_POWER_FRI);
            break;
        default:
            break;
    }
}

CPUBackend::~CPUBackend() {
}

void CPUBackend::onExecuteBegin() const {
#ifdef MNN_DUMP_MEMORY_USAGE
    {
        auto dynamicMemoryInMB = mDynamicAllocator->totalSize() / 1024.0f / 1024.0f;
        FUNC_PRINT_ALL(dynamicMemoryInMB, f);
        auto staticMemoryInMB = mStaticAllocator->totalSize() / 1024.0f / 1024.0f;
        FUNC_PRINT_ALL(staticMemoryInMB, f);
    }
#endif
// setCPUThreadsMode(MNN_CPU_MODE_POWER_FRI);
#ifdef _OPENMP
    omp_set_dynamic(0);
    omp_set_num_threads(mThreadNumber);
#endif
}

bool CPUBackend::onAcquireBuffer(const MNN::Tensor* nativeTensorConst, StorageType storageType) {
    auto nativeTensor = (Tensor*)nativeTensorConst;
    auto& buffer      = nativeTensor->buffer();

    auto size = nativeTensor->size();

    // MNN_PRINT("Acquire size = %d\n", size);
    if (size <= 0) {
        MNN_ASSERT(false);
        return false;
    }
    switch (storageType) {
        case STATIC: {
            buffer.host = (uint8_t*)mStaticAllocator->alloc(size, true);
            break;
        }
        case DYNAMIC: {
            buffer.host = (uint8_t*)(mDynamicAllocator->alloc(size, false));
            break;
        }
        case DYNAMIC_SEPERATE: {
            buffer.host = (uint8_t*)(mDynamicAllocator->alloc(size, true));
            break;
        }
        default:
            break;
    }
    if (nullptr == buffer.host) {
        MNN_ERROR("Alloc buffer error for cpu backend\n");
        return false;
    }
    if (buffer.type.code == halide_type_handle) {
        ::memset(buffer.host, 0, size);
    }
    return true;
}

bool CPUBackend::onReleaseBuffer(const MNN::Tensor* nativeTensor, StorageType storageType) {
    if (nullptr == nativeTensor->buffer().host) {
        return false;
    }
    if (STATIC == storageType) {
        mStaticAllocator->free(nativeTensor->buffer().host, true);
        return true;
    }
    if (DYNAMIC_SEPERATE == storageType) {
        return true;
    }
    mDynamicAllocator->free(nativeTensor->buffer().host);
    return true;
}

/// get execution
Execution* CPUBackend::onCreate(const std::vector<Tensor*>& inputs, const std::vector<Tensor*>& outputs,
                                const MNN::Op* op) {
    auto map  = getCreatorMap();
    auto iter = map->find(op->type());
    if (iter == map->end()) {
        MNN_PRINT("Don't support type %d, %s\n", op->type(), op->name()->c_str());
        return nullptr;
    }
    auto exe = iter->second->onCreate(inputs, outputs, op, this);
    if (nullptr == exe) {
        MNN_PRINT("The Creator Don't support type %d, %s\n", op->type(), op->name()->c_str());
        return nullptr;
    }
    return exe;
}

bool CPUBackend::onAllocateBuffer() {
    return true;
}

bool CPUBackend::onClearBuffer() {
    mDynamicAllocator->release();
    return true;
}

void CPUBackend::onCopyBuffer(const Tensor* srcTensor, const Tensor* dstTensor) const {
    auto& srcBuffer = srcTensor->buffer();
    auto& dstBuffer = dstTensor->buffer();

    MNN_ASSERT(srcBuffer.dimensions == dstBuffer.dimensions);
    MNN_ASSERT(srcBuffer.type == dstBuffer.type);
    if (srcTensor->getDimensionType() == dstTensor->getDimensionType()) {
        for (int i = 0; i < srcBuffer.dimensions; ++i) {
            MNN_ASSERT(srcBuffer.dim[i].extent <= dstBuffer.dim[i].extent);
        }
    }
    // Don't support cpu to gpu / gpu to cpu
    MNN_ASSERT(srcBuffer.host != nullptr && dstBuffer.host != nullptr);

    int sizeofType = srcBuffer.type.bytes();
    if (srcBuffer.dimensions <= 1 ||
        TensorUtils::getDescribe(srcTensor)->dimensionFormat == TensorUtils::getDescribe(dstTensor)->dimensionFormat) {
        ::memcpy(dstBuffer.host, srcBuffer.host, srcTensor->size());
        return;
    }

    if (srcTensor->getDimensionType() == Tensor::TENSORFLOW || dstTensor->getDimensionType() == Tensor::TENSORFLOW) {
        CPUTensorConverter::convert(srcTensor, dstTensor);
        return;
    }

    // NCHW to NC4HW4 or NC4HW4 to NCHW
    int area = 1;
    for (int axis = 2; axis < srcBuffer.dimensions; ++axis) {
        area *= srcBuffer.dim[axis].extent;
    }
    if (sizeofType != 4) {
        MNN_ERROR("Please use NHWC (or Tensorflow dimension type) for copy\n");
        return;
    }
    if (srcBuffer.dimensions > 1 && srcBuffer.dim[1].flags == Tensor::REORDER_4) {
        MNN_ASSERT(sizeofType == 4);
        for (int i = 0; i < srcBuffer.dim[0].extent; ++i) {
            MNNUnpackC4((float*)dstBuffer.host + dstBuffer.dim[0].stride * i,
                        (const float*)srcBuffer.host + srcBuffer.dim[0].stride * i, area, srcBuffer.dim[1].extent);
        }
        return;
    }

    if (dstBuffer.dimensions > 1 && dstBuffer.dim[1].flags == Tensor::REORDER_4) {
        MNN_ASSERT(sizeofType == 4);
        for (int i = 0; i < srcBuffer.dim[0].extent; ++i) {
            MNNPackC4((float*)dstBuffer.host + dstBuffer.dim[0].stride * i,
                      (const float*)srcBuffer.host + srcBuffer.dim[0].stride * i, area, srcBuffer.dim[1].extent);
        }
        return;
    }
}

struct CPUBackendCreator : BackendCreator {
    Backend* onCreate(const Backend::Info& info) const override {
        auto power  = BackendConfig::Power_Normal;
        auto memory = BackendConfig::Memory_Normal;
        if (nullptr != info.user) {
            power  = info.user->power;
            memory = info.user->memory;
        }
        static std::once_flag s_flag;
        std::call_once(s_flag, [&]() {
            registerCPUOps();
        });

        return new CPUBackend(info.numThread, memory, power);
    }
};

void registerCPUBackendCreator() {
    MNNInsertExtraBackendCreator(MNN_FORWARD_CPU, new CPUBackendCreator);
};
} // namespace MNN
