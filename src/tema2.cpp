#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mpi.h>
#include <pthread.h>
#include <set>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <unordered_map>
#include <vector>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100

using namespace std;

struct hashInfo {
	char hash[HASH_SIZE];
	bool owned = false;
	int id = -1; // the # of the segment (they need to be in order)
};

struct fileInfo {
	char filename[MAX_FILENAME];
	vector<hashInfo> hashes; // the hash info of the segments of this file
	int segmentsNr;			 // the number of segments we have
	int completeNr;			 // number of segments needed to consider the file complete

	bool complete = false; // only used by client
	bool wanted = false;   // same

	set<int> owners; // ids of clients that hold pieces of that file - this is only used by tracker
};

MPI_Status mpiStatus;

pmr::unordered_map<string, fileInfo> myFiles;
int nrFilesWanted;

bool finished = false;

char filename[MAX_FILENAME];

void *download_thread_func(void *arg) {
	int rank = *(int *)arg;

	while (nrFilesWanted > 0) {
		// download x10, sync, repeat
		nrFilesWanted--;
		filename[0] = '\0';
		MPI_Send(&filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);
	}

	// once all wanted files are downloaded this thread closes

	return NULL;
}

void *upload_thread_func(void *arg) {
	int rank = *(int *)arg;

	while (true) {
		MPI_Recv(&filename, MAX_FILENAME, MPI_CHAR, MPI_ANY_SOURCE, 1, MPI_COMM_WORLD, &mpiStatus);

		if (mpiStatus.MPI_SOURCE == TRACKER_RANK) {
			break;
		}
	}

	return NULL;
}

void tracker(int numtasks, int rank) {
	pmr::unordered_map<string, fileInfo> trackerFiles; // information for the tracker

	// Receive data from clients (who is seeding what)

	for (int i = 1; i < numtasks; i++) {
		int filesOwned;

		MPI_Recv(&filesOwned, 1, MPI_INT, i, 0, MPI_COMM_WORLD, &mpiStatus);

		printf("[TRACKER] Rank %d has %d files.\n", mpiStatus.MPI_SOURCE, filesOwned);

		// Get all files + hashes to note in the tracker database.
		for (int j = 0; j < filesOwned; j++) {
			fileInfo clientFile;

			clientFile.filename[0] = '\0';
			clientFile.completeNr = -1;
			clientFile.segmentsNr = -1; // doesn't matter

			MPI_Recv(clientFile.filename, MAX_FILENAME, MPI_CHAR, i, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
			MPI_Recv(&clientFile.completeNr, 1, MPI_INT, i, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

			printf("[TRACKER] Rank %d has file \"%s\" (%d/%d) with %d chunks.\n", i, clientFile.filename, j + 1, filesOwned, clientFile.completeNr);

			// We receive a flattened array and then reconstruct it to minimize overhead

			// completeNr * 32 = nr of chars needed to be received for this file
			vector<char> buffer(clientFile.completeNr * HASH_SIZE);
			MPI_Recv(buffer.data(), clientFile.completeNr * HASH_SIZE, MPI_CHAR, i, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

			// reconstruct into hashes (strings)
			for (int k = 0; k < clientFile.completeNr; k++) {
				string hash = string(&buffer[k * HASH_SIZE], HASH_SIZE);

				hashInfo info;
				strncpy(info.hash, hash.c_str(), HASH_SIZE);
				info.id = k;
				info.owned = false; // tracker ofc does not own this

				clientFile.hashes.push_back(info);
			}

			// for (int k = 0; k < clientFile.completeNr; k++) {
			// 	printf("[TRACKER] %d - %s\n", k, clientFile.hashes[k].hash);
			// }

			if (trackerFiles.find(clientFile.filename) == trackerFiles.end()) {
				trackerFiles[clientFile.filename] = clientFile;
			}

			trackerFiles[clientFile.filename].owners.insert(i);
		}
	}

	for (auto &pair : trackerFiles) {
		cout << "[TRACKER] File " << pair.first << " is owned by ";
		for (auto &item : pair.second.owners) {
			cout << item << " ";
		}
		cout << endl;
	}

	// Receive wanted files from clients

	int wantedNr = 0; // we use this to know when to close the swarm

	/*	Some comments about the lone integer as opposed to a more detailed hashmap
		usually a more "specific" spread of information would be better
		ex: clients 1 2 may only want each other's files (noone wants theirs) and can be shutdown individually to save power
		but in this case all the tests are pretty homogenous (everyone wants a piece of everyone, more or less)
		so this simpler way aids us more - once this nr reaches 0 again we can shut down the whole cluster
	*/

	for (int i = 1; i < numtasks; i++) {
		int wanted;
		MPI_Recv(&wanted, 1, MPI_INT, i, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

		wantedNr += wanted;
	}

	printf("[TRACKER] There are %d wanted files across the whole cluster.\n", wantedNr);

	// Go signal
	for (int i = 1; i < numtasks; i++) {
		MPI_Send(NULL, 0, MPI_INT, i, 0, MPI_COMM_WORLD);
	}

	// While loop to check for requests
	while (1) {
		char filename[FILENAME_MAX];

		MPI_Recv(&filename, FILENAME_MAX, MPI_CHAR, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &mpiStatus);

		// Deduce request type by tag
		switch (mpiStatus.MPI_TAG) {
			case 0: // client finished a download (got a wanted file)
				wantedNr--; // one more file

				printf("[TRACKER] Client %d got 1 more file. (%d left)\n", mpiStatus.MPI_SOURCE, wantedNr);

				// send kill message to all once all tasks are done
				if (wantedNr == 0) {
					for (int i = 1; i < numtasks; i++) {
						MPI_Send(filename, MAX_FILENAME, MPI_CHAR, i, 1, MPI_COMM_WORLD);
					}

					// Finish
					return;
				}
				break;
			case 1:
				break;
		}
	}
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

	// Files the client has

	int nrFilesOwned;
	inFile >> nrFilesOwned;

	string fileName;
	int nrChunks;
	for (int i = 0; i < nrFilesOwned; i++) {
		inFile >> fileName >> nrChunks;
		cout << "Rank " << rank << " has file " << fileName << " with " << nrChunks << " chunks.\n";

		fileInfo info;
		strcpy(info.filename, fileName.c_str());
		info.completeNr = nrChunks;
		info.segmentsNr = nrChunks; // doesn't matter because we don't want the file
		info.complete = true;
		info.wanted = false;

		string chunkHash;
		for (int j = 0; j < nrChunks; j++) {
			inFile >> chunkHash;

			hashInfo ownedHash;
			strncpy(ownedHash.hash, chunkHash.c_str(), HASH_SIZE);
			ownedHash.owned = true;
			ownedHash.id = j;

			info.hashes.push_back(ownedHash);
			// cout << "Rank " << rank << " with file \"" << fileName << "\" chunk - #" << j << " " << ownedHash.hash << "\n";
		}

		myFiles[fileName] = info;
	}

	// Files the client wants

	inFile >> nrFilesWanted;

	for (int i = 0; i < nrFilesWanted; i++) {
		inFile >> fileName;
		cout << "Rank " << rank << " wants file \"" << fileName << "\".\n";

		fileInfo info;
		strcpy(info.filename, fileName.c_str());
		info.complete = false;
		info.segmentsNr = 0;	  // we want it and we have no segments
		info.completeNr = 999999; // we do not know how many segments are needed
		info.wanted = true;

		myFiles[fileName] = info;
	}

	inFile.close();

	// Send data to tracker

	MPI_Send(&nrFilesOwned, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

	for (const auto &pair : myFiles) {
		fileInfo myInfo = pair.second; // The value (fileInfo object)

		if (myInfo.complete) {
			MPI_Send(myInfo.filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);
			MPI_Send(&myInfo.completeNr, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

			// We send a flattened array to minimize overhead

			std::vector<char> buffer(myInfo.completeNr * HASH_SIZE, '\0');

			for (int i = 0; i < myInfo.completeNr; i++) {
				std::strncpy(&buffer[i * HASH_SIZE], myInfo.hashes[i].hash, HASH_SIZE);
			}

			MPI_Send(buffer.data(), myInfo.completeNr * HASH_SIZE, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);
		}
	}

	MPI_Send(&nrFilesWanted, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

	// Await OK signal (no data)

	MPI_Recv(NULL, 0, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

	r = pthread_create(&download_thread, NULL, download_thread_func, (void *)&rank);
	if (r) {
		printf("Eroare la crearea thread-ului de download\n");
		exit(-1);
	}

	r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *)&rank);
	if (r) {
		printf("Eroare la crearea thread-ului de upload\n");
		exit(-1);
	}

	r = pthread_join(download_thread, &status);
	if (r) {
		printf("Eroare la asteptarea thread-ului de download\n");
		exit(-1);
	}

	printf("Client %d has finished downloading everything. (upload is still on)\n", rank);

	r = pthread_join(upload_thread, &status);
	if (r) {
		printf("Eroare la asteptarea thread-ului de upload\n");
		exit(-1);
	}

	printf("Client %d has finished.\n", rank);
}

int main(int argc, char *argv[]) {
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
