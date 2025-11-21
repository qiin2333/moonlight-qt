#pragma once

class RemoteStreamConfig {
public:
    bool remoteResolution;
    int remoteResolutionWidth;
    int remoteResolutionHeight;
    bool remoteFps;
    int remoteFpsRate;
    int originalStreamWidth;
    int originalStreamHeight;

    RemoteStreamConfig(
        bool remoteResolution, int remoteResolutionWidth, int remoteResolutionHeight,
        bool remoteFps, int remoteFpsRate,
        int originalStreamWidth, int originalStreamHeight)
    {
        this->remoteResolution = remoteResolution;
        this->remoteResolutionWidth = remoteResolutionWidth;
        this->remoteResolutionHeight = remoteResolutionHeight;
        this->remoteFps = remoteFps;
        this->remoteFpsRate = remoteFpsRate;
        this->originalStreamWidth = originalStreamWidth;
        this->originalStreamHeight = originalStreamHeight;
    }
};
