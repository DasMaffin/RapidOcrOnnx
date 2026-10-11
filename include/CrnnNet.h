#ifndef __OCR_CRNNNET_H__
#define __OCR_CRNNNET_H__

#include "OcrStruct.h"
#include <onnxruntime/core/session/onnxruntime_cxx_api.h>
#include <opencv2/opencv.hpp>

class CrnnNet {
public:

    ~CrnnNet();

    void setNumThread(int numOfThread);

    void setGpuIndex(int gpuIndex);

    void initModel(const std::string &pathStr, const std::string &keysPath);

    // MCCP: load the model and its keys (one character per line, UTF-8) from memory.
    void initModel(const void *modelData, size_t modelLen, const char *keysData, size_t keysLen);

    // MCCP: public, to recognise one text line on its own, without detection. Was private.
    TextLine getTextLine(const cv::Mat &src);

    std::vector<TextLine> getTextLines(std::vector<cv::Mat> &partImg, const char *path, const char *imgName);

private:
    bool isOutputDebugImg = false;
    Ort::Session *session = nullptr;
    Ort::Env env = Ort::Env(ORT_LOGGING_LEVEL_ERROR, "CrnnNet");
    Ort::SessionOptions sessionOptions = Ort::SessionOptions();
    int numThread = 0;

    std::vector<Ort::AllocatedStringPtr> inputNamesPtr;
    std::vector<Ort::AllocatedStringPtr> outputNamesPtr;

    const float meanValues[3] = {127.5, 127.5, 127.5};
    const float normValues[3] = {1.0 / 127.5, 1.0 / 127.5, 1.0 / 127.5};
    const int dstHeight = 48;

    std::vector<std::string> keys;

    TextLine scoreToTextLine(const std::vector<float> &outputData, size_t h, size_t w);
};


#endif //__OCR_CRNNNET_H__
