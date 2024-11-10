//
// Created by Martin Steinegger on 17.09.18.
//

#include "DistanceCalculator.h"
#include "Util.h"
#include "Parameters.h"
#include "Matcher.h"
#include "Debug.h"
#include "DBReader.h"
#include "DBWriter.h"
#include "QueryMatcher.h"
#include "NucleotideMatrix.h"
#include "FastSort.h"
#include "SubstitutionMatrixProfileStates.h"
#include "IndexReader.h"
#include "QueryMatcherTaxonomyHook.h"

#include <fcntl.h>
#include <sys/mman.h>

#ifdef OPENMP
#include <omp.h>
#endif
// #define HAVE_CUDA 1
#ifdef HAVE_CUDA
#include "GpuUtil.h"
#include "Alignment.h"

#endif

#ifdef HAVE_CUDA
void runFilterOnGpu(Parameters & par, BaseMatrix * subMat,
                    DBReader<unsigned int> * qdbr, DBReader<unsigned int> * tdbr,
                    bool sameDB, DBWriter & resultWriter, EvalueComputation * evaluer,
                    QueryMatcherTaxonomyHook *taxonomyHook){
    Debug::Progress progress(qdbr->getSize());
    const int querySeqType = qdbr->getDbtype();
    Sequence qSeq(par.maxSeqLen, querySeqType, subMat, 0, false, par.compBiasCorrection);

    std::vector<Marv::Result> results;
    results.reserve(par.maxResListLen);
    std::vector<hit_t> shortResults;
    std::vector<Matcher::result_t> resultsAln;

    size_t profileBufferLength = par.maxSeqLen;
    int8_t* profile = NULL;
    if (Parameters::isEqualDbtype(querySeqType, Parameters::DBTYPE_HMM_PROFILE) == false) {
        profile = (int8_t*)malloc(subMat->alphabetSize * profileBufferLength * sizeof(int8_t));
    }

    std::string resultBuffer;
    resultBuffer.reserve(262144);
    char buffer[1024+32768];

    size_t compBufferSize = (par.maxSeqLen + 1) * sizeof(float);
    float *compositionBias = NULL;
    if (par.compBiasCorrection == true) {
        compositionBias = (float*)malloc(compBufferSize);
        memset(compositionBias, 0, compBufferSize);
    }

    size_t* offsetData;
    int32_t* lengthData;
    // hash the realpath of par.db2
    std::string tdbrName = par.db2;
    std::string tdbrRelPath = FileUtil::getRealPathFromSymLink(PrefilteringIndexReader::dbPathWithoutIndex(tdbrName));
    size_t hash = Util::hash(tdbrRelPath.c_str(), tdbrRelPath.length());
    std::string shmFileInFile = (par.gpuServer == 0) ? "" : "/dev/shm/" + SSTR(hash);
    if(shmFileInFile != "" && FileUtil::fileExists(shmFileInFile.c_str()) == false){
        Debug(Debug::ERROR) << "--gpu-server " << shmFileInFile << " does not exist";
        EXIT(EXIT_FAILURE);
    }
    std::vector<size_t> offsets;
    std::vector<int32_t> lengths;
    GPUSharedMemory* layout;
    pid_t pid = 0;  // current process ID, only for server
    if (FileUtil::fileExists(shmFileInFile.c_str()) == false) {
        offsets.reserve(tdbr->getSize() + 1);
        lengths.reserve(tdbr->getSize());
        for(size_t id = 0; id < tdbr->getSize(); id++){
            offsets.emplace_back(tdbr->getIndex()[id].offset);
            lengths.emplace_back(tdbr->getIndex()[id].length - 2);
        }
        offsets.emplace_back(offsets.back() + lengths.back());
        offsetData = offsets.data();
        lengthData = lengths.data();
    } else {
        pid = getpid();
        layout = GPUSharedMemory::openSharedMemory(SSTR(hash));
    }

    Marv * marv;
    if(par.gpuServer == 0) {
        int32_t maxTargetLength = lengths.back();
        Marv::AlignmentType type =  (par.prefMode == Parameters::PREF_MODE_UNGAPPED_AND_GAPPED) ?
                Marv::AlignmentType::GAPLESS_SMITH_WATERMAN : Marv::AlignmentType::GAPLESS;
        marv = new Marv(tdbr->getSize(), subMat->alphabetSize, maxTargetLength,
                        par.maxResListLen, type);
        void* h = marv->loadDb(
                tdbr->getDataForFile(0), offsetData, lengthData, tdbr->getDataSizeForFile(0)
        );
        marv->setDb(h);
    }

    // marv.prefetch();
    for (size_t id = 0; id < qdbr->getSize(); id++) {
        size_t queryKey = qdbr->getDbKey(id);
        unsigned int querySeqLen = qdbr->getSeqLen(id);
        char *querySeqData = qdbr->getData(id, 0);
        qSeq.mapSequence(id, queryKey, querySeqData, querySeqLen);
        if (Parameters::isEqualDbtype(querySeqType, Parameters::DBTYPE_HMM_PROFILE)) {
            profile = qSeq.profile_for_alignment;
        } else {
            if ((size_t)qSeq.L >= profileBufferLength) {
                profileBufferLength = (size_t)qSeq.L * 1.5;
                profile = (int8_t*)realloc(profile, subMat->alphabetSize * profileBufferLength * sizeof(int8_t));
            }
            if (compositionBias != NULL) {
                if ((size_t)qSeq.L >= compBufferSize) {
                    compBufferSize = (size_t)qSeq.L * 1.5 * sizeof(float);
                    compositionBias = (float*)realloc(compositionBias, compBufferSize);
                    // memset(compositionBias, 0, compBufferSize);
                }
                SubstitutionMatrix::calcLocalAaBiasCorrection(subMat, qSeq.numSequence, qSeq.L, compositionBias, par.compBiasCorrectionScale);
            }
            for (size_t j = 0; j < (size_t)subMat->alphabetSize; ++j) {
                for (size_t i = 0; i < (size_t)qSeq.L; ++i) {
                    short bias = 0;
                    if (compositionBias != NULL) {
                        bias = static_cast<short>((compositionBias[i] < 0.0) ? (compositionBias[i] - 0.5) : (compositionBias[i] + 0.5));
                    }
                    profile[j * qSeq.L  + i] = subMat->subMatrix[j][qSeq.numSequence[i]] + bias;
                }
            }
        }
        Marv::Stats stats;
        if(par.gpuServer == 0) {
            stats = marv->scan(reinterpret_cast<const char *>(qSeq.numSequence), qSeq.L, profile, results.data());
        }else{
            while(layout->trySetServerReady(pid)==false) {
                std::this_thread::yield();
            }
            memcpy(layout->getQueryPtr(), qSeq.numSequence, qSeq.L);
            memcpy(layout->getProfilePtr(), profile, subMat->alphabetSize * qSeq.L);
            layout->queryLen = qSeq.L;
            layout->clientReady.store(1, std::memory_order_release);
            while(layout->serverReady.load(std::memory_order_acquire) != UINT_MAX) {
                std::this_thread::yield();
            }
            memcpy(results.data(), layout->getResultsPtr(), layout->resultLen * sizeof(Marv::Result));
            stats.results = layout->resultLen;
            layout->resetServerAndClientReady();
        }

        for(size_t i = 0; i < stats.results; i++){
            unsigned int targetKey = tdbr->getDbKey(results[i].id);
            int score = results[i].score;
            if(taxonomyHook != NULL){
                TaxID currTax = taxonomyHook->taxonomyMapping->lookup(targetKey);
                if (taxonomyHook->expression[0]->isAncestor(currTax) == false) {
                    continue;
                }
            }
            // check if evalThr != inf
            // double evalue = 0.0;
            // if (par.evalThr < std::numeric_limits<double>::max()) {
            //     evalue = evaluer->computeEvalue(score, qSeq.L);
            // }
            // bool hasEvalue = (evalue <= par.evalThr);
            bool hasDiagScore = (score > par.minDiagScoreThr);

            const bool isIdentity = (queryKey == targetKey && (par.includeIdentity || sameDB))? true : false;
            // --filter-hits
            if (isIdentity || hasDiagScore) {
                if(par.prefMode == Parameters::PREF_MODE_UNGAPPED_AND_GAPPED){
                    Matcher::result_t res;
                    res.dbKey = targetKey;
                    res.eval = evaluer->computeEvalue(score, qSeq.L);
                    res.dbEndPos = results[i].dbEndPos;
                    res.dbLen = tdbr->getSeqLen(results[i].id);
                    res.qEndPos =  results[i].qEndPos;
                    res.qLen = qSeq.L;
                    unsigned int qAlnLen = std::max(static_cast<unsigned int>(res.qEndPos), static_cast<unsigned int>(1));
                    unsigned int dbAlnLen = std::max(static_cast<unsigned int>(res.dbEndPos), static_cast<unsigned int>(1));
                    //seqId = (alignment.score1 / static_cast<float>(std::max(dbAlnLen, qAlnLen)))  * 0.1656 + 0.1141;
                    res.seqId = Matcher::estimateSeqIdByScorePerCol(score, qAlnLen, dbAlnLen);
                    res.qcov = SmithWaterman::computeCov(0, res.qEndPos, res.qLen );
                    res.dbcov = SmithWaterman::computeCov(0, res.dbEndPos, res.dbLen );
                    res.score = evaluer->computeBitScore(score);
                    if(Alignment::checkCriteria(res, isIdentity, par.evalThr,  par.seqIdThr,  par.alnLenThr,  par.covMode,  par.covThr)){
                        resultsAln.emplace_back(res);
                    }
                } else {
                    hit_t hit;
                    hit.seqId = targetKey;
                    hit.prefScore = score;
                    hit.diagonal = 0;
                    shortResults.emplace_back(hit);
                }
            }
        }
        if(par.prefMode == Parameters::PREF_MODE_UNGAPPED_AND_GAPPED) {
            SORT_PARALLEL(resultsAln.begin(), resultsAln.end(), Matcher::compareHits);
            size_t maxSeqs = std::min(par.maxResListLen, resultsAln.size());
            for (size_t i = 0; i < maxSeqs; ++i) {
                size_t len = Matcher::resultToBuffer(buffer, resultsAln[i], false);
                resultBuffer.append(buffer, len);
            }
        }else{
            SORT_PARALLEL(shortResults.begin(), shortResults.end(), hit_t::compareHitsByScoreAndId);
            size_t maxSeqs = std::min(par.maxResListLen, shortResults.size());
            for (size_t i = 0; i < maxSeqs; ++i) {
                size_t len = QueryMatcher::prefilterHitToBuffer(buffer, shortResults[i]);
                resultBuffer.append(buffer, len);
            }
        }

        resultWriter.writeData(resultBuffer.c_str(), resultBuffer.length(), queryKey, 0);
        resultBuffer.clear();
        shortResults.clear();
        resultsAln.clear();
        progress.updateProgress();
    }
    if(par.gpuServer == 0) {
        delete marv;
    }else{
        GPUSharedMemory::unmap(layout);
    }

    if (compositionBias != NULL) {
        free(compositionBias);
    }
    if (Parameters::isEqualDbtype(querySeqType, Parameters::DBTYPE_HMM_PROFILE) == false) {
        free(profile);
    }
}
#endif

void runFilterOnCpu(Parameters & par, BaseMatrix * subMat, int8_t * tinySubMat,
                    DBReader<unsigned int> * qdbr, DBReader<unsigned int> * tdbr,
                    SequenceLookup * sequenceLookup, bool sameDB, DBWriter & resultWriter, EvalueComputation * evaluer,
                    QueryMatcherTaxonomyHook *taxonomyHook, int alignmentMode){
    std::vector<hit_t> shortResults;
    shortResults.reserve(tdbr->getSize()/2);
    Debug::Progress progress(qdbr->getSize());
    const int targetSeqType = tdbr->getDbtype();
    const int querySeqType = qdbr->getDbtype();
#ifdef OPENMP
    omp_set_nested(1);
#endif

#pragma omp parallel
    {
        unsigned int thread_idx = 0;
#ifdef OPENMP
        thread_idx = (unsigned int) omp_get_thread_num();
#endif
        char buffer[1024+32768];
        std::vector<hit_t> threadShortResults;
        Sequence qSeq(par.maxSeqLen, querySeqType, subMat, 0, false, par.compBiasCorrection);
        Sequence tSeq(par.maxSeqLen, targetSeqType, subMat, 0, false, par.compBiasCorrection);
        SmithWaterman aligner(par.maxSeqLen, subMat->alphabetSize,
                              par.compBiasCorrection, par.compBiasCorrectionScale, targetSeqType);

        std::string resultBuffer;
        resultBuffer.reserve(262144);
        for (size_t id = 0; id < qdbr->getSize(); id++) {
            char *querySeqData = qdbr->getData(id, thread_idx);
            size_t queryKey = qdbr->getDbKey(id);
            unsigned int querySeqLen = qdbr->getSeqLen(id);

            qSeq.mapSequence(id, queryKey, querySeqData, querySeqLen);
//            qSeq.printProfileStatePSSM();
            if(Parameters::isEqualDbtype(qSeq.getSeqType(), Parameters::DBTYPE_HMM_PROFILE) ){
                aligner.ssw_init(&qSeq, qSeq.getAlignmentProfile(), subMat);
            }else{
                aligner.ssw_init(&qSeq, tinySubMat, subMat);
            }
#pragma omp for schedule(static) nowait
            for (size_t tId = 0; tId < tdbr->getSize(); tId++) {
                unsigned int targetKey = tdbr->getDbKey(tId);
                if(taxonomyHook != NULL){
                    TaxID currTax = taxonomyHook->taxonomyMapping->lookup(targetKey);
                    if (taxonomyHook->expression[thread_idx]->isAncestor(currTax) == false) {
                        continue;
                    }
                }

                const bool isIdentity = (queryKey == targetKey && (par.includeIdentity || sameDB))? true : false;
                if(sequenceLookup == NULL){
                    char * targetSeq = tdbr->getData(tId, thread_idx);
                    unsigned int targetSeqLen = tdbr->getSeqLen(tId);
                    tSeq.mapSequence(tId, targetKey, targetSeq, targetSeqLen);
                    // mask numSequence
                    unsigned char xChar = subMat->aa2num[static_cast<int>('X')];
                    for (int i = 0; i < tSeq.L; i++) {
                        tSeq.numSequence[i] = ((targetSeq[i] >= 32 && targetSeq[i] <= 52) || targetSeq[i] >= 97)  ? xChar : tSeq.numSequence[i];
                    }
                }else{
                    tSeq.mapSequence(tId, targetKey, sequenceLookup->getSequence(tId));
                }
                float queryLength = qSeq.L;
                float targetLength = tSeq.L;
                if(Util::canBeCovered(par.covThr, par.covMode, queryLength, targetLength)==false){
                    continue;
                }

                bool hasEvalue = true;
                int score;
                if (alignmentMode == 0) {
                    score = aligner.ungapped_alignment(tSeq.numSequence, tSeq.L);
                } else {
                    std::string backtrace;
                    s_align res;
                    if (isIdentity) {
                        res = aligner.scoreIdentical(
                                tSeq.numSequence, tSeq.L, evaluer, Matcher::SCORE_ONLY, backtrace
                        );
                    } else {
                        res = aligner.ssw_align(
                                tSeq.numSequence,
                                tSeq.numConsensusSequence,
                                tSeq.getAlignmentProfile(),
                                tSeq.L,
                                backtrace,
                                par.gapOpen.values.aminoacid(),
                                par.gapExtend.values.aminoacid(),
                                Matcher::SCORE_ONLY,
                                par.evalThr,
                                evaluer,
                                par.covMode,
                                par.covThr,
                                par.correlationScoreWeight,
                                qSeq.L / 2,
                                tId
                        );
                    }
                    score = res.score1;
                    // check if evalThr != inf
                    double evalue = 0.0;
                    if (par.evalThr < std::numeric_limits<double>::max()) {
                        evalue = evaluer->computeEvalue(score, qSeq.L);
                    }
                    hasEvalue = (evalue <= par.evalThr);
                }
                bool hasDiagScore = (score > par.minDiagScoreThr);
                if (isIdentity || (hasDiagScore && hasEvalue)) {
                    hit_t hit;
                    hit.seqId = targetKey;
                    hit.prefScore = score;
                    hit.diagonal = 0;
                    threadShortResults.emplace_back(hit);
                }
            }
#pragma omp critical
            {
                shortResults.insert(shortResults.end(), threadShortResults.begin(), threadShortResults.end());
                threadShortResults.clear();
            }
#pragma omp barrier
#pragma omp master
            {
                SORT_PARALLEL(shortResults.begin(), shortResults.end(), hit_t::compareHitsByScoreAndId);
                size_t maxSeqs = std::min(par.maxResListLen, shortResults.size());
                for (size_t i = 0; i < maxSeqs; ++i) {
                    size_t len = QueryMatcher::prefilterHitToBuffer(buffer, shortResults[i]);
                    resultBuffer.append(buffer, len);
                }

                resultWriter.writeData(resultBuffer.c_str(), resultBuffer.length(), queryKey, 0);
                resultBuffer.clear();
                shortResults.clear();
                progress.updateProgress();
            }
#pragma omp barrier
        }
    }
}

int prefilterInternal(int argc, const char **argv, const Command &command, int mode) {
    Parameters &par = Parameters::getInstance();
    par.parseParameters(argc, argv, command, true, 0, 0);
    int outputDbtype = (par.prefMode == Parameters::PREF_MODE_UNGAPPED_AND_GAPPED)
                      ? Parameters::DBTYPE_ALIGNMENT_RES : Parameters::DBTYPE_PREFILTER_RES;
    DBWriter resultWriter(par.db3.c_str(), par.db3Index.c_str(), 1, par.compressed, outputDbtype);
    resultWriter.open();
    bool sameDB = (par.db2.compare(par.db1) == 0);
    bool touch = (par.preloadMode != Parameters::PRELOAD_MODE_MMAP);
    IndexReader tDbrIdx(par.db2, par.threads, IndexReader::SEQUENCES, (touch) ? (IndexReader::PRELOAD_INDEX | IndexReader::PRELOAD_DATA) : 0 );
    IndexReader * qDbrIdx = NULL;
    DBReader<unsigned int> * qdbr = NULL;
    DBReader<unsigned int> * tdbr = tDbrIdx.sequenceReader;
    const int targetSeqType = tdbr->getDbtype();
    int querySeqType;
    if (sameDB == true) {
        qDbrIdx = &tDbrIdx;
        qdbr = tdbr;
        querySeqType = targetSeqType;
    } else {
        // open the sequence, prefiltering and output databases
        qDbrIdx = new IndexReader(par.db1, par.threads, IndexReader::SEQUENCES, (touch) ? IndexReader::PRELOAD_INDEX : 0);
        qdbr = qDbrIdx->sequenceReader;
        querySeqType = qdbr->getDbtype();
    }

    SequenceLookup * sequenceLookup = NULL;
    if(Parameters::isEqualDbtype(tDbrIdx.getDbtype(), Parameters::DBTYPE_INDEX_DB)){
        PrefilteringIndexData data = PrefilteringIndexReader::getMetadata(tDbrIdx.index);
        if(data.splits == 1){
            sequenceLookup = PrefilteringIndexReader::getSequenceLookup(0, tDbrIdx.index, par.preloadMode);
        }
    }
    BaseMatrix *subMat;
    EvalueComputation * evaluer;
    int8_t * tinySubMat;
    if (Parameters::isEqualDbtype(querySeqType, Parameters::DBTYPE_NUCLEOTIDES)) {
        subMat = new NucleotideMatrix(par.scoringMatrixFile.values.nucleotide().c_str(), 1.0, 0.0);
        evaluer = new EvalueComputation(tdbr->getAminoAcidDBSize(), subMat, par.gapOpen.values.nucleotide(), par.gapExtend.values.nucleotide());
        tinySubMat = new int8_t[subMat->alphabetSize*subMat->alphabetSize];
        for (int i = 0; i < subMat->alphabetSize; i++) {
            for (int j = 0; j < subMat->alphabetSize; j++) {
                tinySubMat[i*subMat->alphabetSize + j] = subMat->subMatrix[i][j];
            }
        }
    } else {
        // keep score bias at 0.0 (improved ROC)
        subMat = new SubstitutionMatrix(par.scoringMatrixFile.values.aminoacid().c_str(), 2.0, 0.0);
        evaluer = new EvalueComputation(tdbr->getAminoAcidDBSize(), subMat, par.gapOpen.values.aminoacid(), par.gapExtend.values.aminoacid());
        tinySubMat = new int8_t[subMat->alphabetSize*subMat->alphabetSize];
        for (int i = 0; i < subMat->alphabetSize; i++) {
            for (int j = 0; j < subMat->alphabetSize; j++) {
                tinySubMat[i*subMat->alphabetSize + j] = subMat->subMatrix[i][j];
            }
        }
    }


    QueryMatcherTaxonomyHook * taxonomyHook = NULL;
    if(par.PARAM_TAXON_LIST.wasSet){
        taxonomyHook = new QueryMatcherTaxonomyHook(par.db2, tdbr, par.taxonList, par.threads);
    }
    if(par.gpu){
#ifdef HAVE_CUDA
        runFilterOnGpu(par, subMat, qdbr, tdbr, sameDB,
                       resultWriter, evaluer, taxonomyHook);
#else
        Debug(Debug::ERROR) << "MMseqs2 was compiled without CUDA support\n";
        EXIT(EXIT_FAILURE);
#endif
    }else{
        runFilterOnCpu(par, subMat, tinySubMat, qdbr, tdbr, sequenceLookup, sameDB,
                   resultWriter, evaluer, taxonomyHook,  mode);
    }

    resultWriter.close();


    if(taxonomyHook != NULL){
        delete taxonomyHook;
    }

    if(sameDB == false){
        delete qDbrIdx;
    }

    delete [] tinySubMat;
    delete subMat;
    delete evaluer;

    return 0;
}

int ungappedprefilter(int argc, const char **argv, const Command &command) {
    return prefilterInternal(argc, argv, command, 0);
}

int gappedprefilter(int argc, const char **argv, const Command &command) {
    return prefilterInternal(argc, argv, command, 1);
}
