#include <fstream>
#include <iostream>
#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <unordered_map>
#include <unordered_set>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100

using namespace std;

struct fileInfo {
    string filename;
    unordered_set<string> hashes;
    int completeNr; // number of unique hashes needed to consider the file complete
    bool complete = false;
    bool wanted = false;
};

void *download_thread_func(void *arg)
{
    int rank = *(int*) arg;

    return NULL;
}

void *upload_thread_func(void *arg)
{
    int rank = *(int*) arg;

    return NULL;
}

void tracker(int numtasks, int rank) {

}

void peer(int numtasks, int rank) {
    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r;

    ifstream inFile;
    string inFilename = "in" + to_string(rank) + ".txt";
    inFile.open(inFilename);

    // cout << "Rank " << rank << " opened file " << inFilename << ".\n";

    pmr::unordered_map<string, fileInfo> myFiles; // file: vector of chunks

    // Files the client has

    int nrFilesOwned;
    inFile >> nrFilesOwned;

    string fileName;
    int nrChunks;
    for (int i = 0; i < nrFilesOwned; i++) {
        inFile >> fileName >> nrChunks;
        cout << "Rank " << rank << " has file " << fileName << " with " << nrChunks << " chunks.\n";
        
        fileInfo info;
        info.filename = fileName;
        info.complete = true;
        info.wanted = false;

        string chunkHash;
        for (int j = 0; j < nrChunks; j++) {
            inFile >> chunkHash;
            info.hashes.insert(chunkHash);
            // cout << "Rank " << rank << " with file " << fileName << " chunk - " << i << " " << chunkHash << ".\n";
        }

        myFiles[fileName] = info;
    }

    // cout << nrFilesOwned << "\n";

    // Files the client wants

    int nrFilesWanted;
    inFile >> nrFilesWanted;

    for (int i = 0; i < nrFilesWanted; i++) {
        inFile >> fileName;
        cout << "Rank " << rank << " wants file " << fileName << ".\n";

        fileInfo info;
        info.filename = fileName;
        info.complete = false;
        info.wanted = true;

        myFiles[fileName] = info;
    }

    inFile.close();

    r = pthread_create(&download_thread, NULL, download_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de upload\n");
        exit(-1);
    }

    r = pthread_join(download_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_join(upload_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de upload\n");
        exit(-1);
    }
}
 
int main (int argc, char *argv[]) {
    int numtasks, rank;
 
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}
