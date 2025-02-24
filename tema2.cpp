#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <fstream>
#include <fstream>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <climits>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100
#define TAG_OWNED_FILES 1
#define TAG_FILE_NAME 2
#define TAG_NUM_CHUNKS 3
#define TAG_HASH 4
#define TAG_ACK 5
#define TAG_REQUEST 6
#define TAG_SWARM_INFO 7
#define TAG_PEER_LIST 8

struct FileInfo {
    char filename[MAX_FILENAME];
    int num_chunks;
    char hashes[MAX_CHUNKS][HASH_SIZE + 1];
};

struct PeerData {
    int num_owned_files;
    int num_wanted_files;
    FileInfo owned_files[MAX_FILES];
    char wanted_files[MAX_FILES][MAX_FILENAME];
};

struct Swarm {
    int num_segments;
    char hashes[MAX_CHUNKS][HASH_SIZE + 1];
    std::vector<int> peers;
};

std::map<std::string, Swarm> tracker_data;
std::map<int, int> peer_upload;


PeerData data;

void read_input_file(int rank, PeerData &data) {
    std::string filename = "in" + std::to_string(rank) + ".txt";
    std::ifstream file(filename);

    if (!file) {
        std::cerr << "Eroare nu s-a putut deschide fisierul: " << filename << std::endl;
        exit(EXIT_FAILURE);
    }

    file >> data.num_owned_files;
    for (int i = 0; i < data.num_owned_files; i++) {
        file >> data.owned_files[i].filename >> data.owned_files[i].num_chunks;
        for (int j = 0; j < data.owned_files[i].num_chunks; j++) {
            file >> data.owned_files[i].hashes[j];
        }
    }

    file >> data.num_wanted_files;
    for (int i = 0; i < data.num_wanted_files; i++) {
        file >> data.wanted_files[i];
    }

    file.close();
}

// Se trimit informatiile catre tracker
void send_info_tracker(int rank, PeerData &data) {
    MPI_Send(&data.num_owned_files, 1, MPI_INT, TRACKER_RANK, TAG_OWNED_FILES, MPI_COMM_WORLD);

    for (int i = 0; i < data.num_owned_files; i++) {
        MPI_Send(data.owned_files[i].filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, TAG_FILE_NAME, MPI_COMM_WORLD);
        MPI_Send(&data.owned_files[i].num_chunks, 1, MPI_INT, TRACKER_RANK, TAG_NUM_CHUNKS, MPI_COMM_WORLD);
        for (int j = 0; j < data.owned_files[i].num_chunks; j++) {
            MPI_Send(data.owned_files[i].hashes[j], HASH_SIZE + 1, MPI_CHAR, TRACKER_RANK, TAG_HASH, MPI_COMM_WORLD);
        }
    }
}

void receive_file_info_from_peer(int peer_rank) {
    int num_files;
    char filename[MAX_FILENAME];
    int num_chunks;
    char hashes[MAX_CHUNKS][HASH_SIZE + 1];
    MPI_Recv(&num_files, 1, MPI_INT, peer_rank, TAG_OWNED_FILES, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    for (int i = 0; i < num_files; i++) {

        MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, peer_rank, TAG_FILE_NAME, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        MPI_Recv(&num_chunks, 1, MPI_INT, peer_rank, TAG_NUM_CHUNKS, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        // Fiecare fisier va avea un swarm 
        Swarm &swarm = tracker_data[filename];
        swarm.num_segments = num_chunks;

        for (int j = 0; j < num_chunks; j++) {
            MPI_Recv(hashes[j], HASH_SIZE + 1, MPI_CHAR, peer_rank, TAG_HASH, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            std::copy(hashes[j], hashes[j] + HASH_SIZE + 1, swarm.hashes[j]);
        }

        swarm.peers.push_back(peer_rank);
    }
}

// Alegem cel mai putin aglomerat peer pentru a rezolva eficient
int find_peer(std::vector<int>& peers) {
    int min_value, best_peer;
    min_value = INT_MAX;
    best_peer = -1;

    for (int peer : peers) {
        if (peer_upload[peer] < min_value) {
            min_value = peer_upload[peer];
            best_peer = peer;
        }
    }

    return best_peer;
}

void *download_thread_func(void *arg) {
    int rank = *(int *)arg;
    bool files_done = true;

    for (const auto &wanted_file : tracker_data) {
        Swarm swarm;
        int swarm_size, segments_downloaded;
        // Cerem informatiile despre swarm-ul unui fisier
        MPI_Send("REQUEST", 10, MPI_CHAR, TRACKER_RANK, TAG_REQUEST, MPI_COMM_WORLD);
        
        const std::string &filename = wanted_file.first;
        MPI_Send(filename.c_str(), filename.size() + 1, MPI_CHAR, TRACKER_RANK, TAG_REQUEST, MPI_COMM_WORLD);
        MPI_Recv(&swarm.num_segments, 1, MPI_INT, TRACKER_RANK, TAG_SWARM_INFO, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        if (swarm.num_segments == 0) {
            files_done = false;
            continue;
        }

        for (int i = 0; i < swarm.num_segments; i++) {
            MPI_Recv(swarm.hashes[i], HASH_SIZE + 1, MPI_CHAR, TRACKER_RANK, TAG_HASH, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        }

        MPI_Recv(&swarm_size, 1, MPI_INT, TRACKER_RANK, TAG_PEER_LIST, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        swarm.peers.resize(swarm_size);
        for (int &peer : swarm.peers) {
            MPI_Recv(&peer, 1, MPI_INT, TRACKER_RANK, TAG_PEER_LIST, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        }

        std::vector<std::string> downloaded_hashes(swarm.num_segments, "");
       
        segments_downloaded = 0;

        for (int i = 0; i < swarm.num_segments; i++) {
            int best_peer;
            if (!downloaded_hashes[i].empty()) {
                continue;
            }

            bool segment_downloaded = false;
            while (!segment_downloaded) {
                // Alegem cel mai bun peer de la care vom descarca segmentele
                best_peer = find_peer(swarm.peers);
                if (best_peer == -1) {
                    break;
                }
                // Trimitem cerere pentru fiecare segment
                MPI_Send("REQUEST", 10, MPI_CHAR, best_peer, TAG_REQUEST, MPI_COMM_WORLD);
                MPI_Send(swarm.hashes[i], HASH_SIZE + 1, MPI_CHAR, best_peer, TAG_REQUEST, MPI_COMM_WORLD);

                char response[10];
                MPI_Recv(response, 10, MPI_CHAR, best_peer, TAG_ACK, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                // Primire segment
                if (std::string(response) == "ACK") {
                    downloaded_hashes[i] = swarm.hashes[i];
                    segment_downloaded = true;
                    segments_downloaded++;
                    break;
                } else {
                    std::cerr << "Peer-ul nu poate trimite segmentul";
                }
            }

            // Cerem lista actualizata de peers la fiecare 10 segmente
            if (segments_downloaded > 0 && segments_downloaded == 10) {
                int new_swarm_size;

                MPI_Send("LIST", 10, MPI_CHAR, TRACKER_RANK, TAG_REQUEST, MPI_COMM_WORLD);
                MPI_Send(filename.c_str(), filename.size() + 1, MPI_CHAR, TRACKER_RANK, TAG_REQUEST, MPI_COMM_WORLD);

                MPI_Recv(&new_swarm_size, 1, MPI_INT, TRACKER_RANK, TAG_SWARM_INFO, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                swarm.peers.clear();
                for (int j = 0; j < new_swarm_size; j++) {
                    int peer;
                    MPI_Recv(&peer, 1, MPI_INT, TRACKER_RANK, TAG_PEER_LIST, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                    swarm.peers.push_back(peer);
                }

            }
        }

        bool all_segments_downloaded = true;
        for (const auto &hash : downloaded_hashes) {
            if (hash.empty()) {
                all_segments_downloaded = false;
                break;
            }
        }

        // Dupa ce fisierul este descarcat complet, se salveaza
        if (all_segments_downloaded) {
            MPI_Send("COMPLETE", 10, MPI_CHAR, TRACKER_RANK, TAG_ACK, MPI_COMM_WORLD);
            MPI_Send(filename.c_str(), filename.size() + 1, MPI_CHAR, TRACKER_RANK, TAG_ACK, MPI_COMM_WORLD);

            std::string output_filename = "client" + std::to_string(rank) + "_" + filename;
            std::ofstream output_file(output_filename);
            if (output_file) {
                for (const auto &hash : downloaded_hashes) {
                    output_file << hash << "\n";
                }
                output_file.close();
      
            } else {
                std::cerr << "Eroare: Fisierul nu poate fi salvat!\n";
            }
        } else {
            std::cerr << "Peer-ul nu poate descarca tot fisierul!\n";
        }
    }

    // Toate fisierele au fost descrcate
    if (files_done) {
        MPI_Send("DONE", 10, MPI_CHAR, TRACKER_RANK, TAG_ACK, MPI_COMM_WORLD);
        return NULL;
    }

    return NULL;
}

void *upload_thread_func(void *arg) {
    int rank = *(int *)arg;

    while (true) {
        MPI_Status status;
        char request_type[10] = {0};

        MPI_Recv(request_type, 10, MPI_CHAR, MPI_ANY_SOURCE, TAG_REQUEST, MPI_COMM_WORLD, &status);
    
        // Oprire fir de upload
        if (std::string(request_type) == "STOP") {
            break;
        }
        int peer_rank = status.MPI_SOURCE;

        if (std::string(request_type) == "REQUEST") {
            peer_upload[rank]++;
            char segment_hash[HASH_SIZE + 1] = {0};

            MPI_Recv(segment_hash, HASH_SIZE + 1, MPI_CHAR, peer_rank, TAG_REQUEST, MPI_COMM_WORLD, &status);
            
            bool has_segment = false;
            // Verifica daca exista segmentul cerut
            for (int i = 0; i < data.num_owned_files && !has_segment; i++) {
                for (int j = 0; j < data.owned_files[i].num_chunks; j++) {
                    if (strcmp(data.owned_files[i].hashes[j], segment_hash) == 0) {
                        has_segment = true;
                        break;
                    }
                }
            }
            // Confirmare daca exista sau nu segmentul cerut
            if (has_segment) {
                MPI_Send("ACK", 4, MPI_CHAR, peer_rank, TAG_ACK, MPI_COMM_WORLD);
            } else {
                MPI_Send("NACK", 5, MPI_CHAR, peer_rank, TAG_ACK, MPI_COMM_WORLD);
            }
            peer_upload[rank]--;
        } else {
            std::cerr << "Peer-ul nu cunoaste cererea!\n";
        }
    }

    return NULL;
}

void tracker(int numtasks, int rank) {
    int all_clients = 0;

    for (int i = 1; i < numtasks; ++i) {
        peer_upload[i] = 0;
    }

    for (int i = 1; i < numtasks; i++) {
        receive_file_info_from_peer(i);
    }

    for (int i = 1; i < numtasks; i++) {
        MPI_Send("ACK", 4, MPI_CHAR, i, TAG_ACK, MPI_COMM_WORLD);
    }

    while (all_clients < numtasks - 1) {
        MPI_Status status;
        char message_type[10] = {0};
        MPI_Recv(message_type, 10, MPI_CHAR, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        int peer_rank = status.MPI_SOURCE;

        if (std::string(message_type) == "REQUEST") {
            // Se trimite swarm-ul pentru fisierul curent
            char filename[MAX_FILENAME] = {0};

            MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, peer_rank, TAG_REQUEST, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            std::string requested_file(filename);

            if (tracker_data.find(requested_file) != tracker_data.end()) {
                Swarm &swarm = tracker_data[requested_file];
                int swarm_size;

                MPI_Send(&swarm.num_segments, 1, MPI_INT, peer_rank, TAG_SWARM_INFO, MPI_COMM_WORLD);

                for (int i = 0; i < swarm.num_segments; i++) {
                    MPI_Send(swarm.hashes[i], HASH_SIZE + 1, MPI_CHAR, peer_rank, TAG_HASH, MPI_COMM_WORLD);
                }
                
                swarm_size = swarm.peers.size();
                MPI_Send(&swarm_size, 1, MPI_INT, peer_rank, TAG_PEER_LIST, MPI_COMM_WORLD);

                for (int peer : swarm.peers) {
                    MPI_Send(&peer, 1, MPI_INT, peer_rank, TAG_PEER_LIST, MPI_COMM_WORLD);
                }
                swarm.peers.push_back(peer_rank);
            } else {
                int zero_segments = 0;
                MPI_Send(&zero_segments, 1, MPI_INT, peer_rank, TAG_SWARM_INFO, MPI_COMM_WORLD);
            }
        } else if (std::string(message_type) == "COMPLETE") {
            char filename[MAX_FILENAME] = {0};
            MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, peer_rank, TAG_ACK, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            tracker_data[filename].peers.push_back(peer_rank);
        } else if (std::string(message_type) == "DONE") {
            all_clients++;
            if (all_clients == numtasks - 1) {
                // Se trimite mesaj de STOP pentru a opri firul de upload
                for (int i = 1; i < numtasks; i++) {
                    MPI_Send("STOP", 10, MPI_CHAR, i, TAG_REQUEST, MPI_COMM_WORLD);
                }
            }
        } else if (std::string(message_type) == "LIST") {
            char filename[MAX_FILENAME];
            // Se trimite lista de peers actualizata
            MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, peer_rank, TAG_REQUEST, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            std::string requested_file(filename);

            if (tracker_data.find(requested_file) != tracker_data.end()) {
                Swarm &swarm = tracker_data[requested_file];
                int swarm_size = swarm.peers.size();
                MPI_Send(&swarm_size, 1, MPI_INT, peer_rank, TAG_SWARM_INFO, MPI_COMM_WORLD);
                for (int peer : swarm.peers) {
                    MPI_Send(&peer, 1, MPI_INT, peer_rank, TAG_PEER_LIST, MPI_COMM_WORLD);
            }
            } else {
                int empty_swarm = 0;
                MPI_Send(&empty_swarm, 1, MPI_INT, peer_rank, TAG_SWARM_INFO, MPI_COMM_WORLD);
            }
        } else {
            std::cerr << "Tracker-ul nu a primit un mesaj bun!\n";
        }
    }
}

void peer(int numtasks, int rank) {
    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r;

    read_input_file(rank, data);

    for (int i = 0; i < data.num_wanted_files; i++) {
        std::string filename(data.wanted_files[i]);
        if (tracker_data.find(filename) == tracker_data.end()) {
            tracker_data[filename] = Swarm(); 
        }
    }
    send_info_tracker(rank, data);

    char ack[4];
    MPI_Recv(ack, 4, MPI_CHAR, TRACKER_RANK, TAG_ACK, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    if (std::string(ack) != "ACK") {
        return;
    }

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