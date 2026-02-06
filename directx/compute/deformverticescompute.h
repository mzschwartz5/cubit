#pragma once

#include "directx/compute/computeshader.h"
#include <maya/MFnMesh.h>
#include <maya/MDagPath.h>
#include <maya/MFloatPointArray.h>
#include <maya/MFloatVectorArray.h>

struct DeformVerticesConstantBuffer {
    float gridRotationInverse[4][4];
    int vertexCount;
    int padding[3]; // Padding to align to 16 bytes
};

class DeformVerticesCompute : public ComputeShader
{
public:
    DeformVerticesCompute() = default;
    DeformVerticesCompute(
        int numParticles,
        int vertexCount,
        const MMatrix& gridRotationInverse,
        const std::vector<Particle>& originalParticles,             // Will be uploaded to GPU
        const std::vector<uint>& vertexVoxelIds,                    // Will be uploaded to GPU
        const ComPtr<ID3D11UnorderedAccessView>& positionsUAV,
        const ComPtr<ID3D11UnorderedAccessView>& normalsUAV,
        const ComPtr<ID3D11ShaderResourceView>& originalVertPositionsSRV,
        const ComPtr<ID3D11ShaderResourceView>& originalNormalsSRV,
        const ComPtr<ID3D11ShaderResourceView>& particlesSRV,
        const std::vector<uint>& exportVertexIdMap
    ) : ComputeShader(IDR_SHADER1), positionsUAV(positionsUAV), normalsUAV(normalsUAV), 
                                    originalVertPositionsSRV(originalVertPositionsSRV), originalNormalsSRV(originalNormalsSRV), 
                                    particlesSRV(particlesSRV), exportVertexIdMap(exportVertexIdMap)
    {
        initializeBuffers(numParticles, vertexCount, gridRotationInverse, originalParticles, vertexVoxelIds);
    }

    void reset() override {
        DirectX::notifyMayaOfMemoryUsage(originalParticlesBuffer);
        DirectX::notifyMayaOfMemoryUsage(vertexVoxelIdsBuffer);
    }

    void dispatch() override
    {
        ComputeShader::dispatch(numWorkgroups);
    };

    void setParticlesSRV(const ComPtr<ID3D11ShaderResourceView>& srv)
    {
        particlesSRV = srv;
    };

    /**
     * Copies the deformed vertex positions and normals to the given mesh. 
     * The mesh must have the same number of verts/normals as used to create this compute shader.
     * This is primarily intended for exporting the deformed geometry to Alembic, which doesn't support custom shapes.
     */
    void copyGeometryDataToMesh(const MDagPath& meshDagPath) {
        MFnMesh meshFn(meshDagPath);
        MStatus status;

        const ComPtr<ID3D11Buffer>& positionsBuffer = DirectX::getBufferFromView(positionsUAV);
        const ComPtr<ID3D11Buffer>& normalsBuffer = DirectX::getBufferFromView(normalsUAV);

        // Need a vector for positions (see note below about logical vs. extracted vertices)
        std::vector<float> positionData;
        DirectX::copyBufferToVector(positionsBuffer, positionData);
        DirectX::copyBufferToPointer(normalsBuffer, (void*)meshFn.getRawNormals(&status));

        // The extracted vertex data has redundant vertices, split by normals and UVs, etc. The MFnMesh only wants logical vertices.
        // So we need to use the exportVertexIdMap to map from one scheme to the other. The positions of the redundant vertices are identical.
        float* rawPositions = const_cast<float*>(meshFn.getRawPoints(&status));

        for (size_t i = 0; i < exportVertexIdMap.size(); i++) {
            uint mayaVertexId = exportVertexIdMap[i]; // Map extracted vertex ID to logical vertex ID
            // This will overwrite positions multiple times, but they should all be identical for the same logical vertex.
            rawPositions[mayaVertexId * 3] = positionData[i * 3];
            rawPositions[mayaVertexId * 3 + 1] = positionData[i * 3 + 1];
            rawPositions[mayaVertexId * 3 + 2] = positionData[i * 3 + 2];
        }

        meshFn.updateSurface();
    }

private:
    int numWorkgroups = 0;
    std::vector<uint> exportVertexIdMap;

    // Inputs
    ComPtr<ID3D11UnorderedAccessView> positionsUAV;
    ComPtr<ID3D11UnorderedAccessView> normalsUAV;
    ComPtr<ID3D11ShaderResourceView> originalVertPositionsSRV;
    ComPtr<ID3D11ShaderResourceView> originalNormalsSRV;
    ComPtr<ID3D11ShaderResourceView> particlesSRV;

    // Created and owned by this class
    ComPtr<ID3D11Buffer> originalParticlesBuffer;
    ComPtr<ID3D11ShaderResourceView> originalParticlesSRV;

    ComPtr<ID3D11Buffer> vertexVoxelIdsBuffer;
    ComPtr<ID3D11ShaderResourceView> vertexVoxelIdsSRV;

    ComPtr<ID3D11Buffer> constantsBuffer;
    
    void bind() override
    {
        // In case Maya left either the position or normal vertex buffer bound to the IA stage, unbind them here.
        ID3D11Buffer* nullVBs[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
        UINT zeroStrides[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
        UINT zeroOffsets[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
        DirectX::getContext()->IASetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, nullVBs, zeroStrides, zeroOffsets);

        ID3D11ShaderResourceView* srvs[] = { originalVertPositionsSRV.Get(), originalNormalsSRV.Get(), originalParticlesSRV.Get(), particlesSRV.Get(), vertexVoxelIdsSRV.Get() };
        DirectX::getContext()->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

        ID3D11UnorderedAccessView* uavs[] = { positionsUAV.Get(), normalsUAV.Get() };
        DirectX::getContext()->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

        ID3D11Buffer* constBuffers[] = { constantsBuffer.Get() };
        DirectX::getContext()->CSSetConstantBuffers(0, ARRAYSIZE(constBuffers), constBuffers);
    };

    void unbind() override
    {
        ID3D11ShaderResourceView* nullSRVs[] = { nullptr, nullptr, nullptr, nullptr, nullptr };
        DirectX::getContext()->CSSetShaderResources(0, ARRAYSIZE(nullSRVs), nullSRVs);

        ID3D11UnorderedAccessView* nullUAVs[] = { nullptr, nullptr };
        DirectX::getContext()->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUAVs), nullUAVs, nullptr);

        ID3D11Buffer* nullConstBuffers[] = { nullptr };
        DirectX::getContext()->CSSetConstantBuffers(0, ARRAYSIZE(nullConstBuffers), nullConstBuffers);
    };

    void initializeBuffers(int numParticles, int vertexCount, const MMatrix& gridRotationInverse, const std::vector<Particle>& originalParticles, const std::vector<uint>& vertexVoxelIds)
    {
        numWorkgroups = Utils::divideRoundUp(vertexCount, DEFORM_VERTICES_THREADS);

        // We only need one reference particle per voxel, not the whole shebang
        std::vector<Particle> reducedOriginalParticles;
        reducedOriginalParticles.reserve(numParticles / 8);
        for (int i = 0; i < numParticles; i += 8) {
            reducedOriginalParticles.push_back(originalParticles[i]);
        }

        originalParticlesBuffer = DirectX::createReadOnlyBuffer<Particle>(reducedOriginalParticles);
        originalParticlesSRV = DirectX::createSRV(originalParticlesBuffer);

        vertexVoxelIdsBuffer = DirectX::createReadOnlyBuffer<uint>(vertexVoxelIds);
        vertexVoxelIdsSRV = DirectX::createSRV(vertexVoxelIdsBuffer);
       
        DeformVerticesConstantBuffer constants = {};
        gridRotationInverse.get(constants.gridRotationInverse);
        constants.vertexCount = vertexCount;
        constantsBuffer = DirectX::createConstantBuffer<DeformVerticesConstantBuffer>(constants);
    }
};