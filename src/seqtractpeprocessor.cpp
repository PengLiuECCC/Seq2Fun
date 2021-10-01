#include "seqtractpeprocessor.h"
#include <iostream>
#include <unistd.h>
#include <functional>
#include <thread>
#include <memory.h>


SeqTractPeProcessor::SeqTractPeProcessor(Options * opt) {
    mOptions = opt;
    mProduceFinished = false;
    mSampleSize = mOptions->mSeqExtractions.targetGenesSubVec.size();
    featureUSet.clear();
}

SeqTractPeProcessor::~SeqTractPeProcessor() {
    destroyPackRepository();
}

bool SeqTractPeProcessor::process(){

    initPackRepository();
    std::thread producer(std::bind(&SeqTractPeProcessor::producerTask, this));

    int threadnum = mSampleSize + 1;

    mConfigs = new ThreadsConfig2*[threadnum];
    for(int t=0; t<threadnum; t++){
        mConfigs[t] = new ThreadsConfig2(mOptions, t);
    }

    std::thread** threads = new thread*[threadnum];
    for(int t=0; t<threadnum; t++){
        threads[t] = new std::thread(std::bind(&SeqTractPeProcessor::writeTask, this, mConfigs[t]));
    }

    std::thread seqTracT(std::bind(&SeqTractPeProcessor::consumerTask, this));

    producer.join();
    seqTracT.join();
    for(int t=0; t<threadnum; t++){
        threads[t]->join();
    }

    // clean up
    for(int t=0; t<threadnum; t++){
        delete threads[t];
        threads[t] = NULL;
        delete mConfigs[t];
        mConfigs[t] = NULL;
    }

    delete threads;
    threads = NULL;
    delete mConfigs;
    mConfigs = NULL;

    return true;
}

bool SeqTractPeProcessor::processReads(ReadPack* pack){
    string* outputs = new string[mSampleSize + 1];
    for(int p=0;p<pack->count;p++){
        Read* r = pack->data[p];
        auto vec = split2(r->mName, '\t');
        if (vec.size() > 1) {
            auto feature = vec[1];
            if (starts_with(feature, "K")) {
                auto sample = getVecIndex(mOptions->mSeqExtractions.targetGenesSubVec, feature);
                if (sample != -1) {
                    featureUSet.insert(feature);
                    outputs[sample] += r->toString();
                } else {
                    if(getVecIndex(mOptions->mSeqExtractions.targetGenesVec, feature) != -1){
                        sample = mSampleSize;
                        outputs[sample] += r->toString();
                    }
                }
            }
        }
        delete r;
    }
    for(int i=0;i< mSampleSize+1; i++) {
        if(outputs[i].size() == 0)
            continue;
        char* data = new char[outputs[i].size()];
        memcpy(data, outputs[i].c_str(), outputs[i].size());
        mConfigs[i]->input(data, outputs[i].size());
    }
    mOptions->mSeqExtractions.numFeatures = featureUSet.size();
    
    delete pack->data;
    delete pack;
    delete[] outputs;
    return true;
}

void SeqTractPeProcessor::initPackRepository() {
    mRepo.packBuffer = new ReadPack*[PACK_NUM_LIMIT];
    memset(mRepo.packBuffer, 0, sizeof(ReadPack*)*PACK_NUM_LIMIT);
    mRepo.writePos = 0;
    mRepo.readPos = 0;
    mRepo.readCounter = 0;
    
}

void SeqTractPeProcessor::destroyPackRepository() {
    if(mRepo.packBuffer) delete mRepo.packBuffer; mRepo.packBuffer = NULL;
}

void SeqTractPeProcessor::producePack(ReadPack* pack){
    std::unique_lock<std::mutex> lock(mRepo.mtx);
    while(((mRepo.writePos + 1) % PACK_NUM_LIMIT) == mRepo.readPos) {
        mRepo.repoNotFull.wait(lock);
    }

    mRepo.packBuffer[mRepo.writePos] = pack;
    mRepo.writePos++;

    if (mRepo.writePos == PACK_NUM_LIMIT)
        mRepo.writePos = 0;

    mRepo.repoNotEmpty.notify_all();
    lock.unlock();
}

void SeqTractPeProcessor::consumePack(){
    ReadPack* data;
    std::unique_lock<std::mutex> lock(mRepo.mtx);
    // read buffer is empty, just wait here.
    while(mRepo.writePos == mRepo.readPos) {
        if(mProduceFinished){
            lock.unlock();
            return;
        }
        mRepo.repoNotEmpty.wait(lock);
    }

    data = mRepo.packBuffer[mRepo.readPos];
    (mRepo.readPos)++;
    lock.unlock();

    processReads(data);


    if (mRepo.readPos >= PACK_NUM_LIMIT)
        mRepo.readPos = 0;

    mRepo.repoNotFull.notify_all();
}

void SeqTractPeProcessor::producerTask(){
    int slept = 0;
    long readNum = 0;
    bool splitSizeReEvaluated = false;
    Read** data = nullptr;

    for (const auto & in : mOptions->mSeqExtractions.samplesVec) {
        check_file_valid(in);
        data = new Read*[PACK_SIZE];
        memset(data, 0, sizeof (Read*) * PACK_SIZE);
        std::string msg = "Processing sample: " + in;
        loginfo(msg);
        FastqReader reader(in, true);
        int count = 0;
        while (true) {
            Read* read = reader.read();
            if (!read) {
                // the last pack
                ReadPack* pack = new ReadPack;
                pack->data = data;
                pack->count = count;
                producePack(pack);
                data = NULL;
                break;
            }
            data[count] = read;
            count++;
            // a full pack
            if (count == PACK_SIZE) {
                ReadPack* pack = new ReadPack;
                pack->data = data;
                pack->count = count;
                producePack(pack);
                //re-initialize data for next pack
                data = new Read*[PACK_SIZE];
                memset(data, 0, sizeof (Read*) * PACK_SIZE);
                // reset count to 0
                count = 0;
                // if the consumer is far behind this producer, sleep and wait to limit memory usage
                while (mRepo.writePos - mRepo.readPos > PACK_IN_MEM_LIMIT) {
                    //cout<<"sleep"<<endl;
                    slept++;
                    usleep(100);
                }
                readNum += PACK_SIZE;
                if (mOptions->verbose) {
                    if (readNum > 100000 && readNum % 100000 == 0) {
                        std::string msg = "loading " + std::to_string(readNum / 100000) + " * 100K reads" + 
                                " detected " + std::to_string(featureUSet.size()) + " out of " + 
                                std::to_string(mOptions->mSeqExtractions.targetGenesVec.size()) + " features";
                        loginfo(msg);
                    }
                }
            }
        }
    }

    std::unique_lock<std::mutex> lock(mRepo.readCounterMtx);
    mProduceFinished = true;
    lock.unlock();

    // if the last data initialized is not used, free it
    if(data != NULL)
        delete data;
}

void SeqTractPeProcessor::consumerTask(){
    while(true) {
        std::unique_lock<std::mutex> lock(mRepo.readCounterMtx);
        if(mProduceFinished && mRepo.writePos == mRepo.readPos){
            lock.unlock();
            break;
        }
        if(mProduceFinished){
            consumePack();
            lock.unlock();
        } else {
            lock.unlock();
            consumePack();
        }
    }
    // notify all writer threads
    for(int i=0; i<mSampleSize+1; i++) {
        mConfigs[i]->setInputCompleted();
    }
}

void SeqTractPeProcessor::writeTask(ThreadsConfig2* config){
    while(true) {
        if(config->isCompleted()){
            // check one moe time to prevent possible loss of data
            config->output();
            break;
        }
        config->output();
    }
}