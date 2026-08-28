#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif
#include "internal.h"
#include "forge/config.h"
#include <errno.h>
#include <inttypes.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#ifdef __linux__
#include <sched.h>
#if defined(__aarch64__) || defined(__arm__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#endif
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#include <mach/mach.h>
#endif
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif
#ifdef FORGE_WITH_LLAMA
#include "llama.h"
#include "ggml-backend.h"
#include "gguf.h"
#endif

#define GIB UINT64_C(1073741824)

static uint64_t saturating_add(uint64_t a, uint64_t b) {
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}

static uint64_t saturating_multiply(uint64_t a, uint64_t b) {
    return a && b > UINT64_MAX / a ? UINT64_MAX : a * b;
}

static uint64_t remaining(uint64_t available, uint64_t needed) {
    return available > needed ? available - needed : 0;
}

static void hardware_clear_error(forge_error *e) {
    if (e) {
        e->code = FORGE_OK;
        e->message[0] = 0;
    }
}

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
static bool cpu_id(unsigned leaf, unsigned subleaf, unsigned out[4]) {
#ifdef _MSC_VER
    int regs[4];
    __cpuid(regs, (int)(leaf & 0x80000000u));
    if ((unsigned)regs[0] < leaf)
        return false;
    __cpuidex(regs, (int)leaf, (int)subleaf);
    for (size_t i = 0; i < 4; i++)
        out[i] = (unsigned)regs[i];
    return true;
#else
    return __get_cpuid_count(leaf, subleaf, &out[0], &out[1], &out[2], &out[3]) != 0;
#endif
}

static uint64_t cpu_xcr0(void) {
#ifdef _MSC_VER
    return _xgetbv(0);
#else
    unsigned lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return ((uint64_t)hi << 32) | lo;
#endif
}
#endif

static void detect_cpu(forge_hardware *hardware) {
#if defined(_M_X64) || defined(__x86_64__)
    strcpy(hardware->cpu_arch, "x86_64");
#elif defined(_M_IX86) || defined(__i386__)
    strcpy(hardware->cpu_arch, "x86");
#elif defined(_M_ARM64) || defined(__aarch64__)
    strcpy(hardware->cpu_arch, "aarch64");
#elif defined(_M_ARM) || defined(__arm__)
    strcpy(hardware->cpu_arch, "arm");
#elif defined(__riscv)
    strcpy(hardware->cpu_arch, "riscv");
#else
    strcpy(hardware->cpu_arch, "unknown");
#endif

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    unsigned regs[4] = {0};
    if (cpu_id(1, 0, regs)) {
        if (regs[3] & (1u << 26))
            hardware->cpu_features |= FORGE_CPU_SSE2;
        /* AVX requires both hardware support and OS preservation of YMM state.
         * Never execute xgetbv unless the OSXSAVE bit is present. */
        if ((regs[2] & (1u << 27)) && (regs[2] & (1u << 28)) &&
            (cpu_xcr0() & UINT64_C(6)) == UINT64_C(6)) {
            hardware->cpu_features |= FORGE_CPU_AVX;
            if (cpu_id(7, 0, regs) && (regs[1] & (1u << 5)))
                hardware->cpu_features |= FORGE_CPU_AVX2;
        }
    }
    char brand[49] = {0};
    for (unsigned i = 0; i < 3; i++) {
        if (!cpu_id(0x80000002u + i, 0, regs))
            break;
        memcpy(brand + i * 16, regs, 16);
    }
    const char *name = brand;
    while (*name == ' ')
        name++;
    snprintf(hardware->cpu_name, sizeof(hardware->cpu_name), "%s", name);
#elif defined(_WIN32) && (defined(_M_ARM64) || defined(_M_ARM))
    if (IsProcessorFeaturePresent(PF_ARM_NEON_INSTRUCTIONS_AVAILABLE))
        hardware->cpu_features |= FORGE_CPU_NEON;
#elif defined(__linux__) && defined(__aarch64__)
    if (getauxval(AT_HWCAP) & HWCAP_ASIMD)
        hardware->cpu_features |= FORGE_CPU_NEON;
#elif defined(__linux__) && defined(__arm__) && defined(HWCAP_NEON)
    if (getauxval(AT_HWCAP) & HWCAP_NEON)
        hardware->cpu_features |= FORGE_CPU_NEON;
#endif

#ifdef _WIN32
    DWORD logical = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    hardware->logical_cpus = logical ? (unsigned)logical : 1;
    DWORD_PTR process_mask = 0, system_mask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) {
        unsigned available = 0;
        while (process_mask) {
            available += (unsigned)(process_mask & 1);
            process_mask >>= 1;
        }
        if (available && available < hardware->logical_cpus)
            hardware->logical_cpus = available;
    }
#else
    long logical = sysconf(_SC_NPROCESSORS_ONLN);
    hardware->logical_cpus =
        logical > 0 && (unsigned long)logical <= UINT_MAX ? (unsigned)logical : 1;
#ifdef __linux__
    cpu_set_t affinity;
    if (sched_getaffinity(0, sizeof(affinity), &affinity) == 0) {
        int count = CPU_COUNT(&affinity);
        if (count > 0 && (unsigned)count < hardware->logical_cpus)
            hardware->logical_cpus = (unsigned)count;
    }
#endif
#endif

#ifdef __APPLE__
    size_t length = sizeof(hardware->cpu_name);
    if (sysctlbyname("machdep.cpu.brand_string", hardware->cpu_name, &length, NULL, 0) != 0) {
        length = sizeof(hardware->cpu_name);
        if (sysctlbyname("hw.model", hardware->cpu_name, &length, NULL, 0) != 0)
            hardware->cpu_name[0] = 0;
    }
    hardware->cpu_name[sizeof(hardware->cpu_name) - 1] = 0;
    int neon = 0;
    length = sizeof(neon);
    if (sysctlbyname("hw.optional.neon", &neon, &length, NULL, 0) == 0 && neon)
        hardware->cpu_features |= FORGE_CPU_NEON;
#endif
    if (!hardware->cpu_name[0])
        snprintf(hardware->cpu_name, sizeof(hardware->cpu_name), "%s", hardware->cpu_arch);
}

static void detect_memory(forge_hardware *hardware) {
#ifdef _WIN32
    MEMORYSTATUSEX status = {0};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        hardware->ram_total_bytes = status.ullTotalPhys;
        hardware->ram_available_bytes = status.ullAvailPhys;
        hardware->ram_total_known = true;
        hardware->ram_available_known = true;
    }
#elif defined(__APPLE__)
    uint64_t total = 0;
    size_t length = sizeof(total);
    if (sysctlbyname("hw.memsize", &total, &length, NULL, 0) == 0) {
        hardware->ram_total_bytes = total;
        hardware->ram_total_known = true;
    }
    mach_port_t host = mach_host_self();
    vm_size_t page_size = 0;
    vm_statistics64_data_t vm = {0};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_page_size(host, &page_size) == KERN_SUCCESS &&
        host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&vm, &count) == KERN_SUCCESS) {
        /* Free + inactive pages are an available-memory estimate, not a promise
         * that macOS can reclaim all inactive memory immediately. */
        uint64_t pages = (uint64_t)vm.free_count + vm.inactive_count;
        hardware->ram_available_bytes = saturating_multiply(pages, (uint64_t)page_size);
        hardware->ram_available_known = true;
    }
    mach_port_deallocate(mach_task_self(), host);
#else
    long page_size = sysconf(_SC_PAGESIZE);
#ifdef _SC_PHYS_PAGES
    long pages = sysconf(_SC_PHYS_PAGES);
    if (page_size > 0 && pages > 0) {
        hardware->ram_total_bytes = saturating_multiply((uint64_t)pages, (uint64_t)page_size);
        hardware->ram_total_known = true;
    }
#endif
#ifdef _SC_AVPHYS_PAGES
    long available = sysconf(_SC_AVPHYS_PAGES);
    if (page_size > 0 && available >= 0) {
        hardware->ram_available_bytes =
            saturating_multiply((uint64_t)available, (uint64_t)page_size);
        hardware->ram_available_known = true;
    }
#endif
#ifdef __linux__
    FILE *file = fopen("/proc/meminfo", "r");
    if (file) {
        char line[256];
        while (fgets(line, sizeof(line), file)) {
            uint64_t kib = 0;
            if (sscanf(line, "MemAvailable: %" SCNu64 " kB", &kib) == 1) {
                hardware->ram_available_bytes = saturating_multiply(kib, 1024);
                hardware->ram_available_known = true;
                break;
            }
        }
        fclose(file);
    }
#endif
#endif
    if (hardware->ram_total_known && hardware->ram_available_known &&
        hardware->ram_available_bytes > hardware->ram_total_bytes)
        hardware->ram_available_bytes = hardware->ram_total_bytes;
}

forge_status forge_hardware_detect(forge_hardware *hardware, forge_error *e) {
    if (!hardware)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Hardware output is required");
    memset(hardware, 0, sizeof(*hardware));
    detect_cpu(hardware);
    detect_memory(hardware);
#ifdef FORGE_WITH_LLAMA
    /* Enumerate the same backend registry used by inference, without creating a
     * model, context, or device allocation. Keep backend lifetime process-wide. */
    ggml_backend_load_all();
    hardware->gpu_detection_available = true;
    char ids[FORGE_HARDWARE_MAX_GPUS][128] = {{0}};
    for (int pass = 0; pass < 2; pass++) {
        enum ggml_backend_dev_type wanted =
            pass == 0 ? GGML_BACKEND_DEVICE_TYPE_GPU : GGML_BACKEND_DEVICE_TYPE_IGPU;
        for (size_t i = 0; i < ggml_backend_dev_count(); i++) {
            ggml_backend_dev_t device = ggml_backend_dev_get(i);
            if (ggml_backend_dev_type(device) != wanted)
                continue;
            ggml_backend_reg_t reg = ggml_backend_dev_backend_reg(device);
            const char *backend_name = ggml_backend_reg_name(reg);
            if (backend_name && !strcmp(backend_name, "RPC"))
                continue;
            struct ggml_backend_dev_props props = {0};
            ggml_backend_dev_get_props(device, &props);
            bool duplicate = false;
            if (props.device_id && *props.device_id) {
                for (size_t j = 0; j < hardware->gpu_count; j++)
                    if (!strcmp(ids[j], props.device_id))
                        duplicate = true;
            }
            if (duplicate)
                continue;
            if (hardware->gpu_count == FORGE_HARDWARE_MAX_GPUS) {
                hardware->gpu_list_truncated = true;
                continue;
            }
            size_t slot = hardware->gpu_count++;
            forge_gpu_info *gpu = &hardware->gpus[slot];
            snprintf(gpu->name, sizeof(gpu->name), "%s",
                     props.description ? props.description
                                       : (props.name ? props.name : "unnamed GPU"));
            gpu->total_bytes = (uint64_t)props.memory_total;
            gpu->available_bytes = (uint64_t)props.memory_free;
            gpu->memory_known = props.memory_total > 0 && props.memory_free <= props.memory_total;
            gpu->unified_memory = wanted == GGML_BACKEND_DEVICE_TYPE_IGPU;
            /* The pinned Metal backend reports GPU, including on Apple silicon,
             * and exposes recommendedMaxWorkingSetSize minus this process's
             * allocations. It is not a measurement of free dedicated VRAM. */
            gpu->memory_is_budget = backend_name && !strcmp(backend_name, "MTL");
#if defined(__APPLE__) && defined(__aarch64__)
            if (gpu->memory_is_budget)
                gpu->unified_memory = true;
#endif
            if (props.device_id)
                snprintf(ids[slot], sizeof(ids[slot]), "%s", props.device_id);
        }
    }
#endif
    hardware_clear_error(e);
    return FORGE_OK;
}

#ifdef FORGE_WITH_LLAMA
/* Return 1 for a scalar integer, 0 for absence, -1 for any unsupported type.
 * llama's string metadata API omits arrays; checking the public GGUF type first
 * prevents an array-valued dimension being mistaken for an absent default. */
static int metadata_integer(const struct llama_model *model, const struct gguf_context *gguf,
                            const char *architecture, const char *suffix, uint64_t *out) {
    char key[192], value[64];
    snprintf(key, sizeof(key), "%s.%s", architecture, suffix);
    int64_t id = gguf_find_key(gguf, key);
    if (id < 0)
        return 0;
    enum gguf_type type = gguf_get_kv_type(gguf, id);
    if (type != GGUF_TYPE_UINT8 && type != GGUF_TYPE_UINT16 && type != GGUF_TYPE_UINT32 &&
        type != GGUF_TYPE_UINT64 && type != GGUF_TYPE_INT8 && type != GGUF_TYPE_INT16 &&
        type != GGUF_TYPE_INT32 && type != GGUF_TYPE_INT64)
        return -1;
    int32_t length = llama_model_meta_val_str(model, key, value, sizeof(value));
    if (length < 1 || (size_t)length >= sizeof(value))
        return -1;
    for (const char *p = value; *p; p++)
        if (*p < '0' || *p > '9')
            return -1;
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (errno || !end || *end)
        return -1;
    *out = (uint64_t)parsed;
    return 1;
}

static void model_geometry(const struct llama_model *model, const struct gguf_context *gguf,
                           forge_model_requirements *requirements) {
    int32_t arch_length =
        llama_model_meta_val_str(model, "general.architecture", requirements->architecture,
                                 sizeof(requirements->architecture));
    if (arch_length < 1 || (size_t)arch_length >= sizeof(requirements->architecture)) {
        requirements->architecture[0] = 0;
        return;
    }
    const char *arch = requirements->architecture;
    uint64_t layers = 0, context = 0;
    if (metadata_integer(model, gguf, arch, "block_count", &layers) == 1 && layers <= 65535)
        requirements->layer_count = (size_t)layers;
    if (metadata_integer(model, gguf, arch, "context_length", &context) == 1 && context <= SIZE_MAX)
        requirements->training_context = (size_t)context;

    static const char *const supported[] = {"llama", "qwen2", "qwen2moe", "qwen3", "qwen3moe"};
    bool conventional = false;
    for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++)
        if (!strcmp(arch, supported[i]))
            conventional = true;
    if (!conventional || !layers || layers > 65535)
        return;
    uint64_t embedding = 0, heads = 0, kv_heads = 0, key_length = 0, value_length = 0;
    if (metadata_integer(model, gguf, arch, "embedding_length", &embedding) != 1 ||
        metadata_integer(model, gguf, arch, "attention.head_count", &heads) != 1 || !embedding ||
        !heads || embedding > 1048576 || heads > 65535)
        return;
    int kv_status = metadata_integer(model, gguf, arch, "attention.head_count_kv", &kv_heads);
    if (kv_status < 0)
        return;
    if (kv_status == 0)
        kv_heads = heads; /* The upstream conventional-attention default. */
    if (!kv_heads || kv_heads > heads || heads % kv_heads)
        return;
    int key_status = metadata_integer(model, gguf, arch, "attention.key_length", &key_length);
    int value_status = metadata_integer(model, gguf, arch, "attention.value_length", &value_length);
    if (key_status < 0 || value_status < 0)
        return;
    if ((key_status == 0 || value_status == 0) && embedding % heads)
        return;
    if (key_status == 0)
        key_length = embedding / heads;
    if (value_status == 0)
        value_length = embedding / heads;
    if (!key_length || !value_length || key_length > 1048576 || value_length > 1048576)
        return;
    uint64_t kv = saturating_multiply(layers, kv_heads);
    kv = saturating_multiply(kv, saturating_add(key_length, value_length));
    kv = saturating_multiply(kv, 2); /* f16 K and V element sizes, one sequence. */
    if (kv == UINT64_MAX)
        return;
    requirements->kv_bytes_per_token = kv;
    requirements->kv_bytes_known = true;
    snprintf(requirements->note, sizeof(requirements->note),
             "Scalar %s attention metadata; f16 K+V payload for one sequence. "
             "Absent head dimensions use embedding/head_count. Runtime buffers and padding "
             "are not included; sliding-window layers use a full-context upper estimate.",
             arch);
}
#endif

forge_status forge_hardware_model_file(const char *path, forge_model_requirements *requirements,
                                       forge_error *e) {
    if (!path || !*path || !requirements)
        return fg_error(e, FORGE_ERR_ARGUMENT,
                        "A local model path and requirements output are required");
    memset(requirements, 0, sizeof(*requirements));
#ifdef _WIN32
    struct _stat64 st;
    bool regular = _stat64(path, &st) == 0 && (st.st_mode & _S_IFMT) == _S_IFREG;
#else
    struct stat st;
    bool regular = stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
    if (!regular || st.st_size < 4)
        return fg_error(e, FORGE_ERR_IO, "Model must be a readable regular GGUF file: %s", path);
    FILE *file = fopen(path, "rb");
    if (!file)
        return fg_error(e, FORGE_ERR_IO, "Cannot read model metadata: %s", path);
    char magic[4];
    size_t n = fread(magic, 1, sizeof(magic), file);
    fclose(file);
    if (n != sizeof(magic) || memcmp(magic, "GGUF", 4))
        return fg_error(e, FORGE_ERR_MODEL, "Not a GGUF model: %s", path);
    requirements->file_bytes = (uint64_t)st.st_size;
    requirements->model_bytes = requirements->file_bytes;
    requirements->model_bytes_known = true;
    snprintf(requirements->note, sizeof(requirements->note),
             "Only this file's size is known; tensor bytes, split-file totals, attention "
             "geometry and KV memory are unknown without the llama.cpp metadata backend.");
#ifdef FORGE_WITH_LLAMA
    struct llama_model_params params = llama_model_default_params();
    ggml_backend_dev_t no_devices[] = {NULL};
    params.devices = no_devices;
    params.vocab_only = true;
    params.n_gpu_layers = 0;
    params.split_mode = LLAMA_SPLIT_MODE_NONE;
    /* Vocabulary-only loading does not require llama_backend_init. In particular,
     * do not initialize or select GPU devices just to inspect metadata. */
    struct llama_model *model = llama_model_load_from_file(path, params);
    if (!model)
        return fg_error(e, FORGE_ERR_MODEL, "Cannot load GGUF vocabulary/metadata: %s", path);
    requirements->metadata_available = true;
    uint64_t tensor_bytes = llama_model_size(model);
    if (tensor_bytes) {
        requirements->model_bytes = tensor_bytes;
        requirements->tensor_bytes_known = true;
    }
    snprintf(requirements->note, sizeof(requirements->note),
             "GGUF metadata read without loading tensors. KV geometry is unsupported or "
             "incomplete (including recurrent, hybrid, MLA and array-valued attention); "
             "no exact memory-fit claim is available.");
    struct gguf_init_params metadata_params = {true, NULL};
    struct gguf_context *gguf = gguf_init_from_file(path, metadata_params);
    if (gguf) {
        model_geometry(model, gguf, requirements);
        gguf_free(gguf);
    }
    llama_model_free(model);
#endif
    hardware_clear_error(e);
    return FORGE_OK;
}

static uint64_t with_margin(uint64_t bytes) {
    /* Round a 12.5% margin upward without overflowing the numerator. */
    uint64_t margin = bytes / 8 + (bytes % 8 != 0);
    return saturating_add(bytes, margin);
}

static uint64_t memory_reserve(uint64_t available) {
    uint64_t ten_percent = available / 10;
    return ten_percent > GIB ? ten_percent : GIB;
}

static size_t next_context(size_t current, size_t minimum) {
    size_t half = (current / 2 / 128) * 128;
    return half < minimum ? minimum : half;
}

forge_status forge_hardware_plan(const forge_hardware *hardware,
                                 const forge_model_requirements *requirements,
                                 size_t requested_context, size_t output_reserve,
                                 forge_hardware_plan_result *plan, forge_error *e) {
    if (!hardware || !requirements || !plan || requested_context < 128 ||
        requested_context > 1048576 || !output_reserve || output_reserve >= requested_context ||
        hardware->gpu_count > FORGE_HARDWARE_MAX_GPUS ||
        (requirements->kv_bytes_known && !requirements->kv_bytes_per_token) ||
        (requirements->model_bytes_known && !requirements->model_bytes))
        return fg_error(e, FORGE_ERR_ARGUMENT, "Invalid hardware-planning input or context budget");
    if (hardware->ram_total_known && hardware->ram_available_known &&
        hardware->ram_available_bytes > hardware->ram_total_bytes)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Available RAM exceeds measured total RAM");
    for (size_t i = 0; i < hardware->gpu_count; i++) {
        const forge_gpu_info *gpu = &hardware->gpus[i];
        if (gpu->memory_known && gpu->available_bytes > gpu->total_bytes)
            return fg_error(e, FORGE_ERR_ARGUMENT, "Available GPU memory exceeds measured total");
    }
    memset(plan, 0, sizeof(*plan));
    plan->gpu_index = -1;
    plan->context_tokens = requested_context;
    unsigned cpus = hardware->logical_cpus ? hardware->logical_cpus : 1;
    if (cpus > 2)
        cpus--; /* Leave one logical CPU available to the rest of the application. */
    plan->threads = (int)(cpus > 8 ? 8 : cpus);
    strcpy(plan->kv_format, "f16");
    plan->estimated_model_bytes = requirements->model_bytes;
    if (requirements->training_context && requirements->training_context < plan->context_tokens)
        plan->context_tokens = requirements->training_context;
    if (plan->context_tokens < 128 || plan->context_tokens <= output_reserve)
        return fg_error(e, FORGE_ERR_LIMIT,
                        "Model training context is too small for the "
                        "configured output reserve");
    size_t minimum_context = output_reserve + 1;
    if (minimum_context < 128)
        minimum_context = 128;

    uint64_t host_budget = 0, gpu_budget = 0, gpu_available = 0;
    bool host_known = hardware->ram_available_known;
    bool gpu_known = hardware->gpu_detection_available && hardware->gpu_count > 0 &&
                     hardware->gpus[0].memory_known;
    if (host_known) {
        plan->host_reserve_bytes = memory_reserve(hardware->ram_available_bytes);
        host_budget = remaining(hardware->ram_available_bytes, plan->host_reserve_bytes);
    }
    if (gpu_known) {
        gpu_available = hardware->gpus[0].available_bytes;
        if (hardware->gpus[0].unified_memory || hardware->gpus[0].memory_is_budget) {
            if (!host_known)
                gpu_known = false;
            else if (gpu_available > hardware->ram_available_bytes)
                gpu_available = hardware->ram_available_bytes;
        }
        if (gpu_known) {
            plan->gpu_reserve_bytes = memory_reserve(gpu_available);
            gpu_budget = remaining(gpu_available, plan->gpu_reserve_bytes);
        }
    }

    if (!requirements->model_bytes_known || !requirements->kv_bytes_known) {
        size_t fallback = minimum_context > 4096 ? minimum_context : 4096;
        if (plan->context_tokens > fallback)
            plan->context_tokens = fallback;
        plan->context_reduced = plan->context_tokens < requested_context;
        plan->fit = FORGE_FIT_UNKNOWN;
        if (requirements->model_bytes_known && host_known &&
            requirements->model_bytes > host_budget &&
            (!gpu_known || requirements->model_bytes > gpu_budget))
            plan->fit = FORGE_FIT_INSUFFICIENT;
        snprintf(plan->assumptions, sizeof(plan->assumptions),
                 "Model or KV geometry is unknown; automatic offload is disabled and context "
                 "is capped at 4096 where the output reserve permits. File size may omit "
                 "split parts and is not resident memory. f16 KV, no draft model; no fit "
                 "guarantee. CPU threads are capped at 8. OS measurements may exclude "
                 "container/process memory limits.");
        hardware_clear_error(e);
        return FORGE_OK;
    }
    plan->kv_estimate_available = true;
    uint64_t weights = with_margin(requirements->model_bytes);
    uint64_t kv = 0, total = 0;
    /* Full GPU residency still needs host memory for vocabulary, mappings and
     * staging. This is a deliberately coarse floor, not a loader peak estimate. */
    uint64_t staging = GIB + requirements->model_bytes / 16;
    bool gpu_host_ready = host_known && hardware->ram_available_bytes > staging;
    for (;;) {
        uint64_t payload =
            saturating_multiply(requirements->kv_bytes_per_token, (uint64_t)plan->context_tokens);
        kv = with_margin(payload);
        total = saturating_add(weights, kv);
        bool host_fits = host_known && total != UINT64_MAX && total <= host_budget;
        bool gpu_fits = gpu_known && gpu_host_ready && total != UINT64_MAX && total <= gpu_budget;
        if (host_fits || gpu_fits || plan->context_tokens <= minimum_context ||
            (!host_known && !gpu_known))
            break;
        plan->context_tokens = next_context(plan->context_tokens, minimum_context);
    }
    plan->estimated_kv_bytes =
        saturating_multiply(requirements->kv_bytes_per_token, (uint64_t)plan->context_tokens);
    plan->context_reduced = plan->context_tokens < requested_context;
    bool host_fits = host_known && total != UINT64_MAX && total <= host_budget;
    bool gpu_fits = gpu_known && gpu_host_ready && total != UINT64_MAX && total <= gpu_budget;
    if (gpu_fits) {
        plan->gpu_layers = -1;
        plan->gpu_index = 0;
        plan->gpu_headroom_bytes = remaining(gpu_budget, total);
        plan->host_headroom_bytes =
            (hardware->gpus[0].unified_memory || hardware->gpus[0].memory_is_budget)
                ? remaining(host_budget, total)
                : remaining(hardware->ram_available_bytes, staging);
        plan->fit = FORGE_FIT_ESTIMATED;
    } else if (host_fits) {
        plan->fit = FORGE_FIT_ESTIMATED;
        plan->host_headroom_bytes = remaining(host_budget, total);
        /* Partial offload is only a suggestion when the entire model also fits
         * in host RAM. Budget all KV on the GPU and overprice each average layer
         * by 2x; layers may be unequal and actual backend allocation may differ. */
        if (gpu_known && !hardware->gpus[0].unified_memory && !hardware->gpus[0].memory_is_budget &&
            requirements->layer_count > 0 && requirements->layer_count <= 65535 &&
            gpu_budget > kv) {
            uint64_t average =
                weights / requirements->layer_count + (weights % requirements->layer_count != 0);
            uint64_t layer_cost = saturating_multiply(average, 2);
            uint64_t layers = layer_cost ? (gpu_budget - kv) / layer_cost : 0;
            if (layers > requirements->layer_count)
                layers = requirements->layer_count;
            if (layers) {
                plan->gpu_layers = (int)layers;
                plan->gpu_index = 0;
                plan->gpu_headroom_bytes = remaining(
                    gpu_budget, saturating_add(kv, saturating_multiply(layers, layer_cost)));
            }
        }
    } else {
        plan->fit = host_known ? FORGE_FIT_INSUFFICIENT : FORGE_FIT_UNKNOWN;
    }
    snprintf(plan->assumptions, sizeof(plan->assumptions),
             "One sequence, f16 KV payload; model/KV each receive a 12.5%% margin plus "
             "max(1 GiB,10%% available) reserve. GPU 0 only; unified memory is not added "
             "to RAM. Partial layers use 2x average layer bytes. Host staging floor is "
             "1 GiB + model/16. No draft. Fit remains an estimate: batch/allocator/backend "
             "buffers and process/container limits are not modeled.");
    hardware_clear_error(e);
    return FORGE_OK;
}
