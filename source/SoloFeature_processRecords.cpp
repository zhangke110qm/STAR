#include "SoloFeature.h"
#include "streamFuns.h"
#include "TimeFunctions.h"
#include "SequenceFuns.h"

void SoloFeature::processRecords(ReadAlignChunk **RAchunk)
{
    if (pSolo.type==0)
        return;

    time_t rawTime;
    time(&rawTime);
    P.inOut->logMain << timeMonthDayTime(rawTime) << " ... Starting Solo post-map for " <<pSolo.featureNames[featureType] <<endl;
  
    uint64 nReadsInput=0;
    for (int ii=0; ii<P.runThreadN; ii++) {//point to
        readFeatAll[ii]= RAchunk[ii]->RA->soloRead->readFeat[pSolo.featureInd[featureType]];
        readBarAll[ii] = RAchunk[ii]->RA->soloRead->readBar;
        nReadsInput = max(nReadsInput,RAchunk[ii]->RA->iReadAll);
    };
    ++nReadsInput;
    
    rguStride=2;
    if (pSolo.samAttrYes && pSolo.samAttrFeature==featureType) {
        rguStride=3; //to keep readI column
        readInfo.resize(nReadsInput,{(uint64)-1,(uint32)-1});
        time(&rawTime);
        P.inOut->logMain << timeMonthDayTime(rawTime) << " ... Allocated and initialized readInfo array, nReadsInput = " << nReadsInput <<endl;        
    };

    for (int ii=0; ii<P.runThreadN; ii++) {
        readFeatSum->addCounts(*readFeatAll[ii]);
        readBarSum->addCounts(*readBarAll[ii]);
    };

    if (!pSolo.cbWLyes) {//now we can define WL and counts
        pSolo.cbWLsize=readFeatSum->cbReadCountMap.size();
        pSolo.cbWL.resize(pSolo.cbWLsize);
        pSolo.cbWLstr.resize(pSolo.cbWLsize);
        uint64 ii=0;
        for (auto &cb : readFeatSum->cbReadCountMap) {
            pSolo.cbWL[ii] = cb.first;
            pSolo.cbWLstr[ii] = convertNuclInt64toString(pSolo.cbWL[ii],pSolo.cbL); 
            ii++;
        };
        readFeatSum->cbReadCount = new uint32[pSolo.cbWLsize];
        readBarSum->cbReadCountExact = new uint32[pSolo.cbWLsize];

        uint64 icb=0;
        for (auto ii=readFeatSum->cbReadCountMap.cbegin(); ii!=readFeatSum->cbReadCountMap.cend(); ++ii) {
            pSolo.cbWL[icb]=ii->first;
            readFeatSum->cbReadCount[icb]=ii->second;
            readBarSum->cbReadCountExact[icb]=ii->second;
            ++icb;
        };
    };

    //allocate arrays to store CB/gene/UMIs for all reads
    nCB=0;nReadsMapped=0;
    for (uint32 ii=0; ii<pSolo.cbWLsize; ii++) {
        if (readBarSum->cbReadCountExact[ii]>0) {
            nCB++;
            nReadsMapped += readFeatSum->cbReadCount[ii];
        };
    };

    rGeneUMI = new uint32[rguStride*nReadsMapped]; //big array for all CBs - each element is gene and UMI
    rCBp = new uint32*[nCB+1];
    uint32 **rCBpa = new uint32*[pSolo.cbWLsize+1];
    indCB = new uint32[nCB];

    uint32 nReadPerCBmax=0;
    rCBp[0]=rGeneUMI;
    rCBpa[0]=rGeneUMI;
    nCB=0;//will count it again below
    for (uint32 ii=0; ii<pSolo.cbWLsize; ii++) {
        if (readBarSum->cbReadCountExact[ii]>0) {//if no exact matches, this CB is not present
            indCB[nCB]=ii;
            rCBp[nCB+1] = rCBp[nCB] + rguStride*readFeatSum->cbReadCount[ii];
            ++nCB;
        };
        rCBpa[ii+1]=rCBp[nCB];
    };

    //read and store the CB/gene/UMI from files
    time(&rawTime);
    P.inOut->logMain << timeMonthDayTime(rawTime) << " ... Finished allocating arrays for Solo " << nReadsMapped*rguStride*4.0/1024/1024/1024 <<" GB" <<endl;

    for (int ii=0; ii<P.runThreadN; ii++) {//TODO: this can be parallelized
        readFeatAll[ii]->inputRecords(rCBpa, rguStride, readBarSum->cbReadCountExact, streamTranscriptsOut, readInfo);
    };

    for (uint32 iCB=0; iCB<nCB; iCB++) {
        uint64 nr=(rCBpa[indCB[iCB]]-rCBp[iCB])/rguStride;  //number of reads that were matched to WL, rCBpa accumulated reference to the last element+1
        if (nr>nReadPerCBmax)
            nReadPerCBmax=nr;
        readFeatSum->stats.V[readFeatSum->stats.nMatch] += nr;
    };

    for (int ii=0; ii<P.runThreadN; ii++) {
        readFeatSum->addStats(*readFeatAll[ii]);
        readBarSum->addStats(*readBarAll[ii]);
    };

    time(&rawTime);
    P.inOut->logMain << timeMonthDayTime(rawTime) << " ... Finished reading reads from Solo files nCB="<<nCB <<", nReadPerCBmax="<<nReadPerCBmax;
    P.inOut->logMain <<", nMatch="<<readFeatSum->stats.V[readFeatSum->stats.nMatch]<<endl;
    
    if (featureType==3) {
        streamTranscriptsOut->flush();
        ofstream &outStr=ofstrOpen(P.outFileNamePrefix+pSolo.outFileNames[0]+"transcripts.tsv",ERROR_OUT, P);        
        for (uint32 ii=0; ii<Trans.nTr; ii++)
            outStr << Trans.trID[ii] <<"\t"<< Trans.trLen[ii] <<"\t"<< Trans.geName[Trans.trGene[ii]] << '\n';
        outStr.close();
        return; //not implemented yet
    };
        
    //collapse each CB
    nUMIperCB.resize(nCB);
    nGenePerCB.resize(nCB);
    nReadPerCB.resize(nCB);
    uint32 *umiArray = new uint32[nReadPerCBmax*umiArrayStride];
    
    for (uint32 icb=0; icb<nCB; icb++) {//main collapse cycle
        nGenePerCB[icb] =( rCBpa[indCB[icb]]-rCBp[icb] ) / rguStride; //number of reads that were matched to WL, rCBpa accumulated reference to the last element+1
        
        collapseUMI(rCBp[icb], nGenePerCB[icb], nGenePerCB[icb], nUMIperCB[icb], umiArray,indCB[icb]);
        
        readFeatSum->stats.V[readFeatSum->stats.nUMIs] += nUMIperCB[icb];
        if (nGenePerCB[icb]>0)
            ++readFeatSum->stats.V[readFeatSum->stats.nCellBarcodes];
    };

    time(&rawTime);
    P.inOut->logMain << timeMonthDayTime(rawTime) << " ... Finished collapsing UMIs" <<endl;

    ofstream *statsStream = &ofstrOpen(outputPrefix+pSolo.outFileNames[5],ERROR_OUT, P);
    *statsStream << setw(50)<< "Barcodes:\n";
    readBarSum->statsOut(*statsStream);
    *statsStream << setw(50)<< pSolo.featureNames[featureType] <<":\n";
    readFeatSum->statsOut(*statsStream);
    statsStream->flush();
    
    //output nU per gene per CB
    outputResults(false); //unfiltered
    
    if (pSolo.cellFilter.type[0]!="None") {
        cellFiltering();
        outputResults(true);
    };
    
    //summary stats output
    statsOutput();

};