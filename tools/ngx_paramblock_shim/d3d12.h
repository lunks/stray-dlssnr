// Host shim for tools/ngx_paramblock_selftest.cpp. NOT used by the add-on build.
// The parameter block only ever handles ID3D12Resource* as an opaque pointer.
#pragma once
struct ID3D12Resource;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
