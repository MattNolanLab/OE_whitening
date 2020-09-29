#include "ProcessorPlugin.h"
#include <fstream>

using namespace ProcessorPluginSpace;
using namespace Eigen;
using std::sqrt;

#define DATA_CHANNEL 16 //tetrode channels, TODO: hardcoded for nowm need to get the neural data channels

//Change all names for the relevant ones, including "Processor Name"
ProcessorPlugin::ProcessorPlugin() : 
    GenericProcessor("Whitening")
    , abstractFifo(100)
    , bufferLength(2.0f)
{
    subprocessorToDraw = 0;
    numSubprocessors = -1;
    samplingRate = 0;
    numChannels = 0;
}

ProcessorPlugin::~ProcessorPlugin()
{

}

void ProcessorPlugin::updateSettings()
{

    std::cout << "Setting num inputs on ProcessorPlugin to " << getNumInputs() << std::endl;

    numChannelsInSubprocessor.clear();
    subprocessorSampleRate.clear();


    //Assum only one subprocessor, and all data channel has the same sampling rate
    if (getNumInputs() > 0) {
        samplingRate = getDataChannel(0)->getSampleRate();
        numChannels = getNumInputs();

    }
    
    displayBuffers = std::make_shared<AudioSampleBuffer>(8, 100);

    std::cout << "Re-setting num inputs on ProcessorPlugin to " << numChannels << std::endl;
    if (numChannels > 0)
    {
        std::cout << "Sample rate = " << samplingRate << std::endl;
    }



    resizeBuffer();


}



uint32 ProcessorPlugin::getChannelSourceId(const InfoObjectCommon* chan)
{
    return getProcessorFullId(chan->getSourceNodeID(), chan->getSubProcessorIdx());
}

uint32 ProcessorPlugin::getDataSubprocId(int chan) const
{
    if (chan < 0 || chan >= getTotalDataChannels())
    {
        return 0;
    }

    return getChannelSourceId(getDataChannel(chan));
}

void ProcessorPlugin::setSubprocessor(uint32 sp)
{
    subprocessorToDraw = sp;
    std::cout << "ProcessorPlugin setting subprocessor to " << sp << std::endl;
}

uint32 ProcessorPlugin::getSubprocessor() const
{
    return subprocessorToDraw;
}

int ProcessorPlugin::getNumSubprocessorChannels()
{
    if (subprocessorToDraw != 0)
    {
        return numChannelsInSubprocessor[subprocessorToDraw];
    }
    return 0;
}

float ProcessorPlugin::getSubprocessorSampleRate(uint32 subprocId)
{
    auto entry = subprocessorSampleRate.find(subprocId);
    if (entry != subprocessorSampleRate.end())
    {
        return entry->second;
    }
    return 0.0f;
}

bool ProcessorPlugin::resizeBuffer()
{
    int totalResized = 0;

    ScopedLock displayLock(displayMutex);

    int nSamples = (int)samplingRate * bufferLength; //always return the first subprocessor
    int nInputs = numChannels;

    std::cout << "Resizing buffer " << ". Samples: " << nSamples << ", Inputs: " << nInputs << std::endl;

    if (nSamples > 0 && nInputs > 0)
    {
        abstractFifo.setTotalSize(nSamples);
        displayBuffers->setSize(nInputs + 1, nSamples); // add extra channel for TTLs
        displayBuffers->clear();

        //displayBufferIndices[currSubproc].clear();
        //displayBufferIndices[currSubproc].insert(displayBufferIndices[currSubproc].end(), nInputs + 1, 0);
        displayBufferIndices.clear();
        displayBufferIndices.insert(displayBufferIndices.end(), nInputs + 1, 0);
    }

    return true;

}

void ProcessorPlugin::calculateWhiteningMatrix() {
    // Calculate the whitening matrix based on data in the buffer

     /*
    the AudioSampleBuffer is using a HeapBlock underneath. It contains two main segments. The first segment contains
    a list of pointers to the beginning memory location of each channel's data. The second segment contains the actual data.
    getArrayofReadPointers() will return the pointer to the list of channel pointers, while getReadpointers() will return the pointer to the data itself
    data is arranged in channel-major format

    */

    ofstream file;
    file.open("whitening_results.txt");

    
    // Copy data over to the 
  /*  AudioSampleBuffer buffer_copy;
    buffer_copy.makeCopyOf(buffer);*/
    int numSample = displayBuffers->getNumSamples();
    int numChannel = displayBuffers->getNumChannels();
    auto buffer_ptr = displayBuffers->getWritePointer(0); //get the beginning of the data
    Eigen::Map<Matrix<float,Dynamic, Dynamic, RowMajor>> m(buffer_ptr, DATA_CHANNEL,numSample); // by default MatrixXf is column-major
    //m.transposeInPlace();  //need to use this, otherwise encounter aliasing issue

    ofstream bufferData;
    bufferData.open("buffer.csv");
    bufferData << m;
    bufferData.close();


    //MatrixXf m(3,6);

   /* m << 1, 2, 3, 4, 5, 6,
        1, 3, 2, 4, 5, 6,
        1, 3, 2, 4, 5, 8;
    int numSample = 6;*/

    //cout << "m:" << endl << m << endl;

    auto mean = m.rowwise().mean();

    file << "mean" << endl << mean << endl;;
    
    //std::cout << mean << endl;

    // subtract the mean
    m = m.colwise() - mean;

     //Covariation matrix
    auto AAt = m * m.transpose() / numSample;
    file << "Covariance matrix" << endl << AAt << endl;
   /* cout << "Covariance matrix" << endl << AAt <<endl;*/

    // SVD
    Eigen::BDCSVD<MatrixXf> svdSolver;
    svdSolver.compute(AAt, Eigen::ComputeFullU|Eigen::ComputeFullV);

    auto U = svdSolver.matrixU();
    auto V = svdSolver.matrixV();
    auto S = svdSolver.singularValues();
    S.array() += 0.0001; //avoid divide by zero

    //cout << "U" << endl;
    //cout << U << endl;

    //cout << "V" << endl;
    //cout << V << endl;

  /*  cout << "S" << endl;
    cout << S << endl;*/
    file << "S" << endl << S << endl;


    // Apply whitening
    auto sinv = S.matrix().cwiseSqrt().cwiseInverse().asDiagonal();
    m_W = (U * sinv * V.transpose());

    file << "W" << endl << m_W << endl;

   /* cout << "W" << endl;
    cout << m_W << endl;*/

    m_whiteningMatrixReady = true;

    file.close();


}


void ProcessorPlugin::applyWhitening(AudioSampleBuffer& buffer) {
    int numSample = buffer.getNumSamples();
    int numChannel = buffer.getNumChannels();
    auto buffer_ptr = buffer.getWritePointer(0);
    Eigen::Map<MatrixXf> input_data(buffer_ptr, DATA_CHANNEL, numSample);

    // remove mean
    auto mean = input_data.rowwise().mean();
    input_data = input_data.colwise() - mean;

    //whitening
    //input_data.array() *= 0;
    input_data = m_W * input_data;
}

void ProcessorPlugin::process(AudioSampleBuffer& buffer)
{
    // 1. place any new samples into the displayBuffer
  //std::cout << "Display node sample count: " << nSamples << std::endl; ///buffer.getNumSamples() << std::endl;
    if (true)
    {
        ScopedLock displayLock(displayMutex);


        if (readyChannel < DATA_CHANNEL)
        {

            for (int chan = 0; chan < buffer.getNumChannels(); ++chan)
            {

                const int samplesLeft = displayBuffers->getNumSamples() - displayBufferIndices[chan];
                const int nSamples = getNumSamples(chan);

                if (nSamples < samplesLeft)
                {
                    displayBuffers->copyFrom(chan,                      // destChannel
                        displayBufferIndices[chan],  // destStartSample
                        buffer,                    // source
                        chan,                      // source channel
                        0,                         // source start sample
                        nSamples);                 // numSamples

                    displayBufferIndices[chan] = displayBufferIndices[chan] + nSamples;
                }
                else
                {
                    const int extraSamples = nSamples - samplesLeft;

                    displayBuffers->copyFrom(chan,                      // destChannel
                        displayBufferIndices[chan],  // destStartSample
                        buffer,                    // source
                        chan,                      // source channel
                        0,                         // source start sample
                        samplesLeft);              // numSamples

                    // Wrap around
                    displayBuffers->copyFrom(chan,                      // destChannel
                        0,                         // destStartSample
                        buffer,                    // source
                        chan,                      // source channel
                        samplesLeft,               // source start sample
                        extraSamples);             // numSamples

                    displayBufferIndices[chan] = extraSamples;

                    readyChannel++;
                    /*isBufferReady = true;
                    std::cout << "Buffer is ready" << std::endl;

                    calculateWhiteningMatrix();*/
                }
            }


            std::cout << "Buffer index " << displayBufferIndices[0]<<  endl;
           /* for (int i = 0; i < displayBufferIndices.size(); i++) {
                cout << displayBufferIndices[0];
            }
            cout << endl;*/
            
            

        }
        else {
            //cout << "Buffer is ready" << std::endl;
            if (!m_whiteningMatrixReady) {
                calculateWhiteningMatrix();
                cout << "Whitening matrix updated" << endl;
            }
            else {
                applyWhitening(buffer);
            }

        }
        

    }

	//Do whatever processing needed

	//MatrixXd m(2, 2);
	//m(0, 0) = 3;
	//m(1, 0) = 2.5;
	//m(0, 1) = -1;
	//m(1, 1) = m(1, 0) + m(0, 1);
	//std::cout << m << std::endl;
	
	 
}

