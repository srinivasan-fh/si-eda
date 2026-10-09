import Foundation
import Metal

/// Runs the built-in model's matrix products on the GPU with Metal (docs/AI.md, "GPU"). The weights stay where the
/// core memory-mapped them (one no-copy buffer over the whole model file); each call sends the input vectors, runs one
/// threadgroup per weight row (and per 8 input vectors), and reads the products back. Everything else — attention,
/// norms, sampling — stays on the CPU. Any call it cannot run (an unsupported format) goes back to the CPU.
final class MetalMatmul {
    /// GGUF tensor types the kernel reads: F32, F16, BF16, Q4_0, Q4_1, Q5_0, Q5_1, Q8_0, Q4_K, Q5_K, Q6_K.
    static let types: Set<Int32> = [0, 1, 30, 2, 3, 6, 7, 8, 12, 13, 14]

    private struct Params {
        var offset: UInt64
        var rowBytes: UInt64
        var cols: Int32
        var rows: Int32
        var type: Int32
        var batch: Int32
    }

    private let device: MTLDevice
    private let queue: MTLCommandQueue
    private let pipeline: MTLComputePipelineState
    private let weights: MTLBuffer
    private var input: MTLBuffer?
    private var output: MTLBuffer?

    /// nil without a GPU, when `base` is not page-aligned (a memory-mapped file is) or the file exceeds a GPU buffer.
    init?(base: UnsafeRawPointer, size: Int, device: MTLDevice? = MTLCreateSystemDefaultDevice()) {
        let page = Int(getpagesize())
        guard let device, size > 0, Int(bitPattern: base) % page == 0, let queue = device.makeCommandQueue() else { return nil }
        let length = (size + page - 1) / page * page
        guard length <= device.maxBufferLength,
              let weights = device.makeBuffer(bytesNoCopy: UnsafeMutableRawPointer(mutating: base), length: length,
                                              options: .storageModeShared, deallocator: nil),
              let library = try? device.makeLibrary(source: Self.source, options: nil),
              let function = library.makeFunction(name: "matmul"),
              let pipeline = try? device.makeComputePipelineState(function: function) else { return nil }
        self.device = device
        self.queue = queue
        self.weights = weights
        self.pipeline = pipeline
    }

    /// y (batch × rows) = W x for the weight rows `offset` bytes into the file; false when the GPU cannot run it.
    func multiply(offset: UInt64, type: Int32, cols: Int64, rows: Int64, rowBytes: UInt64, x: UnsafePointer<Float>,
                  batch: Int32, y: UnsafeMutablePointer<Float>) -> Bool {
        guard Self.types.contains(type), cols > 0, cols % 32 == 0, rows > 0, rows < Int64(Int32.max), batch > 0,
              offset + rowBytes * UInt64(rows) <= UInt64(weights.length) else { return false }
        let inBytes = Int(batch) * Int(cols) * 4, outBytes = Int(batch) * Int(rows) * 4
        guard let xb = buffer(&input, inBytes), let yb = buffer(&output, outBytes),
              let command = queue.makeCommandBuffer(), let encoder = command.makeComputeCommandEncoder() else { return false }
        xb.contents().copyMemory(from: x, byteCount: inBytes)
        var params = Params(offset: offset, rowBytes: rowBytes, cols: Int32(cols), rows: Int32(rows), type: type, batch: batch)
        encoder.setComputePipelineState(pipeline)
        encoder.setBuffer(weights, offset: 0, index: 0)
        encoder.setBuffer(xb, offset: 0, index: 1)
        encoder.setBuffer(yb, offset: 0, index: 2)
        encoder.setBytes(&params, length: MemoryLayout<Params>.stride, index: 3)
        encoder.dispatchThreadgroups(MTLSize(width: Int(rows), height: (Int(batch) + 7) / 8, depth: 1),
                                     threadsPerThreadgroup: MTLSize(width: 32, height: 1, depth: 1))
        encoder.endEncoding()
        command.commit()
        command.waitUntilCompleted()
        guard command.status == .completed else { return false }
        y.update(from: yb.contents().assumingMemoryBound(to: Float.self), count: Int(batch) * Int(rows))
        return true
    }

    private func buffer(_ slot: inout MTLBuffer?, _ bytes: Int) -> MTLBuffer? {
        if let existing = slot, existing.length >= bytes { return existing }
        slot = device.makeBuffer(length: max(bytes, 1 << 16), options: .storageModeShared)
        return slot
    }

    /// The core's accelerator callback; `context` is an unretained MetalMatmul.
    static let callback: SiedaLlmMatmul = { context, offset, type, cols, rows, rowBytes, x, batch, y in
        guard let context, let x, let y else { return 0 }
        let gpu = Unmanaged<MetalMatmul>.fromOpaque(context).takeUnretainedValue()
        return gpu.multiply(offset: offset, type: type, cols: cols, rows: rows, rowBytes: rowBytes, x: x, batch: batch, y: y) ? 1 : 0
    }

    /// The kernels (compiled when the model is opened). Each lane of a 32-wide threadgroup expands blocks of 32 weights
    /// (the same arithmetic as Core/src/LocalModel.cpp `dequantize`) and multiplies them with up to 8 input vectors.
    static let source = """
    #include <metal_stdlib>
    using namespace metal;

    struct Params { ulong offset; ulong rowBytes; int cols; int rows; int type; int batch; };
    constant int TILE = 8;

    static inline float f16(device const uchar* p) { return float(as_type<half>(ushort(p[0] | (p[1] << 8)))); }
    static inline uint u32(device const uchar* p) { return uint(p[0]) | (uint(p[1]) << 8) | (uint(p[2]) << 16) | (uint(p[3]) << 24); }
    static inline void scaleMin(int j, device const uchar* q, thread float& d, thread float& m) {
        if (j < 4) { d = q[j] & 63; m = q[j + 4] & 63; }
        else { d = (q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4); m = (q[j + 4] >> 4) | ((q[j] >> 6) << 4); }
    }

    // The 32 weights of unit u (elements 32u … 32u + 31) of a row.
    static void unit32(device const uchar* row, int type, int u, thread float* v) {
        switch (type) {
        case 0: { device const float* f = (device const float*)row + u * 32; for (int i = 0; i < 32; ++i) v[i] = f[i]; break; }
        case 1: { device const half* h = (device const half*)row + u * 32; for (int i = 0; i < 32; ++i) v[i] = float(h[i]); break; }
        case 30: { device const ushort* h = (device const ushort*)row + u * 32; for (int i = 0; i < 32; ++i) v[i] = as_type<float>(uint(h[i]) << 16); break; }
        case 2: case 3: case 6: case 7: {
            const bool hasMin = type == 3 || type == 7, five = type == 6 || type == 7;
            device const uchar* p = row + u * (type == 2 ? 18 : type == 3 ? 20 : type == 6 ? 22 : 24);
            const float d = f16(p), mn = hasMin ? f16(p + 2) : (five ? -16.0f : -8.0f) * d;
            device const uchar* hb = p + (hasMin ? 4 : 2);
            const uint qh = five ? u32(hb) : 0;
            device const uchar* qs = hb + (five ? 4 : 0);
            for (int j = 0; j < 16; ++j) {
                const uint h0 = five ? ((qh >> j) << 4) & 0x10 : 0, h1 = five ? (qh >> (j + 12)) & 0x10 : 0;
                v[j] = float((qs[j] & 0xF) | h0) * d + mn;
                v[j + 16] = float((qs[j] >> 4) | h1) * d + mn;
            }
            break;
        }
        case 8: {
            device const uchar* p = row + u * 34;
            const float d = f16(p);
            for (int i = 0; i < 32; ++i) v[i] = float(as_type<char>(p[2 + i])) * d;
            break;
        }
        case 12: case 13: {
            device const uchar* p = row + (u / 8) * (type == 12 ? 144 : 176);
            const int sub = u % 8;
            float sc, mn;
            scaleMin(sub, p + 4, sc, mn);
            const float d = f16(p) * sc, m = f16(p + 2) * mn;
            device const uchar* q = p + (type == 13 ? 48 : 16) + 32 * (sub / 2);
            device const uchar* qh = p + 16;
            for (int l = 0; l < 32; ++l) {
                const int nib = (sub & 1) ? q[l] >> 4 : q[l] & 0xF;
                v[l] = d * float(nib + ((type == 13 && (qh[l] & (1 << sub))) ? 16 : 0)) - m;
            }
            break;
        }
        case 14: {
            device const uchar* p = row + (u / 8) * 210;
            const int sub = u % 8, h = sub / 4, k = sub % 4;
            device const uchar* ql = p + 64 * h + (k & 1) * 32;
            device const uchar* qh = p + 128 + 32 * h;
            device const char* sc = (device const char*)(p + 192) + 8 * h + 2 * k;
            const float d = f16(p + 208);
            for (int l = 0; l < 32; ++l) {
                const int lo = k >= 2 ? ql[l] >> 4 : ql[l] & 0xF;
                v[l] = d * float(sc[l / 16]) * float((lo | (((qh[l] >> (2 * k)) & 3) << 4)) - 32);
            }
            break;
        }
        default: for (int i = 0; i < 32; ++i) v[i] = 0;
        }
    }

    kernel void matmul(device const uchar* w [[buffer(0)]], device const float* x [[buffer(1)]], device float* y [[buffer(2)]],
                       constant Params& p [[buffer(3)]], uint2 group [[threadgroup_position_in_grid]],
                       uint lane [[thread_index_in_simdgroup]]) {
        const int r = int(group.x), b0 = int(group.y) * TILE, nb = min(TILE, p.batch - b0), units = p.cols / 32;
        device const uchar* row = w + p.offset + ulong(r) * p.rowBytes;
        float acc[TILE] = {0, 0, 0, 0, 0, 0, 0, 0};
        for (int u = int(lane); u < units; u += 32) {
            float v[32];
            unit32(row, p.type, u, v);
            for (int b = 0; b < TILE; ++b) {
                if (b >= nb) break;
                device const float4* xb = (device const float4*)(x + ulong(b0 + b) * ulong(p.cols) + ulong(u) * 32);
                float s = 0;
                for (int i = 0; i < 8; ++i) s += dot(float4(v[4 * i], v[4 * i + 1], v[4 * i + 2], v[4 * i + 3]), xb[i]);
                acc[b] += s;
            }
        }
        for (int b = 0; b < TILE; ++b) {
            if (b >= nb) break;
            const float t = simd_sum(acc[b]);
            if (lane == 0) y[ulong(b0 + b) * ulong(p.rows) + ulong(r)] = t;
        }
    }
    """
}
