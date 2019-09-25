#ifndef _IGCMC_H_
#define _IGCMC_H_

#include <cstddef>
#include <string>
#include <stdint.h>
#include <vector>

#ifndef DLL_EXPORT
  #ifdef _WIN32
    #define __EXPORT__ __declspec(dllexport)
  #else
    #define __EXPORT__ __attribute__((visibility("default")))
  #endif
#endif

// Supported kernel argument attributes.
struct cmc_resource_attibute {
    static constexpr const char* ReadOnly  = "read_only";  // This resource is for read only.
    static constexpr const char* WriteOnly = "write_only"; // This resource is for write only.
    static constexpr const char* ReadWrite = "read_write"; // This resource is for read and write.
    static constexpr const char* Buffer    = "buffer_t";   // This resource is a buffer.
    static constexpr const char* SVM       = "svmptr_t";   // This resource is a SVM buffer.
    static constexpr const char* Sampler   = "sampler_t";  // This resource is a sampler.
    static constexpr const char* Image1d   = "image1d_t";  // This resource is a 1D surface.
    static constexpr const char* Image2d   = "image2d_t";  // This resource is a 2D surface.
    static constexpr const char* Image3d   = "image3d_t";  // This resource is a 3D surface.
};

// optional resource access kind
enum class cmc_access_kind : int32_t {
    undef,
    read_only,
    write_only,
    read_write
};

enum class cmc_arg_kind : int32_t {
    General,
    LocalSize,  // IMPLICIT_LOCAL_SIZE
    GroupCount, // IMPLICIT_NUM_GROUPS
    Buffer,     // 1D buffer
    SVM,        // stateless global pointer
    Sampler,
    Image1d,
    Image2d,
    Image3d
};

struct cmc_arg_info {
    // The argument kind.
    cmc_arg_kind kind = cmc_arg_kind::General;

    // The argument index in this kernel.
    int32_t index = 0;

    // the byte offset of this argument in payload
    int32_t offset = 0;

    // The byte size of this argument in payload
    int32_t sizeInBytes = 0;

    // The BTI for this resource, if applicable.
    int32_t BTI = 0;

    // the optional resource access kind, if applicable.
    cmc_access_kind access = cmc_access_kind::undef;
};

// compilation interface bewteen cmc and igc
struct cmc_kernel_info {
    /// The kernel name.
    std::string name;

    /// The kernel argument info.
    std::vector<cmc_arg_info> arg_descs;

    // ThreadPayload
    bool HasLocalIDx = false;
    bool HasLocalIDy = false;
    bool HasLocalIDz = false;
    bool HasGroupID = false;

    // ExecutionEnivronment
    uint32_t SLMSize = 0;
    uint32_t NumGRFRequired = 128;
    uint32_t GRFByteSize = 32;
    bool HasBarriers = false;
    bool HasReadWriteImages = false;
};

struct cmc_compile_info {
    /// The vISA binary size in bytes.
    uint64_t binary_size;

    /// The vISA binary data.
    void* binary;

    uint32_t pointer_size_in_bytes;

    /// The vISA major version.
    uint32_t visa_major_version;

    /// The vISA minor version.
    uint32_t visa_minor_version;

    /// The kernel infomation.
    std::vector<cmc_kernel_info *> *kernel_info;

    /// The context for this compilation. This opaque data holds all memory
    /// allocations that will be freed in the end.
    void* context;
};

extern "C" __EXPORT__ int32_t cmc_load_and_compile(const char* input,
                                                   size_t input_size,
                                                   const char* const options,
                                                   cmc_compile_info** output);

extern "C" __EXPORT__ int32_t cmc_free_compile_info(cmc_compile_info* output);

#endif // _IGCMC_H_
