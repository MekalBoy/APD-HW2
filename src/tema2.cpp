#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mpi.h>
#include <pthread.h>
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

#define SYNC_TIMER 10

using namespace std;

struct hashInfo {
	char hash[HASH_SIZE];
	int owned = 0; // technically a bool, but for ease of use with MPI this is an int (bool to int conversion is implicit in cpp)
	int id = -1;   // the # of the segment (they need to be in order)
};

struct fileInfo {
	char filename[MAX_FILENAME];
	vector<hashInfo> hashes; // the hash info of the segments of this file
	int segmentsNr;			 // the number of segments we have
	int completeNr;			 // number of segments needed to consider the file complete

	bool complete = false; // only used by client
	bool wanted = false;   // same

	vector<int> owners; // ids of clients that hold pieces of that file
};

MPI_Status mpiStatus;

pmr::unordered_map<string, fileInfo> myFiles;
int nrFilesWanted;
vector<string> filesWanted;

void *download_thread_func(void *arg) {
	int rank = *(int *)arg;

	int syncTimer = SYNC_TIMER;
	char filename[MAX_FILENAME];

	// anti-choke algorithm?
	pmr::unordered_map<int, int> disturbances;
	srand(rank);

	while (nrFilesWanted > 0) {

		// Sync (only happens every 10 segments)
		if (syncTimer == SYNC_TIMER) {
			syncTimer = 0;
			for (auto &pair : myFiles) {
				// Request updated swarms for each file that is wanted but still incomplete
				if (pair.second.wanted && !pair.second.complete) {
					MPI_Send(pair.first.c_str(), MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 2, MPI_COMM_WORLD);

					int nrOwners;
					MPI_Recv(&nrOwners, 1, MPI_INT, TRACKER_RANK, 2, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

					myFiles[pair.first].owners.resize(nrOwners);
					MPI_Recv(myFiles[pair.first].owners.data(), nrOwners, MPI_INT, TRACKER_RANK, 2, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
				}
			}
		} else {
			syncTimer++;
		}

		// pick a file of the ones I want (randomly)
		// pick a segment (not sure it matters much - might just be currSegment)
		// pick a seeder (the one disturbed least, randomly, increment "disturbance")
		// if I got the segment - increment currSegment of the file, increment syncTimer

		strncpy(filename, filesWanted[rand() % filesWanted.size()].c_str(), MAX_FILENAME);

		fileInfo *wantedFile = &myFiles[filename];

		hashInfo *wantedHash = &wantedFile->hashes[wantedFile->segmentsNr];

		vector<int> leastDisturbed;
		int minDisturbance = INT_MAX;
		for (int peer : wantedFile->owners) {
			// never seen this guy before - add to disturbances list (we may disturb 'em later)
			if (disturbances.find(peer) == disturbances.end()) {
				disturbances[peer] = 0;
			}

			if (disturbances[peer] < minDisturbance) {
				minDisturbance = disturbances[peer];
				leastDisturbed.clear();
				leastDisturbed.push_back(peer);
			} else if (disturbances[peer] == minDisturbance) {
				leastDisturbed.push_back(peer);
			}
		}

		// possible victims - now randomly choose one!
		int chosenPeer = leastDisturbed[rand() % leastDisturbed.size()];
		// printf("Client %d seeking file \"%s\" hash #%d - %s from %d\n", rank, filename, wantedFile->segmentsNr, wantedHash->hash, chosenPeer);

		MPI_Send(filename, MAX_FILENAME, MPI_CHAR, chosenPeer, 1, MPI_COMM_WORLD);
		MPI_Send(&wantedHash->hash, HASH_SIZE, MPI_CHAR, chosenPeer, 2, MPI_COMM_WORLD);

		MPI_Recv(&wantedHash->owned, 1, MPI_INT, chosenPeer, 3, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

		// Successfully received another hash
		if (wantedHash->owned == 1) {
			wantedFile->segmentsNr++;

			disturbances[chosenPeer]++;

			// Successfully completed a wanted file
			if (wantedFile->segmentsNr == wantedFile->completeNr - 1) { // segmentsNr starts at 0
				nrFilesWanted--;
				wantedFile->complete = true;

				filesWanted.erase(find(filesWanted.begin(), filesWanted.end(), filename));

				// Notify tracker
				MPI_Send(&filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);

				// write the now-complete file in clientR_filename
				ofstream clientFile;
				string clientFilename = "client" + to_string(rank) + "_" + filename;
				clientFile.open(clientFilename);
				printf("Client %d writing file \"%s\" to disk...\n", rank, filename);
				for (int i = 0; i < wantedFile->completeNr; i++) {
					string hashString = wantedFile->hashes[i].hash;
					clientFile << hashString.substr(0, HASH_SIZE);
					if (i != wantedFile->completeNr - 1) {
						clientFile << endl;
					}
				}
				clientFile.close();
			}
		} else {
			// Add more because of an unsuccessful disturbance - should give the peer more time to gather other hashes
			disturbances[chosenPeer] += 5;
			// test through `time make run` for quickness and `make run | wc -l` for miss-count (whilst making logs for each "check")
			// the lower the better for both
		}
	}

	return NULL;
}

void *upload_thread_func(void *arg) {
	int rank = *(int *)arg;

	char filenameUpload[MAX_FILENAME];

	while (true) {
		MPI_Recv(&filenameUpload, MAX_FILENAME, MPI_CHAR, MPI_ANY_SOURCE, 1, MPI_COMM_WORLD, &mpiStatus);

		// tracker will never send a "tag 1" except for when everyone has finished
		if (mpiStatus.MPI_SOURCE == TRACKER_RANK && mpiStatus.MPI_TAG == 1) {
			break;
		}

		fileInfo &requestedFile = myFiles[filenameUpload];

		char requestedHash[HASH_SIZE];

		MPI_Recv(&requestedHash, HASH_SIZE, MPI_CHAR, mpiStatus.MPI_SOURCE, 2, MPI_COMM_WORLD, &mpiStatus);

		// check if we have it and send a yes/no reply (1/0 int)

		for (hashInfo possibleHash : requestedFile.hashes) {
			if (strncmp(possibleHash.hash, requestedHash, HASH_SIZE) == 0) {
				MPI_Send(&possibleHash.owned, 1, MPI_INT, mpiStatus.MPI_SOURCE, 3, MPI_COMM_WORLD);
			}
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

			if (trackerFiles.find(clientFile.filename) == trackerFiles.end()) {
				trackerFiles[clientFile.filename] = clientFile;
			}

			vector<int> &owners = trackerFiles[clientFile.filename].owners;
			if (find(owners.begin(), owners.end(), i) == owners.end()) {
				owners.push_back(i);
			}
		}
	}

	for (auto &pair : trackerFiles) {
		cout << "[TRACKER] File " << pair.first << " is owned by ";
		for (auto &item : pair.second.owners) {
			cout << item << " ";
		}
		cout << endl;
	}

	// Receive wanted files from clients (and send hash information for each)

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

		// also send hash information back for each file
		for (int j = 0; j < wanted; j++) {
			char wantedFilename[MAX_FILENAME];
			MPI_Recv(wantedFilename, MAX_FILENAME, MPI_CHAR, i, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

			fileInfo wantedFile = trackerFiles[wantedFilename];

			MPI_Send(&wantedFile.completeNr, 1, MPI_INT, i, 0, MPI_COMM_WORLD);

			// We send a flattened array to minimize overhead

			std::vector<char> buffer(wantedFile.completeNr * HASH_SIZE, '\0');

			for (int k = 0; k < wantedFile.completeNr; k++) {
				std::strncpy(&buffer[k * HASH_SIZE], wantedFile.hashes[k].hash, HASH_SIZE);
			}

			MPI_Send(buffer.data(), wantedFile.completeNr * HASH_SIZE, MPI_CHAR, i, 0, MPI_COMM_WORLD);
		}
	}

	printf("[TRACKER] There are %d wanted files across the whole cluster.\n", wantedNr);

	// Go signal
	for (int i = 1; i < numtasks; i++) {
		MPI_Send(NULL, 0, MPI_INT, i, 0, MPI_COMM_WORLD);
	}

	// While loop to check for requests
	while (1) {
		char filename[MAX_FILENAME];

		MPI_Recv(&filename, MAX_FILENAME, MPI_CHAR, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &mpiStatus);

		// Deduce request type by tag
		switch (mpiStatus.MPI_TAG) {
			case 0:			// client finished a download (got a wanted file)
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
			case 1: // reserved for client-client communication (download/upload)
				break;
			case 2: { // Sync request from client
				fileInfo &requestedFile = trackerFiles[filename];

				int nrOwners = requestedFile.owners.size();
				MPI_Send(&nrOwners, 1, MPI_INT, mpiStatus.MPI_SOURCE, 2, MPI_COMM_WORLD);

				MPI_Send(requestedFile.owners.data(), nrOwners, MPI_INT, mpiStatus.MPI_SOURCE, 2, MPI_COMM_WORLD);

				// we can also add this requester to the file swarm
				vector<int> &owners = requestedFile.owners;
				if (find(owners.begin(), owners.end(), mpiStatus.MPI_SOURCE) == owners.end()) {
					owners.push_back(mpiStatus.MPI_SOURCE);
				}

				break;
			}
			default:
				printf("[TRACKER] How did we get here?\n");
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
		}

		myFiles[fileName] = info;
	}

	// Files the client wants

	inFile >> nrFilesWanted;

	for (int i = 0; i < nrFilesWanted; i++) {
		inFile >> fileName;
		cout << "Rank " << rank << " wants file \"" << fileName << "\".\n";

		filesWanted.push_back(fileName);

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
		fileInfo myInfo = pair.second;

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

	for (int i = 0; i < nrFilesWanted; i++) {
		const char *wantedFilename = filesWanted[i].c_str();
		MPI_Send(wantedFilename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);

		MPI_Recv(&myFiles[wantedFilename].completeNr, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

		// We receive a flattened array and then reconstruct it to minimize overhead

		vector<char> buffer(myFiles[wantedFilename].completeNr * HASH_SIZE);
		MPI_Recv(buffer.data(), myFiles[wantedFilename].completeNr * HASH_SIZE, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

		// reconstruct into hashes (strings)
		for (int k = 0; k < myFiles[wantedFilename].completeNr; k++) {
			string hash = string(&buffer[k * HASH_SIZE], HASH_SIZE);

			hashInfo info;
			strncpy(info.hash, hash.c_str(), HASH_SIZE);
			info.id = k;
			info.owned = false; // tracker ofc does not own this

			myFiles[wantedFilename].hashes.push_back(info);
		}
	}

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
