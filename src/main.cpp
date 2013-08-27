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

enum PTFromServer : unsigned int{Accept};
enum PTFromClient : unsigned int{Connect, Ping};

template<typename T> class PacketHandler
{
	private:
		using HandlerFunc = std::function<void(T&, Packet&)>;
		std::unordered_map<unsigned int, HandlerFunc> functionHandlers;

	public:
		void handle(unsigned int mType, T& mCaller, Packet& mPacket)
		{
			try
			{
				auto itr(functionHandlers.find(mType));
				if(itr == end(functionHandlers)) { lo << lt("PacketHandler") << "Can't handle packet of type: " << mType << endl; return; }
				itr->second(mCaller, mPacket);
			}
			catch(std::exception& mException)	{ lo << lt("PacketHandler") << "Exception during packet handling: (" << mType << ")" << endl << mException.what() << endl; }
			catch(...)							{ lo << lt("PacketHandler") << "Unknown exception during packet handling: (" << mType << ")" << endl; }
		}

		HandlerFunc& operator[](unsigned int mType) { return functionHandlers[mType]; }
};

class ClientHandler
{
	private:
		unsigned int uid;
		UdpSocket& socket;
		PacketHandler<ClientHandler>& packetHandler;
		bool busy{false}; unsigned int untilTimeout{5};
		IpAddress ip; unsigned short port;

	public:
		ClientHandler(unsigned int mUid, UdpSocket& mSocket, PacketHandler<ClientHandler>& mPacketHandler, unsigned short mPort) : uid(mUid), socket(mSocket), packetHandler(mPacketHandler), port(mPort) { thread([this]{ run(); }).detach(); }

		void run()
		{
			while(true)
			{
				if(busy)
				{
					if(--untilTimeout <= 0) timeout();
				}

				this_thread::sleep_for(chrono::seconds(1));
			}
		}
		void timeout() { busy = false; lo << lt("ClientHandler #" + toStr(uid)) << "Timed out" << endl; }
		void accept(const IpAddress& mIp) { ip = mIp; busy = true; }
		void refreshTimeout() { untilTimeout = 5; }
		void handle(PTFromClient mType, Packet& mPacket) { packetHandler.handle(mType, *this, mPacket); refreshTimeout(); }
		void send(Packet mPacket) { if(socket.send(mPacket, ip, port) != Socket::Done) lo << lt("ClientHandler #" + toStr(uid)) << "Error sending" << endl; }

		bool isBusy() const { return busy; }
		bool getUid() const { return uid; }
};

struct Client
{
	PacketHandler<Client>& packetHandler;
	IpAddress serverIp; unsigned short serverPort;
	UdpSocket socket;

	bool accepted{false};
	unsigned int uid;

	Client(PacketHandler<Client>& mPacketHandler, const IpAddress& mServerIp, unsigned short mServerPort) : packetHandler(mPacketHandler), serverIp(mServerIp), serverPort(mServerPort)
	{
		//if(socket.bind(serverPort) != Socket::Done) { lo << lt("Client") << "Error binding socket to port: " << serverPort << endl; /*return;*/ }
		thread([this]{ run(); }).detach();
	}

	void sendConnectionRequest() { Packet pConnect; pConnect << PTFromClient::Connect; send(pConnect); }
	void connectionRequestAccepted(Packet& mPacket)
	{
		accepted = true; mPacket >> uid;
		lo << lt("Client") << "Connected to server! Uid: " << uid << endl;
	}

	void run()
	{
		lo << lt("Client") << "Client starting on ip: " << serverIp << " and port: " << serverPort << endl;
		lo << lt("Client") << "Client trying to connect!" << endl;

		sendConnectionRequest();

		while(true)
		{
			Packet packet; IpAddress sender; unsigned short senderPort;
			if(socket.receive(packet, sender, senderPort) == Socket::Done)
			{
				lo << lt("Client") << "Received packet from " << sender << " on port " << senderPort << endl;
				unsigned int type; packet >> type;

				if(!accepted && type == PTFromServer::Accept) connectionRequestAccepted(packet);
				else if(accepted) packetHandler.handle(type, *this, packet);
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

	Server(PacketHandler<ClientHandler>& mPacketHandler, unsigned short mPort) : packetHandler(mPacketHandler), port(mPort)
	{
		if(socket.bind(port) != Socket::Done) { lo << lt("Server") << "Error binding socket to port: " << port << endl; return; }
		thread([this]{ run(); }).detach();
	}

	void growIfNeeded()
	{
		if(containsAnyIf(clientHandlers, [](const unique_ptr<ClientHandler>& mCH){ return !mCH->isBusy(); })) return;
		lo << lt("Server") << "Creating new client handlers" << endl;
		for(int i{0}; i < 10; ++i) clientHandlers.emplace_back(new ClientHandler{lastUid++, socket, packetHandler, port});
	}

	void acceptConnection(const IpAddress& mClientIp, unsigned short mClientPort)
	{
		growIfNeeded();

		for(auto& c : clientHandlers)
		{
			if(c->isBusy()) continue;

			c->accept(mClientIp);
			c->refreshTimeout();

			Packet acceptPacket; acceptPacket << PTFromServer::Accept << c->getUid();
			socket.send(acceptPacket, mClientIp, mClientPort);

			lo << lt("Server") << "Accepted client (" << c->getUid() << ")" << endl;
			break;
		}
	}

	void run()
	{
		lo << lt("Server") << "Starting on port: " << port << endl;

		while(true)
		{
			Packet clientPacket; IpAddress clientIp; unsigned short clientPort;
			if(socket.receive(clientPacket, clientIp, clientPort) == Socket::Done)
			{
				lo << lt("Server") << "Received packet from " << clientIp << " on port " << clientPort << endl;
				unsigned int type; clientPacket >> (type);
				if(type == PTFromClient::Connect) acceptConnection(clientIp, clientPort);
				else
				{
					unsigned int chUid; clientPacket >> chUid;
					clientHandlers[chUid]->handle(static_cast<PTFromClient>(type), clientPacket);
				}
			}

			this_thread::sleep_for(chrono::milliseconds(1));
		}
	}
};

// ---

int main()
{
	/*PacketHandler<ClientHandler> sph;
	Server s(sph, 27015);

	this_thread::sleep_for(chrono::milliseconds(100));

	PacketHandler<Client> cph;
	Client c(cph, "127.0.0.1", 27015);

	while(true)
	{
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

			PacketHandler<ClientHandler> ph;
			Server s(ph, port);

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

			PacketHandler<Client> ph;
			Client c(ph, ip, port);

			while(true)
			{
				this_thread::sleep_for(chrono::milliseconds(1));
			}

			break;
		}
		case 2: return 0;
	}

	return 0;
}
