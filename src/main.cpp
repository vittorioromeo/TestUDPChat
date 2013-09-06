// Copyright (c) 2013 Vittorio Romeo
// License: Academic Free License ("AFL") v. 3.0
// AFL License page: http://opensource.org/licenses/AFL-3.0

#include <thread>
#include <future>
#include <chrono>
#include <functional>
#include <SFML/Audio.hpp>
#include <SFML/Graphics.hpp>
#include <SFML/System.hpp>
#include <SFML/Window.hpp>
#include <SFML/Network.hpp>
#include <SSVUtils/SSVUtils.h>
#include <SSVUtilsJson/SSVUtilsJson.h>
#include <SSVStart/SSVStart.h>

using namespace std;
using namespace std::chrono;
using namespace sf;
using namespace ssvu;
using namespace ssvuj;
using namespace ssvs;

int choice(initializer_list<string> mChoices)
{
	lo << lt("Choice") << endl;

	auto idx(0u);
	for(const auto& c : mChoices) { lo << idx << ". " << c << endl; ++idx; }

	int result;
	while(true)
	{
		cin >> result;
		if(result < 0 || result >= static_cast<int>(mChoices.size())) { lo << "Choice invalid, retry" << endl; continue; }
		return result;
	}
}
string strEnter() { lo << lt("Enter string") << endl; string result; cin >> result; return result; }

// ---

bool verbose{true};

enum PT : unsigned int{FromServer, FromClient};
enum PTFromServer : unsigned int{Accept, FSMessage};
enum PTFromClient : unsigned int{Connect, Ping, FCMessage};

template<typename TArg> void buildPacketHelper(Packet& mPacket, TArg&& mArg) { mPacket << mArg; }
template<typename TArg, typename... TArgs> void buildPacketHelper(Packet& mPacket, TArg&& mArg, TArgs&&... mArgs) { mPacket << mArg; buildPacketHelper(mPacket, std::forward<TArgs>(mArgs)...); }

template<PTFromServer TType> Packet buildPacketFromServer()										{ Packet result; result << PT::FromServer << TType; return result; }
template<PTFromServer TType, typename... TArgs> Packet buildPacketFromServer(TArgs&&... mArgs)	{ Packet result; result << PT::FromServer << TType; buildPacketHelper(result, std::forward<TArgs>(mArgs)...); return result; }

template<PTFromClient TType> Packet buildPacketFromClient()										{ Packet result; result << PT::FromClient << TType; return result; }
template<PTFromClient TType, typename... TArgs> Packet buildPacketFromClient(TArgs&&... mArgs)	{ Packet result; result << PT::FromClient << TType; buildPacketHelper(result, std::forward<TArgs>(mArgs)...); return result; }

template<typename T> class PacketHandler
{
	private:
		using HandlerFunc = ssvu::Func<void(T&, Packet&)>;
		std::unordered_map<unsigned int, HandlerFunc> functionHandlers;

	public:
		void handle(unsigned int mType, T& mCaller, Packet& mPacket)
		{
			try
			{
				auto itr(functionHandlers.find(mType));
				if(itr == end(functionHandlers))
				{
					if(verbose) lo << lt("PacketHandler") << "Can't handle packet of type: " << mType << endl;
					return;
				}
				itr->second(mCaller, mPacket);
			}
			catch(std::exception& mException)
			{
				lo << lt("PacketHandler") << "Exception during packet handling: (" << mType << ")" << endl << mException.what() << endl;
			}
			catch(...)
			{
				lo << lt("PacketHandler") << "Unknown exception during packet handling: (" << mType << ")" << endl;
			}
		}

		HandlerFunc& operator[](unsigned int mType) { return functionHandlers[mType]; }
};

struct Server;

class ClientHandler
{
	private:
		Server& server;
		unsigned int uid;
		UdpSocket& socket;
		PacketHandler<ClientHandler>& packetHandler;
		bool busy{false}; unsigned int untilTimeout{5};
		IpAddress clientIp; unsigned short clientPort;
		std::future<void> runFuture;

	public:
		ClientHandler(Server& mServer, unsigned int mUid, UdpSocket& mSocket, PacketHandler<ClientHandler>& mPacketHandler) : server(mServer), uid(mUid), socket(mSocket), packetHandler(mPacketHandler) { }

		void run()
		{
			while(busy)
			{
				if(--untilTimeout <= 0) timeout();
				this_thread::sleep_for(chrono::seconds(1));
			}
		}
		void timeout()
		{
			busy = false;
			lo << lt("ClientHandler #" + toStr(uid)) << "Timed out" << endl;
		}
		void accept(const IpAddress& mClientIp, unsigned short mClientPort)
		{
			clientIp = mClientIp;
			clientPort = mClientPort;
			busy = true;
			runFuture = std::async(std::launch::async, [this]{ run(); });
		}
		void refreshTimeout() { untilTimeout = 5; }
		void handle(PTFromClient mType, Packet& mPacket)
		{
			refreshTimeout();
			if(mType == PTFromClient::Ping) return;
			packetHandler.handle(mType, *this, mPacket);
		}
		void send(Packet mPacket) { if(socket.send(mPacket, clientIp, clientPort) != Socket::Done) lo << lt("ClientHandler #" + toStr(uid)) << "Error sending" << endl; }

		bool isBusy() const			{ return busy; }
		unsigned int getUid() const	{ return uid; }
		Server& getServer()			{ return server; }
};

struct Client
{
	PacketHandler<Client>& packetHandler;
	IpAddress serverIp; unsigned short serverPort;
	UdpSocket socket;
	std::future<void> runFuture;

	bool accepted{false}, busy{false};
	unsigned int uid;
	float pingTime{0.f};

	Client(PacketHandler<Client>& mPacketHandler, const IpAddress& mServerIp, unsigned short mServerPort) : packetHandler(mPacketHandler), serverIp(mServerIp), serverPort(mServerPort)
	{
		if(socket.bind(serverPort) != Socket::Done) { lo << lt("Client") << "Error binding socket to port: " << serverPort << endl; /*return;*/ }
		socket.setBlocking(false);

		busy = true;
		runFuture = std::async(std::launch::async, [this]{ run(); });
	}
	~Client() { busy = false; }

	void sendConnectionRequest()
	{
		Packet pConnect{buildPacketFromClient<PTFromClient::Connect>()};
		send(pConnect);
	}
	void connectionRequestAccepted(Packet mPacket)
	{
		accepted = true; mPacket >> uid;
		lo << lt("Client") << "Connected to server! Uid: " << uid << endl;
	}

	void run()
	{
		lo << lt("Client") << "Client starting on ip: " << serverIp << " and port: " << serverPort << endl;
		lo << lt("Client") << "Client trying to connect!" << endl;

		while(busy)
		{
			if(!accepted) { sendConnectionRequest(); this_thread::sleep_for(chrono::seconds(1)); }

			if(pingTime <= 0.f)
			{
				Packet pingPacket{buildPacketFromClient<PTFromClient::Ping>(uid)}; send(pingPacket);
				pingTime = 2000.f;
			}
			else --pingTime;

			Packet senderPacket; IpAddress senderIp; unsigned short senderPort;
			if(socket.receive(senderPacket, senderIp, senderPort) == Socket::Done)
			{
				if(verbose) lo << lt("Client") << "Received packet from " << senderIp << " on port " << senderPort << endl;

				unsigned int from; senderPacket >> from;
				if(from != PT::FromServer)
				{
					if(verbose) lo << lt("Client") << "Packet from " << senderIp << " on port " << senderPort << " not from server, ignoring" << endl;
				}
				else
				{
					unsigned int type; senderPacket >> type;
					if(!accepted && type == PTFromServer::Accept) connectionRequestAccepted(senderPacket);
					else if(accepted) packetHandler.handle(type, *this, senderPacket);
				}
			}

			this_thread::sleep_for(chrono::milliseconds(1));
		}
	}
	void send(Packet mPacket) { if(socket.send(mPacket, serverIp, serverPort) != Socket::Done) lo << lt("Client") << "Error sending" << endl; }
};

struct Server
{
	PacketHandler<ClientHandler>& packetHandler;
	std::vector<unique_ptr<ClientHandler>> clientHandlers;
	UdpSocket socket;
	unsigned short port;
	unsigned int lastUid{0};
	std::future<void> runFuture;
	bool busy{false};

	Server(PacketHandler<ClientHandler>& mPacketHandler, unsigned short mPort) : packetHandler(mPacketHandler), port(mPort)
	{
		if(socket.bind(port) != Socket::Done) { lo << lt("Server") << "Error binding socket to port: " << port << endl; return; }
		socket.setBlocking(false);

		busy = true;
		runFuture = std::async(std::launch::async, [this]{ run(); });
	}
	~Server() { busy = false; }

	void growIfNeeded()
	{
		if(containsAnyIf(clientHandlers, [](const unique_ptr<ClientHandler>& mCH){ return !mCH->isBusy(); })) return;
		lo << lt("Server") << "Creating new client handlers" << endl;
		for(int i{0}; i < 10; ++i) clientHandlers.emplace_back(new ClientHandler{*this, lastUid++, socket, packetHandler});
	}

	void acceptConnection(const IpAddress& mClientIp, unsigned short mClientPort)
	{
		growIfNeeded();

		for(auto& c : clientHandlers)
		{
			if(c->isBusy()) continue;

			Packet acceptPacket{buildPacketFromServer<PTFromServer::Accept>(c->getUid())};
			if(socket.send(acceptPacket, mClientIp, mClientPort) != Socket::Done)
			{
				lo << lt("Server") << "Error sending accept packet" << endl;
			}
			else
			{
				lo << lt("Server") << "Accepted client (" << c->getUid() << ")" << endl;
				c->accept(mClientIp, mClientPort);
				c->refreshTimeout();
				break;
			}
		}
	}

	void run()
	{
		lo << lt("Server") << "Starting on port: " << port << endl;

		while(busy)
		{
			Packet clientPacket; IpAddress clientIp; unsigned short clientPort;
			if(socket.receive(clientPacket, clientIp, clientPort) == Socket::Done)
			{
				if(verbose) lo << lt("Server") << "Received packet from " << clientIp << " on port " << clientPort << endl;

				unsigned int from; clientPacket >> from;
				if(from != PT::FromClient)
				{
					if(verbose) lo << lt("Server") << "Packet from " << clientIp << " on port " << clientPort << " not from client, ignoring" << endl;
				}
				else
				{
					unsigned int type; clientPacket >> (type);
					if(type == PTFromClient::Connect) acceptConnection(clientIp, clientPort);
					else
					{
						unsigned int chUid; clientPacket >> chUid;
						makeClientHandlerHandle(chUid, type, clientPacket);
					}
				}
			}

			this_thread::sleep_for(chrono::milliseconds(1));
		}
	}

	void makeClientHandlerHandle(unsigned int mUid, unsigned int mType, Packet mPacket)
	{
		if(mUid >= clientHandlers.size() || !clientHandlers[mUid]->isBusy())
		{
			if(verbose) lo << lt("Server") << "Tried to make ClientHandler #" << mUid << " handle packet of type " << mType << " but it's not busy " << endl;
			return;
		}
		clientHandlers[mUid]->handle(static_cast<PTFromClient>(mType), mPacket);
	}
	void makeAllClientHandlersHandle(unsigned int mType, Packet mPacket)
	{
		for(auto& c : clientHandlers) if(!c->isBusy()) c->handle(static_cast<PTFromClient>(mType), mPacket);
	}
	void sendToAllClients(Packet mPacket)
	{
		for(auto& c : clientHandlers) if(c->isBusy()) c->send(mPacket);
	}
};

// ---

int main()
{
	PacketHandler<ClientHandler> sph;
	sph[PTFromClient::FCMessage] = [](ClientHandler& mCH, Packet& mP)
	{
		string message; mP >> message;
		Packet msgPacket{buildPacketFromServer<PTFromServer::FSMessage>(mCH.getUid(), message)};
		mCH.getServer().sendToAllClients(msgPacket);
	};

	PacketHandler<Client> cph;
	cph[PTFromServer::FSMessage] = [](Client&, Packet& mP)
	{
		unsigned int uid; string message;
		mP >> uid >> message;
		lo << lt("Chat message from #" + toStr(uid)) << message << endl;
	};

	/*Server s(sph, 27015);
	this_thread::sleep_for(chrono::milliseconds(100));
	Client c(cph, "127.0.0.1", 27015);

	while(true)
	{
		string input;
		if(std::getline(std::cin, input))
		{
			if(input == "exit") break;
			Packet clientMsg{buildPacketFromClient<PTFromClient::FCMessage>(c.uid, input)};
			c.send(clientMsg);
		}

		this_thread::sleep_for(chrono::milliseconds(1));
	}
	return 0;*/

	lo << "Welcome to the test UDP chat." << endl;
	lo << "Are you server or client?" << endl;

	switch(choice({"Server", "Client", "Exit"}))
	{
		case 0:
		{
			lo << "What port?" << endl;
			int port{stoi(strEnter())};

			Server s(sph, port);

			while(true)
			{
				this_thread::sleep_for(chrono::milliseconds(1));
			}

			break;
		}
		case 1:
		{
			lo << "What ip?" << endl;
			string ip{strEnter()};

			lo << "What port?" << endl;
			int port{stoi(strEnter())};

			Client c(cph, ip, port);

			while(true)
			{
				string input;
				if(std::getline(std::cin, input))
				{
					Packet clientMsg{buildPacketFromClient<PTFromClient::FCMessage>(c.uid, input)};
					c.send(clientMsg);
				}

				this_thread::sleep_for(chrono::milliseconds(1));
			}

			break;
		}
		case 2: return 0;
	}

	return 0;
}

// Multiple clients not working
// What port should the client listen on?
