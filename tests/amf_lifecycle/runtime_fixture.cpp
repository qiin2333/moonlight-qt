#include <windows.h>
#include "public/include/core/Factory.h"

namespace {
class Factory : public amf::AMFFactory
{
public:
    AMF_RESULT AMF_STD_CALL CreateContext(amf::AMFContext**) override
    {
        return AMF_NOT_IMPLEMENTED;
    }
    AMF_RESULT AMF_STD_CALL CreateComponent(amf::AMFContext*, const wchar_t*,
                                            amf::AMFComponent**) override
    {
        return AMF_NOT_IMPLEMENTED;
    }
    AMF_RESULT AMF_STD_CALL SetCacheFolder(const wchar_t*) override { return AMF_NOT_IMPLEMENTED; }
    const wchar_t* AMF_STD_CALL GetCacheFolder() override { return L"fixture"; }
    AMF_RESULT AMF_STD_CALL GetDebug(amf::AMFDebug**) override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL GetTrace(amf::AMFTrace**) override { return AMF_NOT_IMPLEMENTED; }
    AMF_RESULT AMF_STD_CALL GetPrograms(amf::AMFPrograms**) override { return AMF_NOT_IMPLEMENTED; }
};
Factory factory;
} // namespace

extern "C" __declspec(dllexport) AMF_RESULT AMF_CDECL_CALL AMFInit(amf_uint64,
                                                                   amf::AMFFactory** result)
{
    wchar_t mode[16] = {};
    GetEnvironmentVariableW(L"MOONLIGHT_AMF_FIXTURE_MODE", mode, 16);
    *result = mode[0] == L'n' ? nullptr : &factory;
    return mode[0] == L'f' ? AMF_FAIL : AMF_OK;
}
