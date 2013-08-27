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

enum PTFromServer : unsigned int
{
	Accept
};

enum PTFromClient : unsigned int
{
	Connect,
	Ping
};

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
				if(itr == end(functionHandlers))
				{
					lo << lt("PacketHandler") << "Can't handle packet of type: " << mType << endl;
					return;
				}

				itr->second(mCaller, mPacket);
			}
			catch(std::exception& mException)
			{
				lo << "Exception during packet handling: (" << mType << ")" << endl;
				lo << mException.what() << endl;
			}
			catch(...)
			{
				lo << "Unknown exception during packet handling: (" << mType << ")" << endl;
			}
		}

		HandlerFunc& operator[](unsigned int mType) { return functionHandlers[mType]; }
};

unsigned int lastUid{0};

struct ClientHandler
{
	UdpSocket& socket;
	PacketHandler<ClientHandler>& packetHandler;
	bool busy{false};
	unsigned int uid{lastUid++}, untilTimeout{5};
	IpAddress ip; unsigned short port;

	ClientHandler(UdpSocket& mSocket, PacketHandler<ClientHandler>& mPacketHandler, unsigned short mPort) : socket(mSocket), packetHandler(mPacketHandler), port(mPort) { thread([this]{ run(); }).detach(); }

	void run()
	{
		while(true)
		{
			--untilTimeout;
			if(untilTimeout <= 0) lo << "TIMEOUT!!!" << "uid: " << uid << endl;
			this_thread::sleep_for(chrono::seconds(1));
		}
	}
	void accept(const IpAddress& mIp) { ip = mIp; busy = true; }
	void refreshTimeout() { untilTimeout = 5; }
	void handle(PTFromClient mType, Packet& mPacket) { packetHandler.handle(mType, *this, mPacket); refreshTimeout(); }
	void send(Packet mPacket)
	{
		if(socket.send(mPacket, ip, port) != Socket::Done)
			lo << "Clienthandler error sending" << endl;
	}
};

struct Client
{
	PacketHandler<Client>& packetHandler;
	IpAddress ip; unsigned short port;
	UdpSocket socket;

	bool accepted{false};
	unsigned int uid;

	Client(PacketHandler<Client>& mPacketHandler, const IpAddress& mIp, unsigned short mPort) : packetHandler(mPacketHandler), ip(mIp), port(mPort)
	{
		if(socket.bind(mPort) != Socket::Done)
		{
			lo << "Error binding socket to port: " << mPort << endl;
			return;
		}

		socket.setBlocking(true); thread([this]{ run(); }).detach();
	}

	void send(Packet mPacket)
	{
		if(socket.send(mPacket, ip, port) != Socket::Done)
			lo << "Client error sending" << endl;
	}

	void run()
	{
		lo << "Client starting on ip: " << ip << " and port: " << port << endl;
		lo << "Client trying to connect!" << endl;

		Packet pConnect; pConnect << PTFromClient::Connect;
		send(pConnect);

		while(true)
		{
			//lo << "Listening..." << endl;
			Packet packet;

			IpAddress sender; unsigned short senderPort;
			if(socket.receive(packet, sender, senderPort) != Socket::Done)
			{
				//lo << "Failed to receive packet" << endl;
			}
			else
			{
				lo << "Received packet from " << sender << " on port " << senderPort << endl;
				unsigned int type; packet >> type;

				if(!accepted && type == PTFromServer::Accept)
				{
					accepted = true; packet >> uid;
					lo << "Accepted! uid: " << uid << endl;
				}
				else if(accepted)
				{
					packetHandler.handle(type, *this, packet);
				}
			}

			this_thread::sleep_for(chrono::milliseconds(1));
		}
	}
};

struct Server
{
	PacketHandler<ClientHandler>& packetHandler;
	std::vector<unique_ptr<ClientHandler>> clientHandlers;
	UdpSocket socket;
	unsigned short port;

	Server(PacketHandler<ClientHandler>& mPacketHandler, unsigned short mPort) : packetHandler(mPacketHandler), port(mPort)
	{
		if(socket.bind(mPort) != Socket::Done)
		{
			lo << "Error binding socket to port: " << mPort << endl;
			return;
		}

		socket.setBlocking(true); thread([this]{ run(); }).detach();
	}

	void growIfNeeded()
	{
		if(containsAnyIf(clientHandlers, [](const unique_ptr<ClientHandler>& mCH){ return !mCH->busy; })) return;
		lo << lt("Server") << "Creating new client handlers" << endl;
		for(int i{0}; i < 10; ++i) clientHandlers.emplace_back(new ClientHandler{socket, packetHandler, port});
	}

	void run()
	{
		lo << "Server starting on port: " << port << endl;

		while(true)
		{
			//lo << "Listening..." << endl;
			Packet packet;

			IpAddress sender; unsigned short senderPort;
			if(socket.receive(packet, sender, senderPort) != Socket::Done)
			{
				//lo << "Failed to receive packet" << endl;
			}
			else
			{
				lo << "Received packet from " << sender << " on port " << senderPort << endl;
				unsigned int type; packet >> (type);
				if(type == PTFromClient::Connect)
				{
					for(auto& c : clientHandlers)
					{
						if(c->busy) continue;

						c->accept(sender);
						c->refreshTimeout();

						Packet acceptPacket;
						acceptPacket << PTFromServer::Accept << c->uid;
						socket.send(acceptPacket, sender, senderPort);

						lo << lt("Server") << "Accepted client (" << c->uid << ")" << endl;
					}
				}
				else
				{
					unsigned int chUid; packet >> chUid;
					clientHandlers[chUid]->handle(static_cast<PTFromClient>(type), packet);
				}
			}

			this_thread::sleep_for(chrono::milliseconds(1));
		}
	}
};

// ---

int main()
{
	PacketHandler<ClientHandler> sph;
	PacketHandler<Client> cph;

	Server(sph, 27015);
	this_thread::sleep_for(chrono::milliseconds(100));
	Client(cph, IpAddress::getLocalAddress(), 27015);

	while(true)
	{
		this_thread::sleep_for(chrono::milliseconds(1));
	}

	return 0;

	lo << "Welcome to the test UDP chat." << endl;
	lo << "Are you server or client?" << endl;

	switch(choice({"Server", "Client", "Exit"}))
	{
		case 0:
		{
			lo << "What port?" << endl;
			int port{stoi(strEnter())};

			PacketHandler<ClientHandler> ph;
			Server(ph, port);

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
			Client(ph, ip, port);

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
