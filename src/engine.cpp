#include <iostream>
#include <fstream>

#include "engine.h"
#include "NvOnnxParser.h"

void Normalize(cv::Mat *im,
               const std::vector<float> &mean = {0.5f, 0.5f, 0.5f},
               const std::vector<float> &scale = {1 / 0.5f, 1 / 0.5f, 1 / 0.5f},
               const bool is_scale = true)
{
    double e = 1.0;
    if (is_scale)
    {
        e /= 255.0;
    }
    (*im).convertTo(*im, CV_32FC3, e);
    std::vector<cv::Mat> bgr_channels(3);
    cv::split(*im, bgr_channels);
    for (auto i = 0; i < bgr_channels.size(); i++)
    {
        bgr_channels[i].convertTo(bgr_channels[i], CV_32FC1, 1.0 * scale[i],
                                  (0.0 - mean[i]) * scale[i]);
    }
    cv::merge(bgr_channels, *im);
}

void Logger::log(Severity severity, const char *msg) noexcept
{
    // Would advise using a proper logging utility such as https://github.com/gabime/spdlog
    // For the sake of this tutorial, will just log to the console.

    // Only log Warnings or more important.
    if (severity <= Severity::kWARNING)
    {
        std::cout << msg << std::endl;
    }
}

bool Engine::doesFileExist(const std::string &filepath)
{
    std::ifstream f(filepath.c_str());
    return f.good();
}

Engine::Engine(const Options &options)
    : m_options(options) {}

bool Engine::build(std::string onnxModelPath)
{
    // Only regenerate the engine file if it has not already been generated for the specified options
    m_engineName = serializeEngineOptions(m_options);
    std::cout << "Searching for engine file with name: " << m_engineName << std::endl;

    if (doesFileExist(m_engineName))
    {
        std::cout << "Engine found, not regenerating..." << std::endl;
        return true;
    }

    // Was not able to find the engine file, generate...
    std::cout << "Engine not found, generating..." << std::endl;

    // Create our engine builder.
    auto builder = std::unique_ptr<nvinfer1::IBuilder>(nvinfer1::createInferBuilder(m_logger));
    if (!builder)
    {
        return false;
    }

    // Set the max supported batch size
    builder->setMaxBatchSize(m_options.maxBatchSize);

    // Define an explicit batch size and then create the network.
    // More info here: https://docs.nvidia.com/deeplearning/tensorrt/developer-guide/index.html#explicit-implicit-batch
    auto explicitBatch = 1U << static_cast<uint32_t>(NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
    auto network = std::unique_ptr<nvinfer1::INetworkDefinition>(builder->createNetworkV2(explicitBatch));
    if (!network)
    {
        return false;
    }

    // Create a parser for reading the onnx file.
    auto parser = std::unique_ptr<nvonnxparser::IParser>(nvonnxparser::createParser(*network, m_logger));
    if (!parser)
    {
        return false;
    }

    // We are going to first read the onnx file into memory, then pass that buffer to the parser.
    // Had our onnx model file been encrypted, this approach would allow us to first decrypt the buffer.

    std::ifstream file(onnxModelPath, std::ios::binary | std::ios::ate);
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<char> buffer(size);
    if (!file.read(buffer.data(), size))
    {
        throw std::runtime_error("Unable to read engine file");
    }

    auto parsed = parser->parse(buffer.data(), buffer.size());
    if (!parsed)
    {
        return false;
    }

    // Save the input height, width, and channels.
    // Require this info for inference.
    const auto input = network->getInput(0);
    const auto inputName = input->getName();

    auto config = std::unique_ptr<nvinfer1::IBuilderConfig>(builder->createBuilderConfig());
    if (!config)
    {
        return false;
    }

    // Specify the optimization profiles and the
    IOptimizationProfile *defaultProfile = builder->createOptimizationProfile();
    defaultProfile->setDimensions(inputName, OptProfileSelector::kMIN, Dims4(1, m_options.inputDimension[0], m_options.inputDimension[1], m_options.inputDimension[2]));
    defaultProfile->setDimensions(inputName, OptProfileSelector::kOPT, Dims4(1, m_options.inputDimension[0], m_options.inputDimension[1], m_options.inputDimension[2]));
    defaultProfile->setDimensions(inputName, OptProfileSelector::kMAX, Dims4(1, m_options.inputDimension[0], m_options.inputDimension[1], m_options.inputDimension[2]));

    config->addOptimizationProfile(defaultProfile);

    config->setMaxWorkspaceSize(m_options.maxWorkspaceSize);

    // Disable: cublas for PPOCRv3. You can comment this line if you are using PPOCRv2.
    // Reference: https://github.com/NVIDIA/TensorRT/issues/866
    config->setTacticSources(1U);

    if (m_options.FP16)
    {
        config->setFlag(BuilderFlag::kFP16);
    }

    // CUDA stream used for profiling by the builder.
    auto profileStream = samplesCommon::makeCudaStream();
    if (!profileStream)
    {
        return false;
    }
    config->setProfileStream(*profileStream);

    // Build the engine
    std::unique_ptr<IHostMemory> plan{builder->buildSerializedNetwork(*network, *config)};
    if (!plan)
    {
        return false;
    }

    // Write the engine to disk
    std::ofstream outfile(m_engineName, std::ofstream::binary);
    outfile.write(reinterpret_cast<const char *>(plan->data()), plan->size());

    std::cout << "Success, saved engine to " << m_engineName << std::endl;

    return true;
}

Engine::~Engine()
{
    if (m_cudaStream)
    {
        cudaStreamDestroy(m_cudaStream);
    }
}

bool Engine::loadNetwork()
{
    // Read the serialized model from disk
    std::ifstream file(m_engineName, std::ios::binary | std::ios::ate);
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<char> buffer(size);
    if (!file.read(buffer.data(), size))
    {
        throw std::runtime_error("Unable to read engine file");
    }

    std::unique_ptr<IRuntime> runtime{createInferRuntime(m_logger)};
    if (!runtime)
    {
        return false;
    }

    // Set the device index
    auto ret = cudaSetDevice(m_options.deviceIndex);
    if (ret != 0)
    {
        int numGPUs;
        cudaGetDeviceCount(&numGPUs);
        auto errMsg = "Unable to set GPU device index to: " + std::to_string(m_options.deviceIndex) +
                      ". Note, your device has " + std::to_string(numGPUs) + " CUDA-capable GPU(s).";
        throw std::runtime_error(errMsg);
    }

    m_engine = std::unique_ptr<nvinfer1::ICudaEngine>(runtime->deserializeCudaEngine(buffer.data(), buffer.size()));
    if (!m_engine)
    {
        return false;
    }

    m_context = std::unique_ptr<nvinfer1::IExecutionContext>(m_engine->createExecutionContext());
    if (!m_context)
    {
        return false;
    }

    auto cudaRet = cudaStreamCreate(&m_cudaStream);
    if (cudaRet != 0)
    {
        throw std::runtime_error("Unable to create cuda stream");
    }

    return true;
}

int Engine::runInference(const std::vector<cv::Mat> &inputFaceChips, std::vector<std::vector<float>> &featureVectors)
{
    // If uninitialized, set the input and output dimenison of buffer.
    if (if_initialize == 0)
    {
        // Set the input dimension
        m_context->setOptimizationProfile(0);
        auto dims = m_context->getBindingDimensions(0);
        dims.d[0] = inputFaceChips.size();
        dims.d[1] = m_options.inputDimension[0];
        dims.d[2] = m_options.inputDimension[1];
        dims.d[3] = m_options.inputDimension[2];
        Dims4 inputDims{dims.d[0], dims.d[1], dims.d[2], dims.d[3]};

        // Get the output dimension
        m_context->setBindingDimensions(0, dims);
        outputDims = m_context->getBindingDimensions(1);

        if (!m_context->allInputDimensionsSpecified())
        {
            throw std::runtime_error("Error, not all input dimensions specified.");
        }
        
        m_inputBuff.hostBuffer.resize(inputDims);
        m_inputBuff.deviceBuffer.resize(inputDims);

        m_outputBuff.hostBuffer.resize(outputDims);
        m_outputBuff.deviceBuffer.resize(outputDims);

        if_initialize = 1;
    }

    auto *hostDataBuffer = static_cast<float *>(m_inputBuff.hostBuffer.data());

    for (size_t batch = 0; batch < inputFaceChips.size(); ++batch)
    {
        auto image = inputFaceChips[batch];

        // NHWC to NCHW conversion
        // NHWC: For each pixel, its 3 colors are stored together in RGB order.
        // For a 3 channel image, say RGB, pixels of the R channel are stored first, then the G channel and finally the B channel.
        // https://user-images.githubusercontent.com/20233731/85104458-3928a100-b23b-11ea-9e7e-95da726fef92.png
        int offset = 3 * inputFaceChips[0].rows * inputFaceChips[0].cols * batch;
        int r = 0, g = 0, b = 0;
        for (int i = 0; i < 3 * inputFaceChips[0].rows * inputFaceChips[0].cols; ++i)
        {
            if (i % 3 == 0)
            {
                hostDataBuffer[offset + r++] = *(reinterpret_cast<float *>(image.data) + i);
            }
            else if (i % 3 == 1)
            {
                hostDataBuffer[offset + g++ + inputFaceChips[0].rows * inputFaceChips[0].cols] = *(reinterpret_cast<float *>(image.data) + i);
            }
            else
            {
                hostDataBuffer[offset + b++ + inputFaceChips[0].rows * inputFaceChips[0].cols * 2] = *(reinterpret_cast<float *>(image.data) + i);
            }
        }
    }

    // Copy from CPU to GPU
    auto ret = cudaMemcpyAsync(m_inputBuff.deviceBuffer.data(), m_inputBuff.hostBuffer.data(), m_inputBuff.hostBuffer.nbBytes(), cudaMemcpyHostToDevice, m_cudaStream);
    if (ret != 0)
    {
        return false;
    }

    std::vector<void *> predicitonBindings = {m_inputBuff.deviceBuffer.data(), m_outputBuff.deviceBuffer.data()};

    // Run inference
    bool status = m_context->enqueueV2(predicitonBindings.data(), m_cudaStream, nullptr);
    if (!status)
    {
        return false;
    }

    // Copy the results back to CPU memory
    ret = cudaMemcpyAsync(m_outputBuff.hostBuffer.data(), m_outputBuff.deviceBuffer.data(), m_outputBuff.deviceBuffer.nbBytes(), cudaMemcpyDeviceToHost, m_cudaStream);
    if (ret != 0)
    {
        std::cout << "Unable to copy buffer from GPU back to CPU" << std::endl;
        return false;
    }

    ret = cudaStreamSynchronize(m_cudaStream);
    if (ret != 0)
    {
        std::cout << "Unable to synchronize cuda stream" << std::endl;
        return false;
    }

    // Copy to output
    int outsize = outputDims.d[1] * outputDims.d[2];

    std::vector<float> featureVector;
    featureVector.resize(outsize);
    memcpy(featureVector.data(), reinterpret_cast<const char *>(m_outputBuff.hostBuffer.data()), outsize * sizeof(float));
    featureVectors.emplace_back(std::move(featureVector));

    return outsize;
}

std::string Engine::serializeEngineOptions(const Options &options)
{
    std::string engineName = "trt.engine";

    std::vector<std::string> gpuUUIDs;
    getGPUUUIDs(gpuUUIDs);

    if (static_cast<size_t>(options.deviceIndex) >= gpuUUIDs.size())
    {
        throw std::runtime_error("Error, provided device index is out of range!");
    }

    engineName += "." + gpuUUIDs[options.deviceIndex];

    // Serialize the specified options into the filename
    if (options.FP16)
    {
        engineName += ".fp16";
    }
    else
    {
        engineName += ".fp32";
    }

    engineName += "." + std::to_string(options.maxBatchSize) + ".";
    for (size_t i = 0; i < m_options.optBatchSizes.size(); ++i)
    {
        engineName += std::to_string(m_options.optBatchSizes[i]);
        if (i != m_options.optBatchSizes.size() - 1)
        {
            engineName += "_";
        }
    }

    engineName += "." + std::to_string(options.maxWorkspaceSize);

    return engineName;
}

void Engine::getGPUUUIDs(std::vector<std::string> &gpuUUIDs)
{
    int numGPUs;
    cudaGetDeviceCount(&numGPUs);

    for (int device = 0; device < numGPUs; device++)
    {
        cudaDeviceProp prop;
        cudaGetDeviceProperties(&prop, device);

        char uuid[33];
        for (int b = 0; b < 16; b++)
        {
            sprintf(&uuid[b * 2], "%02x", (unsigned char)prop.uuid.bytes[b]);
        }

        gpuUUIDs.push_back(std::string(uuid));
        // by comparing uuid against a preset list of valid uuids given by the client (using: nvidia-smi -L) we decide which gpus can be used.
    }
}

cv::Mat Engine::preprocessImg(const std::string inputImage)
{
    auto img = cv::imread(inputImage, -1);
    cv::Mat resize_img;

    if (img.cols * 1.0 / img.rows * m_options.inputDimension[1] >= m_options.inputDimension[2])
    {
        cv::resize(img, resize_img, cv::Size(m_options.inputDimension[2], m_options.inputDimension[1]));
        Normalize(&resize_img);
    }
    else
    {
        cv::resize(img, resize_img, cv::Size(int(img.cols * 1.0 / img.rows * m_options.inputDimension[1] + 1), m_options.inputDimension[1]), 0.f, 0.f,
                   cv::INTER_LINEAR);
        Normalize(&resize_img);
        // Padding the right part
        cv::copyMakeBorder(resize_img, resize_img, 0, 0, 0, m_options.inputDimension[2] - int(img.cols * 1.0 / img.rows * m_options.inputDimension[1] + 1), cv::BORDER_CONSTANT, {0, 0, 0});
    }

    return resize_img;
}

std::vector<std::string> ReadDict(const std::string &path)
{
    std::ifstream in(path);
    std::string line;
    std::vector<std::string> m_vec;
    if (in)
    {
        while (getline(in, line))
        {
            m_vec.push_back(line);
        }
    }
    else
    {
        std::cout << "no such label file: " << path << ", exit the program..."
                  << std::endl;
        exit(1);
    }
    return m_vec;
}