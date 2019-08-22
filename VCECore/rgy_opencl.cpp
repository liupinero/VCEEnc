﻿// -----------------------------------------------------------------------------------------
// NVEnc by rigaya
// -----------------------------------------------------------------------------------------
//
// The MIT License
//
// Copyright (c) 2014-2016 rigaya
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// ------------------------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <tchar.h>
#include <vector>
#include <atomic>
#include <fstream>
#define CL_EXTERN
#include "rgy_opencl.h"

#if ENABLE_OPENCL

HMODULE RGYOpenCL::openCLHandle = nullptr;

//OpenCLのドライバは場合によってはクラッシュする可能性がある
//クラッシュしたことがあれば、このフラグを立て、以降OpenCLを使用しないようにする
bool RGYOpenCL::openCLCrush = false;

static void to_tchar(TCHAR *buf, uint32_t buf_size, const char *string) {
#if UNICODE
    MultiByteToWideChar(CP_ACP, 0, string, -1, buf, buf_size);
#else
    strcpy_s(buf, buf_size, string);
#endif
};

static inline const char *strichr(const char *str, int c) {
    c = tolower(c);
    for (; *str; str++)
        if (c == tolower(*str))
            return str;
    return NULL;
}
static inline const char *stristr(const char *str, const char *substr) {
    size_t len = 0;
    if (substr && (len = strlen(substr)) != NULL)
        for (; (str = strichr(str, substr[0])) != NULL; str++)
            if (_strnicmp(str, substr, len) == NULL)
                return str;
    return NULL;
}

static bool checkVendor(const char *str, const char *VendorName) {
    if (VendorName == nullptr) {
        return true;
    }
    if (stristr(str, VendorName) != nullptr)
        return true;
    if (stristr(VendorName, "AMD") != nullptr)
        return stristr(str, "Advanced Micro Devices") != nullptr;
    return false;
}


template<typename Functor, typename Target, typename T>
inline cl_int clGetInfo(Functor f, Target target, cl_uint name, T *param) {
    return f(target, name, sizeof(T), param, NULL);
}

template <typename Func, typename Target, typename T>
inline cl_int clGetInfo(Func f, Target target, cl_uint name, vector<T> *param) {
    size_t required;
    cl_int err = f(target, name, 0, NULL, &required);
    if (err != CL_SUCCESS) {
        return err;
    }
    const size_t elements = required / sizeof(T);

    // Temporary to avoid changing param on an error
    vector<T> localData(elements);
    err = f(target, name, required, localData.data(), NULL);
    if (err != CL_SUCCESS) {
        return err;
    }
    if (param) {
        *param = std::move(localData);
    }

    return CL_SUCCESS;
}

// Specialized GetInfoHelper for string params
template <typename Func, typename Target>
inline cl_int clGetInfo(Func f, Target target, cl_uint name, std::string *param) {
    size_t required;
    cl_int err = f(target, name, 0, NULL, &required);
    if (err != CL_SUCCESS) {
        return err;
    }

    // std::string has a constant data member
    // a char vector does not
    if (required > 0) {
        vector<char> value(required+1, '\0');
        err = f(target, name, required, value.data(), NULL);
        if (err != CL_SUCCESS) {
            return err;
        }
        if (param) {
            *param = value.data();
        }
    } else if (param) {
        param->assign("");
    }
    return CL_SUCCESS;
}

template <typename Func, typename Target, size_t N>
inline cl_int clGetInfo(Func f, Target target, cl_uint name, std::array<size_t, N> *param) {
    size_t required;
    cl_int err = f(target, name, 0, NULL, &required);
    if (err != CL_SUCCESS) {
        return err;
    }

    size_t elements = required / sizeof(size_t);
    vector<size_t> value(elements, 0);

    err = f(target, name, required, value.data(), NULL);
    if (err != CL_SUCCESS) {
        return err;
    }

    if (elements > N) {
        elements = N;
    }
    for (size_t i = 0; i < elements; ++i) {
        (*param)[i] = value[i];
    }

    return CL_SUCCESS;
}

static const auto RGY_ERR_TO_OPENCL = make_array<std::pair<RGY_ERR, cl_int>>(
    std::make_pair(RGY_ERR_NONE, CL_SUCCESS),
    std::make_pair(RGY_ERR_DEVICE_NOT_FOUND, CL_DEVICE_NOT_FOUND),
    std::make_pair(RGY_ERR_DEVICE_NOT_AVAILABLE, CL_DEVICE_NOT_AVAILABLE),
    std::make_pair(RGY_ERR_COMPILER_NOT_AVAILABLE, CL_COMPILER_NOT_AVAILABLE),
    std::make_pair(RGY_ERR_MEM_OBJECT_ALLOCATION_FAILURE, CL_MEM_OBJECT_ALLOCATION_FAILURE),
    std::make_pair(RGY_ERR_OUT_OF_RESOURCES, CL_OUT_OF_RESOURCES),
    std::make_pair(RGY_ERR_OUT_OF_HOST_MEMORY, CL_OUT_OF_HOST_MEMORY),
    std::make_pair(RGY_ERR_PROFILING_INFO_NOT_AVAILABLE, CL_PROFILING_INFO_NOT_AVAILABLE),
    std::make_pair(RGY_ERR_MEM_COPY_OVERLAP, CL_MEM_COPY_OVERLAP),
    std::make_pair(RGY_ERR_IMAGE_FORMAT_MISMATCH, CL_IMAGE_FORMAT_MISMATCH),
    std::make_pair(RGY_ERR_IMAGE_FORMAT_NOT_SUPPORTED, CL_IMAGE_FORMAT_NOT_SUPPORTED),
    std::make_pair(RGY_ERR_BUILD_PROGRAM_FAILURE, CL_BUILD_PROGRAM_FAILURE),
    std::make_pair(RGY_ERR_MAP_FAILURE, CL_MAP_FAILURE),
    std::make_pair(RGY_ERR_COMPILE_PROGRAM_FAILURE, CL_COMPILE_PROGRAM_FAILURE),
    std::make_pair(RGY_ERR_INVALID_CALL, CL_INVALID_VALUE),
    std::make_pair(RGY_ERR_INVALID_DEVICE_TYPE, CL_INVALID_DEVICE_TYPE),
    std::make_pair(RGY_ERR_INVALID_PLATFORM, CL_INVALID_PLATFORM),
    std::make_pair(RGY_ERR_INVALID_DEVICE, CL_INVALID_DEVICE),
    std::make_pair(RGY_ERR_INVALID_CONTEXT, CL_INVALID_CONTEXT),
    std::make_pair(RGY_ERR_INVALID_QUEUE_PROPERTIES, CL_INVALID_QUEUE_PROPERTIES),
    std::make_pair(RGY_ERR_INVALID_COMMAND_QUEUE, CL_INVALID_COMMAND_QUEUE),
    std::make_pair(RGY_ERR_INVALID_HOST_PTR, CL_INVALID_HOST_PTR),
    std::make_pair(RGY_ERR_INVALID_MEM_OBJECT, CL_INVALID_MEM_OBJECT),
    std::make_pair(RGY_ERR_INVALID_IMAGE_FORMAT_DESCRIPTOR, CL_INVALID_IMAGE_FORMAT_DESCRIPTOR),
    std::make_pair(RGY_ERR_INVALID_RESOLUTION, CL_INVALID_IMAGE_SIZE),
    std::make_pair(RGY_ERR_INVALID_SAMPLER, CL_INVALID_SAMPLER),
    std::make_pair(RGY_ERR_INVALID_BINARY, CL_INVALID_BINARY),
    std::make_pair(RGY_ERR_INVALID_BUILD_OPTIONS, CL_INVALID_BUILD_OPTIONS),
    std::make_pair(RGY_ERR_INVALID_PROGRAM, CL_INVALID_PROGRAM),
    std::make_pair(RGY_ERR_INVALID_PROGRAM_EXECUTABLE, CL_INVALID_PROGRAM_EXECUTABLE),
    std::make_pair(RGY_ERR_INVALID_KERNEL_NAME, CL_INVALID_KERNEL_NAME),
    std::make_pair(RGY_ERR_INVALID_KERNEL_DEFINITION, CL_INVALID_KERNEL_DEFINITION),
    std::make_pair(RGY_ERR_INVALID_KERNEL, CL_INVALID_KERNEL),
    std::make_pair(RGY_ERR_INVALID_ARG_INDEX, CL_INVALID_ARG_INDEX),
    std::make_pair(RGY_ERR_INVALID_ARG_VALUE, CL_INVALID_ARG_VALUE),
    std::make_pair(RGY_ERR_INVALID_ARG_SIZE, CL_INVALID_ARG_SIZE),
    std::make_pair(RGY_ERR_INVALID_KERNEL_ARGS, CL_INVALID_KERNEL_ARGS),
    std::make_pair(RGY_ERR_INVALID_WORK_DIMENSION, CL_INVALID_WORK_DIMENSION),
    std::make_pair(RGY_ERR_INVALID_WORK_GROUP_SIZE, CL_INVALID_WORK_GROUP_SIZE),
    std::make_pair(RGY_ERR_INVALID_WORK_ITEM_SIZE, CL_INVALID_WORK_ITEM_SIZE),
    std::make_pair(RGY_ERR_INVALID_GLOBAL_OFFSET, CL_INVALID_GLOBAL_OFFSET),
    std::make_pair(RGY_ERR_INVALID_EVENT_WAIT_LIST, CL_INVALID_EVENT_WAIT_LIST),
    std::make_pair(RGY_ERR_INVALID_EVENT, CL_INVALID_EVENT),
    std::make_pair(RGY_ERR_INVALID_OPERATION, CL_INVALID_OPERATION),
    std::make_pair(RGY_ERR_INVALID_GL_OBJECT, CL_INVALID_GL_OBJECT),
    std::make_pair(RGY_ERR_INVALID_BUFFER_SIZE, CL_INVALID_BUFFER_SIZE),
    std::make_pair(RGY_ERR_INVALID_MIP_LEVEL, CL_INVALID_MIP_LEVEL),
    std::make_pair(RGY_ERR_INVALID_GLOBAL_WORK_SIZE, CL_INVALID_GLOBAL_WORK_SIZE)
);
MAP_PAIR_0_1(err, rgy, RGY_ERR, cl, cl_int, RGY_ERR_TO_OPENCL, RGY_ERR_UNKNOWN, CL_INVALID_VALUE);

int initOpenCLGlobal() {
    if (RGYOpenCL::openCLHandle != nullptr) {
        return 0;
    }
    if ((RGYOpenCL::openCLHandle = LoadLibrary(_T("OpenCL.dll"))) == nullptr) {
        return 1;
    }

#define LOAD(name) \
    f_##name = (decltype(f_##name)) GetProcAddress(RGYOpenCL::openCLHandle, #name); \
    if (f_##name == nullptr) { \
        FreeLibrary(RGYOpenCL::openCLHandle); \
        RGYOpenCL::openCLHandle = nullptr; \
        return 1; \
    }

    LOAD(clGetExtensionFunctionAddressForPlatform);
    LOAD(clGetDeviceInfo);
    LOAD(clGetPlatformIDs);
    LOAD(clGetDeviceIDs);
    LOAD(clGetPlatformInfo);

    LOAD(clCreateCommandQueue);
    LOAD(clReleaseCommandQueue);
    LOAD(clCreateContext);
    LOAD(clReleaseContext);

    LOAD(clCreateProgramWithSource);
    LOAD(clBuildProgram);
    LOAD(clGetProgramBuildInfo);
    LOAD(clGetProgramInfo);
    LOAD(clReleaseProgram);

    LOAD(clCreateBuffer);
    LOAD(clCreateImage);
    LOAD(clReleaseMemObject);
    LOAD(clGetMemObjectInfo);
    LOAD(clGetImageInfo);
    LOAD(clCreateKernel);
    LOAD(clReleaseKernel);
    LOAD(clSetKernelArg);
    LOAD(clEnqueueNDRangeKernel);
    LOAD(clEnqueueTask);

    LOAD(clEnqueueReadBuffer);
    LOAD(clEnqueueReadBufferRect);
    LOAD(clEnqueueWriteBuffer);
    LOAD(clEnqueueWriteBufferRect);
    LOAD(clEnqueueCopyBuffer);
    LOAD(clEnqueueCopyBufferRect);

    LOAD(clEnqueueReadImage);
    LOAD(clEnqueueWriteImage);
    LOAD(clEnqueueCopyImage);
    LOAD(clEnqueueCopyImageToBuffer);
    LOAD(clEnqueueCopyBufferToImage);
    LOAD(clEnqueueMapBuffer);
    LOAD(clEnqueueMapImage);
    LOAD(clEnqueueUnmapMemObject);

    LOAD(clWaitForEvents);
    LOAD(clGetEventInfo);
    LOAD(clCreateUserEvent);
    LOAD(clRetainEvent);
    LOAD(clReleaseEvent);
    LOAD(clSetUserEventStatus);
    LOAD(clGetEventProfilingInfo);

    LOAD(clFlush);
    LOAD(clFinish);

    return 0;
}

RGYOpenCLDevice::RGYOpenCLDevice(cl_device_id device) : m_device(device) {

}

RGYOpenCLDeviceInfo RGYOpenCLDevice::info() {
    RGYOpenCLDeviceInfo info;
    try {
        clGetInfo(clGetDeviceInfo, m_device, CL_DEVICE_TYPE, &info.type);
        clGetInfo(clGetDeviceInfo, m_device, CL_DEVICE_VENDOR_ID, &info.vendor_id);
        clGetInfo(clGetDeviceInfo, m_device, CL_DEVICE_MAX_COMPUTE_UNITS, &info.max_compute_units);
        clGetInfo(clGetDeviceInfo, m_device, CL_DEVICE_MAX_CLOCK_FREQUENCY, &info.max_clock_frequency);
        clGetInfo(clGetDeviceInfo, m_device, CL_DEVICE_MAX_SAMPLERS, &info.max_samplers);
        clGetInfo(clGetDeviceInfo, m_device, CL_DEVICE_GLOBAL_MEM_SIZE, &info.global_mem_size);
        clGetInfo(clGetDeviceInfo, m_device, CL_DEVICE_PROFILING_TIMER_RESOLUTION, &info.profiling_timer_resolution);
        clGetInfo(clGetDeviceInfo, m_device, CL_DEVICE_NAME, &info.name);
        clGetInfo(clGetDeviceInfo, m_device, CL_DEVICE_VENDOR, &info.vendor);
        clGetInfo(clGetDeviceInfo, m_device, CL_DRIVER_VERSION, &info.driver_version);
        clGetInfo(clGetDeviceInfo, m_device, CL_DEVICE_PROFILE, &info.profile);
        clGetInfo(clGetDeviceInfo, m_device, CL_DEVICE_VERSION, &info.version);
        clGetInfo(clGetDeviceInfo, m_device, CL_DEVICE_EXTENSIONS, &info.extensions);
    } catch (...) {
        return RGYOpenCLDeviceInfo();
    }
    return info;
}

tstring RGYOpenCLDevice::infostr() {
    const auto dev = info();
    std::stringstream ts;
    ts << dev.name;
    if (dev.max_compute_units > 0) {
        ts << " (" << dev.max_compute_units << " CU)";
    }
    if (dev.max_clock_frequency > 0) {
        ts << " @ " << dev.max_clock_frequency << " MHz";
    }
    if (dev.driver_version.length() > 0) {
        ts << " (" << dev.driver_version << ")";
    }
    return char_to_tstring(ts.str());
}

RGYOpenCLPlatform::RGYOpenCLPlatform(cl_platform_id platform, shared_ptr<RGYLog> pLog) : m_platform(platform), m_d3d9dev(nullptr), m_d3d11dev(nullptr), m_devices(), m_pLog(pLog) {
}

RGY_ERR RGYOpenCLPlatform::createDeviceListD3D11(cl_device_type device_type, void *d3d11dev) {
    if (RGYOpenCL::openCLCrush) {
        return RGY_ERR_OPENCL_CRUSH;
    }
    auto ret = RGY_ERR_NONE;
    cl_uint device_count = 0;
    if (d3d11dev && clGetDeviceIDsFromD3D11KHR == nullptr) {
        f_clGetDeviceIDsFromD3D11KHR = (decltype(f_clGetDeviceIDsFromD3D11KHR))clGetExtensionFunctionAddressForPlatform(m_platform, "clGetDeviceIDsFromD3D11KHR");
    }
    if (d3d11dev && clGetDeviceIDsFromD3D11KHR) {
        m_d3d11dev = d3d11dev;
        try {
            if ((ret = err_cl_to_rgy(clGetDeviceIDsFromD3D11KHR(m_platform, CL_D3D11_DEVICE_KHR, d3d11dev, CL_PREFERRED_DEVICES_FOR_D3D11_KHR, 0, NULL, &device_count))) != RGY_ERR_NONE) {
                m_pLog->write(RGY_LOG_ERROR, _T("Error (clGetDeviceIDsFromD3D11KHR): %s\n"), get_err_mes(ret));
                return ret;
            }
        } catch (...) {
            m_pLog->write(RGY_LOG_ERROR, _T("Crush (clGetDeviceIDsFromD3D11KHR)\n"));
            RGYOpenCL::openCLCrush = true; //クラッシュフラグを立てる
            return RGY_ERR_OPENCL_CRUSH;
        }
        if (device_count > 0) {
            std::vector<cl_device_id> devs(device_count, 0);
            try {
                ret = err_cl_to_rgy(clGetDeviceIDsFromD3D11KHR(m_platform, CL_D3D11_DEVICE_KHR, d3d11dev, CL_PREFERRED_DEVICES_FOR_D3D11_KHR, device_count, devs.data(), &device_count));
            } catch (...) {
                m_pLog->write(RGY_LOG_ERROR, _T("Crush (clGetDeviceIDsFromD3D11KHR)\n"));
                RGYOpenCL::openCLCrush = true; //クラッシュフラグを立てる
                return RGY_ERR_OPENCL_CRUSH;
            }
            if (ret == RGY_ERR_NONE) {
                m_devices = devs;
                return ret;
            }
        }
    } else {
        ret = createDeviceList(device_type);
    }
    return RGY_ERR_NONE;
}

RGY_ERR RGYOpenCLPlatform::createDeviceListD3D9(cl_device_type device_type, void *d3d9dev) {
    if (RGYOpenCL::openCLCrush) {
        return RGY_ERR_OPENCL_CRUSH;
    }
    auto ret = RGY_ERR_NONE;
    cl_uint device_count = 1;
    if (d3d9dev && clGetDeviceIDsFromDX9MediaAdapterKHR == nullptr) {
        f_clGetDeviceIDsFromDX9MediaAdapterKHR = (decltype(f_clGetDeviceIDsFromDX9MediaAdapterKHR))clGetExtensionFunctionAddressForPlatform(m_platform, "clGetDeviceIDsFromDX9MediaAdapterKHR");
    }
    if (d3d9dev && clGetDeviceIDsFromDX9MediaAdapterKHR) {
        m_d3d9dev = d3d9dev;
        std::vector<cl_device_id> devs(device_count, 0);
        try {
            cl_dx9_media_adapter_type_khr type = CL_ADAPTER_D3D9EX_KHR;
            ret = err_cl_to_rgy(clGetDeviceIDsFromDX9MediaAdapterKHR(m_platform, 1, &type, &d3d9dev, CL_PREFERRED_DEVICES_FOR_DX9_MEDIA_ADAPTER_KHR, device_count, devs.data(), &device_count));
        } catch (...) {
            m_pLog->write(RGY_LOG_ERROR, _T("Crush (clGetDeviceIDsFromDX9MediaAdapterKHR)\n"));
            RGYOpenCL::openCLCrush = true; //クラッシュフラグを立てる
            return RGY_ERR_OPENCL_CRUSH;
        }
        if (ret == RGY_ERR_NONE) {
            m_devices = devs;
            return ret;
        }
    } else {
        ret = createDeviceList(device_type);
    }
    return RGY_ERR_NONE;
}

RGY_ERR RGYOpenCLPlatform::createDeviceList(cl_device_type device_type) {
    if (RGYOpenCL::openCLCrush) {
        return RGY_ERR_OPENCL_CRUSH;
    }
    auto ret = RGY_ERR_NONE;
    cl_uint device_count = 0;
    try {
        if ((ret = err_cl_to_rgy(clGetDeviceIDs(m_platform, device_type, 0, NULL, &device_count))) != RGY_ERR_NONE) {
            m_pLog->write(RGY_LOG_ERROR, _T("Error (clGetDeviceIDs): %s\n"), get_err_mes(ret));
            return ret;
        }
    } catch (...) {
        m_pLog->write(RGY_LOG_ERROR, _T("Crush (clGetDeviceIDs)\n"));
        RGYOpenCL::openCLCrush = true; //クラッシュフラグを立てる
        return RGY_ERR_OPENCL_CRUSH;
    }
    if (device_count > 0) {
        std::vector<cl_device_id> devs(device_count, 0);
        try {
            ret = err_cl_to_rgy(clGetDeviceIDs(m_platform, device_type, device_count, devs.data(), &device_count));
        } catch (...) {
            m_pLog->write(RGY_LOG_ERROR, _T("Crush (clGetDeviceIDs)\n"));
            RGYOpenCL::openCLCrush = true; //クラッシュフラグを立てる
            return RGY_ERR_OPENCL_CRUSH;
        }
        if (ret == RGY_ERR_NONE) {
            m_devices = devs;
            return ret;
        }
    }
    return RGY_ERR_NONE;
}

RGYOpenCLPlatformInfo RGYOpenCLPlatform::info() {
    RGYOpenCLPlatformInfo info;
    try {
        clGetInfo(clGetPlatformInfo, m_platform, CL_PLATFORM_PROFILE, &info.profile);
        clGetInfo(clGetPlatformInfo, m_platform, CL_PLATFORM_VERSION, &info.version);
        clGetInfo(clGetPlatformInfo, m_platform, CL_PLATFORM_NAME, &info.name);
        clGetInfo(clGetPlatformInfo, m_platform, CL_PLATFORM_VENDOR, &info.vendor);
        clGetInfo(clGetPlatformInfo, m_platform, CL_PLATFORM_EXTENSIONS, &info.extension);
    } catch (...) {
        return RGYOpenCLPlatformInfo();
    }
    return info;
}

bool RGYOpenCLPlatform::isVendor(const char *vendor) {
    return checkVendor(info().vendor.c_str(), vendor);
}

RGYOpenCLContext::RGYOpenCLContext(shared_ptr<RGYOpenCLPlatform> platform, shared_ptr<RGYLog> pLog) :
    m_platform(std::move(platform)),
    m_context(std::unique_ptr<std::remove_pointer<cl_context>::type, decltype(clReleaseContext)>(nullptr, clReleaseContext)),
    m_queue(),
    m_pLog(pLog),
    m_copyI2B(),
    m_copyB2I() {

}

RGYOpenCLContext::~RGYOpenCLContext() {
    m_copyI2B.reset();
    m_copyB2I.reset();
    m_queue.clear();
    m_context.reset();
    m_platform.reset();
    m_pLog.reset();
}

RGY_ERR RGYOpenCLContext::createContext() {
    if (RGYOpenCL::openCLCrush) {
        return RGY_ERR_OPENCL_CRUSH;
    }
    cl_int err = RGY_ERR_NONE;
    std::vector<cl_context_properties> props = { CL_CONTEXT_PLATFORM, (cl_context_properties)(m_platform->get()) };
    if (m_platform->d3d9dev()) {
        props.push_back(CL_CONTEXT_ADAPTER_D3D9EX_KHR);
        props.push_back((cl_context_properties)m_platform->d3d9dev());
    }
    if (m_platform->d3d11dev()) {
        props.push_back(CL_CONTEXT_D3D11_DEVICE_KHR);
        props.push_back((cl_context_properties)m_platform->d3d11dev());
    }
    //重要: これをいれて、OpenCL使用時はロックするようにしないと、デコードが不安定になり最悪BSOD(0xea)したりする
    props.push_back(CL_CONTEXT_INTEROP_USER_SYNC);
    props.push_back(CL_TRUE);
    props.push_back(0);
    try {
        m_context = unique_context(clCreateContext(props.data(), (cl_uint)m_platform->devs().size(), m_platform->devs().data(), nullptr, nullptr, &err), clReleaseContext);
    } catch (...) {
        m_pLog->write(RGY_LOG_ERROR, _T("Crush (clCreateContext)\n"));
        RGYOpenCL::openCLCrush = true; //クラッシュフラグを立てる
        return RGY_ERR_OPENCL_CRUSH;
    }
    if (err != CL_SUCCESS) {
        m_pLog->write(RGY_LOG_ERROR, _T("Error (clCreateContext): %s\n"), cl_errmes(err));
        return err_cl_to_rgy(err);
    }
    for (int idev = 0; idev < (int)m_platform->devs().size(); idev++) {
        try {
            m_queue.push_back(std::move(RGYOpenCLQueue(clCreateCommandQueue(m_context.get(), m_platform->dev(idev), 0, &err))));
        } catch (...) {
            m_pLog->write(RGY_LOG_ERROR, _T("Crush (clCreateCommandQueue)\n"));
            RGYOpenCL::openCLCrush = true; //クラッシュフラグを立てる
            return RGY_ERR_OPENCL_CRUSH;
        }
    }
    return RGY_ERR_NONE;
}

RGYOpenCLKernelLauncher::RGYOpenCLKernelLauncher(cl_kernel kernel, std::string kernelName, cl_command_queue queue, const RGYWorkSize &local, const RGYWorkSize &global, shared_ptr<RGYLog> pLog) :
    m_kernel(kernel), m_kernelName(kernelName), m_queue(queue), m_local(local), m_global(global), m_pLog(pLog) {
}

RGY_ERR RGYOpenCLKernelLauncher::launch(std::vector<void *> arg_ptrs, std::vector<size_t> arg_size) const {
    assert(arg_ptrs.size() == arg_size.size());
    for (int i = 0; i < (int)arg_ptrs.size(); i++) {
        auto err = err_cl_to_rgy(clSetKernelArg(m_kernel, i, arg_size[i], arg_ptrs[i]));
        if (err != CL_SUCCESS) {
            m_pLog->write(RGY_LOG_ERROR, _T("Error: Failed to set #%d arg to kernel \"%s\": %s\n"), i, char_to_tstring(m_kernelName).c_str(), cl_errmes(err));
            return err;
        }
    }

    auto err = err_cl_to_rgy(clEnqueueNDRangeKernel(m_queue, m_kernel, 3, NULL, m_global(), m_local(), 0, NULL, NULL));
    if (err != CL_SUCCESS) {
        m_pLog->write(RGY_LOG_ERROR, _T("Error: Failed to run kernel \"%s\": %s\n"), char_to_tstring(m_kernelName).c_str(), cl_errmes(err));
        return err;
    }
    return err;
}

RGYOpenCLKernel::RGYOpenCLKernel(cl_kernel kernel, std::string kernelName, shared_ptr<RGYLog> pLog) : m_kernel(kernel), m_kernelName(kernelName), m_pLog(pLog) {

}

RGYOpenCLKernel::~RGYOpenCLKernel() {
    if (m_kernel) {
        clReleaseKernel(m_kernel);
        m_kernel = nullptr;
    }
    m_kernelName.clear();
    m_pLog.reset();
};

RGYOpenCLProgram::RGYOpenCLProgram(cl_program program, shared_ptr<RGYLog> pLog) : m_program(program), m_pLog(pLog) {
};

RGYOpenCLProgram::~RGYOpenCLProgram() {
    if (m_program) {
        clReleaseProgram(m_program);
        m_program = nullptr;
    }
};

RGYOpenCLKernelLauncher RGYOpenCLKernel::config(cl_command_queue queue, const RGYWorkSize &local, const RGYWorkSize &global) {
    return RGYOpenCLKernelLauncher(m_kernel, m_kernelName, queue, local, global, m_pLog);
}

RGYOpenCLKernel RGYOpenCLProgram::kernel(const char *kernelName) {
    cl_int err = CL_SUCCESS;
    auto kernel = clCreateKernel(m_program, kernelName, &err);
    if (err != CL_SUCCESS) {
        m_pLog->write(RGY_LOG_ERROR, _T("Failed to get kernel %s: %s\n"), char_to_tstring(kernelName).c_str(), cl_errmes(err));
    }
    return RGYOpenCLKernel(kernel, kernelName, m_pLog);
}

RGYOpenCLQueue::RGYOpenCLQueue(cl_command_queue queue) : m_queue(std::move(unique_queue(queue, clReleaseCommandQueue))) {};

RGYOpenCLQueue::~RGYOpenCLQueue() {
    m_queue.reset();
}

RGY_ERR RGYOpenCLQueue::flush() const {
    if (!m_queue) {
        return RGY_ERR_NULL_PTR;
    }
    return err_cl_to_rgy(clFlush(m_queue.get()));
}

RGY_ERR RGYOpenCLQueue::finish() const {
    if (!m_queue) {
        return RGY_ERR_NULL_PTR;
    }
    return err_cl_to_rgy(clFinish(m_queue.get()));
}

RGY_ERR RGYOpenCLContext::copyPlane(FrameInfo *planeDst, const FrameInfo *planeSrc, const sInputCrop *planeCrop, bool blocking, int queue_id) {
    cl_int err = CL_SUCCESS;
    const int pixel_size = RGY_CSP_BIT_DEPTH[planeDst->csp] > 8 ? 2 : 1;

    cl_event *ret_event = nullptr;
    size_t dst_origin[3] = { 0, 0, 0 };
    size_t src_origin[3] = { 0, 0, 0 };
    size_t region[3] = { (size_t)planeDst->width * pixel_size, (size_t)planeDst->height, 1 };
    if (planeSrc->mem_type == RGY_MEM_TYPE_GPU) {
        if (planeDst->mem_type == RGY_MEM_TYPE_GPU) {
            err = clEnqueueCopyBufferRect(m_queue[queue_id].get(), (cl_mem)planeSrc->ptr[0], (cl_mem)planeDst->ptr[0], src_origin, dst_origin,
                region, planeSrc->pitch[0], 0, planeDst->pitch[0], 0, 0, nullptr, ret_event);
        } else if (planeDst->mem_type == RGY_MEM_TYPE_GPU_IMAGE) {
            if (!m_copyB2I) {
                const auto options = strsprintf("-D TypeIn=%s -D TypeOut=%s -D IMAGE_SRC=0 -D IMAGE_DST=1 -D in_bit_depth=%d -D out_bit_depth=%d",
                    pixel_size >= 2 ? "ushort" : "uchar", pixel_size >= 2 ? "ushort" : "uchar",
                    RGY_CSP_BIT_DEPTH[planeDst->csp], RGY_CSP_BIT_DEPTH[planeDst->csp]);
                m_copyB2I = buildResource(_T("VCE_FILTER_CL"), _T("EXE_DATA"), options.c_str());
                if (!m_copyB2I) {
                    m_pLog->write(RGY_LOG_ERROR, _T("failed to load VCE_FILTER_CL(m_copyB2I)\n"));
                    return RGY_ERR_OPENCL_CRUSH;
                }
            }
            RGYWorkSize local(32, 8);
            RGYWorkSize global(planeDst->width, planeDst->height);
            auto rgy_err = m_copyB2I->kernel("kernel_copy_plane").config(m_queue[queue_id].get(), local, global).launch(
                (cl_mem)planeDst->ptr[0], planeDst->pitch[0], (cl_mem)planeSrc->ptr[0], planeSrc->pitch[0], planeSrc->width, planeSrc->height,
                planeCrop->e.left, planeCrop->e.up);
            err = err_rgy_to_cl(rgy_err);
        } else if (planeDst->mem_type == RGY_MEM_TYPE_CPU) {
            err = clEnqueueReadBufferRect(m_queue[queue_id].get(), (cl_mem)planeSrc->ptr[0], blocking, src_origin, dst_origin,
                region, planeSrc->pitch[0], 0, planeDst->pitch[0], 0, planeDst->ptr[0], 0, nullptr, ret_event);
        } else {
            return RGY_ERR_UNSUPPORTED;
        }
    } else if (planeSrc->mem_type == RGY_MEM_TYPE_GPU_IMAGE) {
        if (planeDst->mem_type == RGY_MEM_TYPE_GPU) {
            if (!m_copyI2B) {
                const auto options = strsprintf("-D TypeIn=%s -D TypeOut=%s -D IMAGE_SRC=1 -D IMAGE_DST=0 -D in_bit_depth=%d -D out_bit_depth=%d",
                    pixel_size >= 2 ? "ushort" : "uchar", pixel_size >= 2 ? "ushort" : "uchar",
                    RGY_CSP_BIT_DEPTH[planeDst->csp], RGY_CSP_BIT_DEPTH[planeDst->csp]);
                m_copyI2B = buildResource(_T("VCE_FILTER_CL"), _T("EXE_DATA"), options.c_str());
                if (!m_copyI2B) {
                    m_pLog->write(RGY_LOG_ERROR, _T("failed to load VCE_FILTER_CL(m_copyI2B)\n"));
                    return RGY_ERR_OPENCL_CRUSH;
                }
            }
            RGYWorkSize local(32, 8);
            RGYWorkSize global(planeDst->width, planeDst->height);
            auto rgy_err = m_copyI2B->kernel("kernel_copy_plane").config(m_queue[queue_id].get(), local, global).launch(
                (cl_mem)planeDst->ptr[0], planeDst->pitch[0], (cl_mem)planeSrc->ptr[0], planeSrc->pitch[0], planeSrc->width, planeSrc->height,
                planeCrop->e.left, planeCrop->e.up);
            err = err_rgy_to_cl(rgy_err);
        } else if (planeDst->mem_type == RGY_MEM_TYPE_GPU_IMAGE) {
            clGetImageInfo((cl_mem)planeDst->ptr[0], CL_IMAGE_WIDTH, sizeof(region[0]), &region[0], nullptr);
            err = clEnqueueCopyImage(m_queue[queue_id].get(), (cl_mem)planeSrc->ptr[0], (cl_mem)planeDst->ptr[0], src_origin, dst_origin, region, 0, nullptr, ret_event);
        } else if (planeDst->mem_type == RGY_MEM_TYPE_CPU) {
            clGetImageInfo((cl_mem)planeSrc->ptr[0], CL_IMAGE_WIDTH, sizeof(region[0]), &region[0], nullptr);
            err = clEnqueueReadImage(m_queue[queue_id].get(), (cl_mem)planeSrc->ptr[0], blocking, dst_origin,
                region, planeDst->pitch[0], 0, planeDst->ptr[0], 0, nullptr, ret_event);
        } else {
            return RGY_ERR_UNSUPPORTED;
        }
    } else if (planeSrc->mem_type == RGY_MEM_TYPE_CPU) {
        if (planeDst->mem_type == RGY_MEM_TYPE_GPU) {
            err = clEnqueueWriteBufferRect(m_queue[queue_id].get(), (cl_mem)planeDst->ptr[0], blocking, dst_origin, src_origin,
                region, planeDst->pitch[0], 0, planeSrc->pitch[0], 0, planeSrc->ptr[0], 0, nullptr, ret_event);
        } else if (planeDst->mem_type == RGY_MEM_TYPE_GPU_IMAGE) {
            clGetImageInfo((cl_mem)planeDst->ptr[0], CL_IMAGE_WIDTH, sizeof(region[0]), &region[0], nullptr);
            err = clEnqueueWriteImage(m_queue[queue_id].get(), (cl_mem)planeDst->ptr[0], blocking, src_origin,
                region, planeSrc->pitch[0], 0, (void *)planeSrc->ptr[0], 0, nullptr, ret_event);
        } else if (planeDst->mem_type == RGY_MEM_TYPE_CPU) {
            for (int y = 0; y < planeDst->height; y++) {
                memcpy(planeDst->ptr[0] + (y + dst_origin[1]) * planeDst->pitch[0] + dst_origin[0] * pixel_size,
                        planeSrc->ptr[0] + (y + src_origin[1]) * planeSrc->pitch[0] + src_origin[0] * pixel_size,
                        planeDst->width * pixel_size);
            }
        } else {
            return RGY_ERR_UNSUPPORTED;
        }
    } else {
        return RGY_ERR_UNSUPPORTED;
    }
    return err_cl_to_rgy(err);
}

RGY_ERR RGYOpenCLContext::copyFrame(FrameInfo *dst, const FrameInfo *src, const sInputCrop *srcCrop, bool blocking, int queue_id) {
    size_t dst_origin[3] = { 0, 0, 0 };
    if (dst->csp != src->csp) {
        m_pLog->write(RGY_LOG_ERROR, _T("in/out csp should be same in copyFrame.\n"));
        return RGY_ERR_INVALID_CALL;
    }
    const int pixel_size = RGY_CSP_BIT_DEPTH[dst->csp] > 8 ? 2 : 1;

    RGY_ERR err = RGY_ERR_NONE;
    for (int i = 0; i < RGY_CSP_PLANES[dst->csp]; i++) {
        auto planeDst = getPlane(dst, (RGY_PLANE)i);
        auto planeSrc = getPlane(src, (RGY_PLANE)i);
        size_t src_origin[3] = { 0, 0, 0 };
        sInputCrop planeCrop = { 0 };
        if (srcCrop != nullptr) {
            planeCrop = getPlane(srcCrop, src->csp, (RGY_PLANE)i);
            src_origin[0] = planeCrop.e.left * pixel_size;
            src_origin[1] = planeCrop.e.up;
        }
        err = copyPlane(&planeDst, &planeSrc, &planeCrop, blocking, queue_id);
        if (err != RGY_ERR_NONE) {
            m_pLog->write(RGY_LOG_ERROR, _T("Failed to copy frame(%d): %s\n"), i, cl_errmes(err));
            return err_cl_to_rgy(err);
        }
    }
    dst->picstruct = src->picstruct;
    dst->duration = src->duration;
    dst->timestamp = src->timestamp;
    dst->flags = src->flags;
    dst->inputFrameId = src->inputFrameId;
    return err;
}

unique_ptr<RGYOpenCLProgram> RGYOpenCLContext::build(const char *data, const size_t size, const char *options) {
    if (data == nullptr || size == 0) {
        return nullptr;
    }
    cl_int err = CL_SUCCESS;
    cl_program program = clCreateProgramWithSource(m_context.get(), 1, &data, &size, &err);
    if (err != CL_SUCCESS) {
        m_pLog->write(RGY_LOG_ERROR, _T("Error (clCreateProgramWithSource): %s\n"), cl_errmes(err));
        return nullptr;
    }

    err = clBuildProgram(program, (cl_uint)m_platform->devs().size(), m_platform->devs().data(), options, NULL, NULL);
    if (err != CL_SUCCESS) {
        m_pLog->write(RGY_LOG_ERROR, _T("Error (clBuildProgram): %s\n"), cl_errmes(err));

        if (err == CL_BUILD_PROGRAM_FAILURE) {
            for (const auto &device : m_platform->devs()) {
                size_t log_size = 0;
                clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, 0, NULL, &log_size);

                std::vector<char> build_log(log_size);
                clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, log_size, build_log.data(), NULL);

                m_pLog->write(RGY_LOG_ERROR, _T("build log of %s...\n"), char_to_tstring(RGYOpenCLDevice(device).info().name).c_str());
                auto log = char_to_tstring(build_log.data());
                m_pLog->write_log(RGY_LOG_ERROR, log.c_str());
            }
        }
        return nullptr;
    }
    return std::make_unique<RGYOpenCLProgram>(program, m_pLog);
}

unique_ptr<RGYOpenCLProgram> RGYOpenCLContext::build(const std::string &source, const char *options) {
    return build(source.c_str(), source.length(), options);
}

unique_ptr<RGYOpenCLProgram> RGYOpenCLContext::buildFile(const tstring& filename, const char *options) {
    std::ifstream inputFile(filename);
    if (inputFile.bad()) {
        m_pLog->write(RGY_LOG_ERROR, _T("Failed to open source file \"%s\".\n"), filename.c_str());
        return nullptr;
    }
    m_pLog->write(RGY_LOG_DEBUG, _T("Opened file \"%s\""), filename.c_str());
    std::istreambuf_iterator<char> data_begin(inputFile);
    std::istreambuf_iterator<char> data_end;
    std::string source = std::string(data_begin, data_end);
    inputFile.close();
    return build(source, options);
}

unique_ptr<RGYOpenCLProgram> RGYOpenCLContext::buildResource(const TCHAR *name, const TCHAR *type, const char *options) {
    void *data = nullptr;
    int size = getEmbeddedResource(&data, name, type);
    if (data == nullptr || size == 0) {
        m_pLog->write(RGY_LOG_ERROR, _T("Failed to load resource [%s] %s\n"), type, name);
        return nullptr;
    }
    return build((const char *)data, size, options);
}

std::unique_ptr<RGYCLBuf> RGYOpenCLContext::createBuffer(size_t size, cl_mem_flags flags, void *host_ptr) {
    cl_int err = CL_SUCCESS;
    cl_mem mem = clCreateBuffer(m_context.get(), flags, size, host_ptr, &err);
    if (err != CL_SUCCESS) {
        m_pLog->write(RGY_LOG_ERROR, _T("Failed to allocate memory: %s\n"), cl_errmes(err));
    }
    return std::make_unique<RGYCLBuf>(mem, flags, size);
}

unique_ptr<RGYCLBuf> RGYOpenCLContext::copyDataToBuffer(const void *host_ptr, size_t size, cl_mem_flags flags, int queue_id) {
    auto buffer = createBuffer(size, flags);
    if (buffer != nullptr) {
        cl_int err = clEnqueueWriteBuffer(m_queue[queue_id].get(), buffer->mem, true, 0, size, host_ptr, 0, nullptr, nullptr);
        if (err != CL_SUCCESS) {
            m_pLog->write(RGY_LOG_ERROR, _T("Failed to copy data to buffer: %s\n"), cl_errmes(err));
        }
    }
    return buffer;
}

unique_ptr<RGYCLFrame> RGYOpenCLContext::createImageFromFrameBuffer(const FrameInfo &frame, bool normalized, cl_mem_flags flags) {
    FrameInfo frameImage = frame;
    frameImage.mem_type = RGY_MEM_TYPE_GPU_IMAGE;

    for (int i = 0; i < RGY_CSP_PLANES[frame.csp]; i++) {
        const auto plane = getPlane(&frame, (RGY_PLANE)i);
        //パラメータを適切に設定する
        cl_image_format format;
        format.image_channel_order = CL_R;         //チャンネル数
        format.image_channel_data_type =  //データ型
            (normalized) ? ((RGY_CSP_BIT_DEPTH[frame.csp] > 8) ? CL_UNORM_INT16 : CL_UNORM_INT8)
                         : ((RGY_CSP_BIT_DEPTH[frame.csp] > 8) ? CL_UNSIGNED_INT16 : CL_UNSIGNED_INT8);

        cl_image_desc img_desc;
        img_desc.image_type = CL_MEM_OBJECT_IMAGE2D; //2D
        img_desc.image_width = plane.width;   //サイズ
        img_desc.image_height = plane.height; //サイズ
        img_desc.image_depth = 0;
        img_desc.image_array_size = 0;
        img_desc.image_row_pitch = plane.pitch[0];
        img_desc.image_slice_pitch = 0;
        img_desc.num_mip_levels = 0;
        img_desc.num_samples = 0;
        img_desc.buffer = 0;
        img_desc.mem_object = (cl_mem)plane.ptr[0]; //作成したOpenCLバッファをここに指定する

        cl_int err = CL_SUCCESS;
        frameImage.ptr[i] = (uint8_t *)clCreateImage(m_context.get(),
            flags,
            &format, &img_desc,
            nullptr,
            &err);
        if (err != CL_SUCCESS) {
            m_pLog->write(RGY_LOG_ERROR, _T("Failed to allocate memory: %s\n"), cl_errmes(err));
            for (int j = i-1; j >= 0; j--) {
                if (frameImage.ptr[j] != nullptr) {
                    clReleaseMemObject((cl_mem)frameImage.ptr[j]);
                    frameImage.ptr[j] = nullptr;
                }
            }
            return std::make_unique<RGYCLFrame>();
        }
    }
    return std::make_unique<RGYCLFrame>(frameImage, flags);
}

std::unique_ptr<RGYCLFrame> RGYOpenCLContext::createFrameBuffer(const FrameInfo& frame, cl_mem_flags flags) {
    cl_int err = CL_SUCCESS;
    const int pixsize = RGY_CSP_BIT_DEPTH[frame.csp] > 8 ? 2 : 1;
    FrameInfo clframe = frame;
    clframe.mem_type = RGY_MEM_TYPE_GPU;
    for (int i = 0; i < _countof(clframe.ptr); i++) {
        clframe.ptr[0] = nullptr;
        clframe.pitch[0] = 0;
    }
    for (int i = 0; i < RGY_CSP_PLANES[frame.csp]; i++) {
        const auto plane = getPlane(&clframe, (RGY_PLANE)i);
        const int widthByte = plane.width * pixsize;
        const int memPitch = ALIGN(widthByte, 256);
        const int size = memPitch * plane.height;
        cl_mem mem = clCreateBuffer(m_context.get(), flags, size, nullptr, &err);
        if (err != CL_SUCCESS) {
            m_pLog->write(RGY_LOG_ERROR, _T("Failed to allocate memory: %s\n"), cl_errmes(err));
            for (int j = i-1; j >= 0; j--) {
                if (clframe.ptr[j] != nullptr) {
                    clReleaseMemObject((cl_mem)clframe.ptr[j]);
                    clframe.ptr[j] = nullptr;
                }
            }
            return std::make_unique<RGYCLFrame>();
        }
        clframe.pitch[i] = memPitch;
        clframe.ptr[i] = (uint8_t *)mem;
    }
    return std::make_unique<RGYCLFrame>(clframe, flags);
}

RGYOpenCL::RGYOpenCL() : m_pLog(std::make_shared<RGYLog>(nullptr, RGY_LOG_ERROR)) {
    initOpenCLGlobal();
}

RGYOpenCL::RGYOpenCL(shared_ptr<RGYLog> pLog) : m_pLog(pLog) {
    initOpenCLGlobal();
}

RGYOpenCL::~RGYOpenCL() {

}

std::vector<shared_ptr<RGYOpenCLPlatform>> RGYOpenCL::getPlatforms(const char *vendor) {
    std::vector<shared_ptr<RGYOpenCLPlatform>> platform_list;
    if (RGYOpenCL::openCLCrush) {
        return platform_list;
    }

    cl_uint platform_count = 0;
    cl_int ret = CL_SUCCESS;

    //OpenCLのドライバは場合によってはクラッシュする可能性がある
    try {
        if (CL_SUCCESS != (ret = clGetPlatformIDs(0, NULL, &platform_count))) {
            m_pLog->write(RGY_LOG_ERROR, _T("Error (clGetPlatformIDs): %s\n"), cl_errmes(ret));
            return platform_list;
        }
    } catch (...) {
        m_pLog->write(RGY_LOG_ERROR, _T("Crush (clGetPlatformIDs)\n"));
        RGYOpenCL::openCLCrush = true; //クラッシュフラグを立てる
        return platform_list;
    }
    if (platform_count > 0) {
        std::vector<cl_platform_id> platforms(platform_count, 0);
        try {
            if (CL_SUCCESS != (ret = clGetPlatformIDs(platform_count, platforms.data(), &platform_count))) {
                m_pLog->write(RGY_LOG_ERROR, _T("Error (clGetPlatformIDs): %s\n"), cl_errmes(ret));
                return platform_list;
            }
        } catch (...) {
            m_pLog->write(RGY_LOG_ERROR, _T("Crush (clGetPlatformIDs)\n"));
            RGYOpenCL::openCLCrush = true; //クラッシュフラグを立てる
            return platform_list;
        }

        for (int i = 0; i < (int)platform_count; i++) {
            auto platform = std::make_shared<RGYOpenCLPlatform>(platforms[i], m_pLog);
            if (vendor == nullptr || platform->isVendor(vendor)) {
                platform_list.push_back(std::move(platform));
            }
        }
    }
    return platform_list;
}

#endif