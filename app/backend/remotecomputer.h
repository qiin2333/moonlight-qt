#pragma once

class RemoteStreamConfig {
public:
    bool remoteResolution;
    int remoteResolutionWidth;
    int remoteResolutionHeight;
    bool remoteFps;
    int remoteFpsRate;

    RemoteStreamConfig(
        bool remoteResolution, int remoteResolutionWidth, int remoteResolutionHeight,
        bool remoteFps, int remoteFpsRate)
    {
        this->remoteResolution = remoteResolution;
        this->remoteResolutionWidth = remoteResolutionWidth;
        this->remoteResolutionHeight = remoteResolutionHeight;
        this->remoteFps = remoteFps;
        this->remoteFpsRate = remoteFpsRate;
    }
};
