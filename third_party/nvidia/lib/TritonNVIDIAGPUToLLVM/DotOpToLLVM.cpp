#include "PatternTritonGPUOpToLLVM.h"
#include "Utility.h"

#include "triton/Conversion/TritonGPUToLLVM/PatternTritonGPUOpToLLVM.h"

using namespace mlir;
using namespace mlir::triton;

using ::mlir::triton::gpu::getShapePerCTA;
using ::mlir::triton::gpu::NvidiaMmaEncodingAttr;

LogicalResult convertMMA(triton::DotOp op, triton::DotOp::Adaptor adaptor,
                         const LLVMTypeConverter *typeConverter,
                         ConversionPatternRewriter &rewriter, bool isTuring,
                         bool isHopperF64);

LogicalResult convertWGMMA(triton::nvidia_gpu::WarpGroupDotOp op,
                           triton::nvidia_gpu::WarpGroupDotOp::Adaptor adaptor,
                           const LLVMTypeConverter *typeConverter,
                           ConversionPatternRewriter &rewriter, Value thread);
namespace {
struct DotOpConversion : public ConvertOpToLLVMPattern<triton::DotOp> {
  using ConvertOpToLLVMPattern<triton::DotOp>::ConvertOpToLLVMPattern;

  DotOpConversion(LLVMTypeConverter &converter, int computeCapability,
                  PatternBenefit benefit)
      : ConvertOpToLLVMPattern<triton::DotOp>(converter, benefit),
        computeCapability(computeCapability) {}

  LogicalResult
  matchAndRewrite(triton::DotOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    // D = A * B + C
    Value A = op.getA();
    Value D = op.getResult();

    // Here we assume the DotOp's operands always comes from shared memory.
    auto AShapePerCTA = getShapePerCTA(A.getType());
    size_t reduceAxis = 1;
    unsigned K = AShapePerCTA[reduceAxis];
    bool isOuter = K == 1;

    NvidiaMmaEncodingAttr mmaLayout = dyn_cast<NvidiaMmaEncodingAttr>(
        cast<RankedTensorType>(D.getType()).getEncoding());
    if (!isOuter && mmaLayout && supportMMA(op, mmaLayout.getVersionMajor())) {
      if (mmaLayout.getVersionMajor() == 2) {
        bool isHopperF64 =
            computeCapability == 90 &&
            cast<RankedTensorType>(A.getType()).getElementType().isF64();
        return convertMMA(op, adaptor, getTypeConverter(), rewriter,
                          mmaLayout.isTuring(), isHopperF64);
      }

      llvm::report_fatal_error(
          "Unsupported MMA kind found when converting DotOp to LLVM.");
    }

    if (isa<BlockedEncodingAttr>(
            cast<RankedTensorType>(D.getType()).getEncoding()))
      return convertFMADot(op, adaptor, getTypeConverter(), rewriter);

    llvm::report_fatal_error(
        "Unsupported DotOp found when converting TritonGPU to LLVM.");
  }

private:
  int computeCapability;
};

struct WarpGroupDotOpConversion
    : public ConvertOpToLLVMPattern<triton::nvidia_gpu::WarpGroupDotOp> {
  using ConvertOpToLLVMPattern<
      triton::nvidia_gpu::WarpGroupDotOp>::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::nvidia_gpu::WarpGroupDotOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto loc = op.getLoc();
    // D = A * B + C
    Value A = op.getA();
    TypedValue<RankedTensorType> D = op.getResult();

    // Here we assume the DotOp's operands always comes from shared memory.
    auto AShapePerCTA = getShapePerCTA(A.getType());
    size_t reduceAxis = 1;
    unsigned K = AShapePerCTA[reduceAxis];
    bool isOuter = K == 1;

    auto mmaLayout = cast<NvidiaMmaEncodingAttr>(D.getType().getEncoding());
    if (!isOuter && supportMMA(op.getOperand(0), mmaLayout.getVersionMajor())) {
      return convertWGMMA(op, adaptor, getTypeConverter(), rewriter,
                          getThreadId(rewriter, loc));
    }

    return op.emitError(
        "Unsupported WarpGroupDotOp found when converting TritonGPU to LLVM.");
  }
};

struct WarpGroupDotWaitOpConversion
    : public ConvertOpToLLVMPattern<triton::nvidia_gpu::WarpGroupDotWaitOp> {
  using ConvertOpToLLVMPattern<
      triton::nvidia_gpu::WarpGroupDotWaitOp>::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::nvidia_gpu::WarpGroupDotWaitOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto pendings = op.getPendings();
    Location loc = op.getLoc();
    if (adaptor.getInputs().size() <= 1) {
      Value input =
          adaptor.getInputs().size() == 1 ? adaptor.getInputs()[0] : Value();
      rewriter.replaceOpWithNewOp<triton::nvgpu::WGMMAWaitGroupOp>(op, input,
                                                                   pendings);
      return success();
    }
    std::vector<Type> types;
    // Pack the inputs into a single struct.
    for (Value input : adaptor.getInputs()) {
      auto structType = dyn_cast<LLVM::LLVMStructType>(input.getType());
      if (!structType)
        return failure();
      for (Type type : structType.getBody())
        types.push_back(type);
    }
    auto packedType =
        LLVM::LLVMStructType::getLiteral(rewriter.getContext(), types);
    Value packed = rewriter.create<LLVM::UndefOp>(loc, packedType);
    unsigned outputStructIndex = 0;
    for (Value input : adaptor.getInputs()) {
      auto structType = dyn_cast<LLVM::LLVMStructType>(input.getType());
      for (unsigned i = 0; i < structType.getBody().size(); ++i) {
        Value value = rewriter.create<LLVM::ExtractValueOp>(
            loc, structType.getBody()[i], input, i);
        packed = rewriter.create<LLVM::InsertValueOp>(
            loc, packedType, packed, value, outputStructIndex++);
      }
    }
    Value packedOutput =
        rewriter.create<triton::nvgpu::WGMMAWaitGroupOp>(loc, packed, pendings);
    // Unpack the output into the original struct types.
    SmallVector<Value> outputs;
    outputStructIndex = 0;
    for (Value input : adaptor.getInputs()) {
      auto structType = cast<LLVM::LLVMStructType>(input.getType());
      Value unpacked = rewriter.create<LLVM::UndefOp>(loc, structType);
      for (unsigned i = 0; i < structType.getBody().size(); ++i) {
        Value value = rewriter.create<LLVM::ExtractValueOp>(
            loc, packedType.getBody()[outputStructIndex], packedOutput,
            outputStructIndex);
        outputStructIndex++;
        unpacked = rewriter.create<LLVM::InsertValueOp>(loc, structType,
                                                        unpacked, value, i);
      }
      outputs.push_back(unpacked);
    }
    rewriter.replaceOp(op, outputs);
    return success();
  }
};
} // namespace

void mlir::triton::NVIDIA::populateDotOpToLLVMPatterns(
    LLVMTypeConverter &typeConverter, RewritePatternSet &patterns,
    int computeCapability, PatternBenefit benefit) {
  patterns.add<DotOpConversion>(typeConverter, computeCapability, benefit);
  patterns.add<WarpGroupDotOpConversion>(typeConverter, benefit);
  patterns.add<WarpGroupDotWaitOpConversion>(typeConverter, benefit);
}
