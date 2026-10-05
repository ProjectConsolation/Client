#pragma once

#include "image_file.hpp"
#include <wincodec.h>
#include <wrl/client.h>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace image_overrides
{
	inline std::vector<unsigned char> decode_png(const std::span<const unsigned char> file)
	{
		constexpr unsigned char signature[]{137, 80, 78, 71, 13, 10, 26, 10};
		if (file.size() < sizeof(signature) || file.size() > maximum_file_size
			|| !std::equal(std::begin(signature), std::end(signature), file.begin()))
			throw std::runtime_error("invalid PNG signature/size");
		struct com_apartment
		{
			HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			~com_apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
		} apartment;
		if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE)
			throw std::runtime_error("could not initialize PNG decoding apartment");
		auto check = [](const HRESULT result)
		{
			if (FAILED(result)) throw std::runtime_error("Windows PNG decoder rejected image");
		};
		using Microsoft::WRL::ComPtr;
		ComPtr<IWICImagingFactory> factory;
		check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(factory.GetAddressOf())));
		ComPtr<IWICStream> stream;
		check(factory->CreateStream(stream.GetAddressOf()));
		check(stream->InitializeFromMemory(const_cast<BYTE*>(file.data()), static_cast<DWORD>(file.size())));
		ComPtr<IWICBitmapDecoder> decoder;
		check(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
			decoder.GetAddressOf()));
		GUID container{};
		check(decoder->GetContainerFormat(&container));
		if (!IsEqualGUID(container, GUID_ContainerFormatPng)) throw std::runtime_error("expected PNG container");
		ComPtr<IWICBitmapFrameDecode> frame;
		check(decoder->GetFrame(0, frame.GetAddressOf()));
		UINT width = 0, height = 0;
		check(frame->GetSize(&width, &height));
		if (!width || !height || width > 4096 || height > 4096
			|| static_cast<std::size_t>(width) * height * 4 > maximum_file_size)
			throw std::runtime_error("PNG decoded dimensions exceed limits");
		ComPtr<IWICFormatConverter> converter;
		check(factory->CreateFormatConverter(converter.GetAddressOf()));
		// Straight (not premultiplied) BGRA is D3DFMT_A8R8G8B8 in memory.
		check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
			WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
		std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
		check(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()));
		// PNG has no authored mip chain. Do not invent colour-space or normal
		// filtering: upload one level; use DDS/IWI when authored mips matter.
		return make_bgra_definition(width, height, pixels);
	}
}
