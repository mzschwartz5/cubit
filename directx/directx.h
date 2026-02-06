#pragma once
#include <maya/MGlobal.h>
#include <d3d11.h>
#include <vector>
#include <wrl/client.h>
using namespace MHWRender;
using Microsoft::WRL::ComPtr;

class DirectX
{
public:
    DirectX() = delete;
    ~DirectX() = delete;

    static MStatus initialize(HINSTANCE pluginInstance);
    
    static ID3D11Device* getDevice();
    static ID3D11DeviceContext* getContext();
    static HINSTANCE getPluginInstance();

    struct ComPtrHash {
        template <typename T>
        std::size_t operator()(const ComPtr<T>& ptr) const noexcept {
            return std::hash<T*>()(ptr.Get());
        }
    };

    template<typename T>
    static ComPtr<ID3D11Buffer> createReadOnlyBuffer(
        const std::vector<T>& data,
        bool structured = true,
        UINT bindFlags = 0,
        UINT stride = 0
    ) {
        D3D11_BUFFER_DESC bufferDesc = {};

        bufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
        bufferDesc.ByteWidth = static_cast<UINT>(sizeof(T) * data.size());
        bufferDesc.BindFlags = (bindFlags == 0) ? D3D11_BIND_SHADER_RESOURCE : bindFlags;
        bufferDesc.CPUAccessFlags = 0;
        
        if (structured) {
            bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bufferDesc.StructureByteStride = (stride > 0) ? stride : sizeof(T);
        }

        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = data.data();

        ComPtr<ID3D11Buffer> buffer;
        HRESULT hr = dxDevice->CreateBuffer(&bufferDesc, &initData, buffer.GetAddressOf());
        notifyMayaOfMemoryUsage(buffer, true);
        return buffer;   
    }

    template<typename T>
    static ComPtr<ID3D11Buffer> createReadWriteBuffer(
        const std::vector<T>& data,
        bool structured = true,
        UINT bindFlags = 0
    ) {
        D3D11_BUFFER_DESC bufferDesc = {};

        bufferDesc.Usage = D3D11_USAGE_DEFAULT;
        bufferDesc.ByteWidth = static_cast<UINT>(sizeof(T) * data.size());
        bufferDesc.BindFlags = (bindFlags == 0) ? (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS) : bindFlags;
        bufferDesc.CPUAccessFlags = 0;

        if (structured) {
            bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bufferDesc.StructureByteStride = sizeof(T);
        }

        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = data.data();

        ComPtr<ID3D11Buffer> buffer;
        HRESULT hr = dxDevice->CreateBuffer(&bufferDesc, &initData, buffer.GetAddressOf());
        notifyMayaOfMemoryUsage(buffer, true);
        return buffer;   
    }
    
    template<typename T>
    static ComPtr<ID3D11Buffer> createConstantBuffer(const T& data) {
        D3D11_BUFFER_DESC bufferDesc = {};

        bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
        bufferDesc.ByteWidth = static_cast<UINT>(sizeof(T));
        bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bufferDesc.MiscFlags = 0;

        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = &data;

        ComPtr<ID3D11Buffer> buffer;
        HRESULT hr = dxDevice->CreateBuffer(&bufferDesc, &initData, buffer.GetAddressOf());
        notifyMayaOfMemoryUsage(buffer, true);
        return buffer;   
    }

    static ComPtr<ID3D11ShaderResourceView> createSRV(
        const ComPtr<ID3D11Buffer>& buffer,
        UINT elementCount = 0,
        UINT offset = 0,
        DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN
    );

    static ComPtr<ID3D11UnorderedAccessView> createUAV(
        const ComPtr<ID3D11Buffer>& buffer,
        UINT elementCount = 0,
        UINT offset = 0,
        DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN
    );

    /**
     * Prepend data to a buffer.
     */
    template<typename T>
    static void addToBuffer(ComPtr<ID3D11Buffer>& buffer, std::vector<T>& addedData) {
        // Default to a read/write buffer if buffer doesn't exist yet. (Reasonable default since adding to a buffer implies it's writeable)
        // Also assumes structured buffer format.
        if (!buffer) {
            buffer = createReadWriteBuffer<T>(addedData);
            return;
        }

        uint numNewElements = static_cast<uint>(addedData.size());
        uint numExistingElements = getNumElementsInBuffer(buffer);

        addedData.resize(numExistingElements + numNewElements);
        ComPtr<ID3D11Buffer> newBuffer = createBufferFromBufferTemplate<T>(buffer, addedData);
        addedData.resize(numNewElements); // resize back to original size (so .size() is accurate, and we don't waste memory)

        copyBufferSubregion<T>(
            buffer,
            newBuffer,
            0,
            numNewElements,
            numExistingElements
        );

        notifyMayaOfMemoryUsage(buffer, false);
        notifyMayaOfMemoryUsage(newBuffer, true);
        buffer = newBuffer;
    }

    template<typename T>
    static void deleteFromBuffer(ComPtr<ID3D11Buffer>& buffer, uint numRemovedElements, uint offset) {
        // Create a new buffer sized for the data minus the deleted elements
        uint numExistingElements = getNumElementsInBuffer(buffer);
        if (numRemovedElements >= numExistingElements) {
            notifyMayaOfMemoryUsage(buffer, false);
            buffer.Reset();
            return;
        }

        std::vector<T> newData(numExistingElements - numRemovedElements);
        ComPtr<ID3D11Buffer> newBuffer = createBufferFromBufferTemplate<T>(buffer, newData);

        // Combine the old data into the new buffer in (up to) two copies: 
        // the elements before those being removed, and those after.
        if (offset > 0) {
            copyBufferSubregion<T>(
                buffer,
                newBuffer,
                0,             // src copy offset
                0,             // dst copy offset
                offset         // num elements to copy
            );
        }

        if (static_cast<uint>(offset) + numRemovedElements < numExistingElements) {
            copyBufferSubregion<T>(
                buffer,
                newBuffer,
                offset + numRemovedElements,                        // src copy offset
                offset,                                             // dst copy offset
                numExistingElements - (offset + numRemovedElements) // num elements to copy
            );
        }

        notifyMayaOfMemoryUsage(buffer, false);
        buffer = newBuffer;
    }

    /**
     * Generic method to update a constant buffer with new data.
     */
    template<typename T>
    static void updateConstantBuffer(const ComPtr<ID3D11Buffer>& buffer, const T& data) {
        D3D11_MAPPED_SUBRESOURCE mappedResource;
        HRESULT hr = DirectX::getContext()->Map(buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
        if (SUCCEEDED(hr)) {
            memcpy(mappedResource.pData, &data, sizeof(T));
            DirectX::getContext()->Unmap(buffer.Get(), 0);
        } else {
            MGlobal::displayError("Failed to map constant buffer.");
        }
    }

    /**
     * Uses the existingBuffer to create a new buffer with the same flags, but with the provided data.
     * In other words, the existingBuffer is a template for the new buffer.
     */
    template<typename T>
    static ComPtr<ID3D11Buffer> createBufferFromBufferTemplate(
        const ComPtr<ID3D11Buffer>& existingBuffer,
        const std::vector<T>& data
    ) {
        D3D11_BUFFER_DESC desc;
        existingBuffer->GetDesc(&desc);
        bool isStructuredBuffer = (desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) != 0;

        desc.ByteWidth = static_cast<UINT>(sizeof(T) * data.size());
        if (!isStructuredBuffer) desc.StructureByteStride = sizeof(T);

        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = data.data();

        ComPtr<ID3D11Buffer> buffer;
        HRESULT hr = dxDevice->CreateBuffer(&desc, &initData, buffer.GetAddressOf());
        notifyMayaOfMemoryUsage(buffer, true);
        return buffer;
    }

    /**
     * Copy back a GPU buffer to a host vector. 
     * Note: this is for buffers without CPU access flags; we first copy to a staging buffer.
     */
    template<typename T>
    static void copyBufferToVector(
        const ComPtr<ID3D11Buffer>& buffer,
        std::vector<T>& outData
    ) {
        D3D11_BUFFER_DESC desc;
        buffer->GetDesc(&desc);
        size_t elementCount = desc.ByteWidth / sizeof(T);
        outData.resize(elementCount);

        copyBufferToPointer(buffer, outData.data());
    }

    static void copyBufferToPointer(
        const ComPtr<ID3D11Buffer>& buffer,
        void* outData
    );

    /*
    * Clears a UINT buffer with the value 0.
    */
    static void clearUintBuffer(const ComPtr<ID3D11UnorderedAccessView>& uav) {
        // See docs: 4 values are required even though only the first will be used, in our case.
        UINT clearValues[4] = { 0, 0, 0, 0 };
        DirectX::getContext()->ClearUnorderedAccessViewUint(uav.Get(), clearValues);
    }

    // Assumes 1D resources and takes the smaller of the two buffer sizes as the copy size.
    static void copyBufferToBuffer(
        const ComPtr<ID3D11View>& srcView,
        const ComPtr<ID3D11View>& dstView
    );

    static ComPtr<ID3D11Buffer> getBufferFromView(
        const ComPtr<ID3D11View>& view
    );

    static void notifyMayaOfMemoryUsage(const ComPtr<ID3D11Buffer>& buffer, bool acquire = false);
    static int getNumElementsInBuffer(const ComPtr<ID3D11Buffer>& buffer);
    
private:
    static HINSTANCE pluginInstance;
    static ID3D11Device* dxDevice;
    static ID3D11DeviceContext* dxContext;

    template<typename T>
    static void copyBufferSubregion(ComPtr<ID3D11Buffer>& srcBuffer, ComPtr<ID3D11Buffer>& dstBuffer, uint srcOffset, uint dstOffset, uint numElements) {
        D3D11_BOX srcBox = {};
        srcBox.left = srcOffset * sizeof(T);
        srcBox.right = (srcOffset + numElements) * sizeof(T);
        srcBox.top = 0;
        srcBox.bottom = 1;
        srcBox.front = 0;
        srcBox.back = 1;

        DirectX::getContext()->CopySubresourceRegion(
            dstBuffer.Get(),
            0, 
            sizeof(T) * dstOffset, 
            0, 0, 
            srcBuffer.Get(),
            0, 
            &srcBox
        );
    }
};