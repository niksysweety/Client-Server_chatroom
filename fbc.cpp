
#include <thread>
#include <time.h>
#include <unistd.h>

#include <iostream>
#include <memory>
#include <string>

#include <google/protobuf/timestamp.pb.h>
#include "fb.grpc.pb.h"

#include <iostream>
#include <grpc++/grpc++.h>

using fb::Request;
using fb::Response;
using fb::ChatServer;

using grpc::Status;
using fb::Message;
using fb::ListResponse;

using grpc::ClientReader;
using grpc::ClientReaderWriter;

using grpc::Channel;
using grpc::ClientContext;

using grpc::ClientWriter;

using namespace std;

// Create a message with given username and message text
Message CreateMessage(const string& username, const string& text) {
  Message chatMsg;

  chatMsg.set_text(text);
  chatMsg.set_username(username);

  google::protobuf::Timestamp* cur_time =
		  new google::protobuf::Timestamp();
  cur_time->set_seconds(time(NULL)); cur_time->set_nanos(0);

  chatMsg.set_allocated_timestamp(cur_time);

  return chatMsg;
}

class ChatClient {
public:

  ChatClient(shared_ptr<Channel> channel, string user_name, string server_address){
      clientStub_ = ChatServer::NewStub(channel);
      //username of current user
      username = user_name;
      serverAddress = server_address;
  }


  //Calls the List client stub on server to get the the list of available and following rooms
  void List(const string& username){
	ClientContext context;

    //List command request to server
    Request listRequest;

    listRequest.set_username(username);

    //List command response
    ListResponse listResponse;

    Status status = clientStub_->List(&context, listRequest, &listResponse);

    // Print all the rooms names(usernames) if there was no error in response
    if (status.ok()) {
        cout << "List of All Rooms: \n";
        for(string room : listResponse.complete_room_list()) {
        	cout << room << endl;
        }

        cout << "Rooms You're Following: \n";
        for(string room : listResponse.joined_room_list()) cout << room << endl;
    } else {
      // Print the error code received from Grpc
      cout << "error from rpc: " << status.error_code()
    		    << ": " << status.error_message()
                << endl;
    }

  }

  //Join request from client. Call the join stub. user1 is requesting to follow user2
  void Join(const string& username1, const string& username2) {
	ClientContext ctx;

    Request request;
    Response joinResponse;

    request.set_username(username1);
    request.add_params(username2);

    Status status = clientStub_->Join(&ctx, request, &joinResponse);

    if(status.ok()) {

      cout << joinResponse.text() << endl;
    } else{
      cout << "Error from RPC: " << status.error_code()
    			<< ": " << status.error_message()
                << endl;
    }
  }

  //Leave command: user1 wants to leave chatroom of user2
  void Leave(const string& username1, const string& username2){
    ClientContext context;
    Request request;
    Response leaveResponse;

    request.set_username(username1); request.add_params(username2);

    Status status = clientStub_->Leave(&context, request, &leaveResponse);

    if (status.ok()) {
      cout << leaveResponse.text() << endl;
    } else {
      cout << "Error from RPC: " << status.error_code() << ": " << status.error_message()
                << endl;
    }
  }

  string SignIn(const string& username){
    Request signInRequest;
    Response signInResponse;

    ClientContext context;

    signInRequest.set_username(username);

    Status status = clientStub_->SignIn(&context, signInRequest, &signInResponse);

    if(status.ok()){

      return signInResponse.text();
    } else{
      cout << "Error from RPC: " << status.error_code() << ": " << status.error_message()
                << endl;
      return "Error in RPC: ";
    }
  }

  //Chat command. We use RPC bidirectional stream to send & receive messages

  void Chat(const string& username, const string& messages, const string& waitSeconds) {

	ClientContext context;
    shared_ptr<ClientReaderWriter<Message, Message>> chatStream(clientStub_->Chat(&context));

    //Read chat from console and Send them to ChatServer
    thread chatMsgWriter([username, messages, waitSeconds, chatStream]() {

	  if(waitSeconds == "neverStop") {
		// Just started chatting. Send the magic text string
        string chatText = "Start Chat";
        Message chatMsg = CreateMessage(username, chatText);
        chatStream->Write(chatMsg);

        cout << "Enter Chat Messages: " << endl;

        while(getline(cin, chatText)){

          chatMsg = CreateMessage(username, chatText);
          chatStream->Write(chatMsg);
        }
        chatStream->WritesDone();

      } else {
		string chatText = "Start Chat";
        Message chatMsg = CreateMessage(username, chatText);
        chatStream->Write(chatMsg);

		int numMessages = stoi(messages);

        cout << "Enter chat Messages: \n";

        for(int chatMsgIndex=0; chatMsgIndex< numMessages; chatMsgIndex++) {

          chatText = "hello" + to_string(chatMsgIndex);

		  chatMsg = CreateMessage(username, chatText);
          chatStream->Write(chatMsg);
		  cout << chatText << endl;

		  usleep(stoi(waitSeconds));
        }

        chatStream->WritesDone();
	  }
	});

    //Read chat messages from stream
    thread chatMsgReader([username, chatStream]() {
       Message chatMsg;
       cout << "Latest up to 20 messages from all the joined rooms, (if at all any messages were sent):" << endl;

       // Read all messages
       while(chatStream->Read(&chatMsg)) {
	     cout << chatMsg.username() << " --> " << chatMsg.text() << endl;
       }
    });

    //Now we wait from both the reader and writer to complete before exiting
    chatMsgWriter.join();
    chatMsgReader.join();

  }

 private:
  string username;
  string serverAddress;

  unique_ptr<ChatServer::Stub> stub_;
  unique_ptr<ChatServer::Stub> clientStub_;
};

// Parse commands from user.
int commandParser(ChatClient* chatClient, string username, string input){

  size_t splitIndex = input.find_first_of(" ");

  if(splitIndex != string::npos) {
    string command = input.substr(0, splitIndex);

    // Handle commands with arguments
    if(input.length() == splitIndex + 1) {
      cout << "Invalid Command: Please give arguments in the command\n";
      return 0;
    }

    string cmdArgument = input.substr(splitIndex+1, (input.length()-splitIndex));
    if (command == "JOIN"){
      chatClient->Join(username, cmdArgument);

    } else if(command == "LEAVE") {
      chatClient->Leave(username, cmdArgument);

    } else {
      cout << "No such command..!"; return 0;
    }
  } else{
	// Commands without arguments

    if(input == "LIST"){
      chatClient->List(username);
    }
    else if (input == "CHAT"){
      // Return 1 to indicate to switch to chat mode
      return 1;
    }
    else {
      cout << "Invalid Command\n";
      return 0;
    }
  }
  return 0;
}

int main(int argc, char** argv) {

  // Create a RPC Client and connect to server using a RPC channel

  // Initialize all server credentials
  // These are default value which take hold if no input is given from console
  string maxMessages = "5000";

  string hostname = "localhost";
  string portnum = "10001";

  string username = "default_user";

  // Chat mode: indefinite by default
  string chatMode = "neverStop";

  if (argc < 4) 
  {
	  cout << "Please run the client again with hostname portno and username as arguments" << endl;
	  return 0;
  }

  // Read hostname
  hostname = argv[1];

  // Read port no
  portnum = argv[2];

  // Read username
  username = argv[3];

  string signInDetails = hostname + ":" + portnum;

  shared_ptr<Channel> channel = grpc::CreateChannel(signInDetails, grpc::InsecureChannelCredentials());

  ChatClient *chatClient = new ChatClient(channel, username, signInDetails);

  string signInResponse = chatClient->SignIn(username);

  //If somebody is already logged with the same username, exit with error
  if(signInResponse == "Error: Somebody is already logged in with this username"){

    cout << "Error: Somebody is already logged in with this username \n";
    return 0;

  } else {

    cout << signInResponse << endl;

    cout << "Enter commands: \n";
    string command;

    //Receive commands and parse them
    while(getline(cin, command)) {

      if(commandParser(chatClient, username, command) == 1)
    	  break;
    }

    //Start chat. Send and Receive chat messages from the chatStream
    chatClient->Chat(username, maxMessages, chatMode);
  }
  return 0;
}
