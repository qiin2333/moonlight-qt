#pragma once

class RemoteStreamConfig {
public:
    int remoteResolutionScale;
    bool remoteResolution;
    int remoteResolutionWidth;
    int remoteResolutionHeight;
    bool remoteFps;
    int remoteFpsRate;

    RemoteStreamConfig(
        int remoteResolutionScale,
        bool remoteResolution, int remoteResolutionWidth, int remoteResolutionHeight,
        bool remoteFps, int remoteFpsRate)
    {
        this->remoteResolutionScale = remoteResolutionScale;
        this->remoteResolution = remoteResolution;
        this->remoteResolutionWidth = remoteResolutionWidth;
        this->remoteResolutionHeight = remoteResolutionHeight;
        this->remoteFps = remoteFps;
        this->remoteFpsRate = remoteFpsRate;
    }
};
