/*
    ANNT - Artificial Neural Networks C++ library

    Copyright (C) 2018, cvsandbox, cvsandbox@gmail.com

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include "XConvolutionLayer.hpp"
#include "../../Tools/XDataEncodingTools.hpp"
#include "../../Tools/XParallel.hpp"

using namespace std;

namespace ANNT { namespace Neuro {

XConvolutionLayer::XConvolutionLayer( size_t inputWidth, size_t inputHeight, size_t inputDepth,
                                      size_t kernelWidth, size_t kernelHeight, size_t kernelsCount,
                                      BorderMode borderMode, size_t horizontalStep, size_t verticalStep ) :
                                      ITrainableLayer( 0, 0 ),
    mInputWidth( inputWidth ), mInputHeight( inputHeight ), mInputDepth( inputDepth ),
    mOutputWidth( 0 ), mOutputHeight( 0 ),
    mKernelWidth( kernelWidth ), mKernelHeight( kernelHeight ), mKernelsCount( kernelsCount ),
    mHorizontalStep( horizontalStep ), mVerticalStep( verticalStep ), mBorderMode( borderMode ), 
    mPaddedWidth( inputWidth ), mPaddedHeight( inputHeight )
{
    size_t padWidth = 0, padHeight = 0;

    // use input padding, if border handling mode is set to produce same size output
    // (same ouput size will be only when step size is 1; output is always smaller for larger steps)
    if ( mBorderMode == BorderMode::Same )
    {
        padWidth  = mKernelWidth  - 1;
        padHeight = mKernelHeight - 1;

        mPaddedWidth  = mInputWidth  + padWidth;
        mPaddedHeight = mInputHeight + padHeight;
    }

    // calculation of output width/height as:
    //   outSize = ( inSize - kernelSize + padSize ) / step + 1
    mOutputWidth  = ( mInputWidth  - mKernelWidth  + padWidth  ) / mHorizontalStep + 1;
    mOutputHeight = ( mInputHeight - mKernelHeight + padHeight ) / mVerticalStep   + 1;

    // total input/output size
    Initialize( mInputWidth  * mInputHeight  * mInputDepth,
                mOutputWidth * mOutputHeight * mKernelsCount );

    mKernelsWeights = fvector_t( mKernelWidth * mKernelHeight * mInputDepth * mKernelsCount );
    mKernelsBiases  = fvector_t( mKernelsCount );

    Randomize( );
}

// Randomizes layer's weights, clears biases
void XConvolutionLayer::Randomize( )
{
    float halfRange = sqrt( 3.0f / ( mKernelWidth * mKernelHeight * mInputDepth ) );

    for ( auto& w : mKernelsWeights )
    {
        w = ( static_cast<float_t>( rand( ) ) / RAND_MAX ) * ( float_t( 2 ) * halfRange ) - halfRange;
    }
    for ( auto& b : mKernelsBiases )
    {
        b = 0;
    }
}

// Calculates outputs for the given inputs
void XConvolutionLayer::ForwardCompute( const vector<fvector_t*>& inputs,
                                        vector<fvector_t*>& outputs,
                                        const XNetworkContext& ctx )
{
    const float_t* kernelsData  = mKernelsWeights.data( );

    // kernel size in 2D and 3D spaces
    size_t  kernelSize2D = mKernelWidth * mKernelHeight;
    size_t  kernelSize3D = kernelSize2D * mInputDepth;

    // will be using either original input width/heigh or padded
    size_t  inputWidth   = mInputWidth;
    size_t  inputHeight  = mInputHeight;

    // decide if raw data to be used or padded
    if ( mBorderMode == BorderMode::Same )
    {
        inputWidth  = mPaddedWidth;
        inputHeight = mPaddedHeight;
    }

    size_t  inputRowInc     = inputWidth * mVerticalStep;
    // gap size after processing one input row with kernel to the next row to be processed
    size_t  inputNextRowGap = inputWidth - mKernelWidth;

    // process all samples
    XParallel::For( inputs.size( ), ctx.IsTraining( ), [&]( size_t i )
    {
        const float_t* inputData  = inputs[i]->data( );
        float_t*       outputData = outputs[i]->data( );

        if ( mBorderMode == BorderMode::Same )
        {
            // get working buffer for padded inputs
            float_t* paddedInput = static_cast<float_t*>( ctx.GetWorkingBuffer( 0, i ) );

            XDataEncodingTools::AddPadding2d( inputData, paddedInput,
                                              mInputWidth, mInputHeight, mPaddedWidth, mPaddedHeight,
                                              mInputDepth, float_t( 0 ) );
            inputData = paddedInput;
        }

        // clear the output
        fill( outputData, outputData + mOutputsCount, float_t( 0 ) );

        // go through all kernels to build output feature maps
        for ( size_t kernelIndex = 0; kernelIndex < mKernelsCount; kernelIndex++ )
        {
            float_t* outputBase = outputData + kernelIndex * mOutputWidth * mOutputHeight;
            float_t  biasValue  = mKernelsBiases[kernelIndex];

            // go through all input layers (or feature maps produced by previous layers)
            for ( size_t inputDepthIndex = 0; inputDepthIndex < mInputDepth; inputDepthIndex++ )
            {
                const float_t* inputBase  = inputData + inputDepthIndex * inputWidth * inputHeight;
                // get the 2D kernel for current input/output map combination
                const float_t* kernelBase = kernelsData + kernelIndex * kernelSize3D + inputDepthIndex * kernelSize2D;

                // calculate output contributions for the current input map
                for ( size_t oy = 0; oy < mOutputHeight; oy++ )
                {
                    const float_t* inputRow  = inputBase + oy * inputRowInc;
                    float_t*       outputRow = outputBase + oy * mOutputWidth;

                    for ( size_t ox = 0; ox < mOutputWidth; ox++ )
                    {
                        const float_t* kernelPtr = kernelBase;
                        const float_t* inputPtr  = inputRow;
                        float_t        sum       = float_t( 0 );

                        // "convolve" input with the kernel
                        // (we actually do cross-correlation since it does not matter for CNN)
                        for ( size_t ky = 0; ky < mKernelHeight; ky++ )
                        {
                            for ( size_t kx = 0; kx < mKernelWidth; kx++ )
                            {
                                sum += *inputPtr * *kernelPtr;
                                kernelPtr++;
                                inputPtr++;
                            }
                            // since input pointer was already shifted horizontally,
                            // we need to align it back to the start of the next row
                            inputPtr += inputNextRowGap;
                        }

                        *outputRow += sum;
                        *outputRow += biasValue;

                        // shift output/input row pointers to the next position of the sliding kernel's window
                        outputRow++;
                        inputRow += mHorizontalStep;
                    }
                }
            }
        }
    } );
}

// Propagates error to the previous layer and calculates weights/biases gradients
void XConvolutionLayer::BackwardCompute( const vector<fvector_t*>& inputs,
                                         const vector<fvector_t*>& /* outputs */,
                                         const vector<fvector_t*>& deltas,
                                         vector<fvector_t*>& prevDeltas,
                                         fvector_t& gradWeights,
                                         fvector_t& gradBiases,
                                         const XNetworkContext& ctx )
{
    const float_t* kernelsData = mKernelsWeights.data( );
    float_t*       gradWeightsData = gradWeights.data( );

    size_t       outputSize      = mOutputWidth * mOutputHeight;

    // kernel size if 2D and 3D spaces
    size_t       kernelSize2D  = mKernelWidth * mKernelHeight;
    size_t       kernelSize3D  = kernelSize2D * mInputDepth;

    // will be using either original input width/heigh or padded
    size_t       inputWidth    = mInputWidth;
    size_t       inputHeight   = mInputHeight;

    // decide if raw data to be used or padded
    if ( mBorderMode == BorderMode::Same )
    {
        inputWidth  = mPaddedWidth;
        inputHeight = mPaddedHeight;
    }

    size_t inputRowInc         = inputWidth * mVerticalStep;
    // gap size after processing one row of previous deltas to the next row to be processed
    size_t prevDeltaNextRowGap = inputWidth - mKernelWidth;
    
    // 1 - first propagate deltas to the previous layer
    XParallel::For( inputs.size( ), ctx.IsTraining( ), [&]( size_t i )
    {
        const float_t* deltaData     = deltas[i]->data( );
        float_t*       prevDeltaData = prevDeltas[i]->data( );

        if ( mBorderMode == BorderMode::Same )
        {
            // get working buffer for padded previous deltas
            prevDeltaData = static_cast<float_t*>( ctx.GetWorkingBuffer( 1, i ) );
        }

        fill( prevDeltaData, prevDeltaData + mInputsCount, float_t( 0 ) );

        // go through all input feature maps (which are the outputs of the previous layer)
        for ( size_t inputDepthIndex = 0; inputDepthIndex < mInputDepth; inputDepthIndex++ )
        {
            float_t* prevDeltaBase = prevDeltaData + inputDepthIndex * inputWidth * inputHeight;

            // go through all kernels, which were applied to the feature map
            for ( size_t kernelIndex = 0; kernelIndex < mKernelsCount; kernelIndex++ )
            {
                const float_t* deltaBase  = deltaData + kernelIndex * outputSize;
                // get the 2D kernel for then current input/output map combination
                const float_t* kernelBase = kernelsData + kernelIndex * kernelSize3D + inputDepthIndex * kernelSize2D;

                // go through the current deltas of the output produced by the current kernel
                for ( size_t oy = 0; oy < mOutputHeight; oy++ )
                {
                    const float_t* deltaPtr     = deltaBase     + oy * mOutputWidth;
                    float_t*       prevDeltaRow = prevDeltaBase + oy * inputRowInc;

                    for ( size_t ox = 0; ox < mOutputWidth; ox++ )
                    {
                        const float_t* kernelPtr    = kernelBase;
                        float_t*       prevDeltaPtr = prevDeltaRow;

                        // go through the kernel at current image position
                        for ( size_t ky = 0; ky < mKernelHeight; ky++ )
                        {
                            for ( size_t kx = 0; kx < mKernelWidth; kx++ )
                            {
                                *prevDeltaPtr += *deltaPtr * *kernelPtr;

                                kernelPtr++;
                                prevDeltaPtr++;
                            }
                            // since previous delta pointer was already shifted horizontally,
                            // we need to align it back to the start of the next row
                            prevDeltaPtr += prevDeltaNextRowGap;
                        }

                        // shift current/previous delta pointers to the next position of the sliding kernel's window
                        deltaPtr++;
                        prevDeltaRow += mHorizontalStep;
                    }
                }
            }
        }

        if ( mBorderMode == BorderMode::Same )
        {
            // do unpadding of previous deltas
            XDataEncodingTools::RemovePadding2d( prevDeltaData, prevDeltas[i]->data( ), mPaddedWidth, mPaddedHeight, mInputWidth, mInputHeight, mInputDepth );
        }
    } );

    // 2 - accumulate weights' difference

    // go through all input feature maps
    XParallel::For( mInputDepth, ctx.IsTraining( ), [&]( size_t inputDepthIndex )
    {
        for ( size_t i = 0, n = inputs.size( ); i < n; i++ )
        {
            const float_t* deltaData = deltas[i]->data( );
            const float_t* inputData = inputs[i]->data( );

            if ( mBorderMode == BorderMode::Same )
            {
                // get working buffer for padded inputs
                inputData = static_cast<float_t*>( ctx.GetWorkingBuffer( 0, i ) );
            }

            const float_t* inputBase = inputData + inputDepthIndex * inputWidth * inputHeight;

            // go through all kernels, which were applied to the feature map
            for ( size_t kernelIndex = 0; kernelIndex < mKernelsCount; kernelIndex++ )
            {
                const float_t* deltaBase = deltaData + kernelIndex * outputSize;
                // get the 2D portion of weights' gradients for the current input/output map combination
                float_t* gradWeightsPtr  = gradWeightsData + kernelIndex * kernelSize3D + inputDepthIndex * kernelSize2D;

                // calculate gradients for each weight (kernel element)
                for ( size_t ky = 0; ky < mKernelHeight; ky++ )
                {
                    for ( size_t kx = 0; kx < mKernelWidth; kx++ )
                    {
                        float_t sum = float_t( 0 );

                        // multiply output deltas by corresponding inputs
                        for ( size_t oy = 0; oy < mOutputHeight; oy++ )
                        {
                            const float_t* deltaPtr = deltaBase + oy * mOutputWidth;
                            const float_t* inputPtr = inputBase + oy * inputRowInc + ky * inputWidth + kx;

                            for ( size_t ox = 0; ox < mOutputWidth; ox++ )
                            {
                                sum += *deltaPtr * *inputPtr;

                                deltaPtr++;
                                inputPtr += mHorizontalStep;
                            }
                        }

                        *gradWeightsPtr += sum;
                        gradWeightsPtr++;
                    }
                }
            }
        }
    } );

    // 3 - accumulate baises' difference
    XParallel::For( mKernelsCount, ctx.IsTraining( ), [&]( size_t kernelIndex )
    {
        float_t sum = 0;

        for ( size_t i = 0, n = inputs.size( ); i < n; i++ )
        {
            const float_t* deltaPtr = deltas[i]->data( ) + kernelIndex * outputSize;

            for ( size_t outputIndex = 0; outputIndex < outputSize; outputIndex++ )
            {
                sum += *deltaPtr;
                deltaPtr++;
            }
        }

        gradBiases[kernelIndex] += sum;
    } );
}

// Applies updates to the layer's weights and biases
void XConvolutionLayer::UpdateWeights( const fvector_t& weightsUpdate,
                                       const fvector_t& biasesUpdate )
{
    for ( size_t i = 0, n = mKernelsWeights.size( ); i < n; i++ )
    {
        mKernelsWeights[i] += weightsUpdate[i];
    }
    for ( size_t i = 0, n = mKernelsBiases.size( ); i < n; i++ )
    {
        mKernelsBiases[i] += biasesUpdate[i];
    }
}

} } // namespace ANNT::Neuro
