/*
 * GPUMotifCalculator.cpp
 *
 *  Created on: Dec 2, 2018
 *      Author: ori
 */

#include "../includes/GPUMotifCalculator.h"
#include "../includes/MotifVariationConstants.h"

#define gpuErrchk(ans) { gpuAssert((ans), __FILE__, __LINE__); }
inline void gpuAssert(cudaError_t code, const char *file, int line, bool abort =
true) {
    if (code != cudaSuccess) {
        fprintf(stderr, "GPUassert: %s %s %d\n", cudaGetErrorString(code), file,
                line);
        if (abort)
            exit(code);
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////   Global managed variables   ////////////////////////////////////////////////
__managed__ unsigned int globalNumOfNodes;
__managed__ unsigned int globalNumOfMotifs;
__managed__ unsigned int globalNumOfEdges;

__managed__ bool globalDirected;

// DEVICE VARIABLES

//Pointers to the device vectors declared above - no need to delete as the device_vectors live on the stack
__managed__ unsigned int* globalDevicePointerMotifVariations;
__managed__ unsigned int* globalDevicePointerRemovalIndex;
__managed__ unsigned int* globalDevicePointerSortedNodesByDegree;

// For the original graph
__managed__ int64* globalDeviceOriginalGraphOffsets;
__managed__ unsigned int* globalDeviceOriginalGraphNeighbors;

// For the full graph
__managed__ int64* globalDeviceFullGraphOffsets;
__managed__ unsigned int* globalDeviceFullGraphNeighbors;

// Feature array
__managed__ unsigned int* globalDeviceFeatures;

/////////////////////////////   END Global managed variables   /////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////////

void GPUMotifCalculator::init() {
    CacheGraph inverse(true);
    mGraph->InverseGraph(inverse);
    mGraph->CureateUndirectedGraph(inverse, fullGraph);
    this->numOfNodes = this->mGraph->GetNumberOfNodes();
    this->numOfEdges = this->mGraph->GetNumberOfEdges();
    //std::cout << "Load variations" << std::endl;
    this->LoadMotifVariations(level, directed);
    //std::cout << "All motifs" << std::endl;
    this->SetAllMotifs();
    //std::cout << "Sorted Nodes" << std::endl;
    this->SetSortedNodes();
    //std::cout << "Removal Index" << std::endl;
    this->SetRemovalIndex();
    //std::cout << "Feature counters" << std::endl;
    this->InitFeatureCounters();
    //std::cout << "Copy to GPU" << std::endl;
    this->CopyAllToDevice();
    //std::cout << "Done" << std::endl;
    //std::cout << this->removalIndex->size() << std::endl;
}

GPUMotifCalculator::GPUMotifCalculator(int level, bool directed, int cudaDevice) :
        directed(directed), nodeVariations(NULL), allMotifs(NULL), removalIndex(
        NULL), sortedNodesByDegree(NULL), fullGraph(false), numOfMotifs(0), deviceFeatures(
        NULL), cudaDevice(cudaDevice) {
    //check level
    if (level != 3 && level != 4)
        throw invalid_argument("Level must be 3 or 4");
    this->level = level;
    this->features = new std::vector<vector<unsigned int> *>;
}

void GPUMotifCalculator::InitFeatureCounters() {
    for (int node = 0; node < numOfNodes; node++) {
        vector<unsigned int> *motifCounter = new vector<unsigned int>;
        std::set<int> s(this->allMotifs->begin(), this->allMotifs->end());
        this->numOfMotifs = s.size() - 1;
        for (auto motif : s)
            if (motif != -1)
                //				(*motifCounter)[motif] = 0;
                motifCounter->push_back(0);

        features->push_back(motifCounter);
    }
    delete this->allMotifs;
}

void GPUMotifCalculator::LoadMotifVariations(int level, bool directed) {

    //
    //	string suffix;
    //	if (directed)
    //		suffix = "_directed_cpp.txt";
    //	else
    //		suffix = "_undirected_cpp.txt";
    //
    //	string fileName = MOTIF_VARIATIONS_PATH + "/" + std::to_string(level)
    //			+ suffix;
    //	std::ifstream infile(fileName);
    const char* motifVariations[4] = { undirected3, directed3, undirected4,
                                       directed4 };
    const int numOfMotifsOptions[4] = { 8, 64, 64, 4096 };

    int variationIndex = 2 * (level - 3) + (directed ? 1 : 0);
    this->nodeVariations = new std::vector<unsigned int>(
            numOfMotifsOptions[variationIndex]);
    std::istringstream f(motifVariations[variationIndex]);
    std::string line;
    std::string a, b;
    while (getline(f, line)) {
        int x, y;
        int n = line.find(" ");
        a = line.substr(0, n);
        b = line.substr(n);
        try {
            x = stoi(a);
            y = stoi(b);
        } catch (exception &e) {
            y = -1;
        }
        //		cout << line << endl;
        //		cout << x << ":" << y << endl;

        (*nodeVariations)[x] = y;
    }
}

void GPUMotifCalculator::SetAllMotifs() {
    this->allMotifs = new std::vector<int>();

    for (const auto &x : *(this->nodeVariations))
        this->allMotifs->push_back(x);
}

void GPUMotifCalculator::SetSortedNodes() {
    this->sortedNodesByDegree = mGraph->SortedNodesByDegree();
}
/**
 * We iterate over the list of sorted nodes.
 * If node p is in index i in the list, it means that it will be removed after the i-th iteration,
 * or conversely that it is considered "not in the graph" from iteration i+1 onwards.
 * This means that any node with a removal index of j is not considered from iteration j+1.
 * (The check is (removalIndex[node] > currentIteration).
 */
void GPUMotifCalculator::SetRemovalIndex() {
    this->removalIndex = new std::vector<unsigned int>();
    for (int i = 0; i < numOfNodes; i++) {
        removalIndex->push_back(0);
    }
    for (unsigned int index = 0; index < numOfNodes; index++) {
        auto node = sortedNodesByDegree->at(index);
        removalIndex->at(node) = index;
    }
}

void GPUMotifCalculator::CopyAllToDevice() {
    //	thrust::device_vector<unsigned int> deviceMotifVariations; // @suppress("Type cannot be resolved")// @suppress("Symbol is not resolved")
    //	thrust::device_vector<unsigned int> deviceRemovalIndex; // @suppress("Type cannot be resolved") // @suppress("Symbol is not resolved")
    //	thrust::device_vector<unsigned int> deviceSortedNodesByDegree;// @suppress("Type cannot be resolved") // @suppress("Symbol is not resolved")

    /*
     * 1) Allocate unified memory
     * 2) Copy vectors to the memory
     * 3) delete the memory in d'tor
     */

    //	deviceMotifVariations = *(this->nodeVariations);
    //	this->devicePointerMotifVariations = thrust::raw_pointer_cast(&deviceMotifVariations[0]);
//	int i = 0;
//	//std::cout << "Checker: " << i++ << std::endl;
    gpuErrchk(
            cudaDeviceSetLimit(cudaLimitMallocHeapSize,
                               size_t(10) * size_t(numOfNodes) * size_t(numOfNodes)
                               * sizeof(int64)));
    size_t currentLimit;
    gpuErrchk(cudaDeviceGetLimit(&currentLimit, cudaLimitMallocHeapSize));

    //std::cout << "Current limit is: " << currentLimit << std::endl;
    gpuErrchk(
            cudaMallocManaged(&(this->devicePointerMotifVariations),
                              nodeVariations->size() * sizeof(unsigned int)));
    //	//std::cout << "between"<<std::endl;

    //	//std::cout << this->nodeVariations->data()[0] <<std::endl;

    //	//std::cout << this->nodeVariations->size() <<std::endl;

    std::memcpy(this->devicePointerMotifVariations,
                &((*(this->nodeVariations))[0]),
                nodeVariations->size() * sizeof(unsigned int));
    // Removal index
    //	deviceRemovalIndex = *(this->removalIndex);
    //	//std::cout << "Checker: " << i++ << std::endl;
    gpuErrchk(
            cudaMallocManaged(&(this->devicePointerRemovalIndex),
                              removalIndex->size() * sizeof(unsigned int)));
    std::memcpy(this->devicePointerRemovalIndex, this->removalIndex->data(),
                removalIndex->size() * sizeof(unsigned int));
    //Sorted nodes
    //	deviceSortedNodesByDegree = *(this->sortedNodesByDegree);
    //	this->devicePointerSortedNodesByDegree = thrust::raw_pointer_cast(&deviceSortedNodesByDegree[0]);
    //	//std::cout << "Checker: " << i++ << std::endl;
    gpuErrchk(
            cudaMallocManaged(&(this->devicePointerSortedNodesByDegree),
                              sortedNodesByDegree->size() * sizeof(unsigned int)));
    std::memcpy(this->devicePointerSortedNodesByDegree,
                this->sortedNodesByDegree->data(),
                sortedNodesByDegree->size() * sizeof(unsigned int));

    // Feature matrix
    //	//std::cout << "Checker: " << i++ << std::endl;
    //std::cout << "Num of Nodes:" << this->numOfNodes << std::endl;
    //std::cout << "Num of node variations: " << this->nodeVariations->size()
    //<< std::endl;
    unsigned int size = this->numOfNodes * this->nodeVariations->size()
                        * sizeof(unsigned int);
//	//std::cout << "between" << std::endl;
    gpuErrchk(cudaMallocManaged(&(this->deviceFeatures), size));

    // Original graph
    //	//std::cout << "Checker: " << i++ << std::endl;
    gpuErrchk(
            cudaMallocManaged(&deviceOriginalGraphOffsets,
                              (this->numOfNodes + 1) * sizeof(int64)));
    //	//std::cout << "Checker: " << i++ << std::endl;
    gpuErrchk(
            cudaMallocManaged(&deviceOriginalGraphNeighbors,
                              (this->numOfEdges) * sizeof(unsigned int)));
    //	//std::cout << "Checker: " << i++ << std::endl;
    std::memcpy(deviceOriginalGraphOffsets, this->mGraph->GetOffsetList(),
                (this->numOfNodes + 1) * sizeof(int64));
    //	//std::cout << "Checker: " << i++ << std::endl;
    std::memcpy(deviceOriginalGraphNeighbors, this->mGraph->GetNeighborList(),
                (this->numOfEdges) * sizeof(unsigned int));

    // Full graph
    //	//std::cout << "Checker: " << i++ << std::endl;
    gpuErrchk(
            cudaMallocManaged(&deviceFullGraphOffsets,
                              (this->fullGraph.GetNumberOfNodes() + 1) * sizeof(int64)));
    //	//std::cout << "Checker: " << i++ << std::endl;
    gpuErrchk(
            cudaMallocManaged(&deviceFullGraphNeighbors,
                              (this->fullGraph.GetNumberOfEdges())
                              * sizeof(unsigned int)));

    //	//std::cout << "Checker: " << i++ << std::endl;
    std::memcpy(deviceFullGraphOffsets, this->fullGraph.GetOffsetList(),
                (this->fullGraph.GetNumberOfNodes() + 1) * sizeof(int64));
    //	//std::cout << "Checker: " << i++ << std::endl;
    std::memcpy(deviceFullGraphNeighbors, this->fullGraph.GetNeighborList(),
                (this->fullGraph.GetNumberOfEdges()) * sizeof(unsigned int));

    //	//std::cout << "Checker: " << i++ << std::endl;

    //Assign to global variables

    globalNumOfNodes = this->numOfNodes;
    globalNumOfMotifs = this->numOfMotifs;
    globalNumOfEdges = this->numOfEdges;
    globalDirected = this->directed;

    globalDevicePointerMotifVariations = this->devicePointerMotifVariations;
    globalDevicePointerRemovalIndex = this->devicePointerRemovalIndex;
    globalDevicePointerSortedNodesByDegree =
            this->devicePointerSortedNodesByDegree;

    globalDeviceOriginalGraphOffsets = this->deviceOriginalGraphOffsets;
    globalDeviceOriginalGraphNeighbors = this->deviceOriginalGraphNeighbors;
    globalDeviceFullGraphOffsets = this->deviceFullGraphOffsets;
    globalDeviceFullGraphNeighbors = this->deviceFullGraphNeighbors;
    globalDeviceFeatures = this->deviceFeatures;

}

// Kernel and friend functions
__global__
void Motif3Kernel() {
    //GPU allocations
    int index = blockIdx.x * blockDim.x + threadIdx.x;
    int stride = blockDim.x * gridDim.x;
    auto n = globalNumOfNodes;

    for (int i = index; i < n; i += stride){
        // Ascending order - Takes a shorter time than Descending
//	for (int i = n - index - 1; i >= 0 ; i -= stride){
        Motif3Subtree(globalDevicePointerSortedNodesByDegree[i]);
    }
}
__global__
void Motif4Kernel() {
    //GPU allocations
    int index = blockIdx.x * blockDim.x + threadIdx.x;
    int stride = blockDim.x * gridDim.x;
    auto n = globalNumOfNodes;
    // Descending order
    for (int i = index; i < n; i += stride){
        // Ascending order - Takes a shorter time than Descending
//	for (int i = n - index - 1; i >= 0 ; i -= stride){
        Motif4Subtree(globalDevicePointerSortedNodesByDegree[i]);
    }
}

vector<vector<unsigned int> *> *GPUMotifCalculator::Calculate() {

    int blockSize = 256;
    int numBlocks = (this->numOfNodes + blockSize - 1) / blockSize;

    //Prefetch all relevant memory
    /*
     globalDevicePointerMotifVariations = this->devicePointerMotifVariations;
     globalDevicePointerRemovalIndex = this->devicePointerRemovalIndex;
     globalDevicePointerSortedNodesByDegree = this -> devicePointerSortedNodesByDegree;
     globalDeviceOriginalGraphOffsets = this->deviceOriginalGraphOffsets;
     globalDeviceOriginalGraphNeighbors = this -> deviceOriginalGraphNeighbors;
     globalDeviceFullGraphOffsets = this->deviceFullGraphOffsets;
     globalDeviceFullGraphNeighbors = this->deviceFullGraphNeighbors;
     globalDeviceFeatures = this->deviceFeatures;
     */
    cudaSetDevice(this->cudaDevice);
    int device = -1;
    cudaGetDevice(&device);

    int offsetSize = this->numOfNodes + 1;
    int neighborSize = this->numOfEdges;

    cudaMemPrefetchAsync(globalDevicePointerMotifVariations,
                         nodeVariations->size() * sizeof(unsigned int), device, NULL);
    cudaMemPrefetchAsync(globalDevicePointerRemovalIndex,
                         this->numOfNodes * sizeof(unsigned int), device, NULL);
    cudaMemPrefetchAsync(globalDevicePointerSortedNodesByDegree,
                         this->numOfNodes * sizeof(unsigned int), device, NULL);
    cudaMemPrefetchAsync(globalDeviceOriginalGraphOffsets,
                         offsetSize * sizeof(int64), device, NULL);
    cudaMemPrefetchAsync(globalDeviceOriginalGraphNeighbors,
                         neighborSize * sizeof(unsigned int), device, NULL);
    cudaMemPrefetchAsync(globalDeviceFullGraphOffsets,
                         offsetSize * sizeof(int64), device, NULL);
    cudaMemPrefetchAsync(globalDeviceFullGraphNeighbors,
                         neighborSize * sizeof(unsigned int), device, NULL);
    cudaMemPrefetchAsync(globalDeviceFeatures,
                         (this->numOfNodes * this->nodeVariations->size())
                         * sizeof(unsigned int), device, NULL);

    if (this->level == 3) {
        std::cout << "Num of motifs: "<<globalNumOfMotifs <<std::endl;
        Motif3Kernel<<<numBlocks, blockSize>>>();
    } else {
        Motif4Kernel<<<numBlocks, blockSize>>>();
    }
    gpuErrchk(cudaPeekAtLastError());
    gpuErrchk(cudaDeviceSynchronize());
    for (int node = 0; node < this->numOfNodes; node++) {
        for (int motif = 0; motif < this->numOfMotifs; motif++) {
            this->features->at(node)->at(motif) = globalDeviceFeatures[motif
                                                                       + this->numOfMotifs * node];
        }
    }
    return this->features;
}

/*  3-Motif building algorithm
 3-motifs can only be of depth 1 or 2.
 we count the motifs in the following order:
1. Motifs of depth 2; then
2. Motifs of depth 1
*/
__device__
void Motif3Subtree(unsigned int root) {

    /* Since cashGraph cant be modified at runtime, we hold Removal Index,
    which will be the first iteration where the node no longer exists in the graph.
    we will verify that the removal index of a node is lower than the removal index of the root of the current subtree.
    root_idx is also our current iteration */
    int idx_root = globalDevicePointerRemovalIndex[root];
    // all neighbors - ancestors and descendants
    const unsigned int *neighbors = globalDeviceFullGraphNeighbors;
    //offsets of the neighbors graph
    const int64 *offsets = globalDeviceFullGraphOffsets;

    // CASE 1: MOTIF 3 OF DEPTH 2
    //Loop on the first neighbors
    for (int64 n1_idx = offsets[root]; n1_idx < offsets[root + 1]; n1_idx++) {
        unsigned int n1 = neighbors[n1_idx];
        //n1 already handled
        if (globalDevicePointerRemovalIndex[n1] <= idx_root)
            continue;
        //loop on second neighbors
        for (int64 n2_idx = offsets[n1]; n2_idx < offsets[n1 + 1]; n2_idx++) {
            unsigned int n2 = neighbors[n2_idx];
            //n2 already handled
            if (globalDevicePointerRemovalIndex[n2] <= idx_root)
                continue;
            bool edgeExists = AreNeighbors(root,n2) || AreNeighbors(n2,root);
            //if n2 is actually a first neighbor of root
            if (edgeExists) {
                //n2 is after n1 (stops counting the motif twice)
                if (edgeExists && n1 < n2) {
                    unsigned int arr[] = { root, n1, n2 };
                    //update motif counter [r,n1,n2]
                    GroupUpdater(arr, 3);
                }
            } else {
                //     visited_vertices[n2] = true;
                unsigned int arr[] = { root, n1, n2 };
                // update motif counter [r,n1,n2]
                GroupUpdater(arr, 3);
            }										   // end ELSE
        }											// end LOOP_SECOND_NEIGHBORS

    } // end LOOP_FIRST_NEIGHBORS


    // CASE 2: MOTIF 3 OF DEPTH 1
    // All 2-combinations of root's neighbors
    for (int64 i = offsets[root]; i < offsets[root + 1]; i++) {
        for (int64 j = i + 1; j < offsets[root + 1]; j++) {
            //combination
            unsigned int n1 = neighbors[i];
            unsigned int n2 = neighbors[j];
            // motif already handled
            if (globalDevicePointerRemovalIndex[n1] <= idx_root
                || globalDevicePointerRemovalIndex[n2] <= idx_root)
                continue;
            // check n1, n2 not neighbors, and check n1 < n2 to avoid double counting
            if ((n1 < n2) && !(AreNeighbors(n1, n2) || AreNeighbors(n2, n1))) {
                unsigned int arr[] = { root, n1, n2 };
                // update motif counter [r,n1,n2]
                GroupUpdater(arr, 3);
            }
        }
    } // end loop COMBINATIONS_NEIGHBORS_N1
}


/*  4-Motif building algorithm
 4-motifs can be of depth 1, 2 or 3.
1. Motifs of depth 1
2. Motifs of depth 2 can be one of the following:
-  Motifs of the form root, two first neighbors and a second neighbor
-  Motifs of the form root, a first neighbors and two second neighbors
3. Motifs of depth 3.  can only be a chain of the root, and a first, second and third neighbors.
*/
__device__
void Motif4Subtree(unsigned int root) {

    // Same as motif3, idx_root is also our current iteration
    int idx_root = globalDevicePointerRemovalIndex[root];
    // all neighbors - ancestors and descendants
    const unsigned int *neighbors = globalDeviceFullGraphNeighbors;
    //offsets of the neighbors graph
    const int64 *offsets = globalDeviceFullGraphOffsets;

    /* --Motif depth one Builder-- */
    int64 end = offsets[root + 1];
    // Loop on All 3 combinations of root's neighbors
    for (int64 i = offsets[root]; i < end; i++) {
        for (int64 j = i + 1; j < end; j++) {
            if (j == end - 1) //if j is the last element, we can't add an element and therefore it's not a 3-combination
                continue;
            for (int64 k = j + 1; k < end; k++) {
                // 3 combinations of root's neighbors
                unsigned int n11 = neighbors[i];
                unsigned int n12 = neighbors[j];
                unsigned int n13 = neighbors[k];
                // In this case, the motif already handled
                if (globalDevicePointerRemovalIndex[n11] <= idx_root
                    || globalDevicePointerRemovalIndex[n12] <= idx_root
                    || globalDevicePointerRemovalIndex[n13] <= idx_root)
                    continue;
                unsigned int arr[] = { root, n11, n12, n13 };
                // update motif counter [r,n11,n12,n13]
                GroupUpdater(arr, 4);
            }
        }
    }

    // All other cases
    /* --Motif depth 2 Builder */
    for (int64 n1_idx = offsets[root]; n1_idx < offsets[root + 1]; n1_idx++) { // loop first neighbors
        unsigned int n1 = neighbors[n1_idx];
        if (globalDevicePointerRemovalIndex[n1] <= idx_root) // n1 already handled
            continue;


        /* Motifs of the form root, two first neighbors and a second neighbor (root-n1-n2-n11) */
        for (int64 n2_idx = offsets[n1]; n2_idx < offsets[n1 + 1]; n2_idx++) { // loop second neighbors (again)
            unsigned int n2 = neighbors[n2_idx];
            if (globalDevicePointerRemovalIndex[n2] <= idx_root) // n2 already handled
                continue;
            for (int64 n11_idx = offsets[root]; n11_idx < offsets[root + 1];
                 n11_idx++) { // loop first neighbors
                unsigned int n11 = neighbors[n11_idx];
                if (globalDevicePointerRemovalIndex[n11] <= idx_root) // n2 already handled
                    continue;
                /* If there is an edge between n1 and n11, we would have already counted the motif as a motif of depth 2.
                 If no such edge exists, we only want to count the motif once.  */
                if (!AreNeighbors(root,n2) && n11 != n1) {
                    bool edgeExists = AreNeighbors(n2, n11)
                                      || AreNeighbors(n11, n2);
                    if (!edgeExists || (edgeExists && n1 < n11)) {
                        unsigned int arr[] = { root, n1, n11, n2 };
                        // update motif counter [r,n1,n11,n2]
                        GroupUpdater(arr, 4);
                    }												// end if
                }
            }								// end loop INNER FIRST NEIGHBORS
        }									// end loop SECOND NEIGHBORS AGAIN

        /* Motifs of the form root, a first neighbors and two second neighbors - The case of root-n1-n21-n22
         2-combinations on second neighbors */
        end = offsets[n1 + 1];
        for (int64 i = offsets[n1]; i < end; i++) {
            for (int64 j = i + 1; j < end; j++) {

                unsigned int n21 = neighbors[i];
                unsigned int n22 = neighbors[j];
                // In this case - motif already handled
                if (globalDevicePointerRemovalIndex[n21] <= idx_root
                    || globalDevicePointerRemovalIndex[n22] <= idx_root)
                    continue;
                 // If both nodes are not neighbors of root (were seen at level 2) then the motif wasn't counted as a depth 1 motif
                if (!AreNeighbors(root,n21) && !AreNeighbors(n21,root) && !AreNeighbors(root,n22) &&
                    !AreNeighbors(n22,root) && (root!=n22) && (root!=n21) ) {
                    unsigned int arr[] = { root, n1, n21, n22 };
                    GroupUpdater(arr, 4); // update motif counter [r,n1,n21,n22]
                }
            }
        } // end loop SECOND NEIGHBOR COMBINATIONS
    }

    /* Motif 4 depth 3 builder - The case of n1-n2-n3 */
    for (int64 n1_idx = offsets[root]; n1_idx < offsets[root + 1]; n1_idx++) { // loop first neighbors
        unsigned int n1 = neighbors[n1_idx];
        // n1 already handled
        if (globalDevicePointerRemovalIndex[n1] <= idx_root)
            continue;
        for (int64 n2_idx = offsets[n1]; n2_idx < offsets[n1 + 1]; n2_idx++) { // loop second neighbors (third time's the charm)
            unsigned int n2 = neighbors[n2_idx];
            if (globalDevicePointerRemovalIndex[n2] <= idx_root) // n2 already handled
                continue;

            /* According to the rules we proved, check : */
            // n2 is not a first neighbor
            if(AreNeighbors(root,n2) && root!=n2) {
                continue;
            }

            // loop third neighbors
            for (int64 n3_idx = offsets[n2]; n3_idx < offsets[n2 + 1];
                 n3_idx++) {
                unsigned int n3 = neighbors[n3_idx];
                // n2 already handled
                if (globalDevicePointerRemovalIndex[n3] <= idx_root)
                    continue;

                    /* According to the proved rules, check that n3 is not a first neighbor and also not a neighbor of the first neighbor n1.
                    if so, continue */
                    if ((AreNeighbors(root,n3) || (AreNeighbors(n3,root))) || (AreNeighbors(n1,n3) || (AreNeighbors(n3,n1) )) || (root==n3) ) {
                        continue;
                    }
                    unsigned int arr[] = { root, n1, n2, n3 };
                    GroupUpdater(arr, 4);
            }									// end loop THIRD NEIGHBORS
        }				// end loop SECOND NEIGHBORS THIRD TIME'S THE CHARM

    } // end loop FIRST NEIGHBORS

}

__device__
bool AreNeighbors(unsigned int p, unsigned int q) {
    // int64* deviceOriginalGraphOffsets;
    // unsigned int* deviceOriginalGraphNeighbors;
    int first = globalDeviceOriginalGraphOffsets[p],//first array element
    last = globalDeviceOriginalGraphOffsets[p + 1] - 1,	//last array element
    middle;		//mid point of search

    while (first <= last) {
        middle = (int)(first + last) / 2; //this finds the mid point
        ////std::cout << "Binary search: " << middle << std::endl;
        //TODO: fix overflow problem
        if (globalDeviceOriginalGraphNeighbors[middle] == q) {
            return true;
        } else if (globalDeviceOriginalGraphNeighbors[middle] < q)
        {
            first = middle + 1;      //if it's in the upper half

        } else {
            last = middle - 1; // if it's in the lower half
        }
    }
    return false;  // not found

}

__device__
void GroupUpdater(unsigned int group[], int size) {
    // TODO: count overall number of motifs in graph (maybe different class)?
    //printf("In GroupUpdater");

    int groupNumber = GetGroupNumber(group, size);
    int motifNumber = (globalDevicePointerMotifVariations)[groupNumber];
//	if(size==3){
    //std::cout << motifNumber <<","<<group[0]<<","<<group[1]<<","<<group[2]<<std::endl;
//		printf("%d,%u,%u,%u\n",motifNumber,group[0],group[1],group[2]);
//	}

    if (motifNumber != -1) {
        //	printf("Found motif!\n");
        for (int i = 0; i < size; i++)
            atomicAdd(
                    globalDeviceFeatures
                    + (motifNumber + globalNumOfMotifs * group[i]), 1);	//atomic add + access as 1D array : features[motif + M*node] // @suppress("Function cannot be resolved")
        // where M is the number of motifs
    }
}

__device__
int GetGroupNumber(unsigned int group[], int size) {
    int sum = 0;
    int power = 1;
    bool hasEdge;
    if (globalDirected) {
        // Use permutations
        for (int i = 0; i < size; i++) {
            for (int j = 0; j < size; j++) {
                if (i != j) {
                    hasEdge = AreNeighbors(group[i], group[j]);
                    if (hasEdge)
                        sum += power;
                    power *= 2;
                }
            }
        }
    } else {
        // Use combinations
        for (int i = 0; i < size; i++) {
            for (int j = i + 1; j < size; j++) {

                hasEdge = AreNeighbors(group[i], group[j]);
                if (hasEdge)
                    sum += power;
                power *= 2;
            }
        }

    }
    return sum;
}

GPUMotifCalculator::~GPUMotifCalculator() {
    //map the group num to the iso motif
    delete nodeVariations;
    //the index in which we remove the node from the graph. Basically, from this index on the node doesen't exist.
    delete removalIndex;
    //the nodes, sorted in descending order by the degree.
    delete sortedNodesByDegree;

    // Memory resources

    cudaFree(devicePointerMotifVariations);
    cudaFree(devicePointerRemovalIndex);
    cudaFree(devicePointerSortedNodesByDegree);
    // For the original graph
    cudaFree(deviceOriginalGraphOffsets); // @suppress("Function cannot be resolved")
    cudaFree(deviceOriginalGraphNeighbors); // @suppress("Function cannot be resolved")

    // For the full graph
    cudaFree(deviceFullGraphOffsets); // @suppress("Function cannot be resolved")
    cudaFree(deviceFullGraphNeighbors); // @suppress("Function cannot be resolved")

    // Feature array
    cudaFree(deviceFeatures); // @suppress("Function cannot be resolved")
}