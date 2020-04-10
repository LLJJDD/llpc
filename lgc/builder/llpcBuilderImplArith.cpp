/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2020 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  llpcBuilderImplArith.cpp
 * @brief LLPC source file: implementation of arithmetic Builder methods
 ***********************************************************************************************************************
 */
#include "llvm/IR/IntrinsicsAMDGPU.h"

#include "llpcBuilderImpl.h"
#include "llpcPipelineState.h"
#include "llpcTargetInfo.h"

#define DEBUG_TYPE "llpc-builder-impl-arith"

using namespace lgc;
using namespace llvm;

// =====================================================================================================================
// Create calculation of 2D texture coordinates that would be used for accessing the selected cube map face for
// the given cube map texture coordinates. Returns <2 x float>.
Value* BuilderImplArith::CreateCubeFaceCoord(
    Value*        coord,     // [in] Input coordinate <3 x float>
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    Value* coordX = CreateExtractElement(coord, uint64_t(0));
    Value* coordY = CreateExtractElement(coord, 1);
    Value* coordZ = CreateExtractElement(coord, 2);
    Value* cubeMa = CreateIntrinsic(Intrinsic::amdgcn_cubema, {}, { coordX, coordY, coordZ }, nullptr);
    Value* recipMa = CreateFDiv(ConstantFP::get(getFloatTy(), 1.0), cubeMa);
    Value* cubeSc = CreateIntrinsic(Intrinsic::amdgcn_cubesc, {}, { coordX, coordY, coordZ }, nullptr);
    Value* scDivMa = CreateFMul(recipMa, cubeSc);
    Value* resultX = CreateFAdd(scDivMa, ConstantFP::get(getFloatTy(), 0.5));
    Value* cubeTc = CreateIntrinsic(Intrinsic::amdgcn_cubetc, {}, { coordX, coordY, coordZ }, nullptr);
    Value* tcDivMa = CreateFMul(recipMa, cubeTc);
    Value* resultY = CreateFAdd(tcDivMa, ConstantFP::get(getFloatTy(), 0.5));
    Value* result = CreateInsertElement(UndefValue::get(VectorType::get(getFloatTy(), 2)), resultX, uint64_t(0));
    result = CreateInsertElement(result, resultY, 1, instName);
    return result;
}

// =====================================================================================================================
// Create calculation of the index of the cube map face that would be accessed by a texture lookup function for
// the given cube map texture coordinates. Returns a single float with value:
//  0.0 = the cube map face facing the positive X direction
//  1.0 = the cube map face facing the negative X direction
//  2.0 = the cube map face facing the positive Y direction
//  3.0 = the cube map face facing the negative Y direction
//  4.0 = the cube map face facing the positive Z direction
//  5.0 = the cube map face facing the negative Z direction
Value* BuilderImplArith::CreateCubeFaceIndex(
    Value*        coord,     // [in] Input coordinate <3 x float>
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    Value* coordX = CreateExtractElement(coord, uint64_t(0));
    Value* coordY = CreateExtractElement(coord, 1);
    Value* coordZ = CreateExtractElement(coord, 2);
    return CreateIntrinsic(Intrinsic::amdgcn_cubeid, {}, { coordX, coordY, coordZ }, nullptr, instName);
}

// =====================================================================================================================
// Create scalar or vector FP truncate operation with the given rounding mode.
// Currently the rounding mode is only implemented for float/double -> half conversion.
Value* BuilderImplArith::CreateFpTruncWithRounding(
    Value*            value,             // [in] Input value
    Type*             destTy,            // [in] Type to convert to
    unsigned          roundingMode,       // Rounding mode
    const Twine&      instName)           // [in] Name to give instruction(s)
{
    if (value->getType()->getScalarType()->isDoubleTy())
        value = CreateFPTrunc(value, getConditionallyVectorizedTy(getFloatTy(), destTy));

    if (value->getType() == destTy)
        return value;

    assert(value->getType()->getScalarType()->isFloatTy() && destTy->getScalarType()->isHalfTy());

    // RTZ: Use cvt_pkrtz instruction.
    // TODO: We also use this for RTP and RTN for now.
    // TODO: Using a hard-coded value for rmToNearest due to flux in LLVM over
    // the namespace for this value - this will be removed once it has settled
    //if (roundingMode != fp::rmToNearest)
    if (roundingMode != 1 /* rmToNearest */ )
    {
        Value* result = scalarizeInPairs(value,
                                          [this](Value* inVec2)
                                          {
                                              Value* inVal0 = CreateExtractElement(inVec2, uint64_t(0));
                                              Value* inVal1 = CreateExtractElement(inVec2, 1);
                                              return CreateIntrinsic(Intrinsic::amdgcn_cvt_pkrtz,
                                                                     {},
                                                                     { inVal0, inVal1 });
                                          });
        result->setName(instName);
        return result;
    }

    // RTE.
    // float32: sign = [31], exponent = [30:23], mantissa = [22:0]
    // float16: sign = [15], exponent = [14:10], mantissa = [9:0]
    Value* bits32 = CreateBitCast(value, getConditionallyVectorizedTy(getInt32Ty(), value->getType()));

    // sign16 = (bits32 >> 16) & 0x8000
    Value* sign16 = CreateAnd(CreateLShr(bits32, ConstantInt::get(bits32->getType(), 16)),
                               ConstantInt::get(bits32->getType(), 0x8000));

    // exp32 = (bits32 >> 23) & 0xFF
    Value* exp32 = CreateAnd(CreateLShr(bits32, ConstantInt::get(bits32->getType(), 23)),
                              ConstantInt::get(bits32->getType(), 0xFF));

    // exp16 = exp32 - 127 + 15
    Value* exp16 = CreateSub(exp32, ConstantInt::get(exp32->getType(), (127 - 15)));

    // mant32 = bits32 & 0x7FFFFF
    Value* mant32 = CreateAnd(bits32, 0x7FFFFF);

    Value* isNanInf = CreateICmpEQ(exp32, ConstantInt::get(exp32->getType(), 0xFF));
    Value* isNan = CreateAnd(isNanInf, CreateICmpNE(mant32, Constant::getNullValue(mant32->getType())));

    // inf16 = sign16 | 0x7C00
    Value* inf16 = CreateOr(sign16, ConstantInt::get(sign16->getType(), 0x7C00));

    // nan16 = sign16 | 0x7C00 | (mant32 >> 13) | 1
    Value* nan16 = CreateOr(CreateOr(inf16, CreateLShr(mant32, ConstantInt::get(mant32->getType(), 13))),
                             ConstantInt::get(mant32->getType(), 1));

    Value* isTooSmall = CreateICmpSLT(exp16, ConstantInt::get(exp16->getType(), -10));
    Value* isDenorm = CreateICmpSLE(exp16, Constant::getNullValue(exp16->getType()));

    // Calculate how many bits to discard from end of mantissa. Normally 13, but (14 - exp16) if denorm.
    // Also explicitly set implicit top set bit in mantissa if it is denorm.
    Value* numBitsToDiscard = CreateSelect(isDenorm,
                                            CreateSub(ConstantInt::get(exp16->getType(), 14), exp16),
                                            ConstantInt::get(exp16->getType(), 13));
    mant32 = CreateSelect(isDenorm, CreateOr(mant32, ConstantInt::get(mant32->getType(), 0x800000)), mant32);

    // Ensure tiebreak-to-even by adding lowest nondiscarded bit to input mantissa.
    Constant* one = ConstantInt::get(mant32->getType(), 1);
    mant32 = CreateAdd(mant32, CreateAnd(CreateLShr(mant32, numBitsToDiscard), one));

    // Calculate amount to add to do rounding: ((1 << numBitsToDiscard) - 1) >> 1)
    Value* rounder = CreateLShr(CreateSub(CreateShl(one, numBitsToDiscard), one), one);

    // Add rounder amount and discard bits.
    Value* mant16 = CreateLShr(CreateAdd(mant32, rounder), numBitsToDiscard);

    // Combine exponent. Do this with an add, so that, if the rounding overflowed, the exponent automatically
    // gets incremented.
    exp16 = CreateSelect(isDenorm, Constant::getNullValue(exp16->getType()), exp16);
    Value* combined16 = CreateAdd(mant16, CreateShl(exp16, ConstantInt::get(mant16->getType(), 10)));

    // Zero if underflow.
    combined16 = CreateSelect(isTooSmall, Constant::getNullValue(combined16->getType()), combined16);

    // Check if the exponent is now too big.
    isNanInf = CreateOr(isNanInf, CreateICmpUGE(combined16, ConstantInt::get(combined16->getType(), 0x7C00)));

    // Combine in the sign. This gives the final result for zero, normals and denormals.
    combined16 = CreateOr(combined16, sign16);

    // Select in inf or nan as appropriate.
    combined16 = CreateSelect(isNanInf, inf16, combined16);
    combined16 = CreateSelect(isNan, nan16, combined16);

    // Return as (vector of) half.
    return CreateBitCast(CreateTrunc(combined16, getConditionallyVectorizedTy(getInt16Ty(), destTy)), destTy, instName);
}

// =====================================================================================================================
// Create quantize operation: truncates float (or vector) value to a value that is representable by a half.
Value* BuilderImplArith::CreateQuantizeToFp16(
    Value*        value,     // [in] Input value (float or float vector)
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    assert(value->getType()->getScalarType()->isFloatTy());

    Constant* zero = Constant::getNullValue(value->getType());
    // 2^-15 (normalized float16 minimum)
    Constant* minNormalizedHalf = ConstantFP::get(value->getType(), 1.0 / 32768.0);

    Value* trunc = CreateFPTrunc(value, getConditionallyVectorizedTy(getHalfTy(), value->getType()));
    Value* ext = CreateFPExt(trunc, value->getType());
    Value* abs = CreateIntrinsic(Intrinsic::fabs, ext->getType(), ext);
    Value* isLessThanMin = CreateFCmpOLT(abs, minNormalizedHalf);
    Value* isNotZero = CreateFCmpONE(abs, zero);
    Value* isDenorm = CreateAnd(isLessThanMin, isNotZero);
    Value* result = CreateSelect(isDenorm, zero, ext);

    // Check NaN.
    Value* isNan = CreateIsNaN(value);
    return CreateSelect(isNan, value, result, instName);
}

// =====================================================================================================================
// Create signed integer modulo operation, where the sign of the result (if not zero) is the same as the sign
// of the divisor.
Value* BuilderImplArith::CreateSMod(
    Value*        dividend,  // [in] Dividend value
    Value*        divisor,   // [in] Divisor value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    if (divisor->getType()->getScalarType()->isIntegerTy(32) &&
        getPipelineState()->getTargetInfo().getGpuWorkarounds().gfx10.disableI32ModToI16Mod)
    {

        // NOTE: On some hardware, when the divisor is a literal value and less than 0xFFFF, i32 mod will be
        // optimized to i16 mod. There is an existing issue in the backend which makes i16 mod not work.
        // This is the workaround to this issue.
        // TODO: Check if this is still needed and what the backend problem is.
        if (auto divisorConst = dyn_cast<ConstantInt>(divisor))
        {
            if (divisorConst->getZExtValue() <= 0xFFFF)
            {
                // Get a non-constant 0 value. (We know the top 17 bits of the 64-bit PC is always zero.)
                Value* pc = CreateIntrinsic(Intrinsic::amdgcn_s_getpc, {}, {});
                Value* pcHi = CreateExtractElement(CreateBitCast(pc, VectorType::get(getInt32Ty(), 2)), 1);
                Value* nonConstantZero = CreateLShr(pcHi, getInt32(15));
                if (auto vecTy = dyn_cast<VectorType>(divisor->getType()))
                    nonConstantZero = CreateVectorSplat(vecTy->getNumElements(), nonConstantZero);
                // Add the non-constant 0 to the denominator to disable the optimization.
                divisor = CreateAdd(divisor, nonConstantZero);
            }
        }
    }

    Value* srem = CreateSRem(dividend, divisor);
    Value* divisorPlusSrem = CreateAdd(divisor, srem);
    Value* isDifferentSign = CreateICmpSLT(CreateXor(dividend, divisor),
                                            Constant::getNullValue(dividend->getType()));
    Value* remainderNotZero = CreateICmpNE(srem, Constant::getNullValue(srem->getType()));
    Value* resultNeedsAddDivisor = CreateAnd(isDifferentSign, remainderNotZero);
    return CreateSelect(resultNeedsAddDivisor, divisorPlusSrem, srem, instName);
}

// =====================================================================================================================
// Create FP modulo operation, where the sign of the result (if not zero) is the same as the sign
// of the divisor.
Value* BuilderImplArith::CreateFMod(
    Value*        dividend,  // [in] Dividend value
    Value*        divisor,   // [in] Divisor value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    Value* quotient = CreateFMul(CreateFDiv(ConstantFP::get(divisor->getType(), 1.0), divisor), dividend);
    Value* floor = CreateUnaryIntrinsic(Intrinsic::floor, quotient);
    return CreateFSub(dividend, CreateFMul(divisor, floor), instName);
}

// =====================================================================================================================
// Create scalar/vector float/half fused multiply-and-add, to compute a * b + c
Value* BuilderImplArith::CreateFma(
    Value*        a,         // [in] One value to multiply
    Value*        b,         // [in] The other value to multiply
    Value*        c,         // [in] The value to add to the product of A and B
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    if (getPipelineState()->getTargetInfo().getGfxIpVersion().major <= 8)
    {
        // Pre-GFX9 version: Use fmuladd.
        return CreateIntrinsic(Intrinsic::fmuladd, a->getType(), { a, b, c }, nullptr, instName);
    }

    // GFX9+ version: Use fma.
    return CreateIntrinsic(Intrinsic::fma, a->getType(), { a, b, c }, nullptr, instName);
}

// =====================================================================================================================
// Create a "tan" operation for a scalar or vector float or half.
Value* BuilderImplArith::CreateTan(
    Value*        x,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    Constant* one = ConstantFP::get(x->getType(), 1.0);
    Value* sin = CreateUnaryIntrinsic(Intrinsic::sin, x);
    Value* cos = CreateUnaryIntrinsic(Intrinsic::cos, x);
    return CreateFMul(sin, CreateFDiv(one, cos), instName);
}

// =====================================================================================================================
// Create an "asin" operation for a scalar or vector float or half.
Value* BuilderImplArith::CreateASin(
    Value*        x,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // Extend half to float.
    Type* origTy = x->getType();
    Type* extTy = origTy;
    if (extTy->getScalarType()->isHalfTy())
    {
        extTy = getConditionallyVectorizedTy(getFloatTy(), extTy);
        x = CreateFPExt(x, extTy);
    }

    // atan2(x, y), y = sqrt(1 - x * x)
    Value* y = CreateFMul(x, x);
    Value* one = ConstantFP::get(x->getType(), 1.0);
    y = CreateFSub(one , y);
    y = CreateUnaryIntrinsic(Intrinsic::sqrt, y);
    Value* result = CreateATan2(x, y);

    result = CreateFPTrunc(result, origTy);
    result->setName(instName);
    return result;
}

// =====================================================================================================================
// Create an "acos" operation for a scalar or vector float or half.
Value* BuilderImplArith::CreateACos(
    Value*        x,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // Extend half to float.
    Type* origTy = x->getType();
    Type* extTy = origTy;
    if (extTy->getScalarType()->isHalfTy())
    {
        extTy = getConditionallyVectorizedTy(getFloatTy(), extTy);
        x = CreateFPExt(x, extTy);
    }

    // acos coefficient p0 = 0.08132463
    auto coefP0 = getFpConstant(x->getType(), APFloat(APFloat::IEEEdouble(), APInt(64, 0x3FB4D1B0E0000000)));
    // acos coefficient p1 = -0.02363318
    auto coefP1 = getFpConstant(x->getType(), APFloat(APFloat::IEEEdouble(), APInt(64, 0xBF98334BE0000000)));

    Value* result = aSinACosCommon(x, coefP0, coefP1);

    result = CreateFSub(getPiByTwo(result->getType()), result);
    result = CreateFPTrunc(result, origTy);
    result->setName(instName);
    return result;
}

// =====================================================================================================================
// Common code for asin and acos
Value* BuilderImplArith::aSinACosCommon(
    Value*        x,         // [in] Input value X
    Constant*     coefP0,    // [in] p0 coefficient
    Constant*     coefP1)    // [in] p1 coefficient
{
    // asin(x) = sgn(x) * (PI/2 - sqrt(1 - |x|) * (PI/2 + |x| * (PI/4 - 1 + |x| * (p0 + |x| * p1))))
    // acos(x) = PI/2 - the same, but with slightly different coefficients
    Value* absInValue = CreateUnaryIntrinsic(Intrinsic::fabs, x);
    Value* result = CreateFMul(absInValue, coefP1);
    result = CreateFAdd(result, coefP0);
    result = CreateFMul(absInValue, result);
    result = CreateFAdd(result, getPiByFourMinusOne(x->getType()));
    result = CreateFMul(absInValue, result);
    result = CreateFAdd(result, getPiByTwo(x->getType()));

    Value* sqrtTerm = CreateUnaryIntrinsic(Intrinsic::sqrt,
                                            CreateFSub(ConstantFP::get(x->getType(), 1.0), absInValue));
    result = CreateFMul(sqrtTerm, result);
    result = CreateFSub(getPiByTwo(x->getType()), result);
    Value* sign = CreateFSign(x);
    return CreateFMul(sign, result);
}

// =====================================================================================================================
// Create an "atan" operation for a scalar or vector float or half.
Value* BuilderImplArith::CreateATan(
    Value*        yOverX,    // [in] Input value Y/X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // atan(x) = x - x^3 / 3 + x^5 / 5 - x^7 / 7 + x^9 / 9 - x^11 / 11, |x| <= 1.0
    // x = min(1.0, x) / max(1.0, x), make |x| <= 1.0
    Constant* zero = Constant::getNullValue(yOverX->getType());
    Constant* one = ConstantFP::get(yOverX->getType(), 1.0);

    Value* absX = CreateUnaryIntrinsic(Intrinsic::fabs, yOverX);
    Value* max = CreateBinaryIntrinsic(Intrinsic::maxnum, absX, one);
    Value* min = CreateBinaryIntrinsic(Intrinsic::minnum, absX, one);
    Value* boundedX = CreateFMul(min, CreateFDiv(one, max));
    Value* square = CreateFMul(boundedX, boundedX);
    Value* cube = CreateFMul(square, boundedX);
    Value* pow5 = CreateFMul(cube, square);
    Value* pow7 = CreateFMul(pow5, square);
    Value* pow9 = CreateFMul(pow7, square);
    Value* pow11 = CreateFMul(pow9, square);

    // coef1 = 0.99997932
    auto coef1 = getFpConstant(yOverX->getType(), APFloat(APFloat::IEEEdouble(), APInt(64, 0x3FEFFFD4A0000000)));
    // coef3 = -0.33267564
    auto coef3 = getFpConstant(yOverX->getType(), APFloat(APFloat::IEEEdouble(), APInt(64, 0xBFD54A8EC0000000)));
    // coef5 = 0.19389249
    auto coef5 = getFpConstant(yOverX->getType(), APFloat(APFloat::IEEEdouble(), APInt(64, 0x3FC8D17820000000)));
    // coef7 = -0.11735032
    auto coef7 = getFpConstant(yOverX->getType(), APFloat(APFloat::IEEEdouble(), APInt(64, 0xBFBE0AABA0000000)));
    // coef9 = 0.05368138
    auto coef9 = getFpConstant(yOverX->getType(), APFloat(APFloat::IEEEdouble(), APInt(64, 0x3FAB7C2020000000)));
    // coef11 = -0.01213232
    auto coef11 = getFpConstant(yOverX->getType(), APFloat(APFloat::IEEEdouble(), APInt(64, 0xBF88D8D4A0000000)));

    Value* term1 = CreateFMul(boundedX, coef1);
    Value* term3 = CreateFMul(cube, coef3);
    Value* term5 = CreateFMul(pow5, coef5);
    Value* term7 = CreateFMul(pow7, coef7);
    Value* term9 = CreateFMul(pow9, coef9);
    Value* term11 = CreateFMul(pow11, coef11);

    Value* result = CreateFAdd(term1, term3);
    result = CreateFAdd(result, term5);
    result = CreateFAdd(result, term7);
    result = CreateFAdd(result, term9);
    Value* partialResult = CreateFAdd(result, term11);
    result = CreateFMul(partialResult, ConstantFP::get(yOverX->getType(), -2.0));
    result = CreateFAdd(result, getPiByTwo(yOverX->getType()));
    Value* outsideBound = CreateSelect(CreateFCmpOGT(absX, one), one, zero);
    result = CreateFMul(outsideBound, result);
    result = CreateFAdd(partialResult, result);
    return CreateFMul(result, CreateFSign(yOverX));
}

// =====================================================================================================================
// Create an "atan2" operation for a scalar or vector float or half.
// Returns atan(Y/X) but in the correct quadrant for the input value signs.
Value* BuilderImplArith::CreateATan2(
    Value*        y,         // [in] Input value Y
    Value*        x,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // yox = (|x| == |y|) ? ((x == y) ? 1.0 : -1.0) : y/x
    //
    // p0 = sgn(y) * PI/2
    // p1 = sgn(y) * PI
    // atanyox = atan(yox)
    //
    // if (y != 0.0)
    //     if (x != 0.0)
    //         atan(y, x) = (x < 0.0) ? p1 + atanyox : atanyox
    //     else
    //         atan(y, x) = p0
    // else
    //     atan(y, x) = (x > 0.0) ? 0 : PI

    Constant* zero = Constant::getNullValue(y->getType());
    Constant* one = ConstantFP::get(y->getType(), 1.0);
    Constant* negOne = ConstantFP::get(y->getType(), -1.0);

    Value* absX = CreateUnaryIntrinsic(Intrinsic::fabs, x);
    Value* absY = CreateUnaryIntrinsic(Intrinsic::fabs, y);
    Value* signY = CreateFSign(y);
    Value* p0 = CreateFMul(signY, getPiByTwo(signY->getType()));
    Value* p1 = CreateFMul(signY, getPi(signY->getType()));

    Value* absXEqualsAbsY = CreateFCmpOEQ(absX, absY);
    // pOneIfEqual to (x == y) ? 1.0 : -1.0
    Value* oneIfEqual = CreateSelect(CreateFCmpOEQ(x, y), one, negOne);

    Value* yOverX = fDivFast(y, x);

    yOverX = CreateSelect(absXEqualsAbsY, oneIfEqual, yOverX);
    Value* result = CreateATan(yOverX);
    Value* addP1 = CreateFAdd(result, p1);
    result = CreateSelect(CreateFCmpOLT(x, zero), addP1, result);
    result = CreateSelect(CreateFCmpONE(x, zero), result, p0);
    Value* zeroOrPi = CreateSelect(CreateFCmpOGT(x, zero), zero, getPi(x->getType()));
    result = CreateSelect(CreateFCmpONE(y, zero), result, zeroOrPi, instName);
    return result;
}

// =====================================================================================================================
// Create a "sinh" operation for a scalar or vector float or half.
Value* BuilderImplArith::CreateSinh(
    Value*        x,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // (e^x - e^(-x)) / 2.0
    // e^x = 2^(x * 1.442695)
    // 1/log(2) = 1.442695
    // e^x = 2^(x*(1/log(2))) = 2^(x*1.442695))
    Constant* zero = Constant::getNullValue(x->getType());
    Constant* half = ConstantFP::get(x->getType(), 0.5);
    Value* divLog2 = CreateFMul(x, getRecipLog2(x->getType()));
    Value* negDivLog2 = CreateFSub(zero, divLog2);
    Value* exp = CreateUnaryIntrinsic(Intrinsic::exp2, divLog2);
    Value* expNeg = CreateUnaryIntrinsic(Intrinsic::exp2, negDivLog2);
    Value* result = CreateFSub(exp, expNeg);
    return CreateFMul(result, half, instName);
}

// =====================================================================================================================
// Create a "cosh" operation for a scalar or vector float or half.
Value* BuilderImplArith::CreateCosh(
    Value*        x,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // (e^x + e^(-x)) / 2.0
    // e^x = 2^(x * 1.442695)
    // 1/log(2) = 1.442695
    // e^x = 2^(x*(1/log(2))) = 2^(x*1.442695))
    Value* divLog2 = CreateFMul(x, getRecipLog2(x->getType()));
    Value* negDivLog2 = CreateFSub(ConstantFP::get(x->getType(), 0.0), divLog2);
    Value* exp = CreateUnaryIntrinsic(Intrinsic::exp2, divLog2);
    Value* expNeg = CreateUnaryIntrinsic(Intrinsic::exp2, negDivLog2);
    Value* result = CreateFAdd(exp, expNeg);
    return CreateFMul(result, ConstantFP::get(x->getType(), 0.5), instName);
}

// =====================================================================================================================
// Create a "tanh" operation for a scalar or vector float or half.
Value* BuilderImplArith::CreateTanh(
    Value*        x,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // sinh(x) / cosh(x)
    // (e^x - e^(-x))/(e^x + e^(-x))
    // 1/log(2) = 1.442695
    // e^x = 2^(x*(1/log(2))) = 2^(x*1.442695))
    Value* divLog2 = CreateFMul(x, getRecipLog2(x->getType()));
    Value* negDivLog2 = CreateFSub(ConstantFP::get(x->getType(), 0.0), divLog2);
    Value* exp = CreateUnaryIntrinsic(Intrinsic::exp2, divLog2);
    Value* expNeg = CreateUnaryIntrinsic(Intrinsic::exp2, negDivLog2);
    Value* doubleSinh = CreateFSub(exp, expNeg);
    Value* doubleCosh = CreateFAdd(exp, expNeg);
    Value* result = fDivFast(doubleSinh, doubleCosh);
    result->setName(instName);
    return result;
}

// =====================================================================================================================
// Create an "asinh" operation for a scalar or vector float or half.
Value* BuilderImplArith::CreateASinh(
    Value*        x,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // ln(x + sqrt(x*x + 1))
    //             / ln(x + sqrt(x^2 + 1))      when x >= 0
    //  asinh(x) =
    //             \ -ln((sqrt(x^2 + 1)- x))    when x < 0
    Constant* one = ConstantFP::get(x->getType(), 1.0);
    Constant* negOne = ConstantFP::get(x->getType(), -1.0);

    Value* square = CreateFMul(x, x);
    Value* sqrt = CreateUnaryIntrinsic(Intrinsic::sqrt, CreateFAdd(square, one));
    Value* isNonNegative = CreateFCmpOGE(x, Constant::getNullValue(x->getType()));
    Value* sign = CreateSelect(isNonNegative, one, negOne);
    Value* abs = CreateFMul(x, sign);
    Value* result = CreateFAdd(sqrt, abs);
    result = CreateUnaryIntrinsic(Intrinsic::log2, result);
    result = CreateFMul(result, getLog2(x->getType()));
    return CreateFMul(result, sign, instName);
}

// =====================================================================================================================
// Create an "acosh" operation for a scalar or vector float or half.
Value* BuilderImplArith::CreateACosh(
    Value*        x,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // ln(x + sqrt(x*x - 1))
    // x should >= 1, undefined < 1
    Constant* one = ConstantFP::get(x->getType(), 1.0);

    Value* square = CreateFMul(x, x);
    Value* sqrt = CreateUnaryIntrinsic(Intrinsic::sqrt, CreateFSub(square, one));
    Value* result = CreateFAdd(x, sqrt);
    result = CreateUnaryIntrinsic(Intrinsic::log2, result);
    return CreateFMul(result, getLog2(x->getType()));
}

// =====================================================================================================================
// Create an "atanh" operation for a scalar or vector float or half.
Value* BuilderImplArith::CreateATanh(
    Value*        x,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // ln((x + 1)/( 1 - x)) * 0.5f;
    // |x| <1, undefined |x| >= 1
    Constant* one = ConstantFP::get(x->getType(), 1.0);
    Value* onePlusX = CreateFAdd(x, one);
    Value* oneMinusX = CreateFSub(one, x);
    Value* result = CreateFMul(onePlusX, CreateFDiv(one, oneMinusX));
    result = CreateUnaryIntrinsic(Intrinsic::log2, result);
    return CreateFMul(result, getHalfLog2(x->getType()), instName);
}

// =====================================================================================================================
// Create a "power" operation for a scalar or vector float or half, calculating X ^ Y
Value* BuilderImplArith::CreatePower(
    Value*        x,         // [in] Input value X
    Value*        y,         // [in] Input value Y
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    if (x == ConstantFP::get(x->getType(), 2.0))
        return CreateUnaryIntrinsic(Intrinsic::exp2, y, nullptr, instName);

    // llvm.pow only works with (vector of) float.
    if (x->getType()->getScalarType()->isFloatTy())
        return CreateBinaryIntrinsic(Intrinsic::pow, x, y, nullptr, instName);

    // pow(x, y) = exp2(y * log2(x))
    Value *log = CreateUnaryIntrinsic(Intrinsic::log2, x);
    return CreateUnaryIntrinsic(Intrinsic::exp2, CreateFMul(y, log), nullptr, instName);
}

// =====================================================================================================================
// Create an "exp" operation for a scalar or vector float or half.
Value* BuilderImplArith::CreateExp(
    Value*        x,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return CreateUnaryIntrinsic(Intrinsic::exp2,
                                CreateFMul(x, getRecipLog2(x->getType())),
                                nullptr,
                                instName);
}

// =====================================================================================================================
// Create a "log" operation for a scalar or vector float or half.
Value* BuilderImplArith::CreateLog(
    Value*        x,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    Value* log = CreateUnaryIntrinsic(Intrinsic::log2, x);
    return CreateFMul(log, getLog2(x->getType()), instName);
}

// =====================================================================================================================
// Create an inverse square root operation for a scalar or vector FP value.
Value* BuilderImplArith::CreateInverseSqrt(
    Value*        x,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return CreateFDiv(ConstantFP::get(x->getType(), 1.0),
                      CreateUnaryIntrinsic(Intrinsic::sqrt, x),
                      instName);
}

// =====================================================================================================================
// Create "signed integer abs" operation for a scalar or vector integer value.
Value* BuilderImplArith::CreateSAbs(
    Value*        x,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    Value* negX = CreateNeg(x);
    Value* isPositive = CreateICmpSGT(x, negX);
    return CreateSelect(isPositive, x, negX, instName);
}

// =====================================================================================================================
// Create "fsign" operation for a scalar or vector floating-point type, returning -1.0, 0.0 or +1.0 if the input
// value is negative, zero or positive.
Value* BuilderImplArith::CreateFSign(
    Value*        x,         // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    Value* isPositive = CreateFCmpOGT(x, Constant::getNullValue(x->getType()));
    Value* partialResult = CreateSelect(isPositive, ConstantFP::get(x->getType(), 1.0), x);
    Value* isNonNegative = CreateFCmpOGE(partialResult, Constant::getNullValue(x->getType()));
    return CreateSelect(isNonNegative, partialResult, ConstantFP::get(x->getType(), -1.0), instName);
}

// =====================================================================================================================
// Create "ssign" operation for a scalar or vector integer type, returning -1, 0 or +1 if the input
// value is negative, zero or positive.
Value* BuilderImplArith::CreateSSign(
    Value*        x,         // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    Value* isPositive = CreateICmpSGT(x, Constant::getNullValue(x->getType()));
    Value* partialResult = CreateSelect(isPositive, ConstantInt::get(x->getType(), 1, true), x);
    Value* isNonNegative = CreateICmpSGE(partialResult, Constant::getNullValue(x->getType()));
    return CreateSelect(isNonNegative, partialResult, ConstantInt::get(x->getType(), -1, true), instName);
}

// =====================================================================================================================
// Create "fract" operation for a scalar or vector floating-point type, returning x - floor(x).
Value* BuilderImplArith::CreateFract(
    Value*        x,         // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // We need to scalarize this ourselves.
    Value* result = scalarize(x,
                               [this](Value* x)
                               {
                                  return CreateIntrinsic(Intrinsic::amdgcn_fract, x->getType(), x);
                               });
    result->setName(instName);
    return result;
}

// =====================================================================================================================
// Create "smoothStep" operation. Result is 0.0 if x <= edge0 and 1.0 if x >= edge1 and performs smooth Hermite
// interpolation between 0 and 1 when edge0 < x < edge1. This is equivalent to:
// t * t * (3 - 2 * t), where t = clamp ((x - edge0) / (edge1 - edge0), 0, 1)
// Result is undefined if edge0 >= edge1.
Value* BuilderImplArith::CreateSmoothStep(
    Value*        edge0,     // [in] Edge0 value
    Value*        edge1,     // [in] Edge1 value
    Value*        x,         // [in] X (input) value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    if (edge0->getType()->getScalarType()->isHalfTy())
    {
        // Enabling fast math flags for half type here causes test problems.
        // TODO: Investigate this further.
        clearFastMathFlags();
    }
    Value* diff = CreateFSub(x, edge0);
    Constant* one = ConstantFP::get(x->getType(), 1.0);
    Value* t = CreateFMul(diff, CreateFDiv(one, CreateFSub(edge1, edge0)));
    t = CreateFClamp(t, Constant::getNullValue(t->getType()), one);
    Value* tSquared = CreateFMul(t, t);
    Value* term = CreateFAdd(ConstantFP::get(t->getType(), 3.0),
                              CreateFMul(ConstantFP::get(t->getType(), -2.0), t));
    return CreateFMul(tSquared, term, instName);
}

// =====================================================================================================================
// Create "ldexp" operation: given an FP mantissa and int exponent, build an FP value
Value* BuilderImplArith::CreateLdexp(
    Value*        x,         // [in] Mantissa
    Value*        exp,       // [in] Exponent
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // Ensure exponent is i32.
    if (exp->getType()->getScalarType()->isIntegerTy(16))
        exp = CreateSExt(exp, getConditionallyVectorizedTy(getInt32Ty(), exp->getType()));
    else if (exp->getType()->getScalarType()->isIntegerTy(64))
        exp = CreateTrunc(exp, getConditionallyVectorizedTy(getInt32Ty(), exp->getType()));

    // We need to scalarize this ourselves.
    Value* result = scalarize(x,
                               exp,
                               [this](Value* x, Value* exp)
                               {
                                  return CreateIntrinsic(Intrinsic::amdgcn_ldexp, x->getType(), { x, exp });
                               });
    result->setName(instName);
    return result;
}

// =====================================================================================================================
// Create "extract significand" operation: given an FP scalar or vector value, return the significand in the range
// [0.5,1.0), of the same type as the input. If the input is 0, the result is 0. If the input is infinite or NaN,
// the result is undefined.
Value* BuilderImplArith::CreateExtractSignificand(
    Value*        value,     // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // We need to scalarize this ourselves.
    Value* mant = scalarize(value,
                             [this](Value* value)
                             {
                                return CreateIntrinsic(Intrinsic::amdgcn_frexp_mant, value->getType(), value);
                             });
    mant->setName(instName);
    return mant;
}

// =====================================================================================================================
// Create "extract exponent" operation: given an FP scalar or vector value, return the exponent as a signed integer.
// If the input is (vector of) half, the result type is (vector of) i16, otherwise it is (vector of) i32.
// If the input is 0, the result is 0. If the input is infinite or NaN, the result is undefined.
Value* BuilderImplArith::CreateExtractExponent(
    Value*        value,     // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // We need to scalarize this ourselves.
    Type* expTy = value->getType()->getScalarType()->isHalfTy() ? getInt16Ty() : getInt32Ty();
    Value* exp = scalarize(value,
                            [this, expTy](Value* value)
                            {
                                return CreateIntrinsic(Intrinsic::amdgcn_frexp_exp,
                                                       { expTy, value->getType() },
                                                       value);
                            });
    exp->setName(instName);
    return exp;
}

// =====================================================================================================================
// Create vector cross product operation. Inputs must be <3 x FP>
Value* BuilderImplArith::CreateCrossProduct(
    Value*        x,         // [in] Input value X
    Value*        y,         // [in] Input value Y
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    assert(x->getType() == y->getType() && x->getType()->getVectorNumElements() == 3);

    Value* left = UndefValue::get(x->getType());
    Value* right = UndefValue::get(x->getType());
    for (unsigned idx = 0; idx != 3; ++idx)
    {
        left = CreateInsertElement(left,
                                    CreateFMul(CreateExtractElement(x, (idx + 1) % 3),
                                               CreateExtractElement(y, (idx + 2) % 3)),
                                    idx);
        right = CreateInsertElement(right,
                                     CreateFMul(CreateExtractElement(x, (idx + 2) % 3),
                                                CreateExtractElement(y, (idx + 1) % 3)),
                                     idx);
    }
    return CreateFSub(left, right, instName);
}

// =====================================================================================================================
// Create FP scalar/vector normalize operation: returns a scalar/vector with the same direction and magnitude 1.
Value* BuilderImplArith::CreateNormalizeVector(
    Value*        x,         // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    if (!isa<VectorType>(x->getType()))
    {
        // For a scalar, just return -1.0 or +1.0.
        Value* isPositive = CreateFCmpOGT(x, Constant::getNullValue(x->getType()));
        return CreateSelect(isPositive,
                            ConstantFP::get(x->getType(), 1.0),
                            ConstantFP::get(x->getType(), -1.0),
                            instName);
    }

    // For a vector, divide by the length.
    Value* dot = CreateDotProduct(x, x);
    Value* sqrt = CreateIntrinsic(Intrinsic::sqrt, dot->getType(), dot);
    Value* rsq = CreateFDiv(ConstantFP::get(sqrt->getType(), 1.0), sqrt);
    // We use fmul.legacy for float so that a zero vector is normalized to a zero vector,
    // rather than NaNs. We must scalarize it ourselves.
    Value* result = scalarize(x,
                               [this, rsq](Value* x) -> Value*
                               {
                                  if (rsq->getType()->isFloatTy())
                                      return CreateIntrinsic(Intrinsic::amdgcn_fmul_legacy, {}, { x, rsq });
                                  return CreateFMul(x, rsq);
                               });
    result->setName(instName);
    return result;
}

// =====================================================================================================================
// Create "face forward" operation: given three FP scalars/vectors {N, I, Nref}, if the dot product of
// Nref and I is negative, the result is N, otherwise it is -N
Value* BuilderImplArith::CreateFaceForward(
    Value*        n,         // [in] Input value "N"
    Value*        i,         // [in] Input value "I"
    Value*        nref,      // [in] Input value "Nref"
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    Value* dot = CreateDotProduct(i, nref);
    Value* isDotNegative = CreateFCmpOLT(dot, Constant::getNullValue(dot->getType()));
    Value* negN = CreateFSub(Constant::getNullValue(n->getType()), n);
    return CreateSelect(isDotNegative, n, negN, instName);
}

// =====================================================================================================================
// Create "reflect" operation. For the incident vector I and normalized surface orientation N, the result is
// the reflection direction:
// I - 2 * dot(N, I) * N
Value* BuilderImplArith::CreateReflect(
    Value*        i,         // [in] Input value "I"
    Value*        n,         // [in] Input value "N"
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    Value* dot = CreateDotProduct(n, i);
    dot = CreateFMul(dot, ConstantFP::get(dot->getType(), 2.0));
    if (auto vecTy = dyn_cast<VectorType>(n->getType()))
        dot = CreateVectorSplat(vecTy->getNumElements(), dot);
    return CreateFSub(i, CreateFMul(dot, n), instName);
}

// =====================================================================================================================
// Create "refract" operation. For the normalized incident vector I, normalized surface orientation N and ratio
// of indices of refraction eta, the result is the refraction vector:
// k = 1.0 - eta * eta * (1.0 - dot(N,I) * dot(N,I))
// If k < 0.0 the result is 0.0.
// Otherwise, the result is eta * I - (eta * dot(N,I) + sqrt(k)) * N
Value* BuilderImplArith::CreateRefract(
    Value*        i,         // [in] Input value "I"
    Value*        n,         // [in] Input value "N"
    Value*        eta,       // [in] Input value "eta"
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    Constant* one = ConstantFP::get(eta->getType(), 1.0);
    Value* dot = CreateDotProduct(i, n);
    Value* dotSqr = CreateFMul(dot, dot);
    Value* e1 = CreateFSub(one, dotSqr);
    Value* e2 = CreateFMul(eta, eta);
    Value* e3 = CreateFMul(e1, e2);
    Value* k = CreateFSub(one, e3);
    Value* kSqrt = CreateUnaryIntrinsic(Intrinsic::sqrt, k);
    Value* etaDot = CreateFMul(eta, dot);
    Value* innt = CreateFAdd(etaDot, kSqrt);

    if (auto vecTy = dyn_cast<VectorType>(n->getType()))
    {
        eta = CreateVectorSplat(vecTy->getNumElements(), eta);
        innt = CreateVectorSplat(vecTy->getNumElements(), innt);
    }
    i = CreateFMul(i, eta);
    n = CreateFMul(n, innt);
    Value* s = CreateFSub(i, n);
    Value* con = CreateFCmpOLT(k, Constant::getNullValue(k->getType()));
    return CreateSelect(con, Constant::getNullValue(s->getType()), s);
}

// =====================================================================================================================
// Create "fclamp" operation, returning min(max(x, minVal), maxVal). Result is undefined if minVal > maxVal.
// This honors the fast math flags; clear "nnan" in fast math flags in order to obtain the "NaN avoiding
// semantics" for the min and max where, if one input is NaN, it returns the other one.
// It also honors the shader's FP mode being "flush denorm".
Value* BuilderImplArith::CreateFClamp(
    Value*        x,         // [in] Value to clamp
    Value*        minVal,    // [in] Minimum of clamp range
    Value*        maxVal,    // [in] Maximum of clamp range
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // For float, and for half on GFX9+, we can use the fmed3 instruction.
    // But we can only do this if we do not need NaN preservation.
    Value* result = nullptr;
    if (getFastMathFlags().noNaNs() && (x->getType()->getScalarType()->isFloatTy() ||
        (getPipelineState()->getTargetInfo().getGfxIpVersion().major >= 9 && x->getType()->getScalarType()->isHalfTy())))
    {
        result = scalarize(x,
                            minVal,
                            maxVal,
                            [this](Value* x, Value* minVal, Value* maxVal)
                            {
                               return CreateIntrinsic(Intrinsic::amdgcn_fmed3,
                                                      x->getType(),
                                                      { x, minVal, maxVal });
                            });
        result->setName(instName);
    }
    else
    {
        // For half on GFX8 or earlier, or for double, use a combination of fmin and fmax.
        CallInst* max = CreateMaxNum(x, minVal);
        max->setFastMathFlags(getFastMathFlags());
        CallInst* min = CreateMinNum(max, maxVal, instName);
        min->setFastMathFlags(getFastMathFlags());
        result = min;
    }

    // Before GFX9, fmed/fmin/fmax do not honor the hardware FP mode wanting flush denorms. So we need to
    // canonicalize the result here.
    if (getPipelineState()->getTargetInfo().getGfxIpVersion().major < 9)
        result = canonicalize(result);

    result->setName(instName);
    return result;
}

// =====================================================================================================================
// Create "fmin" operation, returning the minimum of two scalar or vector FP values.
// This honors the fast math flags; do not set "nnan" if you want the "return the non-NaN input" behavior.
// It also honors the shader's FP mode being "flush denorm".
Value* BuilderImplArith::CreateFMin(
    Value*        value1,    // [in] First value
    Value*        value2,    // [in] Second value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    CallInst* min = CreateMinNum(value1, value2);
    min->setFastMathFlags(getFastMathFlags());
    Value* result = min;

    // Before GFX9, fmed/fmin/fmax do not honor the hardware FP mode wanting flush denorms. So we need to
    // canonicalize the result here.
    if (getPipelineState()->getTargetInfo().getGfxIpVersion().major < 9)
        result = canonicalize(result);

    result->setName(instName);
    return result;
}

// =====================================================================================================================
// Create "fmax" operation, returning the maximum of two scalar or vector FP values.
// This honors the fast math flags; do not set "nnan" if you want the "return the non-NaN input" behavior.
// It also honors the shader's FP mode being "flush denorm".
Value* BuilderImplArith::CreateFMax(
    Value*        value1,    // [in] First value
    Value*        value2,    // [in] Second value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    CallInst* max = CreateMaxNum(value1, value2);
    max->setFastMathFlags(getFastMathFlags());
    Value* result = max;

    // Before GFX9, fmed/fmin/fmax do not honor the hardware FP mode wanting flush denorms. So we need to
    // canonicalize the result here.
    if (getPipelineState()->getTargetInfo().getGfxIpVersion().major < 9)
        result = canonicalize(result);

    result->setName(instName);
    return result;
}

// =====================================================================================================================
// Create "fmin3" operation, returning the minimum of three scalar or vector float or half values.
// This honors the fast math flags; do not set "nnan" if you want the "return the non-NaN input" behavior.
// It also honors the shader's FP mode being "flush denorm".
Value* BuilderImplArith::CreateFMin3(
    Value*        value1,    // [in] First value
    Value*        value2,    // [in] Second value
    Value*        value3,    // [in] Third value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    CallInst* min1 = CreateMinNum(value1, value2);
    min1->setFastMathFlags(getFastMathFlags());
    CallInst* min2 = CreateMinNum(min1, value3);
    min2->setFastMathFlags(getFastMathFlags());
    Value* result = min2;

    // Before GFX9, fmed/fmin/fmax do not honor the hardware FP mode wanting flush denorms. So we need to
    // canonicalize the result here.
    if (getPipelineState()->getTargetInfo().getGfxIpVersion().major < 9)
        result = canonicalize(result);

    result->setName(instName);
    return result;
}

// =====================================================================================================================
// Create "fmax3" operation, returning the maximum of three scalar or vector float or half values.
// This honors the fast math flags; do not set "nnan" if you want the "return the non-NaN input" behavior.
// It also honors the shader's FP mode being "flush denorm".
Value* BuilderImplArith::CreateFMax3(
    Value*        value1,    // [in] First value
    Value*        value2,    // [in] Second value
    Value*        value3,    // [in] Third value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    CallInst* max1 = CreateMaxNum(value1, value2);
    max1->setFastMathFlags(getFastMathFlags());
    CallInst* max2 = CreateMaxNum(max1, value3);
    max2->setFastMathFlags(getFastMathFlags());
    Value* result = max2;

    // Before GFX9, fmed/fmin/fmax do not honor the hardware FP mode wanting flush denorms. So we need to
    // canonicalize the result here.
    if (getPipelineState()->getTargetInfo().getGfxIpVersion().major < 9)
        result = canonicalize(result);

    result->setName(instName);
    return result;
}

// =====================================================================================================================
// Create "fmid3" operation, returning the middle one of three scalar or vector float or half values.
// This honors the fast math flags; do not set "nnan" if you want the "return the non-NaN input" behavior.
// It also honors the shader's FP mode being "flush denorm".
Value* BuilderImplArith::CreateFMid3(
    Value*        value1,    // [in] First value
    Value*        value2,    // [in] Second value
    Value*        value3,    // [in] Third value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // For float, and for half on GFX9+, we can use the fmed3 instruction.
    // But we can only do this if we do not need NaN preservation.
    Value* result = nullptr;
    if (getFastMathFlags().noNaNs() && (value1->getType()->getScalarType()->isFloatTy() ||
        (getPipelineState()->getTargetInfo().getGfxIpVersion().major >= 9 && value1->getType()->getScalarType()->isHalfTy())))
    {
        result = scalarize(value1,
                            value2,
                            value3,
                            [this](Value* value1, Value* value2, Value* value3)
                            {
                               return CreateIntrinsic(Intrinsic::amdgcn_fmed3,
                                                      value1->getType(),
                                                      { value1, value2, value3 });
                            });
    }
    else
    {
        // For half on GFX8 or earlier, use a combination of fmin and fmax.
        CallInst* min1 = CreateMinNum(value1, value2);
        min1->setFastMathFlags(getFastMathFlags());
        CallInst* max1 = CreateMaxNum(value1, value2);
        max1->setFastMathFlags(getFastMathFlags());
        CallInst* min2 = CreateMinNum(max1, value3);
        min2->setFastMathFlags(getFastMathFlags());
        CallInst* max2 = CreateMaxNum(min1, min2, instName);
        max2->setFastMathFlags(getFastMathFlags());
        result = max2;
    }

    // Before GFX9, fmed/fmin/fmax do not honor the hardware FP mode wanting flush denorms. So we need to
    // canonicalize the result here.
    if (getPipelineState()->getTargetInfo().getGfxIpVersion().major < 9)
        result = canonicalize(result);

    result->setName(instName);
    return result;
}

// =====================================================================================================================
// Generate FP division, using fast fdiv for float to bypass optimization, and using fdiv 1.0 then fmul for
// half or double.
// TODO: IntrinsicsAMDGPU.td says amdgcn.fdiv.fast should not be used outside the backend.
Value* BuilderImplArith::fDivFast(
    Value* numerator,    // [in] Numerator
    Value* denominator)  // [in] Denominator
{
    if (!numerator->getType()->getScalarType()->isFloatTy())
        return CreateFMul(numerator, CreateFDiv(ConstantFP::get(denominator->getType(), 1.0), denominator));

    // We have to scalarize fdiv.fast ourselves.
    return scalarize(numerator,
                     denominator,
                     [this](Value* numerator, Value* denominator) -> Value*
                     {
                        return CreateIntrinsic(Intrinsic::amdgcn_fdiv_fast, {}, { numerator, denominator });
                     });
}

// =====================================================================================================================
// Create "isInfinite" operation: return true if the supplied FP (or vector) value is infinity
Value* BuilderImplArith::CreateIsInf(
    Value*        x,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    return createCallAmdgcnClass(x, CmpClass::NegativeInfinity | CmpClass::PositiveInfinity, instName);
}

// =====================================================================================================================
// Create "isNaN" operation: return true if the supplied FP (or vector) value is NaN
Value* BuilderImplArith::CreateIsNaN(
    Value*        x,         // [in] Input value X
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    // 0x001: signaling NaN, 0x002: quiet NaN
    return createCallAmdgcnClass(x, CmpClass::SignalingNaN | CmpClass::QuietNaN, instName);
}

// =====================================================================================================================
// Helper method to create call to llvm.amdgcn.class, scalarizing if necessary. This is not exposed outside of
// BuilderImplArith.
Value* BuilderImplArith::createCallAmdgcnClass(
    Value*        value,     // [in] Input value
    unsigned      flags,      // Flags for what class(es) to check for
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    Value* result = scalarize(value,
                               [this, flags](Value* value)
                               {
                                  return CreateIntrinsic(Intrinsic::amdgcn_class,
                                                         value->getType(),
                                                         { value, getInt32(flags) });
                               });
    result->setName(instName);
    return result;
}

// =====================================================================================================================
// Create an "insert bitfield" operation for a (vector of) integer type.
// Returns a value where the "pCount" bits starting at bit "pOffset" come from the least significant "pCount"
// bits in "pInsert", and remaining bits come from "pBase". The result is undefined if "pCount"+"pOffset" is
// more than the number of bits (per vector element) in "pBase" and "pInsert".
// If "pBase" and "pInsert" are vectors, "pOffset" and "pCount" can be either scalar or vector of the same
// width. The scalar type of "pOffset" and "pCount" must be integer, but can be different to that of "pBase"
// and "pInsert" (and different to each other too).
Value* BuilderImplArith::CreateInsertBitField(
    Value*        base,                // [in] Base value
    Value*        insert,              // [in] Value to insert (same type as base)
    Value*        offset,              // Bit number of least-significant end of bitfield
    Value*        count,               // Count of bits in bitfield
    const Twine&  instName)             // [in] Name to give instruction(s)
{
    // Make pOffset and pCount vectors of the right integer type if necessary.
    if (auto vecTy = dyn_cast<VectorType>(base->getType()))
    {
        if (!isa<VectorType>(offset->getType()))
            offset = CreateVectorSplat(vecTy->getNumElements(), offset);
        if (!isa<VectorType>(count->getType()))
            count = CreateVectorSplat(vecTy->getNumElements(), count);
    }
    offset = CreateZExtOrTrunc(offset, base->getType());
    count = CreateZExtOrTrunc(count, base->getType());

    Value* baseXorInsert = CreateXor(CreateShl(insert, offset), base);
    Constant* one = ConstantInt::get(count->getType(), 1);
    Value* mask = CreateShl(CreateSub(CreateShl(one, count), one), offset);
    Value* result = CreateXor(CreateAnd(baseXorInsert, mask), base);
    Value* isWholeField = CreateICmpEQ(count,
                                        ConstantInt::get(count->getType(),
                                                         count->getType()->getScalarType()->getPrimitiveSizeInBits()));
    return CreateSelect(isWholeField, insert, result, instName);
}

// =====================================================================================================================
// Create an "extract bitfield" operation for a (vector of) i32.
// Returns a value where the least significant "pCount" bits come from the "pCount" bits starting at bit
// "pOffset" in "pBase", and that is zero- or sign-extended (depending on "isSigned") to the rest of the value.
// If "pBase" and "pInsert" are vectors, "pOffset" and "pCount" can be either scalar or vector of the same
// width. The scalar type of "pOffset" and "pCount" must be integer, but can be different to that of "pBase"
// (and different to each other too).
Value* BuilderImplArith::CreateExtractBitField(
    Value*        base,                // [in] Base value
    Value*        offset,              // Bit number of least-significant end of bitfield
    Value*        count,               // Count of bits in bitfield
    bool          isSigned,             // True for a signed int bitfield extract, false for unsigned
    const Twine&  instName)             // [in] Name to give instruction(s)
{
    // Make pOffset and pCount vectors of the right integer type if necessary.
    if (auto vecTy = dyn_cast<VectorType>(base->getType()))
    {
        if (!isa<VectorType>(offset->getType()))
            offset = CreateVectorSplat(vecTy->getNumElements(), offset);
        if (!isa<VectorType>(count->getType()))
            count = CreateVectorSplat(vecTy->getNumElements(), count);
    }
    offset = CreateZExtOrTrunc(offset, base->getType());
    count = CreateZExtOrTrunc(count, base->getType());

    // For i32, we can use the amdgcn intrinsic and hence the instruction.
    if (base->getType()->getScalarType()->isIntegerTy(32))
    {
        Value* isWholeField = CreateICmpEQ(
                                    count,
                                    ConstantInt::get(count->getType(),
                                                     count->getType()->getScalarType()->getPrimitiveSizeInBits()));
        Value* result = scalarize(base,
                                   offset,
                                   count,
                                   [this, isSigned](Value* base, Value* offset, Value* count)
                                   {
                                      return CreateIntrinsic(isSigned ? Intrinsic::amdgcn_sbfe : Intrinsic::amdgcn_ubfe,
                                                             base->getType(),
                                                             { base, offset, count });
                                   });
        result = CreateSelect(isWholeField, base, result);
        Value* isEmptyField = CreateICmpEQ(count, Constant::getNullValue(count->getType()));
        return CreateSelect(isEmptyField, Constant::getNullValue(count->getType()), result, instName);
    }

    // For other types, extract manually.
    Value* shiftDown = CreateSub(ConstantInt::get(base->getType(),
                                                   base->getType()->getScalarType()->getPrimitiveSizeInBits()),
                                  count);
    Value* shiftUp = CreateSub(shiftDown, offset);
    Value* result = CreateShl(base, shiftUp);
    if (isSigned)
        result = CreateAShr(result, shiftDown);
    else
        result = CreateLShr(result, shiftDown);
    Value* isZeroCount = CreateICmpEQ(count, Constant::getNullValue(count->getType()));
    return CreateSelect(isZeroCount, count, result, instName);
}

// =====================================================================================================================
// Create "find MSB" operation for a (vector of) signed i32. For a postive number, the result is the bit number of
// the most significant 1-bit. For a negative number, the result is the bit number of the most significant 0-bit.
// For a value of 0 or -1, the result is -1.
Value* BuilderImplArith::CreateFindSMsb(
    Value*        value,     // [in] Input value
    const Twine&  instName)   // [in] Name to give instruction(s)
{
    assert(value->getType()->getScalarType()->isIntegerTy(32));

    Constant* negOne = ConstantInt::get(value->getType(), -1);
    Value* leadingZeroCount = scalarize(value,
                                         [this](Value* value)
                                         {
                                            return CreateUnaryIntrinsic(Intrinsic::amdgcn_sffbh, value);
                                         });
    Value* bitOnePos = CreateSub(ConstantInt::get(value->getType(), 31), leadingZeroCount);
    Value* isNegOne = CreateICmpEQ(value, negOne);
    Value* isZero = CreateICmpEQ(value, Constant::getNullValue(value->getType()));
    Value* isNegOneOrZero = CreateOr(isNegOne, isZero);
    return CreateSelect(isNegOneOrZero, negOne, bitOnePos, instName);
}

// =====================================================================================================================
// Create "fmix" operation, returning ( 1 - A ) * X + A * Y. Result would be FP scalar or vector value.
// Returns scalar, if and only if "pX", "pY" and "pA" are all scalars.
// Returns vector, if "pX" and "pY" are vector but "pA" is a scalar, under such condition, "pA" will be splatted.
// Returns vector, if "pX", "pY" and "pA" are all vectors.
// Note that when doing vector calculation, it means add/sub are element-wise between vectors, and the product will
// be Hadamard product.
Value* BuilderImplArith::createFMix(
    Value*        x,        // [in] left Value
    Value*        y,        // [in] right Value
    Value*        a,        // [in] wight Value
    const Twine& instName)   // [in] Name to give instruction(s)
{
    Value* ySubX = CreateFSub(y, x);
    if (auto vectorResultTy = dyn_cast<VectorType>(ySubX->getType()))
    {
        // pX, pY => vector, but pA => scalar
        if (!isa<VectorType>(a->getType()))
            a = CreateVectorSplat(vectorResultTy->getVectorNumElements(), a);
    }

    FastMathFlags fastMathFlags = getFastMathFlags();
    fastMathFlags.setNoNaNs();
    fastMathFlags.setAllowContract();
    CallInst* result = CreateIntrinsic(Intrinsic::fmuladd, x->getType(), {ySubX, a, x}, nullptr, instName);
    result->setFastMathFlags(fastMathFlags);

    return result;
}

// =====================================================================================================================
// Ensure result is canonicalized if the shader's FP mode is flush denorms. This is called on an FP result of an
// instruction that does not honor the hardware's FP mode, such as fmin/fmax/fmed on GFX8 and earlier.
Value* BuilderImplArith::canonicalize(
    Value*  value)   // [in] Value to canonicalize
 {
    const auto& shaderMode = getShaderModes()->getCommonShaderMode(m_shaderStage);
    auto destTy = value->getType();
    FpDenormMode denormMode =
       destTy->getScalarType()->isHalfTy() ? shaderMode.fp16DenormMode :
       destTy->getScalarType()->isFloatTy() ? shaderMode.fp32DenormMode :
       destTy->getScalarType()->isDoubleTy() ? shaderMode.fp64DenormMode :
       FpDenormMode::DontCare;
    if (denormMode == FpDenormMode::FlushOut || denormMode == FpDenormMode::FlushInOut)
    {
        // Has to flush denormals, insert canonicalize to make a MUL (* 1.0) forcibly
        value = CreateUnaryIntrinsic(Intrinsic::canonicalize, value);
    }
    return value;
}

