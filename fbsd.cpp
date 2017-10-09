
#include <iostream>
#include <fstream>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/util/time_util.h>
#include <grpc++/grpc++.h>

#include "fb.grpc.pb.h"

using namespace std;
using google::protobuf::util::TimeUtil;
using google::protobuf::Timestamp;

using grpc::ServerBuilder;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Server;
using grpc::ServerContext;
using grpc::Status;

using grpc::Channel;
using grpc::ClientContext;

//Namespace definitions from our proto defined service
using fb::Request;
using fb::Response;
using fb::ListResponse;
using fb::ChatServer;
using fb::Message;

string hostname = "0.0.0.0";
string follower_file_suffix = "_received_chats_from_followers";

class Client {
public:
	string username;
	ServerReaderWriter<Message, Message>* chat_stream = 0;

	vector<Client *> followers_list;
	vector<Client *> following_list;
	int following_file_size = 0;

	bool is_connected = true;

	//custom equal to operator according to username
	bool operator==(const Client& c) { return (username == c.username); }
};

unordered_map<string, Client> client_db;

// Implementation of our Chat server
class ChatServiceImpl : public ChatServer::Service {

	Status SignIn(ServerContext *context, const Request *request, Response *response) override {
		string username = request->username();
		Client new_client;

		//If client is not present in list, add him
		if (! client_db.count(username)) {

			new_client.username = username;
			new_client.following_file_size = 0;
			client_db[username] = new_client;

			response->set_text("Signed in to server..!");
		} else {
			Client *client = &client_db[username];

			if (client->is_connected) {
				response->set_text("Error: Somebody is already logged in with this username");
			} else {
				string message = "Existing User. Signed in to server..!";
				client->is_connected = true;

				response->set_text(message);
			}

		}

		return Status::OK;
	}

	Status List(ServerContext *context, const Request *request, ListResponse *list_response) override {

		Client *client = &client_db[request->username()];

		// Add all clients' names
		for (auto c: client_db) {
			list_response->add_complete_room_list(c.first);
		}
		// Add all clients (rooms) the user has joined
		for (auto follower: client->following_list) {
			list_response->add_joined_room_list(follower->username);
		}

		return Status::OK;
	}

	Status Join(ServerContext *context, const Request *request, Response *response) override {

		string username1 = request->username();
		string username2 = request->params(0);

		if (username1 == username2) {
			response->set_text("You are already the owner of this room. No need to join externally\n");
			return Status::OK;
		}

		// If user2 does not exist, send error message
		if (! client_db.count(username2)) {
			response->set_text("FAILURE: requested username does not exist");
		} else {
			Client *user1 = &client_db[username1];
			Client *user2 = &client_db[username2];

			// If user1 had already joined user2, do nothing and notify
			auto is_following = find(user1->following_list.begin(), user1->following_list.end(), user2);

			if (is_following != user1->following_list.end()) {
				response->set_text("You are already follwing user " + username2);
			} else {
				// Add user1 to user2's followers list and user2 to user1's following list
				user2->followers_list.push_back(user1);

				user1->following_list.push_back(user2);

				response->set_text("Successfully joined " + username2);
			}
		}

		return Status::OK;
	}

	Status Leave(ServerContext *context, const Request *request, Response *response) override {

		string username1 = request->username();
		string username2 = request->params(0);

		// If user2 does not exist, send error message
		if (! client_db.count(username2)) {
			response->set_text("FAILURE: requested username does not exit");
		} else {
			Client *user1 = &client_db[username1];
			Client *user2 = &client_db[username2];

			// If user1 had not followed user2 in the first place, do nothing and notify
			auto following_iterator = find(user1->following_list.begin(), user1->following_list.end(), user2);
			auto follower_iterator = find(user2->followers_list.begin(), user2->followers_list.end(), user1);

			if (following_iterator == user1->following_list.end()) {
				response->set_text("You were not following user " + username2 + " in the first place.");
			} else {
				// Remove user1 from user2's followers list and user2 from user1's following list
				user2->followers_list.erase(follower_iterator);

				user1->following_list.erase(following_iterator);

				response->set_text("Successfully Left " + username2);
			}
		}

		return Status::OK;
	}

	Status Chat(ServerContext *context, ServerReaderWriter<Message, Message> *stream) override {
		Client *sender;
		Message chat_message;

		// Process all the messages from the stream
		while(stream->Read(&chat_message)) {
			string username = chat_message.username();
			sender = &client_db[username];

			// Write the message to receiver's file
			string filename = username + ".txt";
			ofstream Sender_file(filename, ios::out|ios::app|ios::in);

			//current time to be dumped
			Timestamp cur_timestamp = chat_message.timestamp();
			string cur_time = TimeUtil::ToString(cur_timestamp);

			string content =  username + "<>" + chat_message.text() + "<>" + cur_time + "\n";

			// If chat was ongoing, just write content to file
			if (chat_message.text() != "Start Chat") {
				Sender_file << content;
			} else {
				// Initialize the receiver stream
				if (sender->chat_stream == 0)
					sender->chat_stream = stream;

				vector<string> archived_messages;
				// If chat has just started, indicated by the magic words "Start Chat",
				// fetch and display latest 20 messages from the following file
				string cur_line;
				ifstream client_following_file(username + follower_file_suffix);

				// Calculate present size of file and reset stream to beginning
				string line; int file_size = 0;
				while (getline(client_following_file, line)) file_size++;

				// Update following file size, this is because if the server was restarted midway
				// all state of filesize was lost and has to be recalculated again. Else this variable
				// will be 0
				sender->following_file_size = file_size;

				client_following_file.clear();
				client_following_file.seekg(0, ios::beg);

				int count = 0;
				// Read last 20 lines from the following file
				while (getline(client_following_file, cur_line)) {
					if (sender->following_file_size > 20 && count < (sender->following_file_size - 20)) {
						count++; continue;
					}

					archived_messages.push_back(cur_line);
				}

				// Write these 20 messages to the stream for the client
				Message archived_message;
				for(string archived_msg_text: archived_messages) {

					archived_message.set_text(archived_msg_text);
					stream->Write(archived_message);
				}

				// Continue, as we don't want to send this dummy message to followers
				continue;
			}

			// Send the current received message to all the client's followers
			for(auto follower: sender->followers_list) {

				if (follower->chat_stream != 0 && follower->is_connected)
					follower->chat_stream->Write(chat_message);

				string follower_name = follower->username;
				string following_filename = follower_name + follower_file_suffix;

				// Write the message to the following message file of the client's followers
				ofstream following_file(following_filename, ios::out|ios::in|ios::app);
				following_file << content;
				follower->following_file_size++;
			}

		}

		sender->is_connected = false;
		return Status::OK;
	}
};


void StartChatServer(string port) {
	//bind port
	std::string server_address = hostname + ":" + port;
	ChatServiceImpl service;

	ServerBuilder builder;
	builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());

	//bind service to server
	builder.RegisterService(&service);

	std::unique_ptr<Server> server(builder.BuildAndStart());
	std::cout << "Server listening on " << server_address << std::endl;
	server->Wait();
}


int main(int argc, char ** argv) {
	if (argc < 2) {
		cout << "Please give the port number as argument" << endl;
		return 0;
	}

	// Read port number
	string port_no = argv[1];

	// Start chat server
	StartChatServer(port_no);

	return 0;
}
